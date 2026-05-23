/**
 * @file rendering_context_driver.h
 * @brief 渲染上下文驱动抽象接口
 *
 * IRenderingContextDriver 是 GPU API 实例化和设备枚举的统一抽象层，
 * 位于渲染架构的最底层，负责管理 GPU 设备发现、渲染表面（Surface）
 * 生命周期和设备驱动（RenderingDeviceDriver）创建。
 *
 * @par 架构定位
 *   @code
 *   ┌─────────────────────────────────────────────────────┐
 *   │                  RenderingServer                     │  ← 业务层
 *   ├─────────────────────────────────────────────────────┤
 *   │                  RenderingDevice (RD)                │  ← 设备层
 *   ├─────────────────────────────────────────────────────┤
 *   │           RenderingDeviceDriver (RDD)                │  ← 驱动层
 *   ├─────────────────────────────────────────────────────┤
 *   │          IRenderingContextDriver (RCD)  ← 本文件     │  ← 上下文层
 *   │              └── IGLManager                          │  ← GL 上下文
 *   └─────────────────────────────────────────────────────┘
 *   @endcode
 *
 * @par 职责边界
 *   - **RCD 负责**：API 实例化、设备枚举、Surface 管理、Context 管理、驱动工厂
 *   - **IGLManager 负责**：GL 上下文创建/切换/VSync（RCD 内部持有，不对外暴露）
 *   - **RDD 负责**：GPU 资源操作 + SwapChain（MakeCurrent/SwapBuffers）
 *   - **RD 负责**：资源生命周期、Staging Buffer、命令图
 *
 * @par 多屏多线程模型
 *   RCD 采用 Context/Surface 分离模型，统一支持单线程多 Surface 和
 *   多线程独立上下文两种渲染模式：
 *
 *   @code
 *   模式 1：单线程 + 多 Surface（OpenGL 典型模式）
 *   IRenderingContextDriver (RCD)
 *   ├── Context 0 (HGLRC_0)
 *   │   ├── Surface 0 (HUD)      ──► IGLManager::SurfaceID=0 ──► 主线程串行渲染
 *   │   ├── Surface 1 (仪表盘)   ──► IGLManager::SurfaceID=1 ──► 主线程串行渲染
 *   │   └── Surface 2 (后视镜)   ──► IGLManager::SurfaceID=2 ──► 主线程串行渲染
 *   │       └── 共享 Context 0 通过 MakeCurrent(ctx0, surfN) 切换
 *
 *   模式 2：多线程 + 多 Context（OpenGL 多线程模式）
 *   IRenderingContextDriver (RCD)
 *   ├── Context 0 (HGLRC_0) ──► Surface 0 (HUD)      ──► Thread 0
 *   └── Context 1 (HGLRC_1) ──► Surface 1 (仪表盘)   ──► Thread 1
 *       └── 各上下文资源不共享，各线程独立创建 GPU 资源
 *   @endcode
 *
 * @par 生命周期
 *   1. 构造 RCD 实例（注入 IGLManager）
 *   2. Initialize() — 初始化 API、枚举设备
 *   3. SurfaceCreate() × N — 为每个屏幕创建渲染表面
 *   4. CreateDeviceDriver() — 创建 RDD 实例
 *   5. 运行时：SurfaceSetSize / SurfaceSetVsyncMode
 *   6. SurfaceDestroy() — 销毁渲染表面
 *   7. Shutdown() — 释放所有资源
 *
 * @par 设计参考
 *   - Godot 4.6 RenderingContextDriver（rendering_context_driver.h）
 *   - Vulkan VkInstance + VkPhysicalDevice 枚举模式
 *   - arch_skill.md §1.1 三层 GPU API 抽象
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-08
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#pragma once

#include "typedefs.h"
#include "gl_manager.h"
#include "window.h"
#include "template/local_vector.h"

namespace arhud
{

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
     * @par 与 IGLManager 的关系
     *   RCD 持有 IGLManager 的引用（不拥有所有权），通过 IGLManager
     *   管理 GL 上下文。IGLManager 不对外暴露，上层通过 RCD 的
     *   Context/Surface 管理接口间接使用 IGLManager。
     *   MakeCurrent/SwapBuffers 操作由 RDD SwapChain 负责。
     *
     * @par 线程安全
     *   - Initialize / Shutdown / SurfaceCreate / SurfaceDestroy
     *     应在主线程调用（初始化/清理阶段）
     *   - SurfaceSetSize / SurfaceSetVsyncMode 可在任意线程调用
     *   - CreateDeviceDriver 应在主线程调用
     *
     * @see IGLManager              GL 上下文管理（RCD 内部持有）
     * @see IRenderingDeviceDriver   设备驱动（由 RCD 创建）
     * @see SurfaceInfo              渲染表面信息
     */
    class IRenderingContextDriver
    {
    public:
        /** @brief 渲染表面 ID 类型 */
        using SurfaceID = uint32_t;

        /** @brief 无效表面 ID 常量 */
        static constexpr SurfaceID kInvalidSurfaceId = UINT32_MAX;

        /** @brief 渲染上下文 ID 类型 */
        using ContextID = uint32_t;

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
        struct Vendor
        {
            static constexpr uint32_t kUnknown = 0x0;
            static constexpr uint32_t kAMD = 0x1002;
            static constexpr uint32_t kImgTec = 0x1010;
            static constexpr uint32_t kApple = 0x106B;
            static constexpr uint32_t kNVIDIA = 0x10DE;
            static constexpr uint32_t kARM = 0x13B5;
            static constexpr uint32_t kMicrosoft = 0x1414;
            static constexpr uint32_t kQualcomm = 0x5143;
            static constexpr uint32_t kIntel = 0x8086;
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
        enum class DeviceType : uint32_t
        {
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
        struct Workarounds
        {
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
         *   uint32_t count = rcd->GetDeviceCount();
         *   for (uint32_t i = 0; i < count; ++i) {
         *       const auto& dev = rcd->GetDevice(i);
         *       // 选择独立 GPU 作为渲染设备
         *       if (dev.type == DeviceType::kDiscreteGpu) {
         *           selected = i;
         *           break;
         *       }
         *   }
         *   @endcode
         */
        struct Device
        {
            /** @brief 设备名称（如 "NVIDIA GeForce RTX 4060"） */
            const char *name = "Unknown";

            /** @brief GPU 厂商 ID（PCI Vendor ID） */
            uint32_t vendor = Vendor::kUnknown;

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
         * 只存储 RCD 渲染层的状态，平台层数据通过 IGLManager 查询。
         *
         * @par 职责划分
         *   - IGLManager（平台层）：HDC、HWND、VSync、owner_context_id
         *   - SurfaceInfo（渲染层）：尺寸、resize 标记、GL ID 映射
         */
        struct SurfaceInfo
        {
            /** @brief 关联的 GL 渲染表面 ID */
            IGLManager::SurfaceID gl_surface = IGLManager::kInvalidSurfaceId;

            /** @brief 表面宽度（像素） */
            uint32_t width = 0;

            /** @brief 表面高度（像素） */
            uint32_t height = 0;

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
         * 执行 API 实例化、设备枚举和驱动特性检测。
         * 必须在任何 Surface 或 DeviceDriver 创建之前调用。
         *
         * @param[in] p_screen_id      主屏幕 ID（由 IScreen 分配）
         * @param[in] p_native_window  原生窗口句柄（Win32: HWND）
         * @param[in] p_width          窗口客户区宽度（像素）
         * @param[in] p_height         窗口客户区高度（像素）
         *
         * @return Error::kOK 初始化成功
         * @return Error::kFailed 初始化失败
         *
         * @pre IGLManager 已注入（通过构造函数或设置方法）
         * @pre IGLManager::Initialize() 已成功调用
         *
         * @par OpenGL 实现
         *   1. 通过 IGLManager 创建引导 Surface 和 Context
         *   2. 绑定上下文以加载 GL 函数（gladLoadGL）
         *   3. 查询 GL_RENDERER / GL_VENDOR / GL_VERSION 字符串
         *   4. 填充 Device 信息（厂商 ID、设备类型）
         *   5. 检测驱动变通方案
         *
         * @par Vulkan 实现（未来）
         *   1. 创建 VkInstance
         *   2. 枚举 VkPhysicalDevice
         *   3. 查询设备属性和队列族
         */
        virtual Error Initialize(IScreen::ScreenID p_screen_id,
                                 void *p_native_window,
                                 uint32_t p_width, uint32_t p_height) = 0;

        /**
         * @brief 关闭渲染上下文驱动
         *
         * 销毁所有 Surface 和驱动实例，释放 API 资源。
         * 调用后不应再使用任何 RCD 功能。
         *
         * @pre 已初始化
         * @post 所有 Surface 已销毁，所有 DeviceDriver 已释放
         *
         * @note 不负责销毁 IGLManager（由外部管理生命周期）
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
        virtual uint32_t GetDeviceCount() const = 0;

        /**
         * @brief 获取物理设备信息
         *
         * @param[in] p_device_index 设备索引（0 ~ GetDeviceCount()-1）
         *
         * @return 设备信息常引用
         *
         * @pre p_device_index < GetDeviceCount()
         */
        virtual const Device &GetDevice(uint32_t p_device_index) const = 0;

        /**
         * @brief 查询设备是否支持在指定 Surface 上呈现
         *
         * @param[in] p_device_index 设备索引
         * @param[in] p_surface 渲染表面 ID
         *
         * @return true 设备支持在该 Surface 上呈现
         *
         * @par OpenGL
         *   始终返回 true（OpenGL 上下文已绑定到特定设备）
         *
         * @par Vulkan
         *   检查 VkPhysicalDevice 对 VkSurfaceKHR 的支持
         */
        virtual bool DeviceSupportsPresent(uint32_t p_device_index, SurfaceID p_surface) const = 0;

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
         *   创建 GLDeviceDriver 实例，注入 IGLManager 引用。
         *   GL 命令通过 IGLManager::MakeCurrent() 确保在正确上下文执行。
         *
         * @par Vulkan 实现（未来）
         *   创建 VkDevice，选择队列族，加载设备级扩展。
         */
        virtual IRenderingDeviceDriver *CreateDeviceDriver() = 0;

        /**
         * @brief 释放渲染设备驱动
         *
         * @param[in] p_driver 要释放的设备驱动指针
         *
         * @pre p_driver 由 CreateDeviceDriver() 创建
         * @post p_driver 已销毁，指针失效
         */
        virtual void DriverFree(IRenderingDeviceDriver *p_driver) = 0;

        // ═══════════════════════════════════════════════════════════════════
        // Surface 管理
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 创建渲染表面
         *
         * 为指定屏幕创建可渲染的表面，内部调用 IGLManager::CreateSurface()
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
         * @pre IGLManager 上下文已创建
         * @pre p_native_window 有效
         *
         * @par 实现细节
         *   1. 调用 IGLManager::CreateSurface(screen_id, hwnd, w, h)
         *   2. 记录 SurfaceInfo（gl_surface, size, needs_resize）
         *   3. 返回新分配的 SurfaceID
         *
         * @par 多屏场景
         *   @code
         *   // 为 HUD 屏幕创建渲染表面
         *   SurfaceID surf_hud = rcd->SurfaceCreate(screen_hud, hwnd_hud, 1920, 1080);
         *   // 为仪表盘屏幕创建渲染表面
         *   SurfaceID surf_cluster = rcd->SurfaceCreate(screen_cluster, hwnd_cluster, 1280, 720);
         *   @endcode
         */
        virtual SurfaceID SurfaceCreate(IScreen::ScreenID p_screen_id,
                                        void *p_native_window,
                                        uint32_t p_width, uint32_t p_height) = 0;

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
        virtual void SurfaceSetSize(SurfaceID p_surface, uint32_t p_width, uint32_t p_height) = 0;

        /**
         * @brief 设置 Surface 的 VSync 模式
         *
         * @param[in] p_surface Surface ID
         * @param[in] p_mode VSync 模式
         *
         * @see VSyncMode
         */
        virtual void SurfaceSetVsyncMode(SurfaceID p_surface, VSyncMode p_mode) = 0;

        /**
         * @brief 获取 Surface 的 VSync 模式
         *
         * @param[in] p_surface Surface ID
         * @return 当前 VSync 模式
         * @retval VSyncMode::kEnabled Surface 不存在（安全默认值）
         */
        virtual VSyncMode SurfaceGetVsyncMode(SurfaceID p_surface) const = 0;

        /**
         * @brief 获取 Surface 宽度
         *
         * @param[in] p_surface Surface ID
         * @return 宽度（像素）
         */
        virtual uint32_t SurfaceGetWidth(SurfaceID p_surface) const = 0;

        /**
         * @brief 获取 Surface 高度
         *
         * @param[in] p_surface Surface ID
         * @return 高度（像素）
         */
        virtual uint32_t SurfaceGetHeight(SurfaceID p_surface) const = 0;

        /**
         * @brief 设置 Surface 是否需要调整大小
         *
         * @param[in] p_surface Surface ID
         * @param[in] p_needs_resize true 表示需要调整
         */
        virtual void SurfaceSetNeedsResize(SurfaceID p_surface, bool p_needs_resize) = 0;

        /**
         * @brief 查询 Surface 是否需要调整大小
         *
         * @param[in] p_surface Surface ID
         * @return true 需要调整大小
         * @retval false 不需要或 Surface 无效
         */
        virtual bool SurfaceGetNeedsResize(SurfaceID p_surface) const = 0;

        /**
         * @brief 获取 Surface 的底层 API 表面句柄
         *
         * 返回与平台/API 相关的表面标识符，供驱动层使用。
         *
         * @param[in] p_surface Surface ID
         *
         * @return GL Surface ID（OpenGL）/ VkSurfaceKHR（Vulkan）
         * @retval IGLManager::kInvalidSurfaceId Surface 无效
         */
        virtual IGLManager::SurfaceID SurfaceGetAPISurface(SurfaceID p_surface) const = 0;

        // ═══════════════════════════════════════════════════════════════════
        // Context 管理
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 创建渲染上下文
         *
         * 基于已有 Surface 的 HDC 创建独立的 GL 渲染上下文，
         * 适用于多线程渲染场景。
         * 每个上下文拥有独立的 GPU 资源空间，上下文间资源不共享。
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
         *   内部调用 IGLManager::CreateContext() 创建 HGLRC。
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
         * 释放 GL 渲染上下文及其关联的资源。
         * 销毁前应确保该上下文不在任何线程上使用。
         *
         * @param[in] p_context_id 要销毁的 Context ID
         *
         * @pre p_context_id 有效
         * @post Context 已销毁，ID 失效
         */
        virtual void ContextDestroy(ContextID p_context_id) = 0;

        /**
         * @brief 获取渲染上下文数量
         *
         * @return 当前存活的上下文数量
         */
        virtual uint32_t GetContextCount() const = 0;

        // ═══════════════════════════════════════════════════════════════════
        // 访问器
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 查询是否已初始化
         *
         * @return true 已初始化
         */
        virtual bool IsInitialized() const = 0;
    };

} // namespace arhud
