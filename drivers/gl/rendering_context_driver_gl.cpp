/**
 * @file rendering_context_driver_gl.cpp
 * @brief OpenGL 渲染上下文驱动实现
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-08
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include "rendering_context_driver_gl.h"
#include "rendering_device_driver_gl.h"
#include "io/logger.h"
#include "os/memory.h"
#include <glad/gl.h>

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // 构造 / 析构
    // ═══════════════════════════════════════════════════════════════════════

    GLRenderingContextDriver::GLRenderingContextDriver(IGLManager *p_gl_manager)
        : gl_manager_(p_gl_manager)
    {
    }

    GLRenderingContextDriver::~GLRenderingContextDriver()
    {
        if (initialized_)
        {
            Shutdown();
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // IRenderingContextDriver 接口实现 — 生命周期
    // ═══════════════════════════════════════════════════════════════════════

    Error GLRenderingContextDriver::Initialize(
        IScreen::ScreenID p_screen_id,
        void *p_native_window,
        uint32_t p_width, uint32_t p_height)
    {
        if (initialized_)
        {
            return Error::kOK;
        }

        if (!gl_manager_ || !gl_manager_->IsInitialized())
        {
            return Error::kUnconfigured;
        }

        if (!p_native_window)
        {
            ARHUD_LOG_ERROR("kInvalidParameter", "GLRenderingContextDriver: native_window is null");
            return Error::kInvalidParameter;
        }

        IGLManager::SurfaceID gl_surface = gl_manager_->CreateSurface(
            p_screen_id, p_native_window, p_width, p_height);
        if (gl_surface == IGLManager::kInvalidSurfaceId)
        {
            ARHUD_LOG_ERROR("kFailed", "GLRenderingContextDriver: Failed to create bootstrap surface");
            return Error::kFailed;
        }

        IGLManager::ContextID gl_context = gl_manager_->CreateContext(gl_surface);
        if (gl_context == IGLManager::kInvalidContextId)
        {
            ARHUD_LOG_ERROR("kFailed", "GLRenderingContextDriver: Failed to create bootstrap context");
            gl_manager_->DestroySurface(gl_surface);
            return Error::kFailed;
        }

        Error err = gl_manager_->MakeCurrent(gl_context, gl_surface);
        if (err != Error::kOK)
        {
            ARHUD_LOG_ERROR("kFailed", "GLRenderingContextDriver: Failed to make bootstrap context current");
            gl_manager_->DestroyContext(gl_context);
            gl_manager_->DestroySurface(gl_surface);
            return err;
        }

        SurfaceInfo info;
        info.gl_surface = gl_surface;
        info.width = p_width;
        info.height = p_height;
        info.needs_resize = false;
        surfaces_.PushBack(info);

        created_contexts_.PushBack(gl_context);

        int gl_version = gladLoadGL(reinterpret_cast<GLADloadfunc>(gl_manager_->GetGLProcAddress()));
        if (gl_version == 0)
        {
            ARHUD_LOG_ERROR("kFailed", "GLRenderingContextDriver: Failed to load GL functions via glad");
            return Error::kFailed;
        }

        QueryDeviceInfo();

        initialized_ = true;

        return Error::kOK;
    }

    void GLRenderingContextDriver::Shutdown()
    {
        if (!initialized_)
        {
            return;
        }

        for (uint32_t i = 0; i < created_contexts_.Size(); ++i)
        {
            gl_manager_->DestroyContext(created_contexts_[i]);
        }
        created_contexts_.Clear();

        for (uint32_t i = 0; i < surfaces_.Size(); ++i)
        {
            SurfaceInfo &info = surfaces_[i];
            if (info.gl_surface != IGLManager::kInvalidSurfaceId)
            {
                gl_manager_->DestroySurface(info.gl_surface);
            }
        }
        surfaces_.Clear();

        devices_.Clear();

        initialized_ = false;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // IRenderingContextDriver 接口实现 — 设备枚举
    // ═══════════════════════════════════════════════════════════════════════

    uint32_t GLRenderingContextDriver::GetDeviceCount() const
    {
        return static_cast<uint32_t>(devices_.Size());
    }

    const IRenderingContextDriver::Device &GLRenderingContextDriver::GetDevice(
        uint32_t p_device_index) const
    {
        return devices_[p_device_index];
    }

    bool GLRenderingContextDriver::DeviceSupportsPresent(
        uint32_t p_device_index, SurfaceID p_surface) const
    {
        // OpenGL 始终支持在当前上下文的 Surface 上呈现
        return p_device_index < devices_.Size() && p_surface < surfaces_.Size();
    }

    // ═══════════════════════════════════════════════════════════════════════
    // IRenderingContextDriver 接口实现 — 驱动工厂
    // ═══════════════════════════════════════════════════════════════════════

    IRenderingDeviceDriver *GLRenderingContextDriver::CreateDeviceDriver()
    {
        GLRenderingDeviceDriver *driver = ARHUD_NEW(GLRenderingDeviceDriver)(this, gl_manager_);
        return driver;
    }

    void GLRenderingContextDriver::DriverFree(IRenderingDeviceDriver *p_driver)
    {
        ARHUD_DELETE(p_driver);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // IRenderingContextDriver 接口实现 — Surface 管理
    // ═══════════════════════════════════════════════════════════════════════

    IRenderingContextDriver::SurfaceID GLRenderingContextDriver::SurfaceCreate(
        IScreen::ScreenID p_screen_id,
        void *p_native_window,
        uint32_t p_width, uint32_t p_height)
    {
        if (!initialized_)
        {
            return kInvalidSurfaceId;
        }

        if (!p_native_window)
        {
            return kInvalidSurfaceId;
        }

        // 通过 IGLManager 创建渲染表面
        IGLManager::SurfaceID gl_surf = gl_manager_->CreateSurface(
            p_screen_id, p_native_window, p_width, p_height);

        if (gl_surf == IGLManager::kInvalidSurfaceId)
        {
            return kInvalidSurfaceId;
        }

        // 记录 Surface 信息
        SurfaceInfo info;
        info.gl_surface = gl_surf;
        info.width = p_width;
        info.height = p_height;
        info.needs_resize = false;

        surfaces_.PushBack(info);

        // SurfaceID = 数组索引
        SurfaceID new_id = static_cast<SurfaceID>(surfaces_.Size() - 1);
        return new_id;
    }

    void GLRenderingContextDriver::SurfaceDestroy(SurfaceID p_surface)
    {
        SurfaceInfo *info = GetSurfaceInfo(p_surface);
        if (!info)
        {
            return;
        }

        // 销毁关联的 GL 渲染表面
        if (info->gl_surface != IGLManager::kInvalidSurfaceId)
        {
            gl_manager_->DestroySurface(info->gl_surface);
        }

        // 标记为无效（不删除数组元素，保持索引稳定）
        info->gl_surface = IGLManager::kInvalidSurfaceId;
        info->width = 0;
        info->height = 0;
    }

    void GLRenderingContextDriver::SurfaceSetSize(SurfaceID p_surface, uint32_t p_width, uint32_t p_height)
    {
        SurfaceInfo *info = GetSurfaceInfo(p_surface);
        if (!info)
        {
            return;
        }

        info->width = p_width;
        info->height = p_height;
        info->needs_resize = true;
    }

    void GLRenderingContextDriver::SurfaceSetVsyncMode(SurfaceID p_surface, VSyncMode p_mode)
    {
        const SurfaceInfo *info = GetSurfaceInfo(p_surface);
        if (!info || info->gl_surface == IGLManager::kInvalidSurfaceId)
        {
            return;
        }

        gl_manager_->SetVsyncMode(info->gl_surface, p_mode);
    }

    VSyncMode GLRenderingContextDriver::SurfaceGetVsyncMode(SurfaceID p_surface) const
    {
        const SurfaceInfo *info = GetSurfaceInfo(p_surface);
        if (!info || info->gl_surface == IGLManager::kInvalidSurfaceId)
        {
            return VSyncMode::kEnabled;
        }

        return gl_manager_->GetVsyncMode(info->gl_surface);
    }

    uint32_t GLRenderingContextDriver::SurfaceGetWidth(SurfaceID p_surface) const
    {
        const SurfaceInfo *info = GetSurfaceInfo(p_surface);
        if (!info)
        {
            return 0;
        }

        return info->width;
    }

    uint32_t GLRenderingContextDriver::SurfaceGetHeight(SurfaceID p_surface) const
    {
        const SurfaceInfo *info = GetSurfaceInfo(p_surface);
        if (!info)
        {
            return 0;
        }

        return info->height;
    }

    void GLRenderingContextDriver::SurfaceSetNeedsResize(SurfaceID p_surface, bool p_needs_resize)
    {
        SurfaceInfo *info = GetSurfaceInfo(p_surface);
        if (!info)
        {
            return;
        }

        info->needs_resize = p_needs_resize;
    }

    bool GLRenderingContextDriver::SurfaceGetNeedsResize(SurfaceID p_surface) const
    {
        const SurfaceInfo *info = GetSurfaceInfo(p_surface);
        if (!info)
        {
            return false;
        }

        return info->needs_resize;
    }

    /**
     * @brief 获取 Surface 的 API 层 Surface ID
     *
     * @param[in] p_surface Surface ID
     * @return GL Manager Surface ID，失败返回 kInvalidSurfaceId
     *
     * @par 用途
     *   用于与 GL Manager 交互，传递原生 Surface ID
     *
     * @par 线程安全
     *   - 线程安全（只读操作）
     */
    IGLManager::SurfaceID GLRenderingContextDriver::SurfaceGetAPISurface(SurfaceID p_surface) const
    {
        const SurfaceInfo *info = GetSurfaceInfo(p_surface);
        if (!info)
        {
            return IGLManager::kInvalidSurfaceId;
        }

        return info->gl_surface;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // IRenderingContextDriver 接口实现 — Context 管理
    // ═══════════════════════════════════════════════════════════════════════

    IRenderingContextDriver::ContextID GLRenderingContextDriver::ContextCreate(
        SurfaceID p_surface)
    {
        if (!initialized_)
        {
            return kInvalidContextId;
        }

        SurfaceInfo *info = GetSurfaceInfo(p_surface);
        if (!info)
        {
            return kInvalidContextId;
        }

        IGLManager::ContextID gl_ctx = gl_manager_->CreateContext(
            info->gl_surface);

        if (gl_ctx != IGLManager::kInvalidContextId)
        {
            created_contexts_.PushBack(gl_ctx);
        }

        return static_cast<ContextID>(gl_ctx);
    }

    void GLRenderingContextDriver::ContextDestroy(ContextID p_context_id)
    {
        if (p_context_id == kInvalidContextId)
        {
            return;
        }

        gl_manager_->DestroyContext(static_cast<IGLManager::ContextID>(p_context_id));
    }

    uint32_t GLRenderingContextDriver::GetContextCount() const
    {
        return gl_manager_->GetContextCount();
    }

    // ═══════════════════════════════════════════════════════════════════════
    // IRenderingContextDriver 接口实现 — 访问器
    // ═══════════════════════════════════════════════════════════════════════

    bool GLRenderingContextDriver::IsInitialized() const
    {
        return initialized_;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 内部方法
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 查询 GPU 设备信息
     *
     * 通过 GL 字符串查询填充 Device 结构。
     * 必须在 GL 上下文已绑定的线程上调用。
     *
     * @par 查询流程
     *   1. 查询 GL_VENDOR（厂商名称）
     *   2. 查询 GL_RENDERER（GPU 型号）
     *   3. 查询 GL_VERSION（GL 版本）
     *   4. 从厂商字符串推断 PCI 厂商 ID
     *   5. 从厂商 ID 推断设备类型
     *   6. 检查驱动变通方案
     *   7. 填充 Device 信息并添加到数组
     *
     * @par 线程安全
     *   - 应在 GL 上下文已绑定的线程调用
     *   - 不是线程安全的
     */
    void GLRenderingContextDriver::QueryDeviceInfo()
    {
        const char *vendor = reinterpret_cast<const char *>(glGetString(GL_VENDOR));
        if (vendor)
        {
            size_t len = 0;
            while (vendor[len] && len < sizeof(vendor_string_) - 1)
            {
                vendor_string_[len] = vendor[len];
                ++len;
            }
            vendor_string_[len] = '\0';
        }
        else
        {
            vendor_string_[0] = '\0';
        }

        const char *renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
        if (renderer)
        {
            size_t len = 0;
            while (renderer[len] && len < sizeof(renderer_string_) - 1)
            {
                renderer_string_[len] = renderer[len];
                ++len;
            }
            renderer_string_[len] = '\0';
        }
        else
        {
            renderer_string_[0] = '\0';
        }

        const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
        if (version)
        {
            size_t len = 0;
            while (version[len] && len < sizeof(version_string_) - 1)
            {
                version_string_[len] = version[len];
                ++len;
            }
            version_string_[len] = '\0';
        }
        else
        {
            version_string_[0] = '\0';
        }

        Device dev;
        dev.name = renderer_string_;
        dev.vendor = DetermineVendorId(vendor_string_);
        dev.type = DetermineDeviceType(dev.vendor);
        CheckWorkarounds(dev.vendor, renderer_string_, version_string_, dev.workarounds);
        devices_.PushBack(dev);
    }

    /**
     * @brief 从 GL 厂商字符串推断 PCI 厂商 ID
     *
     * @param p_vendor_string glGetString(GL_VENDOR) 返回的字符串
     * @return PCI 厂商 ID
     *
     * @par 厂商识别策略
     *   - "NVIDIA Corporation" → Vendor::kNVIDIA (0x10DE)
     *   - "Intel" → Vendor::kIntel (0x8086)
     *   - "AMD" / "ATI" → Vendor::kAMD (0x1002)
     *   - "ARM" → Vendor::kARM (0x13B5)
     *   - "Qualcomm" → Vendor::kQualcomm (0x5143)
     *   - "Apple" → Vendor::kApple (0x106B)
     *   - "Microsoft" → Vendor::kMicrosoft (0x1414)
     *   - "Imagination" / "IMG" → Vendor::kImgTec (0x1010)
     *
     * @par 线程安全
     *   - 可在任意线程调用
     *   - 线程安全（只读操作）
     */
    uint32_t GLRenderingContextDriver::DetermineVendorId(const char *p_vendor_string)
    {
        if (!p_vendor_string || p_vendor_string[0] == '\0')
        {
            return Vendor::kUnknown;
        }

        // NVIDIA
        if (strstr(p_vendor_string, "NVIDIA") != nullptr)
        {
            return Vendor::kNVIDIA;
        }

        // Intel
        if (strstr(p_vendor_string, "Intel") != nullptr)
        {
            return Vendor::kIntel;
        }

        // AMD / ATI
        if (strstr(p_vendor_string, "AMD") != nullptr ||
            strstr(p_vendor_string, "ATI") != nullptr ||
            strstr(p_vendor_string, "Advanced Micro Devices") != nullptr)
        {
            return Vendor::kAMD;
        }

        // ARM (Mali)
        if (strstr(p_vendor_string, "ARM") != nullptr)
        {
            return Vendor::kARM;
        }

        // Qualcomm (Adreno)
        if (strstr(p_vendor_string, "Qualcomm") != nullptr)
        {
            return Vendor::kQualcomm;
        }

        // Apple
        if (strstr(p_vendor_string, "Apple") != nullptr)
        {
            return Vendor::kApple;
        }

        // Microsoft (WARP / D3D12 software rasterizer)
        if (strstr(p_vendor_string, "Microsoft") != nullptr)
        {
            return Vendor::kMicrosoft;
        }

        // Imagination Technologies (PowerVR)
        if (strstr(p_vendor_string, "Imagination") != nullptr ||
            strstr(p_vendor_string, "IMG") != nullptr)
        {
            return Vendor::kImgTec;
        }

        return Vendor::kUnknown;
    }

    /**
     * @brief 从厂商 ID 推断设备类型
     *
     * @param p_vendor_id PCI 厂商 ID
     * @return 推断的设备类型
     *
     * @par 设备类型推断
     *   - NVIDIA → kDiscreteGpu（NVIDIA 桌面/移动 GPU 均为独立显卡）
     *   - Intel → kIntegratedGpu（Intel UHD/Iris 为集成显卡）
     *   - AMD → kDiscreteGpu（简化处理：AMD 桌面平台通常为独立 GPU）
     *   - ARM/Qualcomm → kIntegratedGpu（移动端 SoC GPU）
     *   - Microsoft → kCpu（WARP / D3D12 software rasterizer）
     *   - Apple → kIntegratedGpu（Apple Silicon 为集成显卡）
     *
     * @par 线程安全
     *   - 可在任意线程调用
     *   - 线程安全（只读操作）
     */
    IRenderingContextDriver::DeviceType GLRenderingContextDriver::DetermineDeviceType(
        uint32_t p_vendor_id)
    {
        switch (p_vendor_id)
        {
        case Vendor::kNVIDIA:
            return DeviceType::kDiscreteGpu;

        case Vendor::kIntel:
            return DeviceType::kIntegratedGpu;

        case Vendor::kAMD:
            // AMD 可能是集成或独立 GPU
            // 简化处理：AMD 桌面平台通常为独立 GPU
            return DeviceType::kDiscreteGpu;

        case Vendor::kARM:
        case Vendor::kQualcomm:
            return DeviceType::kIntegratedGpu;

        case Vendor::kMicrosoft:
            return DeviceType::kCpu;

        case Vendor::kApple:
            return DeviceType::kIntegratedGpu;

        default:
            return DeviceType::kOther;
        }
    }

    /**
     * @brief 检查驱动变通方案
     *
     * 根据厂商和驱动版本设置 Workarounds。
     *
     * @param p_vendor_id PCI 厂商 ID
     * @param p_renderer_string glGetString(GL_RENDERER) 返回的字符串
     * @param p_version_string glGetString(GL_VERSION) 返回的字符串
     * @param p_workarounds 填充的变通方案
     *
     * @par 已知驱动缺陷
     *   - AMD 驱动在某些版本中，绘制命令后立即执行计算命令可能导致 GPU 挂起
     *   - 设置 avoid_compute_after_draw = true
     *
     * @par 扩展方式
     *   其他厂商特定变通方案可在此扩展。
     *   可根据 p_renderer_string 和 p_version_string 进一步细化。
     *
     * @par 线程安全
     *   - 可在任意线程调用
     *   - 线程安全（只读操作）
     */
    void GLRenderingContextDriver::CheckWorkarounds(
        uint32_t p_vendor_id,
        const char *p_renderer_string,
        const char *p_version_string,
        Workarounds &p_workarounds)
    {
        // AMD 驱动在某些版本中，绘制命令后立即执行计算命令可能导致 GPU 挂起
        if (p_vendor_id == Vendor::kAMD)
        {
            p_workarounds.avoid_compute_after_draw = true;
        }

        // 其他厂商特定变通方案可在此扩展
        (void)p_renderer_string;
        (void)p_version_string;
    }

    /**
     * @brief 获取 Surface 信息指针
     *
     * @param p_surface Surface ID
     * @return SurfaceInfo 指针
     * @retval nullptr Surface 不存在或已销毁
     *
     * @par 验证逻辑
     *   1. 检查 Surface ID 是否在有效范围内
     *   2. 检查 Surface 是否已销毁（gl_surface == kInvalidSurfaceId）
     *
     * @par 线程安全
     *   - 可在任意线程调用
     *   - 线程安全（只读操作）
     */
    IRenderingContextDriver::SurfaceInfo *
    GLRenderingContextDriver::GetSurfaceInfo(SurfaceID p_surface)
    {
        if (p_surface >= surfaces_.Size())
        {
            return nullptr;
        }

        SurfaceInfo *info = &surfaces_[p_surface];

        // 已销毁的 Surface 返回 nullptr
        if (info->gl_surface == IGLManager::kInvalidSurfaceId)
        {
            return nullptr;
        }

        return info;
    }

    /**
     * @brief 获取 Surface 信息常量指针
     *
     * @param p_surface Surface ID
     * @return const SurfaceInfo 指针
     * @retval nullptr Surface 不存在或已销毁
     *
     * @par 验证逻辑
     *   1. 检查 Surface ID 是否在有效范围内
     *   2. 检查 Surface 是否已销毁（gl_surface == kInvalidSurfaceId）
     *
     * @par 线程安全
     *   - 可在任意线程调用
     *   - 线程安全（只读操作）
     */
    const IRenderingContextDriver::SurfaceInfo *
    GLRenderingContextDriver::GetSurfaceInfo(SurfaceID p_surface) const
    {
        if (p_surface >= surfaces_.Size())
        {
            return nullptr;
        }

        const SurfaceInfo *info = &surfaces_[p_surface];

        if (info->gl_surface == IGLManager::kInvalidSurfaceId)
        {
            return nullptr;
        }

        return info;
    }

} // namespace arhud