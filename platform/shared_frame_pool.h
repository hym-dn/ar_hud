/**
 * @file shared_frame_pool.h
 * @brief 共享显存帧池契约——帧缓冲归显示侧分配，渲染侧按 index 借用。
 *
 * @par 设计要点（零回读上屏，design skill §5.5/§7.7）
 *   Route B 的"上屏线程不发渲染命令"不变量，在生产平台上的落地形态是
 *   **显示侧分配帧缓冲、渲染侧写进同一块显存、显示侧用完归还**
 *   （QNX screen_buffer + release event / Linux GBM bo + flip event /
 *   Android AHardwareBuffer + queueBuffer）。本接口是这一形态的
 *   API 无关表达：
 *
 *   - 跨渲染线程↔上屏线程传递的是 **帧 index（token）**，不是像素；
 *   - 像素始终留在显存，全帧 CPU 拷贝次数 = 0；
 *   - `CommitFrame` 内的"等显示侧消费完成"即生产平台的 release 事件
 *     语义（free-after-release，取代 IFramePresenter::Present 的
 *     "CPU 拷贝完成"语义）。
 *
 * @par 与另两条呈现路径的关系
 *   | 路径 | 帧缓冲归属 | 边界上传递 | 全帧拷贝 |
 *   |---|---|---|---|
 *   | Mode R（kReadback） | 渲染侧（RD 私有纹理） | CPU 像素槽 | 2 |
 *   | Mode S（kSwap） | 窗口 backbuffer | 纯信号 | 0（需逐屏切 drawable） |
 *   | Mode X（本接口） | **显示侧** | 帧 index | 0（不切 drawable） |
 *
 *   Mode X 不需要 `wglMakeCurrent` 逐屏切 drawable：渲染目标不是"窗口"
 *   而是一块普通显存，FBO 渲染与当前 drawable 无关。
 *
 * @par 渲染侧的配套义务
 *   帧资源经 RDD 的外部资源导入（`TextureCreateExternal`）变成 GL 可写
 *   纹理；每帧写入前后必须做跨 API 加解锁（`TextureExternalBeginWrite` /
 *   `TextureExternalEndWrite`）——那是唯一的 GPU 侧同步点，本接口不
 *   替代它。
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-11-08
 * @copyright Copyright (c) 2026 ARHud Project
 * @ingroup platform
 */

#ifndef ARHUD_PLATFORM_SHARED_FRAME_POOL_H_
#define ARHUD_PLATFORM_SHARED_FRAME_POOL_H_

#include "typedefs.h"

namespace arhud {

/**
 * @brief 共享帧像素格式（平台侧词表，避免 platform 反向依赖 drivers）
 *
 * 与 DataFormat 的对应关系由各后端实现负责翻译
 * （Win32/DXGI：kRgba8Unorm ↔ DXGI_FORMAT_R8G8B8A8_UNORM）。
 */
enum class SharedFrameFormat : uint32 {
  kRgba8Unorm = 0,  ///< RGBA8（字节序 R,G,B,A），GL 侧等价 GL_RGBA8
};

/**
 * @brief 共享显存帧池接口（显示侧拥有帧缓冲）
 *
 * 每屏一个实例。实现负责：帧缓冲分配、与窗口绑定的呈现通道
 * （Win32: DXGI flip-model swap chain）、以及"帧交出去 → 归还"的节拍。
 *
 * @par 线程模型
 *   - `Initialize`/`Finalize`/`Resize`：渲染线程（引擎在
 *     `RenderViewport::Initialize`/`ApplyPendingResize` 内调用，须无在途帧）；
 *   - `AcquireFrame`：渲染线程（取本帧要写的显存，仅读描述，不加锁）；
 *   - `CommitFrame`：上屏线程（零渲染 API，只做拷贝 + 提交 + 等待归还）。
 *
 * @par 失败语义
 *   `IsAvailable()` 为 false 时本池不可用（平台不支持/设备创建失败/
 *   适配器不匹配），调用方必须回落到 Mode R。构造后未 `Initialize`
 *   亦视为不可用。
 */
class ISharedFramePool {
 public:
  static constexpr uint32 kInvalidFrame = UINT32_MAX;  ///< 无效帧 index

  /**
   * @brief 一帧共享显存的描述（POD，可按值跨层传递）
   *
   * @note `native_*` 是不透明平台指针，仅由同平台的 GPU 后端解释
   *       （Win32：`native_device` = ID3D11Device*，
   *       `native_resource` = ID3D11Texture2D*）。上层不得解引用。
   */
  struct Frame {
    uint32 index = kInvalidFrame;    ///< 环内 index（= 跨线程传递的 token）
    void* native_device = nullptr;   ///< 拥有该资源的 GPU 设备
    void* native_resource = nullptr;  ///< 帧缓冲资源
    uint32 width = 0;               ///< 帧宽（像素）
    uint32 height = 0;              ///< 帧高（像素）
    SharedFrameFormat format = SharedFrameFormat::kRgba8Unorm;  ///< 像素格式

    /** @brief 是否为有效帧描述 */
    bool IsValid() const {
      return index != kInvalidFrame && native_resource != nullptr;
    }
  };

  virtual ~ISharedFramePool() = default;

  /**
   * @brief 创建设备、帧缓冲环与呈现通道
   *
   * @param[in] p_native_window 平台窗口句柄（Win32: HWND）
   * @param[in] p_width         帧宽（像素）
   * @param[in] p_height        帧高（像素）
   *
   * @retval Error::kOK              可用
   * @retval Error::kInvalidParameter 句柄为空或尺寸非法
   * @retval Error::kUnavailable     平台不具备共享显存呈现能力
   */
  virtual Error Initialize(void* p_native_window, uint32 p_width,
                           uint32 p_height) = 0;

  /** @brief 释放帧缓冲环与呈现通道（幂等） */
  virtual void Finalize() = 0;

  /**
   * @brief 尺寸变更：重建帧缓冲环与 swap chain 缓冲
   *
   * @pre 无在途帧（调用方由 FrameImageQueue::HasInFlight 保证）
   */
  virtual void Resize(uint32 p_width, uint32 p_height) = 0;

  /** @brief 本池是否可用（false = 调用方须回落 Mode R） */
  virtual bool IsAvailable() const = 0;

  /** @brief 帧缓冲环深度（≥2；渲染侧按此轮转 token） */
  virtual uint32 GetFrameCount() const = 0;

  /**
   * @brief 取指定帧的描述（不做加锁；跨 API 锁定归渲染侧后端）
   *
   * @param[in] p_index 帧 index，须 < GetFrameCount()
   * @return 帧描述；越界返回无效 Frame
   */
  virtual Frame GetFrame(uint32 p_index) const = 0;

  /**
   * @brief 提交一帧到屏幕（上屏线程）
   *
   * 语义等同生产平台的 post：本调用返回表示帧已交给显示引擎；
   * "该显存可复用"的确认由渲染侧下一次加锁兑现（实现方在文档中说明）。
   *
   * @param[in] p_index      帧 index（由队列 token 承载）
   * @param[in] p_timeout_ms 等待上限（毫秒；实现不需等待时可忽略）
   *
   * @retval Error::kOK      已提交
   * @retval Error::kFailed  提交失败（设备丢失/资源无效）
   * @retval Error::kUnavailable 本池不可用（调用方应回落 Mode R）
   */
  virtual Error CommitFrame(uint32 p_index, uint32 p_timeout_ms) = 0;

  /**
   * @brief 诊断：计算指定帧的平均亮度（0-255），不改变帧内容
   *
   * @par 为什么留在契约里
   *   零拷贝路径没有 CPU 像素可供肉眼/工具核对，"画面到底画进去没有"
   *   只能靠把那块显存读回来采样。本方法是**一次性调试出口**：默认实现
   *   返回 false（不支持），实现在被 `ARHUD_DEBUG_SAMPLE` 打开时才付出
   *   一次 staging 拷贝的代价。它不参与正常呈现。
   *
   * @param[in]  p_index 帧 index
   * @param[out] o_mean  平均亮度（0-255）
   *
   * @return true 采样成功
   */
  virtual bool SampleFrameLuminance(uint32 p_index, float* o_mean) {
    (void)p_index;
    (void)o_mean;
    return false;
  }

  /** @brief 后端名（诊断日志用，ASCII 字面量） */
  virtual const char* GetBackendName() const = 0;
};


}  // namespace arhud

#endif  // ARHUD_PLATFORM_SHARED_FRAME_POOL_H_
