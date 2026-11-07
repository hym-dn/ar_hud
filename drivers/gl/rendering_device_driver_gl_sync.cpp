/**
 * @file rendering_device_driver_gl_sync.cpp
 * @brief GLRenderingDeviceDriver 同步原语实现
 *
 * 包含所有同步相关方法的 OpenGL 实现：
 * - SwapChain 操作（Create/Destroy/Acquire/Present；Resize 未 override，
 *   走契约默认空实现，尺寸簿记由 RD 层维护）
 * - Fence 操作（Create/Destroy/Reset/IsSignaled/Wait）
 * - TimestampQuery 操作（Create/Destroy/Record/GetResult/IsAvailable）
 *
 * @par GL 退化语义总览
 *   契约接口按 Vulkan 语义设计，GL 3.3 无对应原生概念时的退化策略：
 *
 *   | 契约概念       | Vulkan 对应            | GL 退化实现 |
 *   |---------------|------------------------|----------------------------------|
 *   | SwapChain     | VkSwapchainKHR         | Surface↔Context 绑定记录 | |
 * Acquire       | vkAcquireNextImageKHR  | MakeCurrent                      |
 *   | Present       | vkQueuePresentKHR      | SwapBuffers（隐式同步） | |
 * Fence         | VkFence                | glFenceSync / glClientWaitSync   |
 *   | TimestampQuery| VkQueryPool(timestamp) | glQueryCounter(GL_TIMESTAMP) |
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-06-14
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include <glad/gl.h>

#include "io/logger.h"
#include "os/memory.h"
#include "rendering_device_driver_gl.h"

namespace arhud {
// ═══════════════════════════════════════════════════════════════════════
// SwapChain 管理
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 创建 SwapChain（Surface ↔ Context 呈现绑定）
 *
 * @par GL 语义（与 Vulkan 的差异）
 *   Vulkan 的 SwapChain 是可呈现图像队列（Acquire 返回 image，
 *   Present 提交队列）。GL 3.3 无此概念：默认 FBO 即后备缓冲，
 *   SwapBuffers 隐式交换。本实现将 SwapChain 退化为绑定记录
 *   （GLSwapChain 三元组），Acquire/Present 退化为 MakeCurrent/
 *   SwapBuffers。
 *
 * 实现流程：
 *   1. 校验 RCD Surface / Context ID 有效性
 *   2. 经 context_driver_ 将 RCD Surface 映射为 GL Surface
 *   3. 登记 GLSwapChain{rcd_surface, gl_context, gl_surface}
 *
 * @param[in] p_surface RCD 层 Surface ID（呈现目标）
 * @param[in] p_context RCD 层 Context ID（渲染上下文）
 *
 * @return SwapChain ID（1-based 序号，= 容器下标 + 1）
 * @retval kInvalidSwapChainId 参数无效、context_driver_ 缺失或 Surface 映射失败
 *
 * @note ID 为 1-based：0 保留给 kInvalidSwapChainId，下标换算为 id - 1
 * @note 容器只增不删（Destroy 走墓碑置空，槽位不复用）→ ID 全生命周期稳定
 * @note 上层入口是 RD::MakeCurrent()：首次对 (Context, Surface) 组合
 *       调用时自动创建，之后复用同一 SwapChain
 *
 * @see GLSwapChain（rendering_device_driver_gl.h）
 * @see RenderingDevice::MakeCurrent()
 */
IRenderingDeviceDriver::SwapChainID GLRenderingDeviceDriver::SwapChainCreate(
    IRenderingContextDriver::SurfaceID p_surface,
    IRenderingContextDriver::ContextID p_context) {
  if (p_surface == IRenderingContextDriver::kInvalidSurfaceId ||
      p_context == IRenderingContextDriver::kInvalidContextId) {
    return kInvalidSwapChainId;
  }

  if (!context_driver_) {
    return kInvalidSwapChainId;
  }

  IGLContextManager::SurfaceID gl_surf =
      context_driver_->SurfaceGetAPISurface(p_surface);
  if (gl_surf == IGLContextManager::kInvalidSurfaceId) {
    return kInvalidSwapChainId;
  }

  GLSwapChain sc;
  sc.rcd_surface = p_surface;
  sc.gl_context = static_cast<IGLContextManager::ContextID>(p_context);
  sc.gl_surface = gl_surf;

  swap_chains_.PushBack(sc);

  return static_cast<SwapChainID>(swap_chains_.Size());
}

/**
 * @brief 销毁 SwapChain（墓碑置空）
 *
 * GL 无原生 SwapChain 对象需要销毁 —— 仅将槽内三个 ID 字段置为无效
 * 值（墓碑化）：
 *   - 槽位不删除、不复用 → 其余 SwapChain 的 ID 保持稳定
 *   - 后续对墓碑槽的 Acquire / Present 命中无效参数分支，安全失败
 *   - 若需恢复使用，须重新 SwapChainCreate（新槽新 ID）
 *
 * @param[in] p_swapchain SwapChain ID
 *
 * @note 幂等：重复调用、无效 ID 均静默返回
 * @note 不触碰 GL 状态（无 GL 调用），可在任意线程调用
 */
void GLRenderingDeviceDriver::SwapChainDestroy(SwapChainID p_swapchain) {
  if (p_swapchain == kInvalidSwapChainId || p_swapchain > swap_chains_.Size()) {
    return;
  }

  uint32 index = static_cast<uint32>(p_swapchain) - 1;
  if (index < swap_chains_.Size()) {
    swap_chains_[index].rcd_surface =
        IRenderingContextDriver::kInvalidSurfaceId;
    swap_chains_[index].gl_context = IGLContextManager::kInvalidContextId;
    swap_chains_[index].gl_surface = IGLContextManager::kInvalidSurfaceId;
  }
}

/**
 * @brief 获取 SwapChain 的“下一帧图像”（GL：MakeCurrent）
 *
 * GL 退化语义：Vulkan 的 Acquire 返回一张可呈现图像并占用一个
 * image slot；GL 只需把绑定的 Context 与 Surface 设为当前 —— 后续
 * GL 命令的输出即写入该 Surface 的后备缓冲（无 image 概念）。
 *
 * @param[in] p_swapchain SwapChain ID
 *
 * @retval Error::kOK 绑定成功，当前线程已持 GL 上下文
 * @retval Error::kInvalidParameter ID 无效或 SwapChain 已销毁（墓碑槽）
 * @retval Error::kFailed gl_manager_ 缺失（驱动未完成初始化）
 * @retval 其余 IGLContextManager::MakeCurrent 透传的错误码
 *
 * @pre 必须在即将执行 GL 命令的线程上调用（GL 上下文线程亲和）
 * @note 已 Destroy 的 SwapChain 因墓碑字段命中 kInvalidParameter 分支
 */
Error GLRenderingDeviceDriver::SwapChainAcquire(SwapChainID p_swapchain) {
  if (p_swapchain == kInvalidSwapChainId || p_swapchain > swap_chains_.Size()) {
    return Error::kInvalidParameter;
  }

  uint32 index = static_cast<uint32>(p_swapchain) - 1;
  const GLSwapChain& sc = swap_chains_[index];

  if (sc.gl_context == IGLContextManager::kInvalidContextId ||
      sc.gl_surface == IGLContextManager::kInvalidSurfaceId) {
    return Error::kInvalidParameter;
  }

  if (!gl_manager_) {
    return Error::kFailed;
  }

  return gl_manager_->MakeCurrent(sc.gl_context, sc.gl_surface);
}

/**
 * @brief 呈现 SwapChain 的渲染结果（GL：SwapBuffers）
 *
 * GL 退化语义：将 Surface 的后备缓冲提交给窗口系统合成器（Windows
 * 上进入 DWM 合成路径），并按 GL 3.3 交换语义隐式完成 GPU-CPU 同步
 * （Vulkan 的 present 信号量 / image ownership 在 GL 中不存在）。
 *
 * @param[in] p_swapchain SwapChain ID
 *
 * @pre 同一 SwapChain 已先经 SwapChainAcquire() 在本线程 MakeCurrent
 * @note ID 无效、墓碑槽或 gl_manager_ 缺失时静默返回（幂等安全）
 */
void GLRenderingDeviceDriver::SwapChainPresent(SwapChainID p_swapchain) {
  if (p_swapchain == kInvalidSwapChainId || p_swapchain > swap_chains_.Size()) {
    return;
  }

  uint32 index = static_cast<uint32>(p_swapchain) - 1;
  const GLSwapChain& sc = swap_chains_[index];

  if (sc.gl_surface == IGLContextManager::kInvalidSurfaceId) {
    return;
  }

  if (!gl_manager_) {
    return;
  }

  gl_manager_->SwapBuffers(sc.gl_surface);
}

// ═══════════════════════════════════════════════════════════════════════
// GPU Fence（glFenceSync / glClientWaitSync）
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 创建 GPU Fence（glFenceSync）
 *
 * 在当前命令流位置插入同步栅栏：此前提交的所有 GL 命令被 GPU 完成
 * 后触发。GLsync 对应 Vulkan VkFence 的 GPU-CPU 同步角色。
 *
 * @par p_signaled = true 的退化处理
 *   GL 3.3 的 glFenceSync 只能创建未触发状态（无对应 Vulkan
 *   VK_FENCE_CREATE_SIGNALED_BIT 的能力）。请求“已触发”fence 时
 *   返回 kInvalidFenceId —— 本驱动约定无效 ID 视为已触发（见
 *   FenceIsSignaled / FenceWait 对 kInvalidFenceId 的处理），调用方
 *   无感知地获得正确的免等待语义。
 *
 * @param[in] p_signaled 是否创建为已触发状态（GL 不支持，见上）
 *
 * @return Fence ID（GLsync 指针按 uintptr_t 重解释）
 * @retval kInvalidFenceId p_signaled 为 true，或 glFenceSync 分配失败
 *
 * @pre 必须在拥有 GL 上下文的线程上调用（fence 插入当前命令流）
 * @note FenceID 为指针值重解释，不可序列化 / 跨进程传递
 * @note 典型消费者：RD EndFrame 插帧栅栏（延迟销毁的 GPU 完成判据）
 */
IRenderingDeviceDriver::FenceID GLRenderingDeviceDriver::FenceCreate(
    bool p_signaled) {
  if (p_signaled) {
    return kInvalidFenceId;
  }

  GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  if (sync == nullptr) {
    return kInvalidFenceId;
  }

  GL_CHECK_ERROR();

  return static_cast<FenceID>(reinterpret_cast<uintptr_t>(sync));
}

/**
 * @brief 销毁 GPU Fence（glDeleteSync）
 *
 * @param[in] p_fence Fence ID
 *
 * @note 幂等：无效 ID 静默返回
 * @note GLsync 销毁语义：未触发的 fence 标记删除，驱动延迟到触发后
 *       物理回收，不影响 in-flight 命令；但销毁后不得再用于
 *       FenceWait / FenceIsSignaled（GLsync 句柄已失效）
 */
void GLRenderingDeviceDriver::FenceDestroy(FenceID p_fence) {
  if (p_fence == kInvalidFenceId) {
    return;
  }

  GLsync sync = reinterpret_cast<GLsync>(static_cast<uintptr_t>(p_fence));
  glDeleteSync(sync);
}

/**
 * @brief 重置 GPU Fence（GL：空操作）
 *
 * GLsync 无 reset 能力 —— fence 是一次性对象，触发后不可复用。
 * 契约语义为“销毁后重建”（见 IRenderingDeviceDriver::FenceReset
 * 契约注释），需要可复用 fence 的调用方应自行 Create 新对象。
 *
 * @param[in] p_fence Fence ID
 *
 * @note 本实现为空操作，不产生任何 GL 调用
 */
void GLRenderingDeviceDriver::FenceReset(FenceID p_fence) {
  (void)p_fence;
}

/**
 * @brief 查询 GPU Fence 是否已触发（glGetSynciv）
 *
 * @param[in] p_fence Fence ID
 *
 * @return true 已触发（GL_SIGNALED），或 ID 无效（视为已触发）
 * @return false 未触发（GL_UNSIGNALED）
 *
 * @note kInvalidFenceId 返回 true —— 与 FenceCreate 对 p_signaled 的
 *       退化处理（无效 ID ≡ 已触发）配套，调用方语义自洽
 * @note 非阻塞查询，不产生 CPU 等待（与 FenceWait 的区别）
 */
bool GLRenderingDeviceDriver::FenceIsSignaled(FenceID p_fence) {
  if (p_fence == kInvalidFenceId) {
    return true;
  }

  GLsync sync = reinterpret_cast<GLsync>(static_cast<uintptr_t>(p_fence));
  GLint result = 0;
  GLsizei length = 0;
  glGetSynciv(sync, GL_SYNC_STATUS, sizeof(GLint), &length, &result);
  return result == GL_SIGNALED;
}

/**
 * @brief 阻塞等待 GPU Fence 触发（glClientWaitSync，带超时）
 *
 * @param[in] p_fence      Fence ID
 * @param[in] p_timeout_ns 超时（纳秒）
 *
 * @return true 已触发（GL_ALREADY_SIGNALED / GL_CONDITION_SATISFIED）
 * @return false 超时（GL_TIMEOUT_EXPIRED）或失败（GL_WAIT_FAILED）
 *
 * @note kInvalidFenceId 视为已触发，直接返回 true（语义同
 *       FenceIsSignaled 的无效 ID 约定）
 * @note GL_SYNC_FLUSH_COMMANDS_BIT：进入等待前 flush 命令缓冲 ——
 *       若 fence 尚未随命令下发到 GPU 而 CPU 先行阻塞，将死锁；
 *       flush 确保命令先于等待下发（GL 规范建议的防死锁姿势）
 *
 * @par 典型消费者
 *   RD FreeResourcesFromPreviousFrames 以 1 秒超时等待帧栅栏，
 *   确认 GPU 完成后才执行延迟销毁。
 */
bool GLRenderingDeviceDriver::FenceWait(FenceID p_fence, uint64 p_timeout_ns) {
  if (p_fence == kInvalidFenceId) {
    return true;
  }

  GLsync sync = reinterpret_cast<GLsync>(static_cast<uintptr_t>(p_fence));
  GLenum wait_result = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT,
                                        static_cast<GLuint64>(p_timeout_ns));
  return wait_result == GL_ALREADY_SIGNALED ||
         wait_result == GL_CONDITION_SATISFIED;
}

// ═══════════════════════════════════════════════════════════════════════
// GPU 时间戳查询
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 创建 GPU 时间戳查询对象（glGenQueries）
 *
 * @return 查询 ID（GL query 对象名，GLuint 值直接转 uint64）
 * @retval kInvalidTimestampQueryId glGenQueries 失败（对象名为 0）
 *
 * @pre 必须在拥有 GL 上下文的线程上调用
 * @note 0 是 GL 保留的非法 query 对象名，与 kInvalidTimestampQueryId
 *       数值重合，天然作为无效标记
 */
IRenderingDeviceDriver::TimestampQueryID
GLRenderingDeviceDriver::TimestampQueryCreate() {
  GLuint query = 0;
  glGenQueries(1, &query);
  if (query == 0) {
    return kInvalidTimestampQueryId;
  }
  return static_cast<TimestampQueryID>(query);
}

/**
 * @brief 销毁 GPU 时间戳查询对象（glDeleteQueries）
 *
 * @param[in] p_query 查询 ID
 *
 * @note 幂等：无效 ID 静默返回
 */
void GLRenderingDeviceDriver::TimestampQueryDestroy(TimestampQueryID p_query) {
  if (p_query == kInvalidTimestampQueryId)
    return;
  GLuint query = static_cast<GLuint>(p_query);
  glDeleteQueries(1, &query);
}

/**
 * @brief 在当前命令流位置记录 GPU 时间戳（glQueryCounter）
 *
 * 使用 GL_TIMESTAMP 单点绝对时间戳（glQueryCounter），而非
 * glBeginQuery / glEndQuery 的区间统计 —— 两次 Record 的结果相减
 * 即得该区间的 GPU 耗时（对应 Vulkan timestamp query 池语义）。
 *
 * @param[in] p_query 查询 ID
 *
 * @pre 必须在拥有 GL 上下文的线程上调用
 * @note 时间戳在 GPU 执行到该点时写入（非 CPU 提交时刻），提交后
 *       需经 IsAvailable 确认就绪才能取值
 */
void GLRenderingDeviceDriver::TimestampQueryRecord(TimestampQueryID p_query) {
  if (p_query == kInvalidTimestampQueryId)
    return;
  GLuint query = static_cast<GLuint>(p_query);
  glQueryCounter(query, GL_TIMESTAMP);
}

/**
 * @brief 获取时间戳查询结果（GPU 时钟域，纳秒）
 *
 * @param[in] p_query 查询 ID
 *
 * @return GPU 时间戳（纳秒），无效 ID 返回 0
 *
 * @warning GL_QUERY_RESULT 是阻塞语义：结果未就绪时 CPU 阻塞直至
 *          GPU 执行到该查询点。建议先经 TimestampQueryIsAvailable()
 *          确认就绪再取值（无 GL_QUERY_RESULT_NO_WAIT 依赖，保 3.3）
 * @note 返回值为 GPU 时钟域的绝对时间戳，两次 Record 相减得区间耗时
 */
uint64 GLRenderingDeviceDriver::TimestampQueryGetResult(
    TimestampQueryID p_query) {
  if (p_query == kInvalidTimestampQueryId)
    return 0;
  GLuint query = static_cast<GLuint>(p_query);
  GLuint64 result = 0;
  glGetQueryObjectui64v(query, GL_QUERY_RESULT, &result);
  return result;
}

/**
 * @brief 查询时间戳结果是否就绪（非阻塞）
 *
 * @param[in] p_query 查询 ID
 *
 * @return true 结果就绪，可安全调用 TimestampQueryGetResult()
 * @return false GPU 尚未执行到查询点（或 ID 无效）
 *
 * @note 非阻塞查询，用于 GetResult 前的就绪探测，避免阻塞语义卡帧
 */
bool GLRenderingDeviceDriver::TimestampQueryIsAvailable(
    TimestampQueryID p_query) {
  if (p_query == kInvalidTimestampQueryId)
    return false;
  GLuint query = static_cast<GLuint>(p_query);
  GLint available = 0;
  glGetQueryObjectiv(query, GL_QUERY_RESULT_AVAILABLE, &available);
  return available != 0;
}

}  // namespace arhud
