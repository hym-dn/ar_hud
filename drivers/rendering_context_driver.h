/**
 * @file rendering_context_driver.h
 * @brief 渲染上下文驱动抽象接口
 *
 * IRenderingContextDriver 是 GPU API 实例化和设备枚举的统一抽象层，
 * 位于渲染架构的最底层，负责管理 GPU 设备发现、渲染表面（Surface）
 * 生命周期和设备驱动（RenderingDeviceDriver）创建。
 *
 * @par 架构定位
 * @code
 *   ┌─────────────────────────────────────────────────────┐
 *   │            RenderServer / ItemStorage               │  ← 业务层
 *   ├─────────────────────────────────────────────────────┤
 *   │                  RenderingDevice (RD)               │  ← 设备层
 *   ├─────────────────────────────────────────────────────┤
 *   │           RenderingDeviceDriver (RDD)               │  ← 驱动层
 *   ├─────────────────────────────────────────────────────┤
 *   │          IRenderingContextDriver (RCD)  ← 本文件     │  ← 上下文层
 *   │              └── IGLContextManager                  │  ← GL 上下文
 *   └─────────────────────────────────────────────────────┘
 * @endcode
 *
 * @par 职责边界
 *   - **RCD 负责**：API 实例化、设备枚举、Surface 管理、Context 管理、驱动工厂
 *   - **IGLContextManager 负责**：GL 上下文创建/像素格式/MakeCurrent/VSync
 *     （RCD 内部持有，不对外暴露；像素格式索引进程内共享，见 Win32 实现的
 *     ConfigurePixelFormat）
 *   - **RDD 负责**：GPU 资源操作；SwapChain 接口保留（Vulkan 对齐）但 GL
 *     后端在"绘制/上屏分离"模式下退役——呈现不走 RDD
 *   - **RD 负责**：资源生命周期、Staging Buffer、帧循环、命令录制
 *     （DrawList/ComputeList 直录，无 CommandBuffer 池）
 *   - **呈现（Present）**：不在本层——由 platform 层 IFramePresenter 承担
 *     （见下文呈现模式），present 线程零 GL、零 RCD 依赖
 *
 * @par 呈现模式（Windows 双模式并存，2026-08-30 决策）
 *
 *   RCD 采用 Context/Surface 分离模型：MakeCurrent(ContextID, SurfaceID)
 *   双参数签名天然支持"单 Context 绑多 Surface"（drawable 切换）——
 *   这是两种呈现模式共用的接口基础。
 *
 * @code
 *   模式 R（kReadback，回读路径——现状默认）
 *   IRenderingContextDriver (RCD)
 *   ├── Context 0 (离屏 HGLRC，100×100 隐藏窗口承载像素格式)
 *   │   └── 渲染线程独占 MakeCurrent，渲染到 FBO，从不 SwapBuffers
 *   ├── 可见窗口 0 (HUD)   ──不设 GL 像素格式── FramebufferReadback ─►
 * PresentThread ─► GDI 上屏 ├── 可见窗口 1 (仪表盘) ──不设 GL 像素格式──
 * FramebufferReadback ─► PresentThread ─► GDI 上屏 └── 可见窗口 2 (后视镜)
 * ──不设 GL 像素格式── FramebufferReadback ─► PresentThread ─► GDI 上屏 └──
 * per-screen PresentThread 无 GL，纯平台 API（Win32: StretchDIBits） └──
 * 可见窗口不调 SurfaceCreate，不占 RCD Surface 槽位 └── 成本：glReadPixels 回读
 * ~1-2ms/屏 + CPU 帧拷贝（dev 过渡路径）
 *
 *   模式 S（kSwap，Swap 路径——打法 3 + 方案 B，已实现待激活）
 *   IRenderingContextDriver (RCD)
 *   ├── Context 0 (共享 HGLRC，像素格式索引全窗口一致——ConfigurePixelFormat
 * 缓存) │   └── 渲染线程独占：逐屏 MakeCurrent(ctx0, surfN) 切 drawable │ →
 * 渲到该窗口 backbuffer（FBO→composite→默认帧缓冲）→ 不 Swap │       →
 * 立即切下一屏 ├── Surface 0 (HUD)     ──设 GL 像素格式──┐ ├── Surface 1
 * (仪表盘) ──设 GL 像素格式──┤ 每屏一个 PresentThread（零 GL）： └── Surface 2
 * (后视镜) ──设 GL 像素格式──┘ └── SwapBuffers(hdc)（纯 GDI，无需 current
 * context）提交帧给 DWM └── DwmFlush() 等待 DWM 合成完成 =
 * 帧被原子收编（防撕裂保证） └── 之后渲染线程才复用该 backbuffer（对应生产平台
 * release 事件语义） └── 零回读零拷贝；vblank 等待在各 present 线程并行
 *
 *   模式 S 与旧"模式 1"（单线程串行 swap，2026-08-24 否决）的区别：
 *   swap 不在渲染线程内联——否决理由（渲染线程串行等 N 屏 vblank）
 *   由 per-screen present 线程化解；模式 S = 模式 1 的 drawable 拓扑
 *   + 模式 R 的呈现线程外置。
 *
 *   （历史决策：模式 2 = 多线程多 Context 各自 SwapBuffers，2026-08-24
 *    否决——上下文资源不共享；记录见 arhud-multiscreen-arch SKILL §0.2/§3.1）
 *
 *   生产平台（QNX/Linux/Android）不走上述 Windows 模式：离屏 context 渲
 *   FBO → EGLImage/DMA-BUF 导出 → present 线程交 compositor + release
 *   事件驱动 free-after-flip（SKILL §3.1/§3.6）。RCD 接口对此透明。
 * @endcode
 *
 * @par 生命周期
 *   1. 构造 RCD 实例（注入 IGLContextManager）
 *   2. Initialize() — 初始化 API、枚举设备
 *   3. SurfaceCreate() × N — 为每个屏幕创建渲染表面（模式 S）或仅离屏
 *      隐藏窗口（模式 R）
 *   4. CreateDeviceDriver() — 创建 RDD 实例
 *   5. 运行时：SurfaceSetSize / SurfaceSetVsyncMode / MakeCurrent
 *   6. SurfaceDestroy() — 销毁渲染表面
 *   7. Shutdown() — 释放所有资源
 *
 * @par 设计参考
 *   - Godot 4.6 RenderingContextDriver（rendering_context_driver.h）
 *   - Vulkan VkInstance + VkPhysicalDevice 枚举模式
 *   - Kanzi "单 Context + 多 Window Surface"（模式 S 的参照）
 *   - arch_skill.md §1.1 三层 GPU API 抽象
 *
 * @author yameng.he
 * @version 1.1
 * @date 2026-08-30
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_DRIVERS_RENDERING_CONTEXT_DRIVER_H_
#define ARHUD_DRIVERS_RENDERING_CONTEXT_DRIVER_H_

#include "gl_context_manager.h"
#include "template/local_vector.h"
#include "typedefs.h"
#include "window.h"

namespace arhud {

class IRenderingDeviceDriver;

/**
 * @brief 渲染上下文驱动抽象接口
 *
 * 管理 GPU API 实例化、物理设备枚举、渲染表面（Surface）生命周期
 * 和设备驱动（RenderingDeviceDriver）工厂。
 *
 * @par 关键职责
 *   - 初始化图形 API（OpenGL: 加载驱动 / Vulkan: 创建 Instance）
 *   - 枚举物理 GPU 设备（名称、厂商、类型）
 *   - 创建渲染表面（Surface = 可渲染的窗口区域）
 *   - 创建设备驱动（RenderingDeviceDriver 工厂方法）
 *
 * @par 与 IGLContextManager 的关系
 *   RCD 持有 IGLContextManager 的引用（不拥有所有权），通过 IGLContextManager
 *   管理 GL 上下文。IGLContextManager 不对外暴露，上层通过 RCD 的
 *   Context/Surface 管理接口间接使用 IGLContextManager。
 *   MakeCurrent 由 RCD 直接提供（drawable 切换的基础，见文件头"呈现模式"）；
 *   SwapBuffers 不在本层——呈现由 platform 层 IFramePresenter 承担
 *   （present 线程零 GL：Swap 路径直调 SwapBuffers(HDC)+DwmFlush，
 *   回读路径直调 GDI）。
 *
 * @par 线程安全
 *   - Initialize / Shutdown / SurfaceCreate / SurfaceDestroy /
 *     ContextCreate / ContextDestroy 应在主线程调用（初始化/清理阶段）
 *   - MakeCurrent / ReleaseCurrent 在需要绑定上下文的线程上调用
 *     （每个线程管理自己的绑定状态；渲染线程持 context 期间 present
 *     线程不触碰本接口——呈现路径零 GL）
 *   - SurfaceSetSize / SurfaceSetVsyncMode / SurfaceGet* 可在任意线程调用
 *   - CreateDeviceDriver 应在主线程调用
 *
 * @see IGLContextManager              GL 上下文管理（RCD 内部持有）
 * @see IRenderingDeviceDriver   设备驱动（由 RCD 创建）
 * @see IFramePresenter          平台上层呈现（present 线程，零 GL）
 * @see SurfaceInfo              渲染表面信息
 */
class IRenderingContextDriver {
 public:
  /** @brief 渲染表面 ID 类型 */
  using SurfaceID = uint32;

  /** @brief 无效表面 ID 常量 */
  static constexpr SurfaceID kInvalidSurfaceId = UINT32_MAX;

  /** @brief 渲染上下文 ID 类型 */
  using ContextID = uint32;

  /** @brief 无效上下文 ID 常量 */
  static constexpr ContextID kInvalidContextId = UINT32_MAX;

  // ═══════════════════════════════════════════════════════════════════
  // GPU 厂商 ID（对齐 PCI 厂商 ID）
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief GPU 厂商常量
   *
   * 使用 PCI 厂商 ID 标识 GPU 厂商，与 Vulkan VkPhysicalDeviceProperties
   * 中的 vendorID 对齐。OpenGL 通过 glGetString(GL_VENDOR) 字符串匹配。
   */
  struct Vendor {
    static constexpr uint32 kUnknown = 0x0;
    static constexpr uint32 kAMD = 0x1002;
    static constexpr uint32 kImgTec = 0x1010;
    static constexpr uint32 kApple = 0x106B;
    static constexpr uint32 kNVIDIA = 0x10DE;
    static constexpr uint32 kARM = 0x13B5;
    static constexpr uint32 kMicrosoft = 0x1414;
    static constexpr uint32 kQualcomm = 0x5143;
    static constexpr uint32 kIntel = 0x8086;
  };

  // ═══════════════════════════════════════════════════════════════════
  // 设备类型
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief GPU 设备类型
   *
   * 对齐 Vulkan VkPhysicalDeviceType。
   * 用于判断 GPU 的性能特征和功耗等级。
   *
   * @par 车机场景
   *   车机通常使用集成 GPU（Intel/ARM Mali/Adreno），
   *   高端车型可能配备独立 GPU（NVIDIA dGPU）。
   */
  enum class DeviceType : uint32 {
    kOther = 0,
    kIntegratedGpu = 1,
    kDiscreteGpu = 2,
    kVirtualGpu = 3,
    kCpu = 4,
    kMax
  };

  // ═══════════════════════════════════════════════════════════════════
  // 驱动变通方案
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 驱动变通方案标志
   *
   * 记录已知 GPU 驱动缺陷的变通方案。
   * 由 RCD 在设备枚举时根据厂商/型号设置。
   *
   * @par 使用场景
   *   某些 GPU 驱动在特定操作序列下有已知缺陷，
   *   RDD 在执行命令时检查这些标志来选择安全路径。
   */
  struct Workarounds {
    /** @brief 避免在绘制命令后立即执行计算命令（某些 AMD 驱动缺陷） */
    bool avoid_compute_after_draw = false;
  };

  // ═══════════════════════════════════════════════════════════════════
  // 设备信息
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 物理设备信息
   *
   * 描述一个物理 GPU 设备的静态属性。
   * 由 Initialize() 在设备枚举阶段填充。
   *
   * @par 使用示例
   *   @code
   *   auto* rcd = ...;
   *   uint32 count = rcd->GetDeviceCount();
   *   for (uint32 i = 0; i < count; ++i) {
   *       const auto& dev = rcd->GetDevice(i);
   *       // 选择独立 GPU 作为渲染设备
   *       if (dev.type == DeviceType::kDiscreteGpu) {
   *           selected = i;
   *           break;
   *       }
   *   }
   *   @endcode
   */
  struct Device {
    /** @brief 设备名称（如 "NVIDIA GeForce RTX 4060"） */
    const char* name = "Unknown";
    /** @brief GPU 厂商 ID（PCI Vendor ID） */
    uint32 vendor = Vendor::kUnknown;
    /** @brief 设备类型 */
    DeviceType type = DeviceType::kOther;
    /** @brief 驱动变通方案 */
    Workarounds workarounds;
  };

  // ═══════════════════════════════════════════════════════════════════
  // Surface 信息
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 渲染表面信息
   *
   * 只存储 RCD 渲染层的状态，平台层数据通过 IGLContextManager 查询。
   *
   * @par 职责划分
   *   - IGLContextManager（平台层）：HDC、HWND、VSync、owner_context_id
   *   - SurfaceInfo（渲染层）：尺寸、resize 标记、GL ID 映射
   */
  struct SurfaceInfo {
    /** @brief 关联的 GL 渲染表面 ID */
    IGLContextManager::SurfaceID gl_surface =
        IGLContextManager::kInvalidSurfaceId;
    /** @brief 表面宽度（像素） */
    uint32 width = 0;
    /** @brief 表面高度（像素） */
    uint32 height = 0;
    /** @brief 是否需要重新调整大小 */
    bool needs_resize = false;
  };

  // ═══════════════════════════════════════════════════════════════════
  // 构造 / 析构
  // ═══════════════════════════════════════════════════════════════════

  virtual ~IRenderingContextDriver() = default;

  // ═══════════════════════════════════════════════════════════════════
  // 生命周期
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 初始化渲染上下文驱动
   *
   * 执行一次性 API 初始化和设备枚举，不创建任何 Surface/Context。
   * 必须在任何 Surface 或 DeviceDriver 创建之前调用。
   *
   * @param[in] p_native_window 可选的原生窗口句柄（HWND）。
   *            OpenGL 实现创建临时 Surface 时需要有效的窗口 DC 来调用
   *            SetPixelFormat（桌面 DC 不支持此 API）。
   *            传入 nullptr 时回退到桌面 DC（可能失败）。
   *            Vulkan 实现忽略此参数。
   *
   * @return Error::kOK 初始化成功
   * @return Error::kFailed 初始化失败
   *
   * @pre IGLContextManager 已注入（通过构造函数或设置方法）
   * @pre IGLContextManager::Initialize() 已成功调用
   *
   * @par OpenGL 实现
   *   1. 通过 IGLContextManager 创建临时 Surface + Context
   *   2. MakeCurrent 绑定临时上下文
   *   3. gladLoadGL() 加载 OpenGL 函数指针
   *   4. QueryDeviceInfo() 查询 GPU 厂商、型号、设备类型
   *   5. 销毁临时 Surface + Context
   *
   * @par Vulkan 实现（未来）
   *   1. 创建 VkInstance
   *   2. 枚举 VkPhysicalDevice
   *   3. 查询设备属性和队列族
   *
   * @par 统一创建模式
   *   Initialize() 完成后，所有 Surface/Context 统一通过
   *   SurfaceCreate() / ContextCreate() 创建：
   *   @code
   *   rcd.Initialize(native_handle);
   *   auto surf = rcd.SurfaceCreate(screen_id, hwnd, w, h);
   *   auto ctx = rcd.ContextCreate(surf);
   *   rcd.MakeCurrent(ctx, surf);
   *   @endcode
   */
  virtual Error Initialize(void* p_native_window = nullptr) = 0;

  /**
   * @brief 关闭渲染上下文驱动
   *
   * 销毁所有 Surface 和驱动实例，释放 API 资源。
   * 调用后不应再使用任何 RCD 功能。
   *
   * @pre 已初始化
   * @post 所有 Surface 已销毁，所有 DeviceDriver 已释放
   *
   * @note 不负责销毁 IGLContextManager（由外部管理生命周期）
   */
  virtual void Shutdown() = 0;

  // ═══════════════════════════════════════════════════════════════════
  // 设备枚举
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 获取物理设备数量
   *
   * @return 枚举到的 GPU 设备数量
   *
   * @pre Initialize() 已成功调用
   *
   * @par OpenGL
   *   通常返回 1（OpenGL 只能看到当前上下文所在的 GPU）
   *
   * @par Vulkan
   *   可返回多个（Vulkan 支持枚举所有物理设备）
   */
  virtual uint32 GetDeviceCount() const = 0;

  /**
   * @brief 获取物理设备信息
   *
   * @param[in] p_device_index 设备索引（0 ~ GetDeviceCount()-1）
   *
   * @return 设备信息常引用
   *
   * @pre p_device_index < GetDeviceCount()
   */
  virtual const Device& GetDevice(uint32 p_device_index) const = 0;

  /**
   * @brief 查询设备是否支持在指定 Surface 上呈现
   *
   * @param[in] p_device_index 设备索引
   * @param[in] p_surface 渲染表面 ID
   *
   * @return true 设备支持在该 Surface 上呈现
   *
   * @par 占位说明（GL 后端下无独立语义）
   *   GL 的呈现能力随 wglMakeCurrent 隐式确定（绑定成功即可呈现），
   *   不存在"设备 × 表面"的呈现支持矩阵 —— GL 实现退化为有效性
   *   边界检查（恒真式占位）。本查询的真实主场是 Vulkan 设备选择
   *   （枚举后、选定前必查），GL 阶段保留为契约占位。
   *
   * @par OpenGL
   *   恒真占位（仅校验索引边界；"OpenGL 上下文已绑定特定设备"）
   *
   * @par Vulkan
   *   映射 vkGetPhysicalDeviceSurfaceSupportKHR：
   *   检查 VkPhysicalDevice 对 VkSurfaceKHR 的支持（跨适配器
   *   呈现需要中转 blit，设备选择算法依赖本查询决策）
   */
  virtual bool DeviceSupportsPresent(uint32 p_device_index,
                                     SurfaceID p_surface) const = 0;

  // ═══════════════════════════════════════════════════════════════════
  // 驱动工厂
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 创建渲染设备驱动
   *
   * 工厂方法，创建与当前 API 匹配的 IRenderingDeviceDriver 实例。
   * 调用者负责通过 DriverFree() 释放返回的驱动实例。
   *
   * @return 新创建的设备驱动指针
   * @retval nullptr 创建失败
   *
   * @pre Initialize() 已成功调用
   *
   * @par OpenGL 实现
   *   创建 GLDeviceDriver 实例，注入 IGLContextManager 引用。
   *   GL 命令通过 IGLContextManager::MakeCurrent() 确保在正确上下文执行。
   *
   * @par Vulkan 实现（未来）
   *   创建 VkDevice，选择队列族，加载设备级扩展。
   */
  virtual IRenderingDeviceDriver* CreateDeviceDriver() = 0;

  /**
   * @brief 释放渲染设备驱动
   *
   * @param[in] p_driver 要释放的设备驱动指针
   *
   * @pre p_driver 由 CreateDeviceDriver() 创建
   * @post p_driver 已销毁，指针失效
   */
  virtual void DriverFree(IRenderingDeviceDriver* p_driver) = 0;

  // ═══════════════════════════════════════════════════════════════════
  // Surface 管理
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 创建渲染表面
   *
   * 为指定屏幕创建可渲染的表面，内部调用 IGLContextManager::CreateSurface()
   * 创建渲染表面。
   *
   * @param[in] p_screen_id 屏幕 ID（由 IScreen 分配）
   * @param[in] p_native_window 原生窗口句柄（Win32: HWND）
   * @param[in] p_width 窗口客户区宽度（像素）
   * @param[in] p_height 窗口客户区高度（像素）
   *
   * @return 新创建的 Surface ID
   * @retval kInvalidSurfaceId 创建失败
   *
   * @pre Initialize() 已成功调用
   * @pre IGLContextManager 上下文已创建
   * @pre p_native_window 有效
   *
   * @par 实现细节
   *   1. 调用 IGLContextManager::CreateSurface(screen_id, hwnd, w, h)
   *   2. 记录 SurfaceInfo（gl_surface, size, needs_resize）
   *   3. 返回新分配的 SurfaceID
   *
   * @par 多屏场景
   *   @code
   *   // 为 HUD 屏幕创建渲染表面
   *   SurfaceID surf_hud = rcd->SurfaceCreate(screen_hud, hwnd_hud, 1920,
   * 1080);
   *   // 为仪表盘屏幕创建渲染表面
   *   SurfaceID surf_cluster = rcd->SurfaceCreate(screen_cluster, hwnd_cluster,
   * 1280, 720);
   *   @endcode
   */
  virtual SurfaceID SurfaceCreate(IScreen::ScreenID p_screen_id,
                                  void* p_native_window, uint32 p_width,
                                  uint32 p_height) = 0;

  /**
   * @brief 销毁渲染表面
   *
   * 释放 Surface 关联的 GL 屏幕上下文和资源。
   * 销毁前应确保该 Surface 不在任何线程上使用。
   *
   * @param[in] p_surface 要销毁的 Surface ID
   *
   * @pre p_surface 有效
   * @post Surface 已销毁，ID 失效
   */
  virtual void SurfaceDestroy(SurfaceID p_surface) = 0;

  /**
   * @brief 设置 Surface 尺寸
   *
   * 当窗口大小改变时调用，更新 Surface 的尺寸信息。
   * 设置 needs_resize 标志，渲染器在下一帧检查并调整视口。
   *
   * @param[in] p_surface Surface ID
   * @param[in] p_width 新宽度（像素）
   * @param[in] p_height 新高度（像素）
   */
  virtual void SurfaceSetSize(SurfaceID p_surface, uint32 p_width,
                              uint32 p_height) = 0;

  /**
   * @brief 设置 Surface 的 VSync 模式
   *
   * 更新指定渲染表面的垂直同步模式。内部委托给
   * IGLContextManager::SetVsyncMode() 完成实际切换。
   *
   * @param[in] p_surface 渲染表面 ID
   * @param[in] p_mode 目标 VSync 模式
   *
   * @pre p_surface 由 SurfaceCreate() 创建且未被 SurfaceDestroy() 销毁
   *
   * @par 实现细节
   *   - OpenGL: 内部调用 wglSwapIntervalEXT 设置交换间隔
   *   - 若 Surface 无效或已销毁，静默返回
   *
   * @par 线程安全
   *   可在任意线程调用
   *
   * @see VSyncMode
   * @see SurfaceGetVsyncMode
   */
  virtual void SurfaceSetVsyncMode(SurfaceID p_surface, VSyncMode p_mode) = 0;

  /**
   * @brief 获取 Surface 的 VSync 模式
   *
   * 查询指定渲染表面当前的垂直同步模式。
   *
   * @param[in] p_surface 渲染表面 ID
   *
   * @return 当前 VSync 模式
   * @retval VSyncMode::kEnabled Surface 不存在时返回的安全默认值
   *
   * @pre Initialize() 已成功调用
   *
   * @par 线程安全
   *   可在任意线程调用（只读操作）
   *
   * @see SurfaceSetVsyncMode
   */
  virtual VSyncMode SurfaceGetVsyncMode(SurfaceID p_surface) const = 0;

  /**
   * @brief 获取 Surface 宽度
   *
   * 返回渲染表面当前记录的宽度值（像素）。
   * 该值由 SurfaceCreate() 初始化，由 SurfaceSetSize() 更新。
   *
   * @param[in] p_surface 渲染表面 ID
   *
   * @return 宽度（像素）
   * @retval 0 Surface 无效或已销毁
   *
   * @par 线程安全
   *   可在任意线程调用（只读操作）
   *
   * @see SurfaceGetHeight
   * @see SurfaceSetSize
   */
  virtual uint32 SurfaceGetWidth(SurfaceID p_surface) const = 0;

  /**
   * @brief 获取 Surface 高度
   *
   * 返回渲染表面当前记录的高度值（像素）。
   * 该值由 SurfaceCreate() 初始化，由 SurfaceSetSize() 更新。
   *
   * @param[in] p_surface 渲染表面 ID
   *
   * @return 高度（像素）
   * @retval 0 Surface 无效或已销毁
   *
   * @par 线程安全
   *   可在任意线程调用（只读操作）
   *
   * @see SurfaceGetWidth
   * @see SurfaceSetSize
   */
  virtual uint32 SurfaceGetHeight(SurfaceID p_surface) const = 0;

  /**
   * @brief 设置 Surface 是否需要调整大小
   *
   * 标记渲染表面需要在下一帧重新创建交换链 / 调整视口。
   * 通常由窗口系统的 WM_SIZE 消息处理函数调用。
   *
   * @param[in] p_surface 渲染表面 ID
   * @param[in] p_needs_resize true 标记需要调整，false 清除标记
   *
   * @pre p_surface 由 SurfaceCreate() 创建且未被 SurfaceDestroy() 销毁
   *
   * @par 实现细节
   *   仅设置 SurfaceInfo::needs_resize 标志位，
   *   实际的视口调整由渲染器在下一帧渲染前检查并执行。
   *
   * @par 线程安全
   *   可在任意线程调用
   *
   * @see SurfaceGetNeedsResize
   * @see SurfaceSetSize
   */
  virtual void SurfaceSetNeedsResize(SurfaceID p_surface,
                                     bool p_needs_resize) = 0;

  /**
   * @brief 查询 Surface 是否需要调整大小
   *
   * 渲染器在每帧渲染前调用此方法检查是否需要调整视口 / 交换链。
   *
   * @param[in] p_surface 渲染表面 ID
   *
   * @return true 需要调整大小（渲染器应在处理后调用
   * SurfaceSetNeedsResize(false)）
   * @retval false 不需要调整，或 Surface 无效 / 已销毁
   *
   * @par 线程安全
   *   可在任意线程调用（只读操作）
   *
   * @see SurfaceSetNeedsResize
   */
  virtual bool SurfaceGetNeedsResize(SurfaceID p_surface) const = 0;

  /**
   * @brief 获取 Surface 的底层 API 表面句柄
   *
   * 返回与平台 / 图形 API 相关的表面标识符，供 RDD 层
   * 创建交换链（SwapChain）和执行上下文绑定时使用。
   *
   * @param[in] p_surface 渲染表面 ID
   *
   * @return 底层 API 表面句柄
   * @retval IGLContextManager::kInvalidSurfaceId Surface 无效或已销毁
   *
   * @pre Initialize() 已成功调用
   *
   * @par OpenGL
   *   返回 IGLContextManager 内部分配的 SurfaceID，
   *   对应一个已配置像素格式的 HDC + Pbuffer / HWND。
   *
   * @par Vulkan（未来）
   *   返回 VkSurfaceKHR 句柄。
   *
   * @par 线程安全
   *   可在任意线程调用（只读操作）
   */
  virtual IGLContextManager::SurfaceID SurfaceGetAPISurface(
      SurfaceID p_surface) const = 0;

  // ═══════════════════════════════════════════════════════════════════
  // Context 管理
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 创建渲染上下文
   *
   * 基于已有 Surface 的 HDC 创建独立的 GL 渲染上下文。
   * 上下文拥有独立的 GPU 资源空间，上下文间资源不共享。
   *
   * @par 当前用法（绘制/上屏分离 + drawable 切换）
   *   引擎级仅创建 **1 个** Context（离屏隐藏窗口承载像素格式，
   *   模式 R；或首个可见窗口，模式 S）。渲染线程独占该 context，
   *   通过 MakeCurrent(ctx, surfN) 逐屏切换 drawable——
   *   不需要多 context（模式 2 已否决，资源不共享）。
   *
   * @param[in] p_surface 已创建的渲染表面 ID（提供 HDC 和像素格式）
   *
   * @return 新创建的 Context ID
   * @retval kInvalidContextId 创建失败
   *
   * @pre Initialize() 已成功调用
   * @pre p_surface 对应的 Surface 已通过 SurfaceCreate() 创建
   *
   * @par OpenGL 实现
   *   内部调用 IGLContextManager::CreateContext() 创建 HGLRC。
   *   复用 Surface 的 HDC（已配置像素格式），
   *   避免跨线程 GetDC/ReleaseDC 操作。
   *   首次调用走两步法（临时上下文→加载扩展→正式上下文），
   *   后续直接 wglCreateContextAttribsARB。
   *
   * @par Vulkan 实现（未来）
   *   创建 VkDevice（每个 Context 对应一个逻辑设备）。
   */
  virtual ContextID ContextCreate(SurfaceID p_surface) = 0;

  /**
   * @brief 销毁渲染上下文
   *
   * 释放指定 GL 渲染上下文（HGLRC）及其关联的线程绑定状态。
   * 销毁后该 ContextID 失效，不应再被任何线程引用。
   *
   * @param[in] p_context_id 要销毁的 Context ID
   *
   * @pre p_context_id 由 ContextCreate() 创建且未被 ContextDestroy() 销毁
   * @pre 该上下文已从所有线程释放（已调用 ReleaseCurrent() 或线程已退出）
   * @post Context 已销毁，ID 失效
   *
   * @par OpenGL 实现
   *   内部调用 IGLContextManager::DestroyContext()，
   *   执行 wglDeleteContext(hglrc)。
   *   若上下文仍在某线程上为 current，行为未定义。
   *
   * @par 线程安全
   *   应在主线程调用（清理阶段）
   *
   * @see ContextCreate
   * @see ReleaseCurrent
   */
  virtual void ContextDestroy(ContextID p_context_id) = 0;

  /**
   * @brief 获取渲染上下文数量
   *
   * 返回当前由 IGLContextManager 管理的所有存活上下文数量，
   * 包括 Initialize() 时创建的引导上下文和 ContextCreate() 创建的额外上下文。
   *
   * @return 当前存活的上下文数量
   *
   * @par 线程安全
   *   可在任意线程调用（只读操作）
   */
  virtual uint32 GetContextCount() const = 0;

  // ═══════════════════════════════════════════════════════════════════
  // 访问器
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 查询是否已初始化
   *
   * 检查 Initialize() 是否已成功调用且尚未调用 Shutdown()。
   *
   * @return true 已初始化，可以创建 Surface 和 DeviceDriver
   * @retval false 未初始化或已关闭
   */
  virtual bool IsInitialized() const = 0;

  // ═══════════════════════════════════════════════════════════════════
  // 上下文绑定（多线程渲染支持）
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 将指定上下文绑定到指定表面的当前线程
   *
   * 使调用线程上的后续 GL 命令在指定上下文和表面上执行。
   * 多线程渲染时，每个渲染线程在开始渲染前必须调用此方法。
   *
   * @par Drawable 切换（Swap 呈现模式的核心用法）
   *   同一 ContextID 可依次绑定不同 SurfaceID——渲染线程逐屏
   *   MakeCurrent(ctx0, surfN) 即实现"单 Context 多 Drawable"：
   *   资源（纹理/VAO/FBO）全 context 共享，每屏只切输出窗口。
   *   前提由 IGLContextManager 保证：所有 Surface 像素格式
   *   索引一致（Win32 实现的 ConfigurePixelFormat 进程内缓存）。
   *
   * @param[in] p_context_id 要绑定的 Context ID
   * @param[in] p_surface 要绑定的 Surface ID
   *
   * @return Error::kOK 绑定成功
   * @return Error::kFailed 绑定失败（无效的 ID 或平台 API 错误）
   * @return Error::kInvalidParameter 参数无效
   *
   * @pre Initialize() 已成功调用
   * @pre p_context_id 由 ContextCreate() 创建
   * @pre p_surface 由 SurfaceCreate() 创建
   * @pre 同一上下文不能同时绑定到多个线程
   *
   * @par OpenGL 实现
   *   内部调用 IGLContextManager::MakeCurrent(gl_ctx, gl_surface)，
   *   最终执行 wglMakeCurrent(hdc, hglrc)。
   *   绑定后，该线程的所有 GL 调用都在此上下文上执行。
   *
   * @par 线程安全
   *   在需要绑定上下文的线程上调用（每个线程管理自己的绑定状态）
   *
   * @see ReleaseCurrent
   */
  virtual Error MakeCurrent(ContextID p_context_id, SurfaceID p_surface) = 0;

  /**
   * @brief 释放当前线程的 GL 上下文
   *
   * 解除调用线程当前绑定的 GL 上下文。
   * 释放后该线程不应执行任何 GL 命令，直到再次调用 MakeCurrent()。
   *
   * @pre Initialize() 已成功调用
   *
   * @post 调用线程不再持有任何 GL 上下文
   *
   * @par 典型用法
   *   @code
   *   // 线程退出前释放上下文
   *   rcd->ReleaseCurrent();
   *   // 上下文可在另一个线程重新绑定
   *   rcd->MakeCurrent(ctx_id, surface_id);
   *   @endcode
   *
   * @par OpenGL 实现
   *   内部调用 IGLContextManager::ReleaseCurrent()，
   *   最终执行 wglMakeCurrent(hdc, nullptr)。
   *
   * @par 线程安全
   *   在需要释放上下文的线程上调用
   *
   * @see MakeCurrent
   */
  virtual void ReleaseCurrent() = 0;
};

}  // namespace arhud

#endif  // ARHUD_DRIVERS_RENDERING_CONTEXT_DRIVER_H_
