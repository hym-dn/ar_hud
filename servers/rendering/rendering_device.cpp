/**
 * @file rendering_device.cpp
 * @brief RenderingDevice 实现 — 生命周期 + 资源 CRUD（其余拆分到
 * rendering_device_*.cpp）
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-10
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include "rendering_device.h"

#include "os/memory.h"
#include "rendering_device_driver.h"

namespace arhud {

// ═══════════════════════════════════════════════════════════════════════
// 生命周期
// ═══════════════════════════════════════════════════════════════════════
/**
 * @brief 析构函数
 *
 * 如果渲染设备已初始化，则调用 Finalize() 释放所有资源。
 * 确保在对象销毁前正确清理 GPU 资源和内部状态。
 *
 * @note 推荐在析构前显式调用 Finalize()，以便处理可能的错误。
 *
 * @see Finalize()
 */
RenderingDevice::~RenderingDevice() {
  if (initialized_) {
    Finalize();
  }
}

/**
 * @brief 初始化渲染设备
 *
 * 创建 RDD（通过 RCD::CreateDeviceDriver()）、Staging Buffer，
 * 以及默认 RenderPass（1 个颜色附件 + 1 个深度模板附件）和默认 Framebuffer（FBO
 * 0 = 屏幕缓冲区）。
 *
 * 初始化流程：
 *   1. 记录渲染线程 ID（当前线程）
 *   2. 通过 RCD 创建 RDD 并初始化
 *   3. 创建 Staging Buffer（CPU→GPU 传输用）
 *   4. 创建默认 RenderPass + Framebuffer
 *
 * @param[in] p_context_driver
 * 上下文驱动指针（不拥有所有权，由调用者管理生命周期）
 * @param[in] p_frame_count 帧缓冲数量（2 ≤ p_frame_count ≤ kMaxFrameCount）
 * @param[in] p_staging_buffer_size Staging Buffer 每块大小（字节）
 * @param[in] p_context_id 可选的 RCD Context ID，用于在 RDD 初始化前
 * MakeCurrent
 * @param[in] p_surface 可选的 RCD Surface ID，用于在 RDD 初始化前 MakeCurrent
 *
 * @return Error::kOK 初始化成功
 * @retval Error::kAlreadyExists 已初始化
 * @retval Error::kFailed RDD 创建/初始化失败，或 Staging 创建失败
 *
 * @pre p_context_driver != nullptr 且已成功初始化
 * @pre 必须在拥有 GL 上下文的渲染线程上调用
 * @pre 如果提供 p_context_id 和 p_surface，GL 上下文将在此方法内 MakeCurrent
 *
 * @note 失败时会自动回滚已分配的资源（RDD、Staging Buffer）
 *
 * @see Finalize()
 * @see IRenderingContextDriver::CreateDeviceDriver()
 */
Error RenderingDevice::Initialize(
    IRenderingContextDriver* p_context_driver, uint32 p_frame_count,
    uint32 p_staging_buffer_size,
    IRenderingContextDriver::ContextID p_context_id,
    IRenderingContextDriver::SurfaceID p_surface) {
  ARHUD_ASSERT(p_context_driver != nullptr,
               "RenderingContextDriver must not be null");
  ARHUD_ASSERT(p_frame_count >= 2 && p_frame_count <= kMaxFrameCount,
               "Frame count must be between 2 and kMaxFrameCount");

  if (initialized_) {
    return Error::kAlreadyExists;
  }

  render_thread_id_ = Thread::GetCallerId();

  context_driver_ = p_context_driver;

  // 如果提供了 context/surface，在 RDD 初始化前 MakeCurrent
  // 这确保 gladLoadGL 在有 GL 上下文的情况下执行。
  if (p_context_id != 0 || p_surface != 0) {
    Error err = context_driver_->MakeCurrent(p_context_id, p_surface);
    if (err != Error::kOK) {
      ARHUD_LOG_ERROR(
          "kFailed",
          "RenderingDevice: failed to make context current before RDD init");
      context_driver_ = nullptr;
      return Error::kFailed;
    }
  }

  device_driver_ = context_driver_->CreateDeviceDriver();
  if (device_driver_ == nullptr) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice: failed to create device driver");
    context_driver_ = nullptr;
    return Error::kFailed;
  }

  Error err = device_driver_->Initialize(0, p_frame_count);
  if (err != Error::kOK) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice: failed to initialize device driver");
    context_driver_->DriverFree(device_driver_);
    device_driver_ = nullptr;
    context_driver_ = nullptr;
    return Error::kFailed;
  }

  frame_count_ = p_frame_count;
  staging_buffer_size_ = p_staging_buffer_size;
  frame_number_ = 0;

  err = InitializeStagingBuffer();
  if (err != Error::kOK) {
    context_driver_->DriverFree(device_driver_);
    device_driver_ = nullptr;
    context_driver_ = nullptr;
    return err;
  }

  for (uint32 i = 0; i < frame_count_; ++i) {
    frames_[i].Clear();
  }

  // ── 创建默认 RenderPass（1 个颜色附件 + 1 个深度模板附件）──
  Attachment default_attachments[2];
  default_attachments[0].format = DataFormat::kR8G8B8A8Unorm;
  default_attachments[0].samples = TextureSamples::k1;
  default_attachments[0].load_op = AttachmentLoadOp::kClear;
  default_attachments[0].store_op = AttachmentStoreOp::kStore;
  default_attachments[0].stencil_load_op = AttachmentLoadOp::kDontCare;
  default_attachments[0].stencil_store_op = AttachmentStoreOp::kDontCare;
  default_attachments[0].initial_layout =
      TextureLayout::kColorAttachmentOptimal;
  default_attachments[0].final_layout = TextureLayout::kColorAttachmentOptimal;

  default_attachments[1].format = DataFormat::kD24UnormS8Uint;
  default_attachments[1].samples = TextureSamples::k1;
  default_attachments[1].load_op = AttachmentLoadOp::kClear;
  default_attachments[1].store_op = AttachmentStoreOp::kStore;
  default_attachments[1].stencil_load_op = AttachmentLoadOp::kDontCare;
  default_attachments[1].stencil_store_op = AttachmentStoreOp::kDontCare;
  default_attachments[1].initial_layout =
      TextureLayout::kDepthStencilAttachmentOptimal;
  default_attachments[1].final_layout =
      TextureLayout::kDepthStencilAttachmentOptimal;

  Subpass default_subpass;
  AttachmentReference color_ref;
  color_ref.attachment = 0;
  color_ref.layout = TextureLayout::kColorAttachmentOptimal;
  default_subpass.color_references.PushBack(color_ref);

  AttachmentReference depth_ref;
  depth_ref.attachment = 1;
  depth_ref.layout = TextureLayout::kDepthStencilAttachmentOptimal;
  default_subpass.depth_stencil_reference = depth_ref;

  default_render_pass_ =
      RenderPassCreate(VectorView<Attachment>(default_attachments, 2),
                       VectorView<Subpass>(&default_subpass, 1),
                       VectorView<SubpassDependency>(nullptr, 0));

  if (default_render_pass_.IsNull()) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice: failed to create default render pass");
    DestroyStagingBuffer();
    context_driver_->DriverFree(device_driver_);
    device_driver_ = nullptr;
    context_driver_ = nullptr;
    return Error::kFailed;
  }

  // ── 创建默认 Framebuffer 簿记（FBO 0 = 屏幕缓冲区）──
  auto* fb = ARHUD_NEW(Framebuffer);
  fb->driver_id = FramebufferID(static_cast<uint64>(0));
  fb->render_pass = default_render_pass_.GetRid();
  fb->width = 0;
  fb->height = 0;

  RID fb_rid = framebuffer_owner_.MakeRid(fb);
  default_framebuffer_ = RDFramebufferID(fb_rid);

  initialized_ = true;
  return Error::kOK;
}

/**
 * @brief 终止渲染设备，释放所有资源。
 *
 * 按以下顺序释放资源：
 *   1. 销毁所有独立渲染设备（Surface、Context、RDD）
 *   2. 销毁所有帧的 GPU Fence
 *   3. 释放所有 RIDOwner 管理的 GPU 资源（通过 FreeAllResources()）
 *   4. 重置默认 Framebuffer 和 RenderPass
 *   5. 销毁 Staging Buffer
 *   6. 重置依赖追踪映射
 *   7. 释放 RDD（通过 RCD::DriverFree()）
 * @pre 必须在渲染线程上调用（与 Initialize() 同一线程）
 * @pre 所有独立设备必须已在各自渲染线程上调用 Finalize()
 * @pre 所有外部引用已释放
 *
 * @post initialized_ 置为 false，调用后不应再使用任何 RD 方法
 *
 * @note 此方法执行 GPU 资源释放（如 glDeleteFramebuffers），
 *       要求 GL 上下文已绑定到当前线程。
 *
 * @see Initialize()
 * @see FreeAllResources()
 */
void RenderingDevice::Finalize() {
  if (!initialized_) {
    return;
  }

  ARHUD_ASSERT(IsRenderThread(),
               "Finalize() must be called on the render thread (Initialize was "
               "called on a different thread)");

  // 销毁所有帧的 GPU Fence
  for (uint32 i = 0; i < kMaxFrameCount; ++i) {
    if (frames_[i].fence != IRenderingDeviceDriver::kInvalidFenceId) {
      device_driver_->FenceDestroy(frames_[i].fence);
      frames_[i].fence = IRenderingDeviceDriver::kInvalidFenceId;
    }
  }

  FreeAllResources();

  default_framebuffer_ = RDFramebufferID();
  default_render_pass_ = RDRenderPassID();

  DestroyStagingBuffer();

  dependency_map_.Reset();
  reverse_dependency_map_.Reset();

  if (device_driver_ != nullptr && context_driver_ != nullptr) {
    context_driver_->DriverFree(device_driver_);
  }
  device_driver_ = nullptr;
  context_driver_ = nullptr;
  initialized_ = false;
}

// ── 帧循环 → rendering_device_frame.cpp ──

// ═══════════════════════════════════════════════════════════════════════
// Buffer 管理
// ═══════════════════════════════════════════════════════════════════════
/**
 * @brief 创建 Buffer 资源
 *
 * 在 GPU 上创建指定大小和用途的缓冲区。
 *
 * @note 必须在渲染线程调用（GL 上下文归渲染线程所有）；跨线程请经
 *       RenderServer 的 RID 分裂路径路由（ADR-0010）。初始化期
 *       command_queue_ 尚未绑定时允许在主线程直接执行。
 *
 * 创建流程：
 *   1. 调用 RDD::BufferCreate() 创建驱动层 Buffer
 *   2. 创建 Buffer 簿记结构，记录元信息
 *   3. 通过 buffer_owner_ 生成 RID 并返回。
 *
 * @param[in] p_size 缓冲区大小（字节，必须 > 0）
 * @param[in] p_usage 用途标志位（如 kVertex | kStorage）
 * @param[in] p_allocation_type 内存分配类型（kGpu / kCpu）。
 *
 * @return Buffer RID，失败返回空 RID
 *
 * @pre IsInitialized() == true
 * @pre p_size > 0
 *
 * @note 创建成功后 vram_usage_ 增加 p_size 字节
 *
 * @see BufferFree()
 * @see IRenderingDeviceDriver::BufferCreate()
 */
RDBufferID RenderingDevice::BufferCreate(
    uint64 p_size, BitField<BufferUsageBits> p_usage,
    MemoryAllocationType p_allocation_type) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");
  ARHUD_ASSERT(p_size > 0, "Buffer size must be > 0");

  // GL 上下文归渲染线程所有；跨线程调用请经 RenderServer（RID
  // 分裂）路由（ADR-0010）。 初始化期 command_queue_
  // 尚未绑定，允许在主线程直接执行。
  ARHUD_ASSERT(IsRenderThread() || command_queue_ == nullptr,
               "RenderingDevice::BufferCreate must run on the render thread");

  BufferID driver_id =
      device_driver_->BufferCreate(p_size, p_usage, p_allocation_type);
  if (!driver_id.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice::BufferCreate() driver failed, size=%llu",
                    p_size);
    return RDBufferID();
  }

  auto* buf = ARHUD_NEW(Buffer);
  buf->driver_id = driver_id;
  buf->size = p_size;
  buf->usage = p_usage;
  buf->allocation_type = p_allocation_type;

  vram_usage_ += p_size;

  RID rid = buffer_owner_.MakeRid(buf);
  return RDBufferID(rid);
}

/**
 * @brief 释放 Buffer 资源（延迟销毁）
 *
 * 将 Buffer RID 加入下一帧的延迟释放列表，而非立即销毁。
 * 延迟销毁确保当前帧 GPU 仍在使用该 Buffer 时不会被提前回收。
 * 避免 use-after-free 问题。
 *
 * 实际驱动层资源释放发生在 FreeResourcesFromFrame() 中，
 * 当帧号匹配后调用 RDD::BufferFree() 完成释放。
 * @param[in] p_buffer Buffer RID（空 RID 会被安全忽略）。
 *
 * @pre IsInitialized() == true
 *
 * @note 延迟帧数由 frame_count_ 决定（默认 2，即双缓冲）
 *
 * @see BufferCreate()
 * @see FreeResourcesFromFrame()
 */
void RenderingDevice::BufferFree(RDBufferID p_buffer) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  if (p_buffer.IsNull()) {
    return;
  }

  // 入队当前帧槽位（对齐 Godot _free_pending_resources）：
  // 资源与覆盖它的 Fence 同槽，BeginFrame(N+frame_count) 等待本帧
  // Fence 后释放，确保 GPU 完成本帧（含可能的使用）后才销毁
  uint32 frame_index = static_cast<uint32>(frame_number_ % frame_count_);
  frames_[frame_index].buffers_to_free.PushBack(p_buffer.GetRid());
}

/**
 * @brief 获取 Buffer 的驱动层 ID
 *
 * 通过 RID 查找 buffer_owner_ 中的簿记结构，返回对应的驱动层 BufferID。
 * 驱动层 ID 用于 UniformSet 绑定、依赖追踪等需要与 RDD 交互的场景。
 *
 * @param[in] p_buffer Buffer RID
 *
 * @return 驱动层 BufferID
 * @retval BufferID() 无效 RID 或未找到
 *
 * @see BufferCreate()
 * @see FindBufferRidByDriverId()
 */
BufferID RenderingDevice::BufferGetDriverId(RDBufferID p_buffer) const {
  if (p_buffer.IsNull()) {
    return BufferID();
  }

  const Buffer* buf = buffer_owner_.GetOrNull(p_buffer.GetRid());
  if (buf == nullptr) {
    return BufferID();
  }

  return buf->driver_id;
}

/**
 * @brief 获取 Buffer 大小
 *
 * 通过 RID 查找 buffer_owner_ 中的簿记结构，返回 Buffer 的字节大小。
 *
 * @param[in] p_buffer Buffer RID
 *
 * @return Buffer 大小（字节）
 * @retval 0 无效 RID 或未找到
 *
 * @see BufferCreate()
 */
uint64 RenderingDevice::BufferGetSize(RDBufferID p_buffer) const {
  if (p_buffer.IsNull()) {
    return 0;
  }

  const Buffer* buf = buffer_owner_.GetOrNull(p_buffer.GetRid());
  return buf != nullptr ? buf->size : 0;
}

/**
 * @brief 映射 Buffer 到 CPU 内存
 *
 * 将 GPU Buffer 映射到 CPU 可访问的内存地址，允许直接读写 Buffer 数据。
 * 内部通过 BufferGetDriverId() 获取驱动层 ID 后调用 RDD::BufferMap()。
 * @param[in] p_buffer Buffer RID
 *
 * @return CPU 内存指针
 * @retval nullptr 无效 RID、未找到或映射失败。
 *
 * @pre p_buffer 为有效的 Buffer RID
 *
 * @note 映射后必须在使用完毕后调用 BufferUnmap() 解除映射
 *
 * @see BufferUnmap()
 * @see IRenderingDeviceDriver::BufferMap()
 */
uint8* RenderingDevice::BufferMap(RDBufferID p_buffer) {
  if (p_buffer.IsNull()) {
    return nullptr;
  }

  BufferID driver_id = BufferGetDriverId(p_buffer);
  if (!driver_id.IsValid()) {
    return nullptr;
  }

  return device_driver_->BufferMap(driver_id);
}

/**
 * @brief 取消映射 Buffer
 *
 * 解除之前通过 BufferMap() 建立的 CPU 内存映射。
 * 内部通过 BufferGetDriverId() 获取驱动层 ID 后调用 RDD::BufferUnmap()。
 * @param[in] p_buffer Buffer RID
 *
 * @pre p_buffer 为有效的 Buffer RID 且已通过 BufferMap() 映射
 *
 * @note 解除映射后，之前返回的 CPU 指针不再有效
 *
 * @see BufferMap()
 * @see IRenderingDeviceDriver::BufferUnmap()
 */
void RenderingDevice::BufferUnmap(RDBufferID p_buffer) {
  if (p_buffer.IsNull()) {
    return;
  }

  BufferID driver_id = BufferGetDriverId(p_buffer);
  if (!driver_id.IsValid()) {
    return;
  }

  device_driver_->BufferUnmap(driver_id);
}

// ═══════════════════════════════════════════════════════════════════════
// Texture 管理
// ═══════════════════════════════════════════════════════════════════════
/**
 * @brief 创建 Texture 资源
 *
 * 在 GPU 上创建指定格式和视图的纹理。
 *
 * @note 必须在渲染线程调用（GL 上下文归渲染线程所有）；跨线程请经
 *       RenderServer 的 RID 分裂路径路由（ADR-0010）。初始化期
 *       command_queue_ 尚未绑定时允许在主线程直接执行。
 *
 * 创建流程：
 *   1. 调用 RDD::TextureCreate() 创建驱动层 Texture
 *   2. 创建 Texture 簿记结构，记录格式和元信息
 *   3. 通过 texture_owner_ 生成 RID 并返回。
 *
 * @param[in] p_format 纹理格式描述（尺寸、格式、Mipmap 层级数等）
 * @param[in] p_view 纹理视图描述（视图类型、格式重解释等）
 *
 * @return Texture RID，失败返回空 RID
 *
 * @pre IsInitialized() == true
 *
 * @note 创建成功后 vram_usage_ 增加估算的纹理 VRAM 大小
 *
 * @see TextureFree()
 * @see IRenderingDeviceDriver::TextureCreate()
 */
RDTextureID RenderingDevice::TextureCreate(const TextureFormat& p_format,
                                           const TextureView& p_view) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  // GL 上下文归渲染线程所有；跨线程调用请经 RenderServer（RID
  // 分裂）路由（ADR-0010）。 初始化期 command_queue_
  // 尚未绑定，允许在主线程直接执行。
  ARHUD_ASSERT(IsRenderThread() || command_queue_ == nullptr,
               "RenderingDevice::TextureCreate must run on the render thread");

  TextureID driver_id = device_driver_->TextureCreate(p_format, p_view);
  if (!driver_id.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice::TextureCreate() driver failed");
    return RDTextureID();
  }

  auto* tex = ARHUD_NEW(Texture);
  tex->driver_id = driver_id;
  tex->format = p_format;

  vram_usage_ += EstimateTextureVram(p_format.width, p_format.height,
                                     p_format.format, p_format.mipmaps);

  RID rid = texture_owner_.MakeRid(tex);
  return RDTextureID(rid);
}

/**
 * @brief 释放 Texture 资源（延迟销毁）
 *
 * 将 Texture RID 加入下一帧的延迟释放列表，而非立即销毁。
 * 延迟销毁确保当前帧 GPU 仍在使用该 Texture 时不会被提前回收。
 * 实际驱动层资源释放发生在 FreeResourcesFromFrame() 中，
 * 当帧号匹配后调用 RDD::TextureFree() 完成释放。
 * @param[in] p_texture Texture RID（空 RID 会被安全忽略）。
 *
 * @pre IsInitialized() == true
 *
 * @note 延迟帧数由 frame_count_ 决定（默认 2，即双缓冲）
 *
 * @see TextureCreate()
 * @see FreeResourcesFromFrame()
 */
void RenderingDevice::TextureFree(RDTextureID p_texture) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  if (p_texture.IsNull()) {
    return;
  }

  // 入队当前帧槽位（对齐 Godot _free_pending_resources）：
  // 资源与覆盖它的 Fence 同槽，BeginFrame(N+frame_count) 等待本帧
  // Fence 后释放，确保 GPU 完成本帧（含可能的使用）后才销毁
  uint32 frame_index = static_cast<uint32>(frame_number_ % frame_count_);
  frames_[frame_index].textures_to_free.PushBack(p_texture.GetRid());
}

/**
 * @brief 以显示侧显存导入 Texture（Mode X 零拷贝上屏）
 *
 * @par 流程
 *   1. RDD::TextureCreateExternal 建立后端别名（不分配显存）
 *   2. Texture 簿记照 TextureCreate 的形态构造——RenderPass 兼容性
 *      与 FBO 附件推断都读 format，故必须填齐 width/height/format/type
 *   3. vram_usage_ 照常累加：显存确实被本后端使用，只是不归它拥有，
 *      与 FreeTextureDriverResource 的无条件扣减保持平衡
 *
 * @param[in] p_source 外部资源描述
 *
 * @return Texture RID；不支持或导入失败返回空 RID
 */
RDTextureID RenderingDevice::TextureCreateExternal(
    const ExternalTextureSource& p_source) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  // 同 TextureCreate 的线程闸门：GL 上下文归渲染线程，初始化期
  // command_queue_ 尚未绑定时允许主线程直调
  ARHUD_ASSERT(IsRenderThread() || command_queue_ == nullptr,
               "RenderingDevice::TextureCreateExternal must run on the render "
               "thread");

  if (!p_source.IsValid()) {
    ARHUD_LOG_ERROR("kInvalidParameter",
                    "RenderingDevice::TextureCreateExternal() invalid source");
    return RDTextureID();
  }

  TextureID driver_id = device_driver_->TextureCreateExternal(p_source);
  if (!driver_id.IsValid()) {
    // 不支持/探测失败是环境事实（回落 Mode R 由调用方决定），
    // 用 Info 级避免在探测期刷错误日志
    ARHUD_LOG_INFO(
        "RenderingDevice::TextureCreateExternal() driver unavailable — "
        "caller should fall back to readback present path");
    return RDTextureID();
  }

  TextureFormat format;
  format.format = p_source.format;
  format.width = p_source.width;
  format.height = p_source.height;
  format.texture_type = TextureType::k2D;
  format.mipmaps = 1;
  format.usage.SetFlag(TextureUsageBits::kColorAttachment);
  // 存储由平台侧拥有且每帧整体覆写：内容无需跨帧保留
  format.is_discardable = true;

  auto* tex = ARHUD_NEW(Texture);
  tex->driver_id = driver_id;
  tex->format = format;

  vram_usage_ += EstimateTextureVram(p_source.width, p_source.height,
                                     p_source.format, 1);

  RID rid = texture_owner_.MakeRid(tex);
  return RDTextureID(rid);
}

/**
 * @brief 立即释放 Texture（跳过帧槽延迟）
 *
 * @param[in] p_texture Texture RID（空 RID 静默忽略）
 *
 * @pre 该资源无在途 GPU 引用（外部导入纹理由
 *      FrameImageQueue::HasInFlight == false 保证）
 */
void RenderingDevice::TextureFreeImmediate(RDTextureID p_texture) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  if (p_texture.IsNull()) {
    return;
  }
  FreeTextureDriverResource(p_texture.GetRid());
}

/**
 * @brief 开始写外部导入纹理（跨 API 锁定）
 *
 * @param[in] p_texture Texture RID
 * @return true 已取得绘制权；false 非外部纹理或锁定失败（本帧须跳过）
 */
bool RenderingDevice::TextureExternalBeginWrite(RDTextureID p_texture) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  if (p_texture.IsNull()) {
    return false;
  }
  const Texture* tex = texture_owner_.GetOrNull(p_texture.GetRid());
  if (tex == nullptr) {
    return false;
  }
  return device_driver_->TextureExternalBeginWrite(tex->driver_id);
}

/**
 * @brief 结束写外部导入纹理（交还显示侧）
 *
 * @param[in] p_texture Texture RID
 */
void RenderingDevice::TextureExternalEndWrite(RDTextureID p_texture) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  if (p_texture.IsNull()) {
    return;
  }
  const Texture* tex = texture_owner_.GetOrNull(p_texture.GetRid());
  if (tex == nullptr) {
    return;
  }
  device_driver_->TextureExternalEndWrite(tex->driver_id);
}

/** @brief 后端是否已确认可导入外部显存 */
bool RenderingDevice::SupportsExternalTexture() const {
  if (device_driver_ == nullptr) {
    return false;
  }
  return device_driver_->SupportsExternalTexture();
}

/**
 * @brief 获取 Texture 的驱动层 ID
 *
 * 通过 RID 查找 texture_owner_ 中的簿记结构，返回对应的驱动层 TextureID。
 * 驱动层 ID 用于 UniformSet 绑定、Framebuffer 附件、依赖追踪等场景。
 *
 * @param[in] p_texture Texture RID
 *
 * @return 驱动层 TextureID
 * @retval TextureID() 无效 RID 或未找到
 *
 * @see TextureCreate()
 * @see FindTextureRidByDriverId()
 */
TextureID RenderingDevice::TextureGetDriverId(RDTextureID p_texture) const {
  if (p_texture.IsNull()) {
    return TextureID();
  }

  const Texture* tex = texture_owner_.GetOrNull(p_texture.GetRid());
  if (tex == nullptr) {
    return TextureID();
  }

  return tex->driver_id;
}

/**
 * @brief 获取 Texture 的格式描述
 *
 * 通过 RID 查找 texture_owner_ 中的簿记结构，返回纹理创建时指定的格式描述。
 * 包含纹理尺寸、数据格式、Mipmap 层级数等信息。
 *
 * @param[in] p_texture Texture RID
 *
 * @return 纹理格式描述的常量引用
 * @pre p_texture 必须为有效的 Texture RID（否则触发断言）。
 *
 * @note 返回的是内部簿记结构的引用，生命周期与 Texture 资源绑定
 *
 * @see TextureCreate()
 */
const TextureFormat& RenderingDevice::TextureGetFormat(
    RDTextureID p_texture) const {
  ARHUD_ASSERT(p_texture.IsValid(), "Invalid RDTextureID");
  const Texture* tex = texture_owner_.GetOrNull(p_texture.GetRid());
  ARHUD_ASSERT(tex != nullptr, "Texture not found in owner");
  return tex->format;
}

// ═══════════════════════════════════════════════════════════════════════
// Sampler 管理
// ═══════════════════════════════════════════════════════════════════════
/**
 * @brief 创建 Sampler 资源
 *
 * 在 GPU 上创建纹理采样器，定义纹理采样时的过滤、寻址和比较行为。
 *
 * @note 必须在渲染线程调用（GL 上下文归渲染线程所有）；跨线程请经
 *       RenderServer 的 RID 分裂路径路由（ADR-0010）。初始化期
 *       command_queue_ 尚未绑定时允许在主线程直接执行。
 *
 * 创建流程：
 *   1. 调用 RDD::SamplerCreate() 创建驱动层 Sampler
 *   2. 创建 Sampler 簿记结构，记录采样器状态
 *   3. 通过 sampler_owner_ 生成 RID 并返回。
 *
 * @param[in] p_state 采样器状态描述（过滤模式、寻址模式、LOD 范围等）
 *
 * @return Sampler RID，失败返回空 RID
 *
 * @pre IsInitialized() == true
 *
 * @see SamplerFree()
 * @see IRenderingDeviceDriver::SamplerCreate()
 */
RDSamplerID RenderingDevice::SamplerCreate(const SamplerState& p_state) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  // GL 上下文归渲染线程所有；跨线程调用请经 RenderServer（RID
  // 分裂）路由（ADR-0010）。 初始化期 command_queue_
  // 尚未绑定，允许在主线程直接执行。
  ARHUD_ASSERT(IsRenderThread() || command_queue_ == nullptr,
               "RenderingDevice::SamplerCreate must run on the render thread");

  SamplerID driver_id = device_driver_->SamplerCreate(p_state);
  if (!driver_id.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice::SamplerCreate() driver failed");
    return RDSamplerID();
  }

  auto* sampler = ARHUD_NEW(Sampler);
  sampler->driver_id = driver_id;
  sampler->state = p_state;

  RID rid = sampler_owner_.MakeRid(sampler);
  return RDSamplerID(rid);
}

/**
 * @brief 释放 Sampler 资源（延迟销毁）
 *
 * 将 Sampler RID 加入下一帧的延迟释放列表，而非立即销毁。
 * 延迟销毁确保当前帧 GPU 仍在使用该 Sampler 时不会被提前回收。
 * 实际驱动层资源释放发生在 FreeResourcesFromFrame() 中，
 * 当帧号匹配后调用 RDD::SamplerFree() 完成释放。
 * @param[in] p_sampler Sampler RID（空 RID 会被安全忽略）。
 *
 * @pre IsInitialized() == true
 *
 * @note 延迟帧数由 frame_count_ 决定（默认 2，即双缓冲）
 *
 * @see SamplerCreate()
 * @see FreeResourcesFromFrame()
 */
void RenderingDevice::SamplerFree(RDSamplerID p_sampler) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  if (p_sampler.IsNull()) {
    return;
  }

  // 入队当前帧槽位（对齐 Godot _free_pending_resources）：
  // 资源与覆盖它的 Fence 同槽，BeginFrame(N+frame_count) 等待本帧
  // Fence 后释放，确保 GPU 完成本帧（含可能的使用）后才销毁
  uint32 frame_index = static_cast<uint32>(frame_number_ % frame_count_);
  frames_[frame_index].samplers_to_free.PushBack(p_sampler.GetRid());
}

/**
 * @brief 获取 Sampler 的驱动层 ID
 *
 * 通过 RID 查找 sampler_owner_ 中的簿记结构，返回对应的驱动层 SamplerID。
 * 驱动层 ID 用于 UniformSet 中 kSamplerWithTexture 类型的绑定。
 *
 * @param[in] p_sampler Sampler RID
 *
 * @return 驱动层 SamplerID
 * @retval SamplerID() 无效 RID 或未找到
 *
 * @see SamplerCreate()
 */
SamplerID RenderingDevice::SamplerGetDriverId(RDSamplerID p_sampler) const {
  if (p_sampler.IsNull()) {
    return SamplerID();
  }

  const Sampler* sampler = sampler_owner_.GetOrNull(p_sampler.GetRid());
  if (sampler == nullptr) {
    return SamplerID();
  }

  return sampler->driver_id;
}

// ═══════════════════════════════════════════════════════════════════════
// Shader 管理
// ═══════════════════════════════════════════════════════════════════════
/**
 * @brief 从源码创建 Shader 资源
 *
 * 编译着色器源码并创建着色器资源。
 *
 * @note 必须在渲染线程调用（GL 上下文归渲染线程所有）；跨线程请经
 *       RenderServer 的 RID 分裂路径路由（ADR-0010）。初始化期
 *       command_queue_ 尚未绑定时允许在主线程直接执行。
 *
 * 创建流程：
 *   1. 调用 RDD::ShaderCreateFromSource() 编译着色器
 *   2. 创建 Shader 簿记结构，记录反射数据
 *   3. 若编译成功，缓存 Uniform 列表、Push Constant 大小、阶段位掩码
 *   4. 通过 shader_owner_ 生成 RID 并返回。
 *
 * @param[in] p_stage_sources 着色器阶段源码数组（每个元素包含阶段和源码）
 * @param[in] p_source_format 源码格式（OpenGL 仅支持 kGLSL）
 * @param[in] p_uniforms Uniform 反射描述数组
 * @param[in] p_push_constant_size Push Constant 块大小（字节）。
 *
 * @return Shader RID（即使编译失败也会返回有效 RID，可通过 ShaderIsValid()
 * 检查）
 *
 * @pre IsInitialized() == true
 *
 * @note 编译失败时返回的 Shader 仍然有效（is_valid == false），
 *       可用于错误诊断，但不可用于 Pipeline 创建
 *
 * @see ShaderFree()
 * @see ShaderIsValid()
 * @see IRenderingDeviceDriver::ShaderCreateFromSource()
 */
RDShaderID RenderingDevice::ShaderCreateFromSource(
    VectorView<ShaderStageSource> p_stage_sources,
    ShaderSourceFormat p_source_format, VectorView<ShaderUniform> p_uniforms,
    uint32 p_push_constant_size) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  // GL 上下文归渲染线程所有；跨线程调用请经 RenderServer（RID
  // 分裂）路由（ADR-0010）。 初始化期 command_queue_
  // 尚未绑定，允许在主线程直接执行。
  ARHUD_ASSERT(
      IsRenderThread() || command_queue_ == nullptr,
      "RenderingDevice::ShaderCreateFromSource must run on the render thread");

  ShaderID driver_id = device_driver_->ShaderCreateFromSource(
      p_stage_sources, p_source_format, p_uniforms, p_push_constant_size);

  auto* shader = ARHUD_NEW(Shader);
  shader->driver_id = driver_id;
  shader->is_valid = driver_id.IsValid();

  if (shader->is_valid) {
    for (uint32 i = 0; i < p_uniforms.Size(); ++i) {
      shader->uniforms.PushBack(p_uniforms[i]);
    }
    shader->push_constant_size = p_push_constant_size;

    bool has_compute = false;
    for (uint32 i = 0; i < p_stage_sources.Size(); ++i) {
      shader->stage_bits.SetFlag(p_stage_sources[i].stage);
      if (p_stage_sources[i].stage == ShaderStage::kCompute) {
        has_compute = true;
      }
    }
    shader->is_compute = has_compute;
  }

  RID rid = shader_owner_.MakeRid(shader);
  return RDShaderID(rid);
}

/**
 * @brief 释放 Shader 资源（延迟销毁）
 *
 * 将 Shader RID 加入下一帧的延迟释放列表，而非立即销毁。
 * 延迟销毁确保当前帧 GPU 仍在使用该 Shader（如已绑定的 Pipeline）
 * 时不会被提前回收。
 * 实际驱动层资源释放发生在 FreeResourcesFromFrame() 中，
 * 当帧号匹配后调用 RDD::ShaderFree() 完成释放。
 * @param[in] p_shader Shader RID（空 RID 会被安全忽略）。
 *
 * @pre IsInitialized() == true
 *
 * @note 延迟帧数由 frame_count_ 决定（默认 2，即双缓冲）
 *
 * @see ShaderCreateFromSource()
 * @see FreeResourcesFromFrame()
 */
void RenderingDevice::ShaderFree(RDShaderID p_shader) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  if (p_shader.IsNull()) {
    return;
  }

  // 入队当前帧槽位（对齐 Godot _free_pending_resources）：
  // 资源与覆盖它的 Fence 同槽，BeginFrame(N+frame_count) 等待本帧
  // Fence 后释放，确保 GPU 完成本帧（含可能的使用）后才销毁
  uint32 frame_index = static_cast<uint32>(frame_number_ % frame_count_);
  frames_[frame_index].shaders_to_free.PushBack(p_shader.GetRid());
}

/**
 * @brief 获取 Shader 的驱动层 ID
 *
 * 通过 RID 查找 shader_owner_ 中的簿记结构，返回对应的驱动层 ShaderID。
 * 驱动层 ID 用于 Pipeline 创建、UniformSet 绑定等场景。
 *
 * @param[in] p_shader Shader RID
 *
 * @return 驱动层 ShaderID
 * @retval ShaderID() 无效 RID 或未找到
 *
 * @see ShaderCreateFromSource()
 */
ShaderID RenderingDevice::ShaderGetDriverId(RDShaderID p_shader) const {
  if (p_shader.IsNull()) {
    return ShaderID();
  }

  const Shader* shader = shader_owner_.GetOrNull(p_shader.GetRid());
  if (shader == nullptr) {
    return ShaderID();
  }

  return shader->driver_id;
}

/**
 * @brief 获取 Shader 的 Uniform 反射数据
 *
 * 返回 Shader 创建时缓存的 Uniform 描述列表，包含每个 Uniform 的名称。
 * 类型、绑定位置和数组大小等反射信息。
 *
 * @param[in] p_shader Shader RID
 *
 * @return Uniform 列表的常量引用
 * @retval 空列表 无效 RID 或未找到（返回静态空列表引用）。
 *
 * @see ShaderCreateFromSource()
 * @see ShaderUniform
 */
const LocalVector<ShaderUniform>& RenderingDevice::ShaderGetUniforms(
    RDShaderID p_shader) const {
  static const LocalVector<ShaderUniform> kEmptyUniforms;

  if (p_shader.IsNull()) {
    return kEmptyUniforms;
  }

  const Shader* shader = shader_owner_.GetOrNull(p_shader.GetRid());
  if (shader == nullptr) {
    return kEmptyUniforms;
  }

  return shader->uniforms;
}

/**
 * @brief 获取 Shader 的 Push Constant 块大小
 * 返回 Shader 创建时指定的 Push Constant 块大小（字节）。
 * Push Constant 用于向着色器传递少量频繁更新的数据（如变换矩阵），
 * 通过命令直接写入渲染管线，无需 Uniform Buffer。
 *
 * @param[in] p_shader Shader RID
 *
 * @return Push Constant 块大小（字节）
 * @retval 0 无效 RID 或未找到
 *
 * @see ShaderCreateFromSource()
 */
uint32 RenderingDevice::ShaderGetPushConstantSize(RDShaderID p_shader) const {
  if (p_shader.IsNull()) {
    return 0;
  }

  const Shader* shader = shader_owner_.GetOrNull(p_shader.GetRid());
  if (shader == nullptr) {
    return 0;
  }

  return shader->push_constant_size;
}

/**
 * @brief 检查 Shader 是否有效（编译成功）
 *
 * 查询 Shader 簿记结构中的 is_valid 标志。
 * ShaderCreateFromSource() 总是返回有效 RID，但编译失败时 is_valid 为 false。
 *
 * @param[in] p_shader Shader RID
 *
 * @retval true 着色器编译成功，可用于 Pipeline 创建
 * @retval false 无效 RID、未找到或编译失败
 * @see ShaderCreateFromSource()
 * @see ShaderIsCompute()
 */
bool RenderingDevice::ShaderIsValid(RDShaderID p_shader) const {
  if (p_shader.IsNull()) {
    return false;
  }

  const Shader* shader = shader_owner_.GetOrNull(p_shader.GetRid());
  if (shader == nullptr) {
    return false;
  }

  return shader->is_valid;
}

/**
 * @brief 检查 Shader 是否为计算着色器
 *
 * 查询 Shader 簿记结构中的 is_compute 标志。
 * 在 ShaderCreateFromSource() 中，若任一阶段源码的 stage 为 kCompute，
 * 则标记为计算着色器。
 *
 * @param[in] p_shader Shader RID
 *
 * @retval true 包含计算着色器阶段（kCompute）
 * @retval false 无效 RID、未找到或非计算着色器
 *
 * @see ShaderCreateFromSource()
 * @see ShaderIsValid()
 * @see ComputePipelineCreate()
 */
bool RenderingDevice::ShaderIsCompute(RDShaderID p_shader) const {
  if (p_shader.IsNull()) {
    return false;
  }

  const Shader* shader = shader_owner_.GetOrNull(p_shader.GetRid());
  if (shader == nullptr) {
    return false;
  }

  return shader->is_compute;
}

// ═══════════════════════════════════════════════════════════════════════
// UniformSet 管理
// ═══════════════════════════════════════════════════════════════════════
/**
 * @brief 创建 UniformSet 资源
 *
 * 将一组已绑定的 Uniform（Buffer、Texture、Sampler 等）打包为一个 UniformSet。
 * 绑定到指定 Shader 的指定 Set 索引上。创建时自动建立与 Shader 及所引用
 * Buffer/Texture 的依赖关系（DependencyAdd），确保级联销毁顺序正确。
 *
 * @note 必须在渲染线程调用以保证 GL 上下文操作线程安全；跨线程请经
 *       RenderServer 的 RID 分裂路径路由（ADR-0010）。初始化期
 *       command_queue_ 尚未绑定时允许在主线程直接执行。
 *
 * @param[in] p_uniforms  绑定 Uniform 数组，每个元素描述一组同类型的 GPU
 * 资源绑定
 * @param[in] p_shader    关联的 Shader RID，UniformSet 的布局必须与该 Shader 的
 * Uniform 声明匹配
 * @param[in] p_set_index Shader 中目标 Set 的索引（对应 GLSL 中的
 * layout(set=N)）。
 * @return 新创建的 UniformSet RID，失败返回空 RID
 * @retval RDUniformSetID() Shader 无效、驱动层创建失败或 Uniform 引用的资源 ID
 * 无法解析
 *
 * @pre initialized_ == true
 * @pre p_shader 为有效的 Shader RID（由 ShaderCreateFromSource 创建且尚未释放）
 * @pre p_set_index 在 Shader 的 Set 范围内。
 *
 * @note 依赖追踪细节：
 *   - UniformSet → Shader（始终建立）
 *   - UniformSet → Buffer（kUniformBuffer / kStorageBuffer / kDynamic 变体）
 *   - UniformSet → Texture（kTexture / kImage / kTextureBuffer / kImageBuffer /
 * kInputAttachment）
 *   - UniformSet → Texture（kSamplerWithTexture 成对 ID 中取 Texture 部分）。
 *
 * @see UniformSetFree()
 * @see DependencyAdd()
 */
RDUniformSetID RenderingDevice::UniformSetCreate(
    VectorView<BoundUniform> p_uniforms, RDShaderID p_shader,
    uint32 p_set_index) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  // GL 上下文归渲染线程所有；跨线程调用请经 RenderServer（RID
  // 分裂）路由（ADR-0010）。 初始化期 command_queue_
  // 尚未绑定，允许在主线程直接执行。
  ARHUD_ASSERT(
      IsRenderThread() || command_queue_ == nullptr,
      "RenderingDevice::UniformSetCreate must run on the render thread");

  ShaderID shader_driver_id = ShaderGetDriverId(p_shader);
  if (!shader_driver_id.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice::UniformSetCreate() invalid shader RID");
    return RDUniformSetID();
  }

  UniformSetID driver_id = device_driver_->UniformSetCreate(
      p_uniforms, shader_driver_id, p_set_index);
  if (!driver_id.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice::UniformSetCreate() driver failed");
    return RDUniformSetID();
  }

  auto* uniform_set = ARHUD_NEW(UniformSet);
  uniform_set->driver_id = driver_id;
  uniform_set->shader_id = shader_driver_id;
  uniform_set->set_index = p_set_index;

  RID rid = uniform_set_owner_.MakeRid(uniform_set);
  // UniformSet depends on its Shader
  DependencyAdd(p_shader.GetRid(), rid);
  // UniformSet depends on each Buffer/Texture it references
  for (uint32 i = 0; i < p_uniforms.Size(); ++i) {
    const BoundUniform& uniform = p_uniforms[i];
    switch (uniform.type) {
      case UniformType::kUniformBuffer:
      case UniformType::kStorageBuffer:
      case UniformType::kUniformBufferDynamic:
      case UniformType::kStorageBufferDynamic: {
        for (uint32 j = 0; j < uniform.ids.Size(); ++j) {
          RID buffer_rid = FindBufferRidByDriverId(uniform.ids[j]);
          if (buffer_rid.IsValid()) {
            DependencyAdd(buffer_rid, rid);
          }
        }
        break;
      }
      case UniformType::kTexture:
      case UniformType::kImage:
      case UniformType::kTextureBuffer:
      case UniformType::kImageBuffer:
      case UniformType::kInputAttachment: {
        for (uint32 j = 0; j < uniform.ids.Size(); ++j) {
          TextureID tex_driver_id(uniform.ids[j].GetId());
          RID texture_rid = FindTextureRidByDriverId(tex_driver_id);
          if (texture_rid.IsValid()) {
            DependencyAdd(texture_rid, rid);
          }
        }
        break;
      }
      case UniformType::kSamplerWithTexture:
      case UniformType::kSamplerWithTextureBuffer: {
        // IDs come in pairs: (SamplerID, TextureID)
        for (uint32 j = 0; j < uniform.ids.Size(); j += 2) {
          // Sampler part: us depends on the Sampler
          SamplerID sampler_driver_id(uniform.ids[j].GetId());
          RID sampler_rid = FindSamplerRidByDriverId(sampler_driver_id);
          if (sampler_rid.IsValid()) {
            DependencyAdd(sampler_rid, rid);
          }

          if (j + 1 < uniform.ids.Size()) {
            TextureID tex_driver_id(uniform.ids[j + 1].GetId());
            RID texture_rid = FindTextureRidByDriverId(tex_driver_id);
            if (texture_rid.IsValid()) {
              DependencyAdd(texture_rid, rid);
            }
          }
        }
        break;
      }
      default:
        break;
    }
  }
  return RDUniformSetID(rid);
}

/**
 * @brief 释放 UniformSet 资源（延迟销毁）
 *
 * 将 UniformSet 的 RID 放入下一帧的待释放列表，
 * 而非立即销毁。这确保当前帧中 GPU 仍可能引用该 UniformSet 时不会被提前回收。
 * 避免 use-after-free。实际驱动层资源在 FreeResourcesFromPreviousFrames()()
 * 中按帧号安全释放。
 *
 * @param[in] p_uniform_set 要释放的 UniformSet RID
 *
 * @pre initialized_ == true
 *
 * @note 传入空 RID 时安全返回（no-op）。
 *
 * @see UniformSetCreate()
 * @see FreeResourcesFromPreviousFrames()
 */
void RenderingDevice::UniformSetFree(RDUniformSetID p_uniform_set) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  if (p_uniform_set.IsNull()) {
    return;
  }

  // 入队当前帧槽位（对齐 Godot _free_pending_resources）：
  // 资源与覆盖它的 Fence 同槽，BeginFrame(N+frame_count) 等待本帧
  // Fence 后释放，确保 GPU 完成本帧（含可能的使用）后才销毁
  uint32 frame_index = static_cast<uint32>(frame_number_ % frame_count_);
  frames_[frame_index].uniform_sets_to_free.PushBack(p_uniform_set.GetRid());
}

/**
 * @brief 获取 UniformSet 的驱动层 ID
 *
 * 通过 RD 层 RID 查找簿记结构，返回底层 RDD 层的 UniformSetID。
 * 用于在驱动层操作（如绑定 UniformSet）时获取原生句柄。
 *
 * @param[in] p_uniform_set UniformSet RID
 *
 * @return 驱动层 UniformSetID
 * @retval UniformSetID() RID 为空或已在 Owner 中找不到对应资源
 *
 * @see UniformSetCreate()
 */
UniformSetID RenderingDevice::UniformSetGetDriverId(
    RDUniformSetID p_uniform_set) const {
  if (p_uniform_set.IsNull()) {
    return UniformSetID();
  }

  const UniformSet* us = uniform_set_owner_.GetOrNull(p_uniform_set.GetRid());
  if (us == nullptr) {
    return UniformSetID();
  }

  return us->driver_id;
}

// ═══════════════════════════════════════════════════════════════════════
// VertexFormat 管理
// ═══════════════════════════════════════════════════════════════════════
/**
 * @brief 创建 VertexFormat 资源
 *
 * 根据顶点属性描述数组创建顶点格式对象，描述顶点缓冲区中各属性的
 * 布局（位置、格式、步长等）。VertexFormat 是 Pipeline 创建的依赖项之一。
 * 定义了顶点着色器输入与顶点缓冲区数据的映射关系。
 * @note 必须在渲染线程调用以保证 GL 上下文操作线程安全；跨线程请经
 *       RenderServer 的 RID 分裂路径路由（ADR-0010）。初始化期
 *       command_queue_ 尚未绑定时允许在主线程直接执行。
 *
 * @param[in] p_vertex_attribs 顶点属性描述数组，每个元素定义一个顶点属性的
 *                              位置（location）、数据格式、偏移和步长
 *
 * @return 新创建的 VertexFormat RID，失败返回空 RID
 * @retval RDVertexFormatID() 驱动层创建失败
 * @pre initialized_ == true
 * @pre p_vertex_attribs 非空且格式合法。
 *
 * @note VertexFormat 不参与依赖追踪（无 DependencyAdd 调用），
 *       但 Pipeline 创建时会建立 Pipeline → VertexFormat 的依赖关系。
 *
 * @see VertexFormatFree()
 * @see RenderPipelineCreate()
 */
RDVertexFormatID RenderingDevice::VertexFormatCreate(
    VectorView<VertexAttribute> p_vertex_attribs) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  // GL 上下文归渲染线程所有；跨线程调用请经 RenderServer（RID
  // 分裂）路由（ADR-0010）。 初始化期 command_queue_
  // 尚未绑定，允许在主线程直接执行。
  ARHUD_ASSERT(
      IsRenderThread() || command_queue_ == nullptr,
      "RenderingDevice::VertexFormatCreate must run on the render thread");

  VertexFormatID driver_id =
      device_driver_->VertexFormatCreate(p_vertex_attribs);
  if (!driver_id.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice::VertexFormatCreate() driver failed");
    return RDVertexFormatID();
  }

  auto* vf = ARHUD_NEW(VertexFormat);
  vf->driver_id = driver_id;

  RID rid = vertex_format_owner_.MakeRid(vf);
  return RDVertexFormatID(rid);
}

/**
 * @brief 释放 VertexFormat 资源（延迟销毁）
 *
 * 将 VertexFormat 的 RID 放入下一帧的待释放列表，
 * 而非立即销毁。这确保当前帧中引用的 VertexFormat 或 Pipeline
 * 仍可使用它进行绘制。实际驱动层资源在帧结束时安全释放。
 *
 * @param[in] p_vertex_format 要释放的 VertexFormat RID
 *
 * @pre initialized_ == true
 *
 * @note 传入空 RID 时安全返回（no-op）。
 *
 * @see VertexFormatCreate()
 */
void RenderingDevice::VertexFormatFree(RDVertexFormatID p_vertex_format) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  if (p_vertex_format.IsNull()) {
    return;
  }

  // 入队当前帧槽位（对齐 Godot _free_pending_resources）：
  // 资源与覆盖它的 Fence 同槽，BeginFrame(N+frame_count) 等待本帧
  // Fence 后释放，确保 GPU 完成本帧（含可能的使用）后才销毁
  uint32 frame_index = static_cast<uint32>(frame_number_ % frame_count_);
  frames_[frame_index].vertex_formats_to_free.PushBack(
      p_vertex_format.GetRid());
}

/**
 * @brief 获取 VertexFormat 的驱动层 ID
 *
 * 通过 RD 层 RID 查找簿记结构，返回底层 RDD 层的 VertexFormatID。
 * 用于 Pipeline 创建时传递原生顶点格式句柄到驱动层。
 *
 * @param[in] p_vertex_format VertexFormat RID
 *
 * @return 驱动层 VertexFormatID
 * @retval VertexFormatID() RID 为空或已在 Owner 中找不到对应资源
 *
 * @see VertexFormatCreate()
 */
VertexFormatID RenderingDevice::VertexFormatGetDriverId(
    RDVertexFormatID p_vertex_format) const {
  if (p_vertex_format.IsNull()) {
    return VertexFormatID();
  }

  const VertexFormat* vf =
      vertex_format_owner_.GetOrNull(p_vertex_format.GetRid());
  if (vf == nullptr) {
    return VertexFormatID();
  }

  return vf->driver_id;
}

// ═══════════════════════════════════════════════════════════════════════
// RenderPass 管理
// ═══════════════════════════════════════════════════════════════════════
/**
 * @brief 创建 RenderPass 资源
 *
 * 描述一次渲染过程的附件布局、子通道和子通道间依赖关系。
 * RenderPass 是 Framebuffer 和 Pipeline 创建的依赖项。
 *   - Framebuffer 绑定 RenderPass 并附加具体的 Texture 附件
 *   - RenderPipeline 通过 RenderPass 确定渲染目标和附件格式。
 *
 * @note 必须在渲染线程调用以保证 GL 上下文操作线程安全；跨线程请经
 *       RenderServer 的 RID 分裂路径路由（ADR-0010）。初始化期
 *       command_queue_ 尚未绑定时允许在主线程直接执行。
 *
 * @param[in] p_attachments       附件描述数组，定义 RenderPass 使用的所有附件
 *                                 （颜色、深度/模板等）的格式、采样数、加载/存储操作
 * @param[in] p_subpasses         子通道描述数组，每个子通道引用部分附件作为
 *                                 颜色输入/输出和深度/模板附件
 * @param[in] p_subpass_dependencies 子通道间依赖关系数组，描述执行顺序。
 *                                      内存屏障需求。
 *
 * @return 新创建的 RenderPass RID，失败返回空 RID
 * @retval RDRenderPassID() 驱动层创建失败
 * @pre initialized_ == true
 * @pre p_attachments 至少包含一个附件
 * @pre p_subpasses 至少包含一个子通道
 *
 * @note Initialize() 会自动创建默认 RenderPass（1 个颜色 + 1 个深度模板附件），
 *       代表屏幕缓冲区的渲染布局。
 *
 * @see RenderPassFree()
 * @see FramebufferCreate()
 */
RDRenderPassID RenderingDevice::RenderPassCreate(
    VectorView<Attachment> p_attachments, VectorView<Subpass> p_subpasses,
    VectorView<SubpassDependency> p_subpass_dependencies) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  // GL 上下文归渲染线程所有；跨线程调用请经 RenderServer（RID
  // 分裂）路由（ADR-0010）。 初始化期 command_queue_
  // 尚未绑定，允许在主线程直接执行。
  ARHUD_ASSERT(
      IsRenderThread() || command_queue_ == nullptr,
      "RenderingDevice::RenderPassCreate must run on the render thread");

  RenderPassID driver_id = device_driver_->RenderPassCreate(
      p_attachments, p_subpasses, p_subpass_dependencies);
  if (!driver_id.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice::RenderPassCreate() driver failed");
    return RDRenderPassID();
  }

  auto* rp = ARHUD_NEW(RenderPass);
  rp->driver_id = driver_id;
  for (uint32 i = 0; i < p_attachments.Size(); ++i) {
    rp->attachments.PushBack(p_attachments[i]);
  }

  RID rid = render_pass_owner_.MakeRid(rp);
  return RDRenderPassID(rid);
}

/**
 * @brief 释放 RenderPass 资源（延迟销毁）
 *
 * 将 RenderPass 的 RID 放入下一帧的待释放列表，
 * 而非立即销毁。这确保当前帧中引用的 RenderPass 或 Framebuffer
 * 和 Pipeline 仍可使用它。实际驱动层资源在帧结束时安全释放。
 *
 * @param[in] p_render_pass 要释放的 RenderPass RID
 *
 * @pre initialized_ == true
 *
 * @note 传入空 RID 时安全返回（no-op）。
 *
 * @see RenderPassCreate()
 */
void RenderingDevice::RenderPassFree(RDRenderPassID p_render_pass) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  if (p_render_pass.IsNull()) {
    return;
  }

  // 入队当前帧槽位（对齐 Godot _free_pending_resources）：
  // 资源与覆盖它的 Fence 同槽，BeginFrame(N+frame_count) 等待本帧
  // Fence 后释放，确保 GPU 完成本帧（含可能的使用）后才销毁
  uint32 frame_index = static_cast<uint32>(frame_number_ % frame_count_);
  frames_[frame_index].render_passes_to_free.PushBack(p_render_pass.GetRid());
}

/**
 * @brief 获取 RenderPass 的驱动层 ID
 *
 * 通过 RD 层 RID 查找簿记结构，返回底层 RDD 层的 RenderPassID。
 * 用于 Framebuffer 创建、Pipeline 创建时传递原生 RenderPass 句柄到驱动层。
 *
 * @param[in] p_render_pass RenderPass RID
 *
 * @return 驱动层 RenderPassID
 * @retval RenderPassID() RID 为空或已在 Owner 中找不到对应资源
 *
 * @see RenderPassCreate()
 */
RenderPassID RenderingDevice::RenderPassGetDriverId(
    RDRenderPassID p_render_pass) const {
  if (p_render_pass.IsNull()) {
    return RenderPassID();
  }

  const RenderPass* rp = render_pass_owner_.GetOrNull(p_render_pass.GetRid());
  if (rp == nullptr) {
    return RenderPassID();
  }

  return rp->driver_id;
}

// ═══════════════════════════════════════════════════════════════════════
// Framebuffer 管理
// ═══════════════════════════════════════════════════════════════════════
/**
 * @brief 创建 Framebuffer 资源
 *
 * 将一个 Texture 附件绑定到指定的 RenderPass，创建帧缓冲对象。
 * Framebuffer 定义了实际渲染目标的 Texture 集合，与 RenderPass 描述配合。
 * 附件布局配合使用。创建时自动建立 Framebuffer 与 RenderPass 的依赖关系，
 * 确保 RenderPass 在 Framebuffer 之前不会被级联销毁。
 *
 * @note 必须在渲染线程调用以保证 GL 上下文操作线程安全；跨线程请经
 *       RenderServer 的 RID 分裂路径路由（ADR-0010）。初始化期
 *       command_queue_ 尚未绑定时允许在主线程直接执行。
 *
 * @param[in] p_render_pass 关联的 RenderPass RID，定义附件布局
 * @param[in] p_attachments 附件 Texture RID 数组，顺序和数量必须与
 *                           RenderPass 的附件描述匹配
 * @param[in] p_width       帧缓冲宽度（像素）
 * @param[in] p_height      帧缓冲高度（像素）。
 *
 * @return 新创建的 Framebuffer RID，失败返回空 RID
 * @retval RDFramebufferID() RenderPass 无效、附件 Texture 无效或驱动层创建失败
 *
 * @pre initialized_ == true
 * @pre p_render_pass 为有效的 RenderPass RID
 * @pre p_attachments 中所有 Texture RID 均为有效且尚未释放。
 *
 * @note Initialize() 会自动创建默认 Framebuffer（FBO 0，代表屏幕缓冲区），
 *       可通过 GetDefaultFramebuffer() 获取。
 * @see FramebufferFree()
 * @see DependencyAdd()
 */
RDFramebufferID RenderingDevice::FramebufferCreate(
    RDRenderPassID p_render_pass, VectorView<RDTextureID> p_attachments,
    uint32 p_width, uint32 p_height) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  // GL 上下文归渲染线程所有；跨线程调用请经 RenderServer（RID
  // 分裂）路由（ADR-0010）。 初始化期 command_queue_
  // 尚未绑定，允许在主线程直接执行。
  ARHUD_ASSERT(
      IsRenderThread() || command_queue_ == nullptr,
      "RenderingDevice::FramebufferCreate must run on the render thread");

  RenderPassID rp_driver_id = RenderPassGetDriverId(p_render_pass);
  if (!rp_driver_id.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "RenderingDevice::FramebufferCreate() invalid render pass RID");
    return RDFramebufferID();
  }

  LocalVector<TextureID> driver_attachments;
  driver_attachments.Reserve(p_attachments.Size());
  for (uint32 i = 0; i < p_attachments.Size(); ++i) {
    TextureID tex_driver_id = TextureGetDriverId(p_attachments[i]);
    if (!tex_driver_id.IsValid()) {
      ARHUD_LOG_ERROR("kFailed",
                      "RenderingDevice::FramebufferCreate() invalid texture "
                      "attachment at index %u",
                      i);
      return RDFramebufferID();
    }
    driver_attachments.PushBack(tex_driver_id);
  }

  FramebufferID driver_id = device_driver_->FramebufferCreate(
      rp_driver_id,
      VectorView<TextureID>(driver_attachments.Data(),
                            driver_attachments.Size()),
      p_width, p_height);
  if (!driver_id.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice::FramebufferCreate() driver failed");
    return RDFramebufferID();
  }

  auto* fb = ARHUD_NEW(Framebuffer);
  fb->driver_id = driver_id;
  fb->render_pass = p_render_pass.GetRid();
  fb->width = p_width;
  fb->height = p_height;

  RID rid = framebuffer_owner_.MakeRid(fb);
  // Framebuffer depends on its RenderPass
  DependencyAdd(p_render_pass.GetRid(), rid);
  return RDFramebufferID(rid);
}

/**
 * @brief 释放 Framebuffer 资源（延迟销毁）
 *
 * 将 Framebuffer 的 RID 放入下一帧的待释放列表，
 * 而非立即销毁。这确保当前帧中正在使用的 Framebuffer 进行
 * DrawList 录制的操作不会被中断。实际驱动层资源在帧结束时安全释放。
 *
 * @param[in] p_framebuffer 要释放的 Framebuffer RID
 *
 * @pre initialized_ == true
 *
 * @note 传入空 RID 时安全返回（no-op）。
 *
 * @see FramebufferCreate()
 */
void RenderingDevice::FramebufferFree(RDFramebufferID p_framebuffer) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  if (p_framebuffer.IsNull()) {
    return;
  }

  // 入队当前帧槽位（对齐 Godot _free_pending_resources）：
  // 资源与覆盖它的 Fence 同槽，BeginFrame(N+frame_count) 等待本帧
  // Fence 后释放，确保 GPU 完成本帧（含可能的使用）后才销毁
  uint32 frame_index = static_cast<uint32>(frame_number_ % frame_count_);
  frames_[frame_index].framebuffers_to_free.PushBack(p_framebuffer.GetRid());
}

/**
 * @brief 获取 Framebuffer 的驱动层 ID
 *
 * 通过 RD 层 RID 查找簿记结构，返回底层 RDD 层的 FramebufferID。
 * 用于 DrawListBegin() 时绑定 FBO 到 GL 上下文。
 * @param[in] p_framebuffer Framebuffer RID
 *
 * @return 驱动层 FramebufferID
 * @retval FramebufferID() RID 为空或已在 Owner 中找不到对应资源
 *
 * @see FramebufferCreate()
 */
FramebufferID RenderingDevice::FramebufferGetDriverId(
    RDFramebufferID p_framebuffer) const {
  if (p_framebuffer.IsNull()) {
    return FramebufferID();
  }

  const Framebuffer* fb = framebuffer_owner_.GetOrNull(p_framebuffer.GetRid());
  if (fb == nullptr) {
    return FramebufferID();
  }

  return fb->driver_id;
}

void RenderingDevice::FramebufferReadback(RDFramebufferID p_framebuffer,
                                          uint32 p_x, uint32 p_y,
                                          uint32 p_width, uint32 p_height,
                                          void* p_dst) {
  FramebufferID driver_id = FramebufferGetDriverId(p_framebuffer);
  device_driver_->FramebufferReadback(driver_id, p_x, p_y, p_width, p_height,
                                      p_dst);
}

// ═══════════════════════════════════════════════════════════════════════
// Pipeline 管理
// ═══════════════════════════════════════════════════════════════════════
/**
 * @brief 计算渲染管线的缓存键
 *
 * 将 RenderPipelineCreate 的全部参数通过 Boost 风格 hash combining
 * 哈希为单一 uint64 键，用于 render_pipeline_cache_ 的去重查找。
 * 相同参数组合产生相同键值，避免重复创建底层 GLPipelineState。
 *
 * 哈希覆盖的字段包括：
 *   - Shader RID、VertexFormat RID、RenderPrimitive、RenderPass RID
 *   - PipelineRasterizationState（全部字段，含 float 位模式）
 *   - PipelineMultisampleState（全部字段）
 *   - PipelineDepthStencilState（含 front/back stencil 操作）
 *   - PipelineColorBlendState（含每个附件的混合参数和写入掩码）
 *   - 动态状态标志位
 *
 * @param[in] p_shader               Shader RID
 * @param[in] p_vertex_format        VertexFormat RID
 * @param[in] p_render_primitive     渲染图元类型（三角形、线段等）
 * @param[in] p_rasterization_state  光栅化状态（深度钳位、线框、剔除等）
 * @param[in] p_multisample_state    多重采样状态（采样数、alpha-to-coverage
 * 等）
 * @param[in] p_depth_stencil_state  深度/模板状态（深度测试、模板操作等）
 * @param[in] p_blend_state          颜色混合状态（混合因子、逻辑操作等）
 * @param[in] p_dynamic_state        动态状态标志位（可在绘制时修改的状态）
 * @param[in] p_render_pass          RenderPass RID
 *
 * @return 缓存键（PipelineCacheKey），hash 字段为组合哈希值。
 *
 * @note float 值通过 memcpy 转为 uint64 的位模式进行哈希。
 *       确保 NaN/-0.0 等特殊情况的一致性。
 *
 * @see RenderPipelineCreate()
 */
auto RenderingDevice::ComputeRenderPipelineCacheKey(
    RDShaderID p_shader, RDVertexFormatID p_vertex_format,
    RenderPrimitive p_render_primitive,
    const PipelineRasterizationState& p_rasterization_state,
    const PipelineMultisampleState& p_multisample_state,
    const PipelineDepthStencilState& p_depth_stencil_state,
    const PipelineColorBlendState& p_blend_state,
    BitField<PipelineDynamicStateFlags> p_dynamic_state,
    RDRenderPassID p_render_pass) const -> PipelineCacheKey {
  uint64 h = 0;

  // Boost 风格 hash combining
  auto hash_combine = [&h](uint64 value) {
    h ^= value + 0x9e3779b9 + (h << 6) + (h >> 2);
  };

  // 辅助：将 float 的位模式转为 uint64
  auto float_to_bits = [](float f) -> uint64 {
    uint32 bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return static_cast<uint64>(bits);
  };

  // Shader + VertexFormat + RenderPrimitive + RenderPass
  hash_combine(p_shader.GetRid().GetId());
  hash_combine(p_vertex_format.GetRid().GetId());
  hash_combine(static_cast<uint64>(p_render_primitive));
  hash_combine(p_render_pass.GetRid().GetId());

  // PipelineRasterizationState
  hash_combine(static_cast<uint64>(p_rasterization_state.enable_depth_clamp));
  hash_combine(static_cast<uint64>(p_rasterization_state.discard_primitives));
  hash_combine(static_cast<uint64>(p_rasterization_state.wireframe));
  hash_combine(static_cast<uint64>(p_rasterization_state.cull_mode));
  hash_combine(static_cast<uint64>(p_rasterization_state.front_face));
  hash_combine(static_cast<uint64>(p_rasterization_state.depth_bias_enabled));
  hash_combine(float_to_bits(p_rasterization_state.depth_bias_constant_factor));
  hash_combine(float_to_bits(p_rasterization_state.depth_bias_clamp));
  hash_combine(float_to_bits(p_rasterization_state.depth_bias_slope_factor));
  hash_combine(float_to_bits(p_rasterization_state.line_width));
  hash_combine(static_cast<uint64>(p_rasterization_state.patch_control_points));

  // PipelineMultisampleState
  hash_combine(static_cast<uint64>(p_multisample_state.sample_count));
  hash_combine(static_cast<uint64>(p_multisample_state.enable_sample_shading));
  hash_combine(float_to_bits(p_multisample_state.min_sample_shading));
  hash_combine(
      static_cast<uint64>(p_multisample_state.enable_alpha_to_coverage));
  hash_combine(static_cast<uint64>(p_multisample_state.enable_alpha_to_one));

  // PipelineDepthStencilState
  hash_combine(static_cast<uint64>(p_depth_stencil_state.enable_depth_test));
  hash_combine(static_cast<uint64>(p_depth_stencil_state.enable_depth_write));
  hash_combine(
      static_cast<uint64>(p_depth_stencil_state.depth_compare_operator));
  hash_combine(static_cast<uint64>(p_depth_stencil_state.enable_depth_range));
  hash_combine(float_to_bits(p_depth_stencil_state.depth_range_min));
  hash_combine(float_to_bits(p_depth_stencil_state.depth_range_max));
  hash_combine(static_cast<uint64>(p_depth_stencil_state.enable_stencil));

  // Stencil front_op
  hash_combine(static_cast<uint64>(p_depth_stencil_state.front_op.fail));
  hash_combine(static_cast<uint64>(p_depth_stencil_state.front_op.pass));
  hash_combine(static_cast<uint64>(p_depth_stencil_state.front_op.depth_fail));
  hash_combine(static_cast<uint64>(p_depth_stencil_state.front_op.compare));
  hash_combine(
      static_cast<uint64>(p_depth_stencil_state.front_op.compare_mask));
  hash_combine(static_cast<uint64>(p_depth_stencil_state.front_op.write_mask));
  hash_combine(static_cast<uint64>(p_depth_stencil_state.front_op.reference));

  // Stencil back_op
  hash_combine(static_cast<uint64>(p_depth_stencil_state.back_op.fail));
  hash_combine(static_cast<uint64>(p_depth_stencil_state.back_op.pass));
  hash_combine(static_cast<uint64>(p_depth_stencil_state.back_op.depth_fail));
  hash_combine(static_cast<uint64>(p_depth_stencil_state.back_op.compare));
  hash_combine(static_cast<uint64>(p_depth_stencil_state.back_op.compare_mask));
  hash_combine(static_cast<uint64>(p_depth_stencil_state.back_op.write_mask));
  hash_combine(static_cast<uint64>(p_depth_stencil_state.back_op.reference));

  // PipelineColorBlendState
  hash_combine(static_cast<uint64>(p_blend_state.enable_logic_op));
  hash_combine(static_cast<uint64>(p_blend_state.logic_op));
  hash_combine(static_cast<uint64>(p_blend_state.attachments.Size()));
  for (uint32 i = 0; i < p_blend_state.attachments.Size(); ++i) {
    const auto& att = p_blend_state.attachments[i];
    hash_combine(static_cast<uint64>(att.enable_blend));
    hash_combine(static_cast<uint64>(att.src_color_blend_factor));
    hash_combine(static_cast<uint64>(att.dst_color_blend_factor));
    hash_combine(static_cast<uint64>(att.color_blend_op));
    hash_combine(static_cast<uint64>(att.src_alpha_blend_factor));
    hash_combine(static_cast<uint64>(att.dst_alpha_blend_factor));
    hash_combine(static_cast<uint64>(att.alpha_blend_op));
    hash_combine(static_cast<uint64>(att.write_r));
    hash_combine(static_cast<uint64>(att.write_g));
    hash_combine(static_cast<uint64>(att.write_b));
    hash_combine(static_cast<uint64>(att.write_a));
  }
  hash_combine(float_to_bits(p_blend_state.blend_constant_r));
  hash_combine(float_to_bits(p_blend_state.blend_constant_g));
  hash_combine(float_to_bits(p_blend_state.blend_constant_b));
  hash_combine(float_to_bits(p_blend_state.blend_constant_a));

  // Dynamic state
  hash_combine(static_cast<uint64>(p_dynamic_state.GetRaw()));

  PipelineCacheKey key;
  key.hash = h;
  return key;
}

/**
 * @brief 创建渲染管线资源
 *
 * 根据 Shader、VertexFormat、渲染状态和 RenderPass 创建完整的渲染管线。
 * 内部先通过 ComputeRenderPipelineCacheKey() 计算缓存键，
 * 在 render_pipeline_cache_ 中查找是否已存在相同参数的 Pipeline。
 * 若存在则直接返回缓存的 RID（去重），否则委托驱动层创建新的管线对象。
 *
 * 创建成功后自动建立 Pipeline → Shader、Pipeline → VertexFormat、
 * Pipeline → RenderPass 三向依赖关系，确保级联销毁顺序正确。
 *
 * @note 必须在渲染线程调用以保证 GL 上下文操作线程安全；跨线程请经
 *       RenderServer 的 RID 分裂路径路由（ADR-0010）。初始化期
 *       command_queue_ 尚未绑定时允许在主线程直接执行。
 *
 * @param[in] p_shader               Shader RID（必须包含顶点/片段着色器阶段。
 * @param[in] p_vertex_format        VertexFormat RID（顶点布局描述）
 * @param[in] p_render_primitive     渲染图元类型（三角形、线段等）
 * @param[in] p_rasterization_state  光栅化状态（剔除、深度偏移、线宽等）
 * @param[in] p_multisample_state    多重采样状态（采样数、alpha-to-coverage
 * 等）
 * @param[in] p_depth_stencil_state  深度/模板状态（测试、写入、模板操作等）
 * @param[in] p_blend_state 颜色混合状态（混合因子、逻辑操作、写入掩码等）
 * @param[in] p_dynamic_state        动态状态标志位（可在 DrawList
 * 中动态修改的状态）
 * @param[in] p_render_pass          RenderPass RID（渲染目标和附件布局）。
 *
 * @return 新创建的 Pipeline RID，或缓存命中时返回已有的 RID
 * @retval RDPipelineID() Shader/VertexFormat/RenderPass 无效或驱动层创建失败
 *
 * @pre initialized_ == true
 * @pre p_shader 为有效的非计算 Shader RID
 * @pre p_vertex_format 为有效的 VertexFormat RID
 * @pre p_render_pass 为有效的 RenderPass RID
 *
 * @note 缓存命中时不会创建新的驱动层对象，直接返回已有 RID。
 *       Pipeline 的 cache_key 字段保存了缓存键，PipelineFree
 * 时用于从缓存中移除。
 *
 * @see RenderPipelineCreate()
 * @see ComputeRenderPipelineCacheKey()
 * @see PipelineFree()
 */
RDPipelineID RenderingDevice::RenderPipelineCreate(
    RDShaderID p_shader, RDVertexFormatID p_vertex_format,
    RenderPrimitive p_render_primitive,
    const PipelineRasterizationState& p_rasterization_state,
    const PipelineMultisampleState& p_multisample_state,
    const PipelineDepthStencilState& p_depth_stencil_state,
    const PipelineColorBlendState& p_blend_state,
    BitField<PipelineDynamicStateFlags> p_dynamic_state,
    RDRenderPassID p_render_pass) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  // GL 上下文归渲染线程所有；跨线程调用请经 RenderServer（RID
  // 分裂）路由（ADR-0010）。 初始化期 command_queue_
  // 尚未绑定，允许在主线程直接执行。
  ARHUD_ASSERT(
      IsRenderThread() || command_queue_ == nullptr,
      "RenderingDevice::RenderPipelineCreate must run on the render thread");

  // 检查 Pipeline 缓存，避免重复创建相同参数的 GLPipelineState
  PipelineCacheKey cache_key = ComputeRenderPipelineCacheKey(
      p_shader, p_vertex_format, p_render_primitive, p_rasterization_state,
      p_multisample_state, p_depth_stencil_state, p_blend_state,
      p_dynamic_state, p_render_pass);

  RID* cached = render_pipeline_cache_.GetPtr(cache_key);
  if (cached != nullptr) {
    return RDPipelineID(*cached);
  }

  ShaderID shader_driver_id = ShaderGetDriverId(p_shader);
  VertexFormatID vf_driver_id = VertexFormatGetDriverId(p_vertex_format);
  RenderPassID rp_driver_id = RenderPassGetDriverId(p_render_pass);

  if (!shader_driver_id.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "RenderingDevice::RenderPipelineCreate() invalid shader RID");
    return RDPipelineID();
  }
  if (!vf_driver_id.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "RenderingDevice::RenderPipelineCreate() invalid vertex format RID");
    return RDPipelineID();
  }
  if (!rp_driver_id.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "RenderingDevice::RenderPipelineCreate() invalid render pass RID");
    return RDPipelineID();
  }

  PipelineID driver_id = device_driver_->RenderPipelineCreate(
      shader_driver_id, vf_driver_id, p_render_primitive, p_rasterization_state,
      p_multisample_state, p_depth_stencil_state, p_blend_state,
      p_dynamic_state, rp_driver_id);
  if (!driver_id.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice::RenderPipelineCreate() driver failed");
    return RDPipelineID();
  }

  auto* pipeline = ARHUD_NEW(Pipeline);
  pipeline->driver_id = driver_id;
  pipeline->shader_rid = p_shader.GetRid();
  pipeline->cache_key = cache_key;

  RID rid = pipeline_owner_.MakeRid(pipeline);
  // Pipeline depends on Shader, VertexFormat, and RenderPass
  DependencyAdd(p_shader.GetRid(), rid);
  DependencyAdd(p_vertex_format.GetRid(), rid);
  DependencyAdd(p_render_pass.GetRid(), rid);
  render_pipeline_cache_.Insert(cache_key, rid);
  return RDPipelineID(rid);
}

/**
 * @brief 创建计算管线资源
 *
 * 根据计算着色器创建计算管线。与渲染管线不同，计算管线不需要
 * VertexFormat、RenderPass 或渲染状态，只需一个计算着色器。
 * 创建前会验证 Shader 是否为计算着色器（ShaderIsCompute），
 * 非计算着色器将被拒绝。
 *
 * 创建成功后自动建立 Pipeline 与 Shader 的依赖关系。
 *
 * @note 必须在渲染线程调用以保证 GL 上下文操作线程安全；跨线程请经
 *       RenderServer 的 RID 分裂路径路由（ADR-0010）。初始化期
 *       command_queue_ 尚未绑定时允许在主线程直接执行。
 *
 * @param[in] p_shader 计算着色器 RID（必须包含 kCompute 阶段。
 *
 * @return 新创建的 Pipeline RID，失败返回空 RID
 * @retval RDPipelineID() Shader 无效、非计算着色器或驱动层创建失败
 *
 * @pre initialized_ == true
 * @pre p_shader 为有效的计算 Shader RID（ShaderIsCompute 返回 true）。
 *
 * @note 计算管线不参与 render_pipeline_cache_ 缓存（cache_key 为零值）。
 *       每次调用都会创建新的驱动层对象。
 * @note 需要 OpenGL 4.3+ 支持（glCreateProgramPipelines + GL_COMPUTE_SHADER）。
 *
 * @see ComputeListBindPipeline()
 * @see PipelineFree()
 */
RDPipelineID RenderingDevice::ComputePipelineCreate(RDShaderID p_shader) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  // GL 上下文归渲染线程所有；跨线程调用请经 RenderServer（RID
  // 分裂）路由（ADR-0010）。 初始化期 command_queue_
  // 尚未绑定，允许在主线程直接执行。
  ARHUD_ASSERT(
      IsRenderThread() || command_queue_ == nullptr,
      "RenderingDevice::ComputePipelineCreate must run on the render thread");

  // 验证着色器为计算着色器
  if (!ShaderIsCompute(p_shader)) {
    ARHUD_LOG_ERROR(
        "kInvalidParameter",
        "RenderingDevice::ComputePipelineCreate() shader is not compute");
    return RDPipelineID();
  }

  ShaderID shader_driver_id = ShaderGetDriverId(p_shader);
  if (!shader_driver_id.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "RenderingDevice::ComputePipelineCreate() invalid shader driver ID");
    return RDPipelineID();
  }

  PipelineID driver_id =
      device_driver_->ComputePipelineCreate(shader_driver_id);
  if (!driver_id.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice::ComputePipelineCreate() driver failed");
    return RDPipelineID();
  }

  auto* pipeline = ARHUD_NEW(Pipeline);
  pipeline->driver_id = driver_id;
  pipeline->shader_rid = p_shader.GetRid();
  pipeline->cache_key = PipelineCacheKey{};
  pipeline->is_compute = true;

  RID rid = pipeline_owner_.MakeRid(pipeline);
  DependencyAdd(p_shader.GetRid(), rid);
  return RDPipelineID(rid);
}

/**
 * @brief 释放 Pipeline 资源（延迟销毁）
 *
 * 将 Pipeline 的 RID 放入下一帧的待释放列表，
 * 而非立即销毁。这确保当前帧中正在使用的 Pipeline 进行
 * 绘制或 DrawList 不会被中断。实际驱动层资源在帧结束时安全释放。
 *
 * @param[in] p_pipeline 要释放的 Pipeline RID
 *
 * @pre initialized_ == true
 *
 * @note 传入空 RID 时安全返回（no-op）。
 *
 * @see RenderPipelineCreate()
 * @see ComputePipelineCreate()
 */
void RenderingDevice::PipelineFree(RDPipelineID p_pipeline) {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  if (p_pipeline.IsNull()) {
    return;
  }

  // 入队当前帧槽位（对齐 Godot _free_pending_resources）：
  // 资源与覆盖它的 Fence 同槽，BeginFrame(N+frame_count) 等待本帧
  // Fence 后释放，确保 GPU 完成本帧（含可能的使用）后才销毁
  uint32 frame_index = static_cast<uint32>(frame_number_ % frame_count_);
  frames_[frame_index].pipelines_to_free.PushBack(p_pipeline.GetRid());
}

/**
 * @brief 获取 Pipeline 的驱动层 ID
 *
 * 通过 RD 层 RID 查找簿记结构，返回底层 RDD 层的 PipelineID。
 * 用于 DrawListBindRenderPipeline() 和 ComputeListBindPipeline()
 * 时传递原生管线的句柄到驱动层。
 *
 * @param[in] p_pipeline Pipeline RID
 *
 * @return 驱动层 PipelineID
 * @retval PipelineID() RID 为空或已在 Owner 中找不到对应资源
 *
 * @see RenderPipelineCreate()
 * @see ComputePipelineCreate()
 */
PipelineID RenderingDevice::PipelineGetDriverId(RDPipelineID p_pipeline) const {
  if (p_pipeline.IsNull()) {
    return PipelineID();
  }

  const Pipeline* pipe = pipeline_owner_.GetOrNull(p_pipeline.GetRid());
  if (pipe == nullptr) {
    return PipelineID();
  }

  return pipe->driver_id;
}

// ── DrawList/ComputeList → rendering_device_draw_list.cpp ──

// ═══════════════════════════════════════════════════════════════════════
// TimestampQuery 管理
// ═══════════════════════════════════════════════════════════════════════
/**
 * @brief 创建 GPU 时间戳查询对象
 *
 * 在驱动层创建一个时间戳查询对象，用于测量 GPU 执行时间。
 * 创建后需通过 TimestampQueryRecord() 在 GPU 命令流中插入时间戳标记，
 * 再通过 TimestampQueryGetResult() 获取结果。
 * @return 查询对象 ID
 * @retval IRenderingDeviceDriver::kInvalidTimestampQueryId 创建失败
 *
 * @pre initialized_ == true
 *
 * @note 典型用法：创建两个查询对象，分别在操作前和操作后调用 Record，
 *       再通过 GetResult 获取两个时间戳并相减得到 GPU 执行耗时。
 * @note 生命周期用户自管：本 API 为驱动层直通（RD 不登记、不延迟销毁），
 *       调用方必须自行 TimestampQueryDestroy()，否则 Finalize 后查询
 *       对象泄漏（GL: glDeleteQueries 不会再被调用）。
 *
 * @see TimestampQueryRecord()
 * @see TimestampQueryGetResult()
 * @see TimestampQueryDestroy()
 */
IRenderingDeviceDriver::TimestampQueryID
RenderingDevice::TimestampQueryCreate() {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");
  return device_driver_->TimestampQueryCreate();
}

/**
 * @brief 销毁 GPU 时间戳查询对象
 *
 * 释放驱动层的时间戳查询对象资源。销毁后该查询 ID 不再有效。
 *
 * @param[in] p_query 要销毁的查询对象 ID
 *
 * @pre initialized_ == true
 *
 * @note 传入 kInvalidTimestampQueryId 时安全返回（no-op）。
 *
 * @see TimestampQueryCreate()
 */
void RenderingDevice::TimestampQueryDestroy(
    IRenderingDeviceDriver::TimestampQueryID p_query) {
  if (p_query == IRenderingDeviceDriver::kInvalidTimestampQueryId)
    return;
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");
  device_driver_->TimestampQueryDestroy(p_query);
}

/**
 * @brief 记录当前 GPU 时间戳
 * 在当前 GPU 命令流位置插入时间戳标记。当 GPU 执行到该位置时，
 * 硬件会记录当前的 GPU 时钟值。之后可通过 TimestampQueryGetResult()
 * 获取该时间戳。
 *
 * @param[in] p_query 查询对象 ID
 *
 * @pre initialized_ == true
 * @pre p_query 为有效的查询对象 ID（由 TimestampQueryCreate 创建）
 * @note 传入 kInvalidTimestampQueryId 时安全返回（no-op）
 * @note 时间戳在 GPU 命令执行完毕后才可用，需先检查
 * TimestampQueryIsAvailable()()
 *
 * @see TimestampQueryGetResult()
 * @see TimestampQueryIsAvailable()
 */
void RenderingDevice::TimestampQueryRecord(
    IRenderingDeviceDriver::TimestampQueryID p_query) {
  if (p_query == IRenderingDeviceDriver::kInvalidTimestampQueryId)
    return;
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");
  device_driver_->TimestampQueryRecord(p_query);
}

/**
 * @brief 获取时间戳查询结果
 * 获取由 TimestampQueryRecord() 记录的 GPU 时间戳值。
 * 此调用为阻塞操作：如果 GPU 尚未执行到时间戳标记位置
 * CPU 将等待直到结果可用。
 *
 * @param[in] p_query 查询对象 ID
 *
 * @return GPU 时间戳（纳秒）
 * @retval 0 查询 ID 无效
 *
 * @pre initialized_ == true
 * @pre p_query 为有效的查询对象 ID 且已通过 TimestampQueryRecord() 记录
 *
 * @note 传入 kInvalidTimestampQueryId 时返回 0
 * @note 阻塞等待可能导致 CPU-GPU 同步停顿，建议在帧结束后批量获取结果。
 *       或先用 TimestampQueryIsAvailable() 非阻塞检查。
 * @see TimestampQueryRecord()
 * @see TimestampQueryIsAvailable()
 */
uint64 RenderingDevice::TimestampQueryGetResult(
    IRenderingDeviceDriver::TimestampQueryID p_query) {
  if (p_query == IRenderingDeviceDriver::kInvalidTimestampQueryId)
    return 0;
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");
  return device_driver_->TimestampQueryGetResult(p_query);
}

/**
 * @brief 检查时间戳查询结果是否可用
 *
 * 非阻塞地检查由 TimestampQueryRecord() 记录的时间戳是否已被
 * GPU 写入。可用于避免 TimestampQueryGetResult() 的阻塞等待。
 * @param[in] p_query 查询对象 ID
 *
 * @return true 结果已可用，可安全调用 TimestampQueryGetResult()()
 * @return false 结果尚未可用或查询 ID 无效
 *
 * @pre initialized_ == true
 *
 * @note 传入 kInvalidTimestampQueryId 时返回 false
 *
 * @see TimestampQueryGetResult()
 */
bool RenderingDevice::TimestampQueryIsAvailable(
    IRenderingDeviceDriver::TimestampQueryID p_query) {
  if (p_query == IRenderingDeviceDriver::kInvalidTimestampQueryId)
    return false;
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");
  return device_driver_->TimestampQueryIsAvailable(p_query);
}

// ═══════════════════════════════════════════════════════════════════════
// 依赖追踪
// ═══════════════════════════════════════════════════════════════════════
/**
 * @brief 声明资源依赖关系
 *
 * 记录 p_dependent 依赖于 p_base 的双向依赖关系。
 * 维护两个映射：
 *   - dependency_map_：p_base → { p_dependent, ... }（正向：被依赖方 →
 * 依赖方集合）
 *   - reverse_dependency_map_：p_dependent → { p_base, ... }（反向：依赖方 →
 * 被依赖方集合）。
 *
 * 当 p_base 被释放时，FreeResourceDependencies() 会遍历 dependency_map_
 * 找到所有依赖方并递归释放，确保销毁顺序正确（先释放依赖方，再释放被依赖方），
 * 防止 use-after-free。
 *
 * @param[in] p_base      被依赖的基础资源 RID（如 Shader、Buffer、Texture）
 * @param[in] p_dependent 依赖方的资源 RID（如
 * UniformSet、Pipeline、Framebuffer）。
 *
 * @pre p_base 与 p_dependent 均为有效的 RID
 *
 * @note 同一对 (p_base, p_dependent) 可安全重复调用（HashSet 自动去重）。
 *
 * @see DependencyRemove()
 * @see FreeResourceDependencies()
 */
void RenderingDevice::DependencyAdd(RID p_base, RID p_dependent) {
  ARHUD_ASSERT(p_base.IsValid(), "Base RID must be valid");
  ARHUD_ASSERT(p_dependent.IsValid(), "Dependent RID must be valid");

  dependency_map_[p_base].Insert(p_dependent);
  reverse_dependency_map_[p_dependent].Insert(p_base);
}

/**
 * @brief 移除资源依赖关系
 *
 * 并从 dependency_map_ 和 reverse_dependency_map_ 中同时移除
 * p_base 与 p_dependent 的依赖记录。两侧映射保持同步。
 * @param[in] p_base      被依赖的基础资源 RID
 * @param[in] p_dependent 依赖方的资源 RID
 *
 * @note 若指定的依赖关系不存在，操作安全返回（no-op）。
 *
 * @see DependencyAdd()
 */
void RenderingDevice::DependencyRemove(RID p_base, RID p_dependent) {
  HashSet<RID>* deps = dependency_map_.GetPtr(p_base);
  if (deps != nullptr) {
    deps->Erase(p_dependent);
  }

  HashSet<RID>* rev_deps = reverse_dependency_map_.GetPtr(p_dependent);
  if (rev_deps != nullptr) {
    rev_deps->Erase(p_base);
  }
}

/**
 * @brief 根据驱动层 BufferID 查找 RD 层 RID
 *
 * 线性遍历 buffer_owner_ 中所有 Buffer 簿记，查找 driver_id 匹配的条目，
 * 返回对应的 RD 层 RID。主要用于 UniformSetCreate() 中的依赖追踪。
 * 当 BoundUniform 引用 Buffer 的驱动层 ID 时，需要反查 RD 层 RID
 * 以建立 UniformSet 与 Buffer 的依赖关系。
 *
 * @param[in] p_driver_id 驱动层 Buffer ID
 *
 * @return 匹配的 Buffer RID
 * @retval RID() 未找到匹配项（空 RID）。
 *
 * @note 时间复杂度 O(n)，n 为当前存活的 Buffer 数量。
 *       仅在 UniformSetCreate 时调用，频率较低，可接受。
 *
 * @see UniformSetCreate()
 * @see FindTextureRidByDriverId()
 */
RID RenderingDevice::FindBufferRidByDriverId(BufferID p_driver_id) {
  RID result;
  buffer_owner_.ForEach([&](RID p_rid, Buffer* p_buffer) {
    if (p_buffer->driver_id == p_driver_id) {
      result = p_rid;
    }
  });
  return result;
}

/**
 * @brief 根据驱动层 TextureID 查找 RD 层 RID
 *
 * 线性遍历 texture_owner_ 中所有 Texture 簿记，查找 driver_id 匹配的条目，
 * 返回对应的 RD 层 RID。主要用于 UniformSetCreate() 中的依赖追踪。
 * 当 BoundUniform 引用 Texture 的驱动层 ID 时，需要反查 RD 层 RID
 * 以建立 UniformSet 与 Texture 的依赖关系。
 *
 * @param[in] p_driver_id 驱动层 Texture ID
 *
 * @return 匹配的 Texture RID
 * @retval RID() 未找到匹配项（空 RID）。
 *
 * @note 时间复杂度 O(n)，n 为当前存活的 Texture 数量。
 *       仅在 UniformSetCreate 时调用，频率较低，可接受。
 *
 * @see UniformSetCreate()
 * @see FindBufferRidByDriverId()
 */
RID RenderingDevice::FindTextureRidByDriverId(TextureID p_driver_id) {
  RID result;
  texture_owner_.ForEach([&](RID p_rid, Texture* p_texture) {
    if (p_texture->driver_id == p_driver_id) {
      result = p_rid;
    }
  });
  return result;
}

/**
 * @brief 根据驱动层 SamplerID 查找 RD 层 RID
 *
 * 线性遍历 sampler_owner_ 中所有 Sampler 簿记，查找 driver_id 匹配的条目，
 * 返回对应的 RD 层 RID。用于 UniformSetCreate() 中 kSamplerWithTexture
 * 成对 ID 的 Sampler 部分依赖登记。
 *
 * @param[in] p_driver_id 驱动层 Sampler ID
 *
 * @return 匹配的 Sampler RID
 * @retval RID() 未找到匹配项（空 RID）。
 *
 * @note 时间复杂度 O(n)，n 为当前存活的 Sampler 数量。
 *       仅在 UniformSetCreate 时调用，频率较低，可接受。
 *
 * @see UniformSetCreate()
 * @see FindBufferRidByDriverId()
 */
RID RenderingDevice::FindSamplerRidByDriverId(SamplerID p_driver_id) {
  RID result;
  sampler_owner_.ForEach([&](RID p_rid, Sampler* p_sampler) {
    if (p_sampler->driver_id == p_driver_id) {
      result = p_rid;
    }
  });
  return result;
}

// ── 延迟销毁 → rendering_device_frame.cpp ──
// ── Staging Buffer → rendering_device_staging.cpp ──

// ═══════════════════════════════════════════════════════════════════════
// VRAM 预算
// ═══════════════════════════════════════════════════════════════════════
/**
 * @brief 设置 VRAM 预算上限
 *
 * 设置 GPU 显存（VRAM）使用量的预算上限。Storage 层在创建资源时
 * 可检查当前使用量是否接近预算，从而做出资源分配决策。
 * 此机制用于 QNX 等嵌入式平台的安全内存管控。
 *
 * @param[in] p_budget_bytes 预算上限（字节），0 表示无限。
 *
 * @note 预算值为软限制，不会阻止超出预算的资源创建，仅提供查询依据。
 * @see GetVramUsage()
 * @see GetVramBudget()
 */
void RenderingDevice::SetVramBudget(uint64 p_budget_bytes) {
  vram_budget_ = p_budget_bytes;
}

/**
 * @brief 获取当前 VRAM 使用量
 * 返回当前所有已创建 Buffer 和 Texture 占用的 VRAM 总量估算值。
 * 在 BufferCreate() 和 TextureCreate() 中累加，
 * 在对应资源释放时递减。
 *
 * @return 当前 VRAM 使用量（字节）
 * @see SetVramBudget()
 * @see GetVramBudget()
 */
uint64 RenderingDevice::GetVramUsage() const {
  return vram_usage_;
}

/**
 * @brief 获取 VRAM 预算上限
 *
 * 返回 SetVramBudget() 设置的 VRAM 预算上限。
 * @return 预算上限（字节），0 表示无限。
 *
 * @see SetVramBudget()
 * @see GetVramUsage()
 */
uint64 RenderingDevice::GetVramBudget() const {
  return vram_budget_;
}

}  // namespace arhud
