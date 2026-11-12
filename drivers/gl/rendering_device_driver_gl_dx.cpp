/**
 * @file rendering_device_driver_gl_dx.cpp
 * @brief GL 后端的跨 API 资源共享导入（Mode X 零拷贝上屏）。
 *
 * @par 职责边界
 *   本文件只做两件事：
 *   1. 与平台 interop（Win32: WGL_NV_DX_interop2）建立/关闭设备关联；
 *   2. 把显示侧显存别名成 GL 可写纹理，并在每帧写入前后取得/交还
 *      绘制权。
 *   帧缓冲的分配、呈现与归还节拍仍归 platform::ISharedFramePool ——
 *   本文件不碰 DXGI/GDI，也不持有任何显存的所有权。
 *
 * @par 为何不需要 GL 侧存储分配
 *   别名建立成功后，纹理对象的数据存储由 DX 资源提供（等价于不可变
 *   存储），故簿记 is_immutable=true 并禁止一切上传路径。FBO 附件与
 *   RenderPass 格式一致性判定沿用 texture_infos_ 里的 DataFormat，
 *   与自有纹理同路——上层无需分支。
 *
 * @par 同步契约（扩展规范原文要求）
 *   未加锁就绘制、占用中解别名，都是未定义行为。因此每帧严格配对：
 *   @code
 *   TextureExternalBeginWrite  // Acquire：显示侧 → GL
 *     ...本帧 pass 写入...
 *   TextureExternalEndWrite    // Release：GL → 显示侧（驱动保证写完可见）
 *   @endcode
 *   配对状态记在 GLTextureInfo::dx_held，EndWrite 只在一对一时下调。
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-11-08
 * @copyright Copyright (c) 2026 ARHud Project
 * @ingroup drivers
 */

#include "rendering_device_driver_gl.h"

#include <glad/gl.h>

#include "gpu_interop.h"
#include "io/logger.h"

namespace arhud {

/**
 * @brief 建立与帧池 GPU device 的互操作关联（幂等）
 *
 * @par 探测一次定终身
 *   入口点缺失 / OpenDevice 失败都记入 dx_interop_probed_/ready_，
 *   不反复重试：探测本身要求 current GL 上下文，反复失败会刷日志
 *   且每帧重付驱动成本。
 *
 * @par 换设备即失效
 *   已用 device 与本次不同（例如帧池重建）时先关旧再开新；旧关联下
 *   仍存活的别名会失效——调用方须在此前销毁全部外部纹理
 *   （RD::TextureFreeImmediate 的责任）。
 *
 * @param[in] p_dx_device 平台侧 GPU 设备（Win32: ID3D11Device*）
 *
 * @retval Error::kOK          可用
 * @retval Error::kUnavailable 平台未提供 interop 实现或打开失败
 */
Error GLRenderingDeviceDriver::DXInteropOpen(void* p_dx_device) {
  IGPUInterop* interop =
      gl_manager_ != nullptr ? gl_manager_->GetGPUInterop() : nullptr;
  if (interop == nullptr || p_dx_device == nullptr) {
    if (!dx_interop_probed_) {
      ARHUD_LOG_INFO(
          "GLDriver: no GPU interop provided by platform — shared-VRAM "
          "present path unavailable (readback fallback)");
    }
    dx_interop_probed_ = true;
    dx_interop_ready_ = false;
    return Error::kUnavailable;
  }

  if (dx_interop_ready_) {
    if (dx_interop_dx_device_ == p_dx_device) {
      return Error::kOK;
    }
    // 设备换了：旧关联必须显式关闭（其下别名由调用方先行解除）
    DXInteropClose();
  }

  dx_interop_probed_ = true;

  void* device_handle = interop->OpenDevice(p_dx_device);
  if (device_handle == nullptr) {
    // 常见原因：扩展缺失、驱动不支持 DX11 资源、device 与 GL context
    // 不在同一适配器（混合显卡）。属可回落的环境事实，不当错误。
    ARHUD_LOG_INFO(
        "GLDriver: interop OpenDevice failed — shared-VRAM present path "
        "unavailable on this adapter/driver (readback fallback)");
    dx_interop_ready_ = false;
    return Error::kUnavailable;
  }

  dx_interop_device_ = device_handle;
  dx_interop_dx_device_ = p_dx_device;
  dx_interop_ready_ = true;
  // 跨 API 共享的前提是"GL 与 D3D11 同 GPU"。混合显卡（Optimus）下
  // OpenDevice 可能成功但实际跨适配器，症状是 Present1 在 dxgi.dll 内
  // AV——故这里把 GL 侧 GPU 身份打出来，便于与帧池的 adapter 日志对比。
  const GLubyte* gl_renderer = glGetString(GL_RENDERER);
  const GLubyte* gl_vendor = glGetString(GL_VENDOR);
  ARHUD_LOG_INFO("GLDriver: shared-VRAM import ready via %s (GL_RENDERER=%s "
                 "GL_VENDOR=%s)",
                 interop->GetName(),
                 gl_renderer != nullptr ? reinterpret_cast<const char*>(
                                              gl_renderer)
                                        : "?",
                 gl_vendor != nullptr ? reinterpret_cast<const char*>(gl_vendor)
                                      : "?");
  return Error::kOK;
}

/** @brief 关闭互操作设备关联（幂等；须在 current GL 上下文时调用） */
void GLRenderingDeviceDriver::DXInteropClose() {
  if (dx_interop_device_ == nullptr) {
    return;
  }
  IGPUInterop* interop =
      gl_manager_ != nullptr ? gl_manager_->GetGPUInterop() : nullptr;
  if (interop != nullptr) {
    interop->CloseDevice(dx_interop_device_);
  }
  dx_interop_device_ = nullptr;
  dx_interop_dx_device_ = nullptr;
  dx_interop_ready_ = false;
}

/**
 * @brief 以显示侧显存创建 GL 纹理（不分配存储）
 *
 * @par 实现步骤
 *   1. DXInteropOpen（幂等，首次在此探测真实可用性）
 *   2. glGenTextures 取对象名——interop 要求"已存在的 GL 对象名"
 *   3. CreateAlias 把 DX 资源绑到该名字（写用途 = WRITE_DISCARD）
 *   4. 簿记进 texture_infos_：target/format/尺寸照抄描述，
 *      is_immutable=true（存储由 DX 拥有），dx_alias=别名句柄
 *
 * @par 失败处理
 *   别名建立失败即回滚 GL 名字并返回空 ID——不留半注册状态，
 *   否则后续 Acquire/Release 会对着无效句柄操作。
 *
 * @param[in] p_source 外部资源描述
 *
 * @return 纹理 ID（可直接作 FBO 颜色附件）
 * @retval TextureID() 不可用或导入失败
 */
TextureID GLRenderingDeviceDriver::TextureCreateExternal(
    const ExternalTextureSource& p_source) {
  if (!p_source.IsValid()) {
    ARHUD_LOG_WARN("GLDriver: TextureCreateExternal invalid source");
    return TextureID();
  }

  if (DXInteropOpen(p_source.native_device) != Error::kOK) {
    return TextureID();
  }

  IGPUInterop* interop = gl_manager_->GetGPUInterop();

  GLuint gl_texture = 0;
  glGenTextures(1, &gl_texture);
  if (gl_texture == 0) {
    ARHUD_LOG_ERROR("kFailed", "TextureCreateExternal: glGenTextures 0 (%ux%u)",
                    p_source.width, p_source.height);
    return TextureID();
  }

  void* alias = interop->CreateAlias(
      dx_interop_device_, p_source.native_resource,
      static_cast<uint32>(gl_texture), GL_TEXTURE_2D, p_source.writable);
  if (alias == nullptr) {
    glDeleteTextures(1, &gl_texture);
    ARHUD_LOG_WARN("GLDriver: interop alias failed (%ux%u) — 资源类型/用法不受支持",
                   p_source.width, p_source.height);
    return TextureID();
  }

  GLTextureInfo info;
  info.target = GL_TEXTURE_2D;
  info.format = p_source.format;
  info.width = p_source.width;
  info.height = p_source.height;
  info.depth = 1;
  info.mipmaps = 1;
  info.layers = 1;
  // 存储归 DX：禁止一切 gTexImage/glTexStorage 与上传路径触碰
  info.is_immutable = true;
  info.dx_alias = alias;
  info.dx_held = false;

  TextureID tex_id(static_cast<uint64>(gl_texture));
  texture_infos_.Insert(tex_id.GetId(), info);

  ARHUD_LOG_INFO("GLDriver: shared texture imported (%ux%u, gl_name=%u)",
                 p_source.width, p_source.height,
                 static_cast<uint32>(gl_texture));
  return tex_id;
}

/**
 * @brief 解除某个 GL 纹理名的 interop 别名（TextureFree 内前置调用）
 *
 * @param[in] p_gl_name GL 纹理名（非外部纹理静默返回）
 */
void GLRenderingDeviceDriver::DXInteropUnregisterTexture(uint32 p_gl_name) {
  GLTextureInfo* info = texture_infos_.GetPtr(static_cast<uint64>(p_gl_name));
  if (info == nullptr || info->dx_alias == nullptr) {
    return;
  }

  IGPUInterop* interop =
      gl_manager_ != nullptr ? gl_manager_->GetGPUInterop() : nullptr;
  if (interop != nullptr && dx_interop_device_ != nullptr) {
    if (info->dx_held) {
      // 占用中解别名是规范禁止的用法：先交还再解，并留痕
      //（调用顺序错误——见 RenderViewport 的 Begin/EndWrite 配对）
      ARHUD_LOG_WARN("GLDriver: releasing alias of a held shared texture "
                     "(gl_name=%u)",
                     p_gl_name);
      interop->ReleaseFromWriting(dx_interop_device_, info->dx_alias);
      info->dx_held = false;
    }
    interop->DestroyAlias(dx_interop_device_, info->dx_alias);
  }
  info->dx_alias = nullptr;
}

/**
 * @brief 取得绘制权（每帧写入本纹理前调用）
 *
 * @param[in] p_texture 外部纹理 ID
 * @return true 已取得；false 非外部纹理/未登记/取得失败
 */
bool GLRenderingDeviceDriver::TextureExternalBeginWrite(TextureID p_texture) {
  if (!p_texture.IsValid() || !dx_interop_ready_) {
    return false;
  }
  GLTextureInfo* info =
      texture_infos_.GetPtr(static_cast<uint64>(p_texture.GetId()));
  if (info == nullptr || info->dx_alias == nullptr) {
    return false;
  }
  if (info->dx_held) {
    // 上一帧未交还 = 配对错误，绝不能二次取得（驱动侧状态会错乱）
    ARHUD_LOG_WARN("GLDriver: shared texture already held (gl_name=%u)",
                   static_cast<uint32>(p_texture.GetId()));
    return false;
  }

  IGPUInterop* interop = gl_manager_->GetGPUInterop();
  if (!interop->AcquireForWriting(dx_interop_device_, info->dx_alias)) {
    return false;
  }
  info->dx_held = true;
  return true;
}

/**
 * @brief 交还绘制权（本帧写完后调用；驱动保证对端可见性）
 *
 * @param[in] p_texture 外部纹理 ID
 */
void GLRenderingDeviceDriver::TextureExternalEndWrite(TextureID p_texture) {
  if (!p_texture.IsValid()) {
    return;
  }
  GLTextureInfo* info =
      texture_infos_.GetPtr(static_cast<uint64>(p_texture.GetId()));
  if (info == nullptr || info->dx_alias == nullptr || !info->dx_held) {
    return;
  }

  IGPUInterop* interop =
      gl_manager_ != nullptr ? gl_manager_->GetGPUInterop() : nullptr;
  if (interop != nullptr) {
    interop->ReleaseFromWriting(dx_interop_device_, info->dx_alias);
  }
  info->dx_held = false;
}

}  // namespace arhud
