/**
 * @file rendering_device_frame.cpp
 * @brief RenderingDevice 帧循环 + 延迟销毁实现
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
// 帧循环
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 开始新的一帧
 *
 * 递增帧号，等待并回收 frame_count_ 帧前槽位的延迟销毁资源
 * （对齐 Godot _begin_frame：stall_for_frame → free_pending_resources，
 * 写入本帧槽位前先等待该槽位上一轮的 GPU Fence，确认 GPU 已完成
 * 那一轮的使用），重置 Staging Buffer 偏移。
 *
 * @pre IsInitialized() == true
 * @pre 必须在渲染线程上调用
 *
 * @note 帧槽位回收后，completed_frame = frame_number_ - frame_count_
 *       有 Fence 等待的事实背书（Staging Block 回收判定依赖此边界）。
 * @note Staging Buffer 采用帧引用追踪机制，只有所有引用帧都已完成的
 *       Block 才可回收。如果没有可回收的 Block，回退到第一个 Block，
 *       StagingBufferAllocate 会创建临时 Buffer 处理溢出。
 */
void RenderingDevice::BeginFrame() {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  frame_number_++;

  // ── 帧槽位回收（Godot _begin_frame 同款序）──
  // 写入本帧槽位前：等待该槽位上一轮（frame_number_ - frame_count_ 帧）
  // 插入的 GPU Fence（EndFrame 时插入），确认 GPU 已完成那一帧，
  // 然后释放其延迟销毁资源。此调用同时销毁旧 Fence 并清空槽位。
  FreeResourcesFromPreviousFrames();

  uint64 completed_frame =
      (frame_number_ >= frame_count_) ? (frame_number_ - frame_count_) : 0;

  // 修剪小 Block 已完成帧引用，防止 frame_references 无界增长
  // （此前 RemoveCompletedReferences 从未被调用，8B/帧累积泄漏）
  small_staging_block_.RemoveCompletedReferences(completed_frame);

  // 小 Block 与主 Block 同一保守回收协议：引用帧全部完成后才重置
  // fill_amount。不可回收时保留（新分配继续尾部追加，不覆盖在途
  // 数据；空间用尽时小档分配自动落到主 Block / 临时块兜底）。
  // GL 3.3 瞬时映射下靠 Unmap 同步兜底，persistent mapping
  // （GL 4.4 / Vulkan）下此门控是正确性必需。
  if (small_staging_block_.IsReclaimable(completed_frame)) {
    small_staging_block_.fill_amount = 0;
  }

  // ── Staging Buffer 跨帧管理（帧引用追踪）──
  // 每个 Staging Block 记录引用它的帧号列表（frame_references）。
  // 在 BeginFrame 中，只有所有引用帧都已完成（帧号 <= frame_number_ -
  // frame_count_） 的 Block 才可回收（确保 GPU 已完成对该 Block 数据的读取）。
  // 如果所有 Block 都不可回收（GPU 仍在使用），回退到第一个 Block，
  // StagingBufferAllocate 会创建临时 Buffer 处理溢出。

  // ── 临时块延迟释放（is_temporary）──
  // 档 3 动态创建的块只在存活期内复用，引用帧全部完成后即释放，
  // 防止 staging 总量只增不减（高水位滞留直至 kMaxStagingTotalSize）。
  // 倒序遍历 + RemoveAtUnordered（O(1) 删除，破坏序但此处无序要求）。
  for (int32 i = static_cast<int32>(staging_buffer_blocks_.Size()) - 1; i >= 0;
       --i) {
    StagingBufferBlock& block = staging_buffer_blocks_[i];
    if (block.is_temporary && (block.frame_references.IsEmpty() ||
                               block.IsReclaimable(completed_frame))) {
      device_driver_->BufferFree(block.driver_id);
      staging_buffer_total_size_ -= block.size;
      staging_buffer_blocks_.RemoveAtUnordered(static_cast<uint32>(i));
    }
  }

  staging_buffer_block_index_ = UINT32_MAX;

  for (uint32 i = 0; i < staging_buffer_blocks_.Size(); ++i) {
    StagingBufferBlock& block = staging_buffer_blocks_[i];
    if (block.frame_references.IsEmpty() ||
        block.IsReclaimable(completed_frame)) {
      block.fill_amount = 0;
      block.frame_references.Clear();
      if (staging_buffer_block_index_ == UINT32_MAX) {
        staging_buffer_block_index_ = i;
      }
    }
  }

  // 如果没有可回收的块（所有块都在使用），选择第一个块
  // StagingBufferAllocate 会处理溢出情况
  if (staging_buffer_block_index_ == UINT32_MAX) {
    staging_buffer_block_index_ = 0;
  }
}

/**
 * @brief 结束当前帧（渲染线程，B3 起同线程录+立即执行）
 *
 * 关闭当前活跃的 DrawList 和 ComputeList，插入本帧 GPU Fence。
 * 延迟销毁回收不在本方法执行——移至下一 BeginFrame（等待该 Fence
 * 时释放，消除旧实现"帧末等待自己刚插入的 Fence"的全量同步）。
 *
 * @pre IsInitialized() == true
 * @pre 必须在渲染线程上调用（Engine::RenderThreadFunc 帧循环内）
 *
 * @note 如果存在未关闭的 RenderPass，会在结束前自动关闭。
 * @note Staging Block 的帧引用在分配时记录（StagingBufferAllocate）。
 */
void RenderingDevice::EndFrame() {
  ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

  if (draw_list_.active) {
    if (draw_list_.render_pass_active) {
      device_driver_->CommandEndRenderPass();
    }
    draw_list_.active = false;
    draw_list_.render_pass_active = false;
  }

  if (compute_list_.active) {
    compute_list_.active = false;
  }

  // 插入 GPU Fence（命令直录，录即执行）
  uint32 frame_index = static_cast<uint32>(frame_number_ % frame_count_);
  Frame& frame = frames_[frame_index];
  if (frame.fence != IRenderingDeviceDriver::kInvalidFenceId) {
    device_driver_->FenceDestroy(frame.fence);
  }
  frame.fence = device_driver_->FenceCreate(false);

  // 帧引用追踪：延迟销毁靠 frames_[] 帧槽列表 + frame_references，
  // 不需要 per-resource 的 frame_used 标记。
  // small_staging_block_ 的帧引用在 EndFrame 时记录
  if (small_staging_block_.fill_amount > 0) {
    bool already_referenced = false;
    for (uint32 i = 0; i < small_staging_block_.frame_references.Size(); ++i) {
      if (small_staging_block_.frame_references[i] == frame_number_) {
        already_referenced = true;
        break;
      }
    }
    if (!already_referenced) {
      small_staging_block_.frame_references.PushBack(frame_number_);
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════
// 延迟销毁
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 根据 RID 自动识别资源类型并释放驱动层资源
 *
 * 依次检查各资源 Owner 是否持有该 RID，匹配后调用对应的 Free 方法
 * 释放驱动层资源。支持的资源类型包括：Buffer、Texture、Sampler、Shader、
 * UniformSet、VertexFormat、RenderPass、Framebuffer、Pipeline。
 *
 * @param[in] p_rid 要释放的资源标识符
 *
 * @note 如果 RID 不属于任何已知的资源 Owner，则不做任何操作。
 */
void RenderingDevice::FreeDriverResourceByRid(RID p_rid) {
  if (buffer_owner_.Owns(p_rid)) {
    FreeBufferDriverResource(p_rid);
    return;
  }
  if (texture_owner_.Owns(p_rid)) {
    FreeTextureDriverResource(p_rid);
    return;
  }
  if (sampler_owner_.Owns(p_rid)) {
    FreeSamplerDriverResource(p_rid);
    return;
  }
  if (shader_owner_.Owns(p_rid)) {
    FreeShaderDriverResource(p_rid);
    return;
  }
  if (uniform_set_owner_.Owns(p_rid)) {
    FreeUniformSetDriverResource(p_rid);
    return;
  }
  if (vertex_format_owner_.Owns(p_rid)) {
    FreeVertexFormatDriverResource(p_rid);
    return;
  }
  if (render_pass_owner_.Owns(p_rid)) {
    FreeRenderPassDriverResource(p_rid);
    return;
  }
  if (framebuffer_owner_.Owns(p_rid)) {
    FreeFramebufferDriverResource(p_rid);
    return;
  }
  if (pipeline_owner_.Owns(p_rid)) {
    FreePipelineDriverResource(p_rid);
    return;
  }
}

/**
 * @brief 释放资源的所有引用者（级联）并清理自身依赖边
 *
 * 对齐 Godot _free_dependencies 的完整两段语义：
 *   1. **级联释放**：使用显式工作列表（work list）迭代遍历依赖图，
 *      释放引用 p_base_rid 的所有资源（及其引用者，逐层下探）。
 *   2. **反向边清理**：移除 p_base_rid 自身作为"依赖方"登记的边
 *      （reverse_dependency_map_[p_base_rid] 中的各 base），防止
 *      依赖图残留陈旧边——否则正常运行期每次正确释放 UniformSet/
 *      Pipeline/Framebuffer 后其边永久滞留两张 map（无界增长，
 *      RID 世代校验挡得住误释放但挡不住内存累积）。
 *
 * @param[in] p_base_rid 基础资源的标识符，其所有引用者将被级联释放
 *
 * @note 依赖关系通过 dependency_map_ 维护，使用 HashSet
 * 存储每个资源的引用者集合。
 * @note 在遍历过程中会修改 dependency_map_，因此先拷贝依赖集合再遍历。
 * @note 释放顺序保证：引用者先死、本体后死——各 FreeXxxDriverResource
 *       第一步递归回调本方法，dep_rid 的引用者全部释放后才销毁
 *       dep_rid 自身的驱动对象（外层循环"先 Free 后 PushBack"的行序
 *       不代表实际死亡顺序）。work_list 与递归构成双保险。
 * @note 所有 FreeXxxDriverResource 开头统一调用本方法——对齐 Godot free_rid
 *       统一入口 "Recursively erase dependencies first" 的模式。
 *
 * @par 递归边界（拓扑约束，防后人踩坑）
 *   本方法与 FreeDriverResourceByRid/FreeXxxDriverResource 存在结构上的
 *   递归环路（级联释放 dependent 时，其 Free 又回调本方法），但当前
 *   依赖图为两层拓扑——引用者恒为 UniformSet/Pipeline/Framebuffer，
 *   而这三类无人引用（dependency_map_ 查不到它们的键）→ 级联回调
 *   的级联半场立即落空，仅执行反向边清理（幂等，边已被外层拆掉）。
 *   递归实际深度封顶 2 层嵌套，无失控风险。
 *   **新增依赖边（如给 UniformSet/Pipeline 加"被引用"追踪）会加深
 *   递归**；届时需重构为纯迭代（FreeXxx 不再回调，级联统一由
 *   work_list 驱动）。幂等性由拆边（DependencyRemove 先行）与
 *   RID 世代校验（已释放 RID 的 GetOrNull 返回 nullptr）双重兜底。
 */
void RenderingDevice::FreeResourceDependencies(RID p_base_rid) {
  // ── 第 1 段：级联释放引用者（迭代实现防深层栈溢出）──
  LocalVector<RID> work_list;
  work_list.PushBack(p_base_rid);

  while (!work_list.IsEmpty()) {
    RID current_rid = work_list.Back();
    work_list.PopBack();

    HashSet<RID>* deps = dependency_map_.GetPtr(current_rid);
    if (deps == nullptr) {
      continue;
    }

    // Collect dependents to free (copy because we modify the map during
    // iteration)
    LocalVector<RID> dependents;
    for (auto it = deps->Begin(); it != deps->End(); ++it) {
      dependents.PushBack(*it);
    }

    for (uint32 i = 0; i < dependents.Size(); ++i) {
      RID dep_rid = dependents[i];

      // Remove the dependency
      DependencyRemove(current_rid, dep_rid);

      // Determine resource type and free it.
      // 顺序保证：FreeXxxDriverResource 的第一步是递归回调
      // FreeResourceDependencies(dep_rid)——dep_rid 的引用者
      // （及更深层）先于 dep_rid 自身驱动对象的销毁被释放，
      // 即"引用者先死、本体后死"，无悬垂引用窗口。
      FreeDriverResourceByRid(dep_rid);

      // Process dependents of this dependent iteratively.
      // 双保险：上面的递归已处理级联并 Erase 了
      // dependency_map_[dep_rid]，此处 push 回去 pop 查表恒空；
      // 若将来某类 Free 不再递归，work_list 兜住下一层级联。
      work_list.PushBack(dep_rid);
    }
  }

  // ── 第 2 段：反向边清理（Godot _free_dependencies 下半场）──
  // p_base_rid 即将消失，把它作为依赖方登记的边全部拆除：
  // 各 base 的 dependency_map_ 集合中移除 p_base_rid，
  // 然后删除 reverse_dependency_map_[p_base_rid] 条目。
  HashSet<RID>* rev_deps = reverse_dependency_map_.GetPtr(p_base_rid);
  if (rev_deps != nullptr) {
    LocalVector<RID> bases;
    for (auto it = rev_deps->Begin(); it != rev_deps->End(); ++it) {
      bases.PushBack(*it);
    }
    for (uint32 i = 0; i < bases.Size(); ++i) {
      DependencyRemove(bases[i], p_base_rid);
    }
    reverse_dependency_map_.Erase(p_base_rid);
  }

  // 级联半场已把 dependency_map_[p_base_rid] 拆空（或本无引用者），
  // 移除可能残留的空集合条目
  dependency_map_.Erase(p_base_rid);
}

// ═══════════════════════════════════════════════════════════════════════
// 内部方法 — 延迟销毁
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 回收旧帧的延迟销毁资源
 *
 * 等待当前帧槽位（frame_number_ % frame_count_）上一轮使用时
 * （frame_number_ - frame_count_ 帧）EndFrame 插入的 GPU Fence，
 * 确认 GPU 已完成那一帧的执行，然后释放该槽位中所有待销毁的资源。
 * 资源在入队时（XxxFree）与覆盖它的 Fence 同槽，等待即覆盖。
 *
 * @pre 由 BeginFrame() 在写入本帧槽位之前调用
 *      （对齐 Godot _begin_frame 的 stall_for_frame + free_pending_resources）
 *
 * @note 通过 Fence 同步（1 秒超时）确保 GPU 不会在资源仍被使用时释放；
 *       超时（GPU 挂死/严重落后）时告警后仍释放，避免卡死渲染线程。
 * @note 调用后该槽位 Fence 置空；本帧新 Fence 由 EndFrame 稍后插入。
 * @note kReadback 模式下 EndFrame 后的 glReadPixels 已隐式同步，
 *       FenceWait 通常立即返回（GL_ALREADY_SIGNALED）——等待成本 ≈ 0；
 *       kSwap 模式下该等待构成帧间背压（Godot _stall_for_frame 同款）。
 */
void RenderingDevice::FreeResourcesFromPreviousFrames() {
  // 槽位即当前帧槽位：(N - fc) % fc ≡ N % fc（恒等），
  // 该槽上一轮使用者正是 frame_number_ - frame_count_ 帧，
  // 其 EndFrame 插入的 Fence 现存于此
  uint32 frame_index = static_cast<uint32>(frame_number_ % frame_count_);
  Frame& frame = frames_[frame_index];

  // 等待 GPU Fence 确保 GPU 已完成该帧的执行
  if (frame.fence != IRenderingDeviceDriver::kInvalidFenceId) {
    bool signaled =
        device_driver_->FenceWait(frame.fence, 1'000'000'000ULL);  // 1 秒超时
    if (!signaled) {
      // 超时意味着 GPU 挂死或落后超过 1 秒；继续释放存在
      // use-after-free 风险，但卡死渲染线程更糟——告警暴露
      ARHUD_LOG_WARN(
          "FenceWait timeout: GPU still busy on frame %llu, freeing slot %u "
          "resources anyway",
          static_cast<unsigned long long>(frame_number_ - frame_count_),
          frame_index);
    }
    device_driver_->FenceDestroy(frame.fence);
    frame.fence = IRenderingDeviceDriver::kInvalidFenceId;
  }

  FreeResourcesFromFrame(frame);
  frame.Clear();
}

/**
 * @brief 释放指定帧槽位中的所有待释放资源
 *
 * 按依赖顺序（Pipeline → Framebuffer → RenderPass → UniformSet → Shader →
 * VertexFormat → Sampler → Texture → Buffer）释放帧中所有延迟销毁的资源。
 * 每种资源类型通过对应的 FreeDriverResource 方法释放驱动层资源。
 *
 * @param[in] p_frame 要释放资源的帧槽位引用
 *
 * @note 释放顺序很重要：高级资源（如 Pipeline）必须在低级资源（如
 * Buffer）之前释放， 因为高级资源可能依赖低级资源的驱动句柄。
 */
void RenderingDevice::FreeResourcesFromFrame(Frame& p_frame) {
  for (uint32 i = 0; i < p_frame.pipelines_to_free.Size(); ++i) {
    FreePipelineDriverResource(p_frame.pipelines_to_free[i]);
  }
  for (uint32 i = 0; i < p_frame.framebuffers_to_free.Size(); ++i) {
    FreeFramebufferDriverResource(p_frame.framebuffers_to_free[i]);
  }
  for (uint32 i = 0; i < p_frame.render_passes_to_free.Size(); ++i) {
    FreeRenderPassDriverResource(p_frame.render_passes_to_free[i]);
  }
  for (uint32 i = 0; i < p_frame.uniform_sets_to_free.Size(); ++i) {
    FreeUniformSetDriverResource(p_frame.uniform_sets_to_free[i]);
  }
  for (uint32 i = 0; i < p_frame.shaders_to_free.Size(); ++i) {
    FreeShaderDriverResource(p_frame.shaders_to_free[i]);
  }
  for (uint32 i = 0; i < p_frame.vertex_formats_to_free.Size(); ++i) {
    FreeVertexFormatDriverResource(p_frame.vertex_formats_to_free[i]);
  }
  for (uint32 i = 0; i < p_frame.samplers_to_free.Size(); ++i) {
    FreeSamplerDriverResource(p_frame.samplers_to_free[i]);
  }
  for (uint32 i = 0; i < p_frame.textures_to_free.Size(); ++i) {
    FreeTextureDriverResource(p_frame.textures_to_free[i]);
  }
  for (uint32 i = 0; i < p_frame.buffers_to_free.Size(); ++i) {
    FreeBufferDriverResource(p_frame.buffers_to_free[i]);
  }
}

/**
 * @brief 释放单个 Buffer 的驱动层资源
 *
 * 先释放该 Buffer 的所有依赖项，然后从 VRAM 使用量中扣除 Buffer 大小，
 * 调用驱动层 BufferFree 释放 GPU 资源，最后释放 RID 槽位。
 *
 * @param[in] p_rid Buffer 的资源标识符
 */
void RenderingDevice::FreeBufferDriverResource(RID p_rid) {
  // First free dependents
  FreeResourceDependencies(p_rid);

  Buffer* buf = buffer_owner_.GetOrNull(p_rid);
  if (buf == nullptr) {
    return;
  }

  vram_usage_ -= buf->size;

  device_driver_->BufferFree(buf->driver_id);
  ARHUD_DELETE(buf);
  buffer_owner_.Free(p_rid);
}

/**
 * @brief 释放单个 Texture 的驱动层资源
 *
 * 先释放该 Texture 的所有依赖项，然后根据纹理格式估算 VRAM 使用量并扣除，
 * 调用驱动层 TextureFree 释放 GPU 资源，最后释放 RID 槽位。
 *
 * @param[in] p_rid Texture 的资源标识符
 */
void RenderingDevice::FreeTextureDriverResource(RID p_rid) {
  // First free dependents
  FreeResourceDependencies(p_rid);

  Texture* tex = texture_owner_.GetOrNull(p_rid);
  if (tex == nullptr) {
    return;
  }

  vram_usage_ -= EstimateTextureVram(tex->format.width, tex->format.height,
                                     tex->format.format, tex->format.mipmaps);

  device_driver_->TextureFree(tex->driver_id);
  ARHUD_DELETE(tex);
  texture_owner_.Free(p_rid);
}

/**
 * @brief 释放单个 Sampler 的驱动层资源
 *
 * 先级联释放引用该 Sampler 的资源（UniformSet，经
 * kSamplerWithTexture 对登记的依赖边），然后调用驱动层
 * SamplerFree 释放 GPU 采样器资源，销毁 Sampler 对象并释放 RID 槽位。
 *
 * @param[in] p_rid Sampler 的资源标识符
 */
void RenderingDevice::FreeSamplerDriverResource(RID p_rid) {
  // First free dependents (UniformSets referencing via sampler+texture pairs)
  FreeResourceDependencies(p_rid);

  Sampler* sampler = sampler_owner_.GetOrNull(p_rid);
  if (sampler == nullptr) {
    return;
  }

  device_driver_->SamplerFree(sampler->driver_id);
  ARHUD_DELETE(sampler);
  sampler_owner_.Free(p_rid);
}

/**
 * @brief 释放单个 Shader 的驱动层资源
 *
 * 先级联释放引用该 Shader 的资源（UniformSet、Pipeline——
 * UniformSetCreate/PipelineCreate 均登记 shader→dependent 边），
 * 然后调用驱动层 ShaderFree 释放 GPU 着色器资源，销毁 Shader
 * 对象并释放 RID 槽位。不级联会留下持悬垂 ShaderID 的存活资源。
 *
 * @param[in] p_rid Shader 的资源标识符
 */
void RenderingDevice::FreeShaderDriverResource(RID p_rid) {
  // First free dependents (UniformSets / Pipelines referencing this shader)
  FreeResourceDependencies(p_rid);

  Shader* shader = shader_owner_.GetOrNull(p_rid);
  if (shader == nullptr) {
    return;
  }

  device_driver_->ShaderFree(shader->driver_id);
  ARHUD_DELETE(shader);
  shader_owner_.Free(p_rid);
}

/**
 * @brief 释放单个 UniformSet 的驱动层资源
 *
 * 先清理自身登记的依赖边（shader/buffer/texture/sampler → 本 us，
 * 防止依赖图残留陈旧边），然后调用驱动层 UniformSetFree 释放 GPU
 * 均匀集资源，销毁 UniformSet 对象并释放 RID 槽位。
 *
 * @param[in] p_rid UniformSet 的资源标识符
 */
void RenderingDevice::FreeUniformSetDriverResource(RID p_rid) {
  // Remove own edges (cascade half is a no-op: nothing references a UniformSet)
  FreeResourceDependencies(p_rid);

  UniformSet* us = uniform_set_owner_.GetOrNull(p_rid);
  if (us == nullptr) {
    return;
  }

  device_driver_->UniformSetFree(us->driver_id);
  ARHUD_DELETE(us);
  uniform_set_owner_.Free(p_rid);
}

/**
 * @brief 释放单个 VertexFormat 的驱动层资源
 *
 * 先释放依赖该 VertexFormat 的资源（如引用它的 Pipeline），
 * 然后调用驱动层 VertexFormatFree 释放 GPU 资源，销毁 VertexFormat 对象，
 * 并释放 RID 槽位。
 *
 * @param[in] p_rid VertexFormat 的资源标识符
 */
void RenderingDevice::FreeVertexFormatDriverResource(RID p_rid) {
  // First free dependents (Pipelines that reference this vertex format)
  FreeResourceDependencies(p_rid);

  VertexFormat* vf = vertex_format_owner_.GetOrNull(p_rid);
  if (vf == nullptr) {
    return;
  }

  device_driver_->VertexFormatFree(vf->driver_id);
  ARHUD_DELETE(vf);
  vertex_format_owner_.Free(p_rid);
}

/**
 * @brief 释放单个 RenderPass 的驱动层资源
 *
 * 先释放依赖该 RenderPass 的资源（如引用它的 Framebuffer、Pipeline），
 * 然后调用驱动层 RenderPassFree 释放 GPU 资源，销毁 RenderPass 对象，
 * 并释放 RID 槽位。
 *
 * @param[in] p_rid RenderPass 的资源标识符
 */
void RenderingDevice::FreeRenderPassDriverResource(RID p_rid) {
  // First free dependents (Framebuffers, Pipelines that reference this render
  // pass)
  FreeResourceDependencies(p_rid);

  RenderPass* rp = render_pass_owner_.GetOrNull(p_rid);
  if (rp == nullptr) {
    return;
  }

  device_driver_->RenderPassFree(rp->driver_id);
  ARHUD_DELETE(rp);
  render_pass_owner_.Free(p_rid);
}

/**
 * @brief 释放单个 Framebuffer 的驱动层资源
 *
 * 检查 driver_id 是否有效，若有效则调用驱动层 FramebufferFree 释放 GPU 资源。
 * 销毁 Framebuffer 对象并释放 RID 槽位。
 *
 * @param[in] p_rid Framebuffer 的资源标识符
 *
 * @note 某些 Framebuffer 可能没有创建驱动层资源（driver_id 无效），
 *       此时仅销毁宿主对象并释放 RID。
 */
void RenderingDevice::FreeFramebufferDriverResource(RID p_rid) {
  // Remove own edges (cascade half is a no-op: nothing references a
  // Framebuffer)
  FreeResourceDependencies(p_rid);

  Framebuffer* fb = framebuffer_owner_.GetOrNull(p_rid);
  if (fb == nullptr) {
    return;
  }

  if (fb->driver_id.IsValid()) {
    device_driver_->FramebufferFree(fb->driver_id);
  }
  ARHUD_DELETE(fb);
  framebuffer_owner_.Free(p_rid);
}

/**
 * @brief 释放单个 Pipeline 的驱动层资源
 *
 * 从 Pipeline 缓存（render_pipeline_cache_）中移除对应条目，
 * 调用驱动层 PipelineFree 释放 GPU 资源，销毁 Pipeline 对象，
 * 并释放 RID 槽位。
 *
 * @param[in] p_rid Pipeline 的资源标识符
 */
void RenderingDevice::FreePipelineDriverResource(RID p_rid) {
  // Remove own edges (cascade half is a no-op: nothing references a Pipeline)
  FreeResourceDependencies(p_rid);

  Pipeline* pipe = pipeline_owner_.GetOrNull(p_rid);
  if (pipe == nullptr) {
    return;
  }

  // 从 Pipeline 缓存中移除
  render_pipeline_cache_.Erase(pipe->cache_key);

  device_driver_->PipelineFree(pipe->driver_id);
  ARHUD_DELETE(pipe);
  pipeline_owner_.Free(p_rid);
}

/**
 * @brief 释放所有资源（Finalize 时调用）
 *
 * 按依赖顺序释放所有帧槽位中的延迟销毁资源，然后遍历所有资源 Owner
 * 释放剩余的驱动层资源。释放顺序：Pipeline → Framebuffer →
 * RenderPass → UniformSet → Shader → VertexFormat → Sampler → Texture →
 * Buffer。
 *
 * @pre 必须在 Finalize() 中调用，确保不再有新的资源创建或命令提交
 *
 * @note 该方法会清空所有资源 Owner 和 Pipeline 缓存，调用后设备处于干净状态。
 */
void RenderingDevice::FreeAllResources() {
  for (uint32 i = 0; i < frame_count_; ++i) {
    FreeResourcesFromFrame(frames_[i]);
    frames_[i].Clear();
  }

  pipeline_owner_.ForEach([this](RID p_rid, Pipeline* p_obj) {
    device_driver_->PipelineFree(p_obj->driver_id);
    ARHUD_DELETE(p_obj);
  });
  pipeline_owner_.Clear();
  render_pipeline_cache_.Clear();

  framebuffer_owner_.ForEach([this](RID p_rid, Framebuffer* p_obj) {
    if (p_obj->driver_id.IsValid()) {
      device_driver_->FramebufferFree(p_obj->driver_id);
    }
    ARHUD_DELETE(p_obj);
  });
  framebuffer_owner_.Clear();

  render_pass_owner_.ForEach([this](RID p_rid, RenderPass* p_obj) {
    device_driver_->RenderPassFree(p_obj->driver_id);
    ARHUD_DELETE(p_obj);
  });
  render_pass_owner_.Clear();

  uniform_set_owner_.ForEach([this](RID p_rid, UniformSet* p_obj) {
    device_driver_->UniformSetFree(p_obj->driver_id);
    ARHUD_DELETE(p_obj);
  });
  uniform_set_owner_.Clear();

  shader_owner_.ForEach([this](RID p_rid, Shader* p_obj) {
    device_driver_->ShaderFree(p_obj->driver_id);
    ARHUD_DELETE(p_obj);
  });
  shader_owner_.Clear();

  vertex_format_owner_.ForEach([this](RID p_rid, VertexFormat* p_obj) {
    device_driver_->VertexFormatFree(p_obj->driver_id);
    ARHUD_DELETE(p_obj);
  });
  vertex_format_owner_.Clear();

  sampler_owner_.ForEach([this](RID p_rid, Sampler* p_obj) {
    device_driver_->SamplerFree(p_obj->driver_id);
    ARHUD_DELETE(p_obj);
  });
  sampler_owner_.Clear();

  texture_owner_.ForEach([this](RID p_rid, Texture* p_obj) {
    device_driver_->TextureFree(p_obj->driver_id);
    ARHUD_DELETE(p_obj);
  });
  texture_owner_.Clear();

  buffer_owner_.ForEach([this](RID p_rid, Buffer* p_obj) {
    device_driver_->BufferFree(p_obj->driver_id);
    ARHUD_DELETE(p_obj);
  });
  buffer_owner_.Clear();
}

}  // namespace arhud
