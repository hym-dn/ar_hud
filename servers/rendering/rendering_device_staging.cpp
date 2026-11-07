/**
 * @file rendering_device_staging.cpp
 * @brief RenderingDevice Staging Buffer + 数据更新实现
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-06-14
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include "os/memory.h"
#include "rendering_device.h"
#include "rendering_device_driver.h"

namespace arhud {

// ═══════════════════════════════════════════════════════════════════════
// Staging Buffer
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 初始化 Staging Buffer
 *
 * 创建 frame_count_ 个主 Staging Block 和 1 个小 Staging Block。
 * 所有 Block 使用 CPU 内存分配并持久映射，用于在帧录制期间暂存
 * 需要上传到 GPU 的 Buffer / Texture 数据。
 *
 * 主 Block 按帧轮转使用，小 Block 用于微小上传的聚合分配。
 *
 * @retval Error::kOK 初始化成功
 * @retval Error::kFailed Block 创建或映射失败
 *
 * @pre Initialize() 已调用
 * @pre device_driver_ 有效
 * @note 失败时会自动调用 DestroyStagingBuffer() 清理已分配资源
 */
Error RenderingDevice::InitializeStagingBuffer() {
  for (uint32 i = 0; i < frame_count_; ++i) {
    StagingBufferBlock block;
    block.driver_id = device_driver_->BufferCreate(
        staging_buffer_size_,
        BitField<BufferUsageBits>(BufferUsageBits::kTransferFrom) |
            BitField<BufferUsageBits>(BufferUsageBits::kTransferTo),
        MemoryAllocationType::kCpu);
    if (!block.driver_id.IsValid()) {
      ARHUD_LOG_ERROR(
          "kFailed",
          "RenderingDevice: failed to create staging buffer block %u", i);
      DestroyStagingBuffer();
      return Error::kFailed;
    }

    block.data_ptr = device_driver_->BufferMap(block.driver_id);
    if (block.data_ptr == nullptr) {
      ARHUD_LOG_ERROR("kFailed",
                      "RenderingDevice: failed to map staging buffer block %u",
                      i);
      device_driver_->BufferFree(block.driver_id);
      DestroyStagingBuffer();
      return Error::kFailed;
    }

    block.frame_references.Clear();
    block.fill_amount = 0;
    block.size = staging_buffer_size_;
    block.is_temporary = false;

    staging_buffer_blocks_.PushBack(block);
  }

  staging_buffer_total_size_ =
      static_cast<uint64>(frame_count_) * staging_buffer_size_;

  small_staging_block_.driver_id = device_driver_->BufferCreate(
      kSmallStagingBlockSize,
      BitField<BufferUsageBits>(BufferUsageBits::kTransferFrom) |
          BitField<BufferUsageBits>(BufferUsageBits::kTransferTo),
      MemoryAllocationType::kCpu);
  if (!small_staging_block_.driver_id.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "RenderingDevice: failed to create small staging buffer block");
    DestroyStagingBuffer();
    return Error::kFailed;
  }

  small_staging_block_.data_ptr =
      device_driver_->BufferMap(small_staging_block_.driver_id);
  if (small_staging_block_.data_ptr == nullptr) {
    ARHUD_LOG_ERROR(
        "kFailed", "RenderingDevice: failed to map small staging buffer block");
    device_driver_->BufferFree(small_staging_block_.driver_id);
    small_staging_block_.driver_id = BufferID();
    DestroyStagingBuffer();
    return Error::kFailed;
  }

  small_staging_block_.frame_references.Clear();
  small_staging_block_.fill_amount = 0;

  return Error::kOK;
}

/**
 * @brief 销毁 Staging Buffer
 *
 * 释放所有主 Staging Block 和小 Staging Block 的 GPU Buffer 资源，
 * 清空 data_ptr 指针并重置 total_size 计数器。
 *
 * @note 调用后 staging_buffer_blocks_ 被清空，staging_buffer_total_size_ 归零。
 * @note 可安全地在部分初始化失败时被 InitializeStagingBuffer() 调用。
 */
void RenderingDevice::DestroyStagingBuffer() {
  for (uint32 i = 0; i < staging_buffer_blocks_.Size(); ++i) {
    StagingBufferBlock& block = staging_buffer_blocks_[i];
    block.data_ptr = nullptr;
    if (block.driver_id.IsValid()) {
      device_driver_->BufferFree(block.driver_id);
      block.driver_id = BufferID();
    }
  }
  staging_buffer_blocks_.Clear();
  staging_buffer_total_size_ = 0;

  small_staging_block_.data_ptr = nullptr;
  if (small_staging_block_.driver_id.IsValid()) {
    device_driver_->BufferFree(small_staging_block_.driver_id);
    small_staging_block_.driver_id = BufferID();
  }
}

/**
 * @brief 从 Staging Buffer 分配空间（三级策略）
 *
 * 按以下优先级依次尝试分配：
 * 1. 小 Block 路径：p_size <= kSmallUploadMax 时，从小 Staging Block 尾部
 *    分配，32 字节对齐，适合大量微小上传的聚合。
 * 2. 主 Block 路径：p_size <= kLargeUploadThreshold 时，优先从当前轮转
 *    Block (staging_buffer_block_index_) 分配；若空间不足，扫描所有
 *    Block 寻找已可回收（IsReclaimable）且容量足够的 Block。
 * 3. 新 Block 路径：超出上述阈值或所有 Block 空间不足时，动态创建
 *    新 Block（大小取 256B 对齐的请求值，is_temporary 标记，引用帧
 *    全部完成后由 BeginFrame 延迟释放），受 kMaxStagingTotalSize
 *    总量上限保护。
 *
 * @param[in]  p_size       请求的字节数，必须 > 0
 * @param[out] r_allocation 分配结果，成功时包含 data_ptr、driver_id、offset
 *
 * @retval Error::kOK    分配成功
 * @retval Error::kFailed p_size 为 0、Block 创建/映射失败或总量超限
 *
 * @pre InitializeStagingBuffer() 已调用
 * @note 分配成功后调用方负责写入 data_ptr 所指向的内存区域
 * @note 每次分配均会记录当前帧号到 Block 的 frame_references，
 *       用于后续 IsReclaimable() 判断回收时机
 */
Error RenderingDevice::StagingBufferAllocate(
    uint32 p_size, StagingBufferAllocation& r_allocation) {
  if (p_size == 0) {
    return Error::kFailed;
  }

  if (p_size <= kSmallUploadMax) {
    static constexpr uint32 kSmallAlignment = 32;
    uint32 aligned_size =
        (p_size + kSmallAlignment - 1) & ~(kSmallAlignment - 1);

    if (aligned_size <=
        kSmallStagingBlockSize - small_staging_block_.fill_amount) {
      r_allocation.data_ptr =
          small_staging_block_.data_ptr + small_staging_block_.fill_amount;
      r_allocation.driver_id = small_staging_block_.driver_id;
      r_allocation.offset = small_staging_block_.fill_amount;
      small_staging_block_.fill_amount += aligned_size;
      // 记录当前帧引用
      if (small_staging_block_.frame_references.IsEmpty() ||
          small_staging_block_.frame_references.Back() != frame_number_) {
        small_staging_block_.frame_references.PushBack(frame_number_);
      }
      return Error::kOK;
    }
  }

  if (p_size <= kLargeUploadThreshold) {
    static constexpr uint32 kMediumAlignment = 256;
    uint32 aligned_size =
        (p_size + kMediumAlignment - 1) & ~(kMediumAlignment - 1);

    if (staging_buffer_block_index_ < staging_buffer_blocks_.Size()) {
      StagingBufferBlock& block =
          staging_buffer_blocks_[staging_buffer_block_index_];

      if (aligned_size <= block.size - block.fill_amount) {
        r_allocation.data_ptr = block.data_ptr + block.fill_amount;
        r_allocation.driver_id = block.driver_id;
        r_allocation.offset = block.fill_amount;
        block.fill_amount += aligned_size;
        // 记录当前帧引用
        if (block.frame_references.IsEmpty() ||
            block.frame_references.Back() != frame_number_) {
          block.frame_references.PushBack(frame_number_);
        }
        return Error::kOK;
      }
    }

    // 与 BeginFrame 的 Fence 等待同一边界：本帧开始时已确认
    // frame_number_ - frame_count_ 及更早的帧全部完成
    uint64 completed_frame =
        (frame_number_ >= frame_count_) ? (frame_number_ - frame_count_) : 0;
    for (uint32 i = 0; i < staging_buffer_blocks_.Size(); ++i) {
      StagingBufferBlock& block = staging_buffer_blocks_[i];
      if (block.frame_references.IsEmpty() ||
          block.IsReclaimable(completed_frame)) {
        if (aligned_size <= block.size - block.fill_amount) {
          staging_buffer_block_index_ = i;
          r_allocation.data_ptr = block.data_ptr + block.fill_amount;
          r_allocation.driver_id = block.driver_id;
          r_allocation.offset = block.fill_amount;
          block.fill_amount += aligned_size;
          // 记录当前帧引用
          if (block.frame_references.IsEmpty() ||
              block.frame_references.Back() != frame_number_) {
            block.frame_references.PushBack(frame_number_);
          }
          return Error::kOK;
        }
      }
    }
  }

  // 临时块按 256B 对齐取请求大小（不补齐主块大小）：
  // 生命周期只有 frame_count 帧，补齐只造成无谓的内存 churn。
  // aligned_size 同时用于块内偏移累加，保证块被复用时后续分配
  // 起点仍满足 256B 对齐。
  static constexpr uint32 kTemporaryBlockAlignment = 256;
  uint32 aligned_size =
      (p_size + kTemporaryBlockAlignment - 1) & ~(kTemporaryBlockAlignment - 1);
  uint32 new_block_size = aligned_size;

  if (staging_buffer_total_size_ + new_block_size > kMaxStagingTotalSize) {
    ARHUD_LOG_ERROR("kFailed",
                    "StagingBufferAllocate: total staging size exceeded limit "
                    "(%llu > %llu), size=%u",
                    static_cast<unsigned long long>(staging_buffer_total_size_ +
                                                    new_block_size),
                    static_cast<unsigned long long>(kMaxStagingTotalSize),
                    p_size);
    return Error::kFailed;
  }

  StagingBufferBlock new_block;
  new_block.driver_id = device_driver_->BufferCreate(
      new_block_size,
      BitField<BufferUsageBits>(BufferUsageBits::kTransferFrom) |
          BitField<BufferUsageBits>(BufferUsageBits::kTransferTo),
      MemoryAllocationType::kCpu);
  if (!new_block.driver_id.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed", "StagingBufferAllocate: failed to create new block, size=%u",
        new_block_size);
    return Error::kFailed;
  }

  new_block.data_ptr = device_driver_->BufferMap(new_block.driver_id);
  if (new_block.data_ptr == nullptr) {
    ARHUD_LOG_ERROR("kFailed",
                    "StagingBufferAllocate: failed to map new block");
    device_driver_->BufferFree(new_block.driver_id);
    return Error::kFailed;
  }

  new_block.frame_references.Clear();
  new_block.frame_references.PushBack(frame_number_);
  new_block.fill_amount = 0;
  new_block.size = new_block_size;
  new_block.is_temporary = true;

  staging_buffer_blocks_.PushBack(new_block);
  staging_buffer_total_size_ += new_block_size;
  staging_buffer_block_index_ = staging_buffer_blocks_.Size() - 1;

  StagingBufferBlock& block =
      staging_buffer_blocks_[staging_buffer_block_index_];
  r_allocation.data_ptr = block.data_ptr + block.fill_amount;
  r_allocation.driver_id = block.driver_id;
  r_allocation.offset = block.fill_amount;
  block.fill_amount += aligned_size;

  return Error::kOK;
}

/**
 * @brief 解除暂存缓冲区的内存映射
 *
 * 在执行 GPU 拷贝命令（CommandCopyBuffer / CommandCopyBufferToTexture）前调用，
 * 将 Staging Block 的 CPU 映射解除，确保驱动侧可见最新写入的数据。
 *
 * @param[in] p_driver_id 待解除映射的 Staging Block 驱动 ID
 *
 * @pre p_driver_id 对应一个已映射的 Staging Block
 * @note 与 StagingBufferRemap() 成对使用，形成 Unmap → GPU Copy → Remap 序列
 */
void RenderingDevice::StagingBufferUnmap(BufferID p_driver_id) {
  device_driver_->BufferUnmap(p_driver_id);
}

/**
 * @brief 重新映射暂存缓冲区
 *
 * 在 GPU 拷贝命令完成后调用，重新获取 Staging Block 的 CPU 映射指针，
 * 以便后续帧继续向该 Block 写入数据。
 *
 * 根据 p_driver_id 自动匹配小 Staging Block 或主 Staging Block，
 * 并更新其 data_ptr 为新映射地址。
 *
 * @param[in] p_driver_id 待重新映射的 Staging Block 驱动 ID
 *
 * @pre p_driver_id 对应一个已通过 StagingBufferUnmap() 解除映射的 Block
 * @note 若重新映射失败（返回 nullptr），将记录错误日志但不中断流程
 */
void RenderingDevice::StagingBufferRemap(BufferID p_driver_id) {
  uint8* new_ptr = device_driver_->BufferMap(p_driver_id);
  if (new_ptr == nullptr) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "StagingBufferRemap: failed to remap staging buffer, driver_id=%llu",
        static_cast<unsigned long long>(p_driver_id.GetId()));
    return;
  }

  if (small_staging_block_.driver_id == p_driver_id) {
    small_staging_block_.data_ptr = new_ptr;
    return;
  }

  for (uint32 i = 0; i < staging_buffer_blocks_.Size(); ++i) {
    if (staging_buffer_blocks_[i].driver_id == p_driver_id) {
      staging_buffer_blocks_[i].data_ptr = new_ptr;
      return;
    }
  }
}

/**
 * @brief 通过 Staging Buffer 更新 GPU Buffer 数据
 *
 * 将 p_data 中的 p_size 字节写入目标 GPU Buffer 的指定偏移处。
 * @note 必须在渲染线程调用：Staging Buffer 非线程安全、GL 上下文归
 *       渲染线程所有。跨线程请经 RenderServer 的 RID 分裂路径路由
 *       （ADR-0010）；初始化期 command_queue_ 尚未绑定时允许在主线程直接执行。
 *
 * 执行流程：分配 Staging 空间 → memcpy → Unmap →
 * CommandCopyBuffer → Remap，完成 CPU→GPU 数据传输。
 *
 * @param[in] p_buffer 目标 GPU Buffer 的资源 ID
 * @param[in] p_offset 目标 Buffer 内的字节偏移
 * @param[in] p_size   要更新的字节数
 * @param[in] p_data   源数据指针
 *
 * @retval Error::kOK    更新成功
 * @retval Error::kFailed 参数无效、RID 无效或 Staging 分配失败
 *
 * @pre p_buffer 为有效的已创建 Buffer RID
 * @pre p_data != nullptr 且 p_size > 0
 */
Error RenderingDevice::BufferUpdate(RDBufferID p_buffer, uint32 p_offset,
                                    uint32 p_size, const void* p_data) {
  if (p_buffer.IsNull() || p_data == nullptr || p_size == 0) {
    return Error::kFailed;
  }

  BufferID dst_driver_id = BufferGetDriverId(p_buffer);
  if (!dst_driver_id.IsValid()) {
    ARHUD_LOG_ERROR("kFailed", "BufferUpdate: invalid buffer RID");
    return Error::kFailed;
  }

  // staging buffer 非线程安全、GL 上下文归渲染线程；跨线程请经
  // RenderServer（RID 分裂） 路由（ADR-0010）。初始化期 command_queue_
  // 尚未绑定，允许在主线程直接执行。
  ARHUD_ASSERT(IsRenderThread() || command_queue_ == nullptr,
               "RenderingDevice::BufferUpdate must run on the render thread");

  StagingBufferAllocation alloc;
  Error err = StagingBufferAllocate(p_size, alloc);
  if (err != Error::kOK) {
    return err;
  }

  memcpy(alloc.data_ptr, p_data, p_size);

  StagingBufferUnmap(alloc.driver_id);

  BufferCopyRegion region;
  region.src_offset = alloc.offset;
  region.dst_offset = p_offset;
  region.size = p_size;

  device_driver_->CommandCopyBuffer(alloc.driver_id, dst_driver_id,
                                    VectorView<BufferCopyRegion>(&region, 1));

  StagingBufferRemap(alloc.driver_id);

  return Error::kOK;
}

/**
 * @brief 通过 Staging Buffer 更新 GPU Texture 数据
 *
 * 将 p_data 中的像素数据写入目标纹理的指定层（layer）和 Mipmap 层级。
 * 拷贝区域尺寸由纹理格式中对应 Mipmap 层级的宽高深决定（MipmapSize）。
 *
 * @note 必须在渲染线程调用：Staging Buffer 非线程安全、GL 上下文归
 *       渲染线程所有。跨线程请经 RenderServer 的 RID 分裂路径路由
 *       （ADR-0010）；初始化期 command_queue_ 尚未绑定时允许在主线程直接执行。
 *
 * 执行流程：分配 Staging 空间 → memcpy → Unmap →
 * CommandCopyBufferToTexture → Remap。
 *
 * @param[in] p_texture   目标 GPU Texture 的资源 ID
 * @param[in] p_layer     纹理数组中的层索引（2D 纹理通常为 0）
 * @param[in] p_mipmap    目标 Mipmap 层级（0 为最高分辨率）
 * @param[in] p_data      源像素数据指针
 * @param[in] p_data_size 源数据的字节数
 *
 * @retval Error::kOK    更新成功
 * @retval Error::kFailed 参数无效、RID 无效或 Staging 分配失败
 *
 * @pre p_texture 为有效的已创建 Texture RID
 * @pre p_data != nullptr 且 p_data_size > 0
 */
Error RenderingDevice::TextureUpdate(RDTextureID p_texture, uint32 p_layer,
                                     uint32 p_mipmap, const void* p_data,
                                     uint32 p_data_size) {
  if (p_texture.IsNull() || p_data == nullptr || p_data_size == 0) {
    return Error::kFailed;
  }

  // staging buffer 非线程安全、GL 上下文归渲染线程；跨线程请经
  // RenderServer（RID 分裂） 路由（ADR-0010）。初始化期 command_queue_
  // 尚未绑定，允许在主线程直接执行。
  ARHUD_ASSERT(IsRenderThread() || command_queue_ == nullptr,
               "RenderingDevice::TextureUpdate must run on the render thread");

  TextureID dst_driver_id = TextureGetDriverId(p_texture);
  if (!dst_driver_id.IsValid()) {
    ARHUD_LOG_ERROR("kFailed", "TextureUpdate: invalid texture RID");
    return Error::kFailed;
  }

  const TextureFormat& format = TextureGetFormat(p_texture);

  StagingBufferAllocation alloc;
  Error err = StagingBufferAllocate(p_data_size, alloc);
  if (err != Error::kOK) {
    return err;
  }

  std::memcpy(alloc.data_ptr, p_data, p_data_size);

  StagingBufferUnmap(alloc.driver_id);

  BufferTextureCopyRegion region;
  region.buffer_offset = alloc.offset;
  region.row_pitch = 0;
  region.texture_subresource.aspect = TextureAspect::kColor;
  region.texture_subresource.layer = p_layer;
  region.texture_subresource.mipmap = p_mipmap;
  region.texture_offset_x = 0;
  region.texture_offset_y = 0;
  region.texture_offset_z = 0;
  region.texture_region_width = MipmapSize(format.width, p_mipmap);
  region.texture_region_height = MipmapSize(format.height, p_mipmap);
  region.texture_region_depth = MipmapSize(format.depth, p_mipmap);

  device_driver_->CommandCopyBufferToTexture(
      alloc.driver_id, dst_driver_id,
      VectorView<BufferTextureCopyRegion>(&region, 1));

  StagingBufferRemap(alloc.driver_id);

  return Error::kOK;
}

/**
 * @brief 生成纹理 Mipmap 链
 *
 * 根据纹理创建时指定的 Mipmap 层级数，由驱动自动计算并填充
 * 从 level 0 向下逐级递减的所有 Mipmap 层级。
 *
 * @note 必须在渲染线程调用：OpenGL 上下文归渲染线程所有。跨线程请经
 *       RenderServer 的 RID 分裂路径路由（ADR-0010）；初始化期
 *       command_queue_ 尚未绑定时允许在主线程直接执行。
 *
 * @param[in] p_texture 目标纹理的资源 ID
 *
 * @retval Error::kOK    Mipmap 生成成功，或纹理仅有 1 级（无需生成）
 * @retval Error::kFailed p_texture 无效或纹理对象不存在
 *
 * @pre p_texture 为有效的已创建 Texture RID
 * @note 若纹理 format.mipmaps <= 1，直接返回 kOK 不执行任何操作
 */
Error RenderingDevice::TextureGenerateMipmaps(RDTextureID p_texture) {
  if (p_texture.IsNull()) {
    return Error::kFailed;
  }

  // GL 上下文归渲染线程所有；跨线程调用请经 RenderServer（RID
  // 分裂）路由（ADR-0010）。 初始化期 command_queue_
  // 尚未绑定，允许在主线程直接执行。
  ARHUD_ASSERT(
      IsRenderThread() || command_queue_ == nullptr,
      "RenderingDevice::TextureGenerateMipmaps must run on the render thread");

  const Texture* texture = texture_owner_.GetOrNull(p_texture.GetRid());
  if (texture == nullptr) {
    return Error::kFailed;
  }

  if (texture->format.mipmaps <= 1) {
    return Error::kOK;
  }

  device_driver_->TextureGenerateMipmaps(texture->driver_id);
  return Error::kOK;
}

// ═══════════════════════════════════════════════════════════════════════
// VRAM 预算跟踪
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 估算纹理 VRAM 占用
 *
 * 根据纹理宽度、高度、数据格式和 Mipmap 层级数，计算纹理所有层级
 * 的总 VRAM 占用字节数。每像素字节数（bpp）通过 switch 覆盖常用
 * DataFormat 枚举值确定，未识别的格式默认按 4 bpp 计算。
 *
 * 每个 Mipmap 层级的尺寸通过 MipmapSize() 计算（即 max(1, dim >> level)）。
 *
 * @param[in] p_w       纹理基础宽度（像素）
 * @param[in] p_h       纹理基础高度（像素）
 * @param[in] p_format  纹理数据格式
 * @param[in] p_mipmaps Mipmap 层级总数（含 level 0）
 *
 * @return 所有 Mipmap 层级的总字节数估算值
 *
 * @note 此函数为纯 CPU 估算，不查询 GPU 实际分配
 * @note 不包含压缩格式（BC/ETC/ASTC）的特殊处理，压缩格式结果可能偏大
 */
uint32 RenderingDevice::EstimateTextureVram(uint32 p_w, uint32 p_h,
                                            DataFormat p_format,
                                            uint32 p_mipmaps) {
  // 估算每像素字节数（覆盖常用格式）
  uint32 bpp = 4;  // 默认 4 字节（R8G8B8A8）
  switch (p_format) {
    case DataFormat::kR4G4UnormPack8:
      bpp = 1;
      break;
    case DataFormat::kR4G4B4A4UnormPack16:
    case DataFormat::kB4G4R4A4UnormPack16:
    case DataFormat::kR5G6B5UnormPack16:
    case DataFormat::kB5G6R5UnormPack16:
    case DataFormat::kR5G5B5A1UnormPack16:
    case DataFormat::kB5G5R5A1UnormPack16:
    case DataFormat::kA1R5G5B5UnormPack16:
      bpp = 2;
      break;
    case DataFormat::kR8Unorm:
    case DataFormat::kR8Snorm:
    case DataFormat::kR8Uint:
    case DataFormat::kR8Sint:
    case DataFormat::kR8Srgb:
    case DataFormat::kS8Uint:
      bpp = 1;
      break;
    case DataFormat::kR8G8Unorm:
    case DataFormat::kR8G8Snorm:
    case DataFormat::kR8G8Uint:
    case DataFormat::kR8G8Sint:
    case DataFormat::kR8G8Srgb:
      bpp = 2;
      break;
    case DataFormat::kR8G8B8Unorm:
    case DataFormat::kR8G8B8Snorm:
    case DataFormat::kR8G8B8Uint:
    case DataFormat::kR8G8B8Sint:
    case DataFormat::kR8G8B8Srgb:
    case DataFormat::kB8G8R8Unorm:
    case DataFormat::kB8G8R8Snorm:
    case DataFormat::kB8G8R8Uint:
    case DataFormat::kB8G8R8Sint:
    case DataFormat::kB8G8R8Srgb:
      bpp = 3;
      break;
    case DataFormat::kR8G8B8A8Unorm:
    case DataFormat::kR8G8B8A8Snorm:
    case DataFormat::kR8G8B8A8Uint:
    case DataFormat::kR8G8B8A8Sint:
    case DataFormat::kR8G8B8A8Srgb:
    case DataFormat::kB8G8R8A8Unorm:
    case DataFormat::kB8G8R8A8Snorm:
    case DataFormat::kB8G8R8A8Uint:
    case DataFormat::kB8G8R8A8Sint:
    case DataFormat::kB8G8R8A8Srgb:
    case DataFormat::kA2R10G10B10UnormPack32:
    case DataFormat::kA2B10G10R10UnormPack32:
    case DataFormat::kB10G11R11UfloatPack32:
    case DataFormat::kE5B9G9R9UfloatPack32:
    case DataFormat::kD16Unorm:
    case DataFormat::kD16UnormS8Uint:
    case DataFormat::kD24UnormS8Uint:
    case DataFormat::kD32Sfloat:
      bpp = 4;
      break;
    case DataFormat::kR16Unorm:
    case DataFormat::kR16Snorm:
    case DataFormat::kR16Uint:
    case DataFormat::kR16Sint:
    case DataFormat::kR16Sfloat:
    case DataFormat::kR16G16Unorm:
    case DataFormat::kR16G16Snorm:
    case DataFormat::kR16G16Uint:
    case DataFormat::kR16G16Sint:
    case DataFormat::kR16G16Sfloat:
      bpp = 2;
      break;
    case DataFormat::kR16G16B16A16Unorm:
    case DataFormat::kR16G16B16A16Snorm:
    case DataFormat::kR16G16B16A16Uint:
    case DataFormat::kR16G16B16A16Sint:
    case DataFormat::kR16G16B16A16Sfloat:
    case DataFormat::kD32SfloatS8Uint:
      bpp = 8;
      break;
    case DataFormat::kR32Uint:
    case DataFormat::kR32Sint:
    case DataFormat::kR32Sfloat:
      bpp = 4;
      break;
    case DataFormat::kR32G32Uint:
    case DataFormat::kR32G32Sint:
    case DataFormat::kR32G32Sfloat:
      bpp = 8;
      break;
    case DataFormat::kR32G32B32Uint:
    case DataFormat::kR32G32B32Sint:
    case DataFormat::kR32G32B32Sfloat:
      bpp = 12;
      break;
    case DataFormat::kR32G32B32A32Uint:
    case DataFormat::kR32G32B32A32Sint:
    case DataFormat::kR32G32B32A32Sfloat:
      bpp = 16;
      break;
    default:
      bpp = 4;
      break;
  }

  // 累加所有 Mipmap 层级的尺寸
  uint32 total = 0;
  for (uint32 level = 0; level < p_mipmaps; ++level) {
    uint32 w = MipmapSize(p_w, level);
    uint32 h = MipmapSize(p_h, level);
    total += w * h * bpp;
  }

  return total;
}

}  // namespace arhud
