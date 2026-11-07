/**
 * @file render_buffers.h
 * @brief 渲染目标封装（Color + Depth + RenderPass + Framebuffer）
 *
 * RenderBuffers 将渲染目标的相关资源（颜色纹理、深度纹理、
 * RenderPass、Framebuffer）封装为一个整体，简化离屏渲染的
 * 资源管理。
 *
 * @par 设计参考
 *   - Godot 4.6 RenderBufferData（render_scene_buffers.h）
 *   - Godot 将 Color/Depth/RenderPass/FBO 打包为 RenderBufferData
 *
 * @par 典型使用
 *   @code
 *   RenderBuffers rb;
 *   rb.Create(rd, 1920, 720, DataFormat::kR8G8B8A8Unorm);
 *
 *   // 渲染到离屏目标
 *   DrawListID dl = rd->DrawListBegin(rb.framebuffer, true, true);
 *   // ... 绘制 ...
 *   rd->DrawListEnd();
 *
 *   // 使用颜色纹理（如后处理输入）
 *   RDTextureID color = rb.color_texture;
 *
 *   // 窗口 resize 时重建
 *   rb.Recreate(rd, new_width, new_height);
 *
 *   rb.Destroy(rd);
 *   @endcode
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-06-04
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_SERVERS_RENDERING_RENDER_BUFFERS_H_
#define ARHUD_SERVERS_RENDERING_RENDER_BUFFERS_H_

#include "rendering_device.h"

namespace arhud {

/**
 * @brief 渲染目标封装
 *
 * 将 Color Texture + Depth Texture + RenderPass + Framebuffer
 * 封装为一个整体，简化离屏渲染的资源创建和销毁。
 *
 * @par 与 Godot RenderBufferData 的对比
 *   | 特性 | Godot | ARHud |
 *   |------|-------|-------|
 *   | Color 附件 | GLuint | RDTextureID |
 *   | Depth 附件 | GLuint | RDTextureID |
 *   | FBO | GLuint | RDFramebufferID |
 *   | RenderPass | 无（GL 无此概念） | RDRenderPassID |
 *   | MSAA | 支持 | 预留 |
 *   | Resize | recreate | Recreate() |
 */
struct RenderBuffers {
  uint32 width = 0;                                      ///< 渲染目标宽度
  uint32 height = 0;                                     ///< 渲染目标高度
  DataFormat color_format = DataFormat::kR8G8B8A8Unorm;  ///< 颜色附件格式

  RDTextureID color_texture;    ///< 颜色附件纹理
  RDTextureID depth_texture;    ///< 深度/模板附件纹理
  RDRenderPassID render_pass;   ///< 关联的 RenderPass
  RDFramebufferID framebuffer;  ///< 关联的 Framebuffer

  /**
   * @brief 创建渲染目标资源
   *
   * 创建 Color Texture + Depth Texture + RenderPass + Framebuffer。
   *
   * @param[in] p_rd           RenderingDevice 指针
   * @param[in] p_width        宽度（像素）
   * @param[in] p_height       高度（像素）
   * @param[in] p_color_format 颜色附件格式（默认 RGBA8）
   *
   * @retval Error::kOK 创建成功
   * @retval Error::kFailed 创建失败
   */
  Error Create(RenderingDevice* p_rd, uint32 p_width, uint32 p_height,
               DataFormat p_color_format = DataFormat::kR8G8B8A8Unorm) {
    width = p_width;
    height = p_height;
    color_format = p_color_format;

    // 创建颜色附件纹理
    TextureFormat color_fmt;
    color_fmt.width = p_width;
    color_fmt.height = p_height;
    color_fmt.format = p_color_format;
    color_fmt.mipmaps = 1;
    color_fmt.usage.SetFlag(TextureUsageBits::kColorAttachment);
    color_fmt.usage.SetFlag(TextureUsageBits::kSampling);
    TextureView color_view;
    color_texture = p_rd->TextureCreate(color_fmt, color_view);
    if (color_texture.IsNull()) {
      return Error::kFailed;
    }

    // 创建深度/模板附件纹理
    TextureFormat depth_fmt;
    depth_fmt.width = p_width;
    depth_fmt.height = p_height;
    depth_fmt.format = DataFormat::kD24UnormS8Uint;
    depth_fmt.mipmaps = 1;
    depth_fmt.usage.SetFlag(TextureUsageBits::kDepthStencilAttachment);
    TextureView depth_view;
    depth_texture = p_rd->TextureCreate(depth_fmt, depth_view);
    if (depth_texture.IsNull()) {
      p_rd->TextureFree(color_texture);
      color_texture = RDTextureID();
      return Error::kFailed;
    }

    // 创建 RenderPass
    Attachment attachments[2];
    attachments[0].format = p_color_format;
    attachments[0].samples = TextureSamples::k1;
    attachments[0].load_op = AttachmentLoadOp::kClear;
    attachments[0].store_op = AttachmentStoreOp::kStore;
    attachments[0].stencil_load_op = AttachmentLoadOp::kDontCare;
    attachments[0].stencil_store_op = AttachmentStoreOp::kDontCare;
    attachments[0].initial_layout = TextureLayout::kColorAttachmentOptimal;
    attachments[0].final_layout = TextureLayout::kColorAttachmentOptimal;

    attachments[1].format = DataFormat::kD24UnormS8Uint;
    attachments[1].samples = TextureSamples::k1;
    attachments[1].load_op = AttachmentLoadOp::kClear;
    attachments[1].store_op = AttachmentStoreOp::kStore;
    attachments[1].stencil_load_op = AttachmentLoadOp::kDontCare;
    attachments[1].stencil_store_op = AttachmentStoreOp::kDontCare;
    attachments[1].initial_layout =
        TextureLayout::kDepthStencilAttachmentOptimal;
    attachments[1].final_layout = TextureLayout::kDepthStencilAttachmentOptimal;

    Subpass subpass;
    AttachmentReference color_ref;
    color_ref.attachment = 0;
    color_ref.layout = TextureLayout::kColorAttachmentOptimal;
    subpass.color_references.PushBack(color_ref);

    AttachmentReference depth_ref;
    depth_ref.attachment = 1;
    depth_ref.layout = TextureLayout::kDepthStencilAttachmentOptimal;
    subpass.depth_stencil_reference = depth_ref;

    render_pass =
        p_rd->RenderPassCreate(VectorView<Attachment>(attachments, 2),
                               VectorView<Subpass>(&subpass, 1),
                               VectorView<SubpassDependency>(nullptr, 0));
    if (render_pass.IsNull()) {
      p_rd->TextureFree(depth_texture);
      p_rd->TextureFree(color_texture);
      depth_texture = RDTextureID();
      color_texture = RDTextureID();
      return Error::kFailed;
    }

    // 创建 Framebuffer
    RDTextureID fb_attachments[2] = {color_texture, depth_texture};
    framebuffer = p_rd->FramebufferCreate(
        render_pass, VectorView<RDTextureID>(fb_attachments, 2), p_width,
        p_height);
    if (framebuffer.IsNull()) {
      p_rd->RenderPassFree(render_pass);
      p_rd->TextureFree(depth_texture);
      p_rd->TextureFree(color_texture);
      render_pass = RDRenderPassID();
      depth_texture = RDTextureID();
      color_texture = RDTextureID();
      return Error::kFailed;
    }

    return Error::kOK;
  }

  /**
   * @brief 重建渲染目标（窗口 resize 时调用）
   *
   * 销毁旧资源并创建新尺寸的资源。
   *
   * @param[in] p_rd     RenderingDevice 指针
   * @param[in] p_width  新宽度
   * @param[in] p_height 新高度
   *
   * @retval Error::kOK 重建成功
   * @retval Error::kFailed 重建失败
   */
  Error Recreate(RenderingDevice* p_rd, uint32 p_width, uint32 p_height) {
    Destroy(p_rd);
    return Create(p_rd, p_width, p_height, color_format);
  }

  /**
   * @brief 销毁渲染目标资源
   *
   * @param[in] p_rd RenderingDevice 指针
   */
  void Destroy(RenderingDevice* p_rd) {
    if (!framebuffer.IsNull()) {
      p_rd->FramebufferFree(framebuffer);
      framebuffer = RDFramebufferID();
    }
    if (!render_pass.IsNull()) {
      p_rd->RenderPassFree(render_pass);
      render_pass = RDRenderPassID();
    }
    if (!depth_texture.IsNull()) {
      p_rd->TextureFree(depth_texture);
      depth_texture = RDTextureID();
    }
    if (!color_texture.IsNull()) {
      p_rd->TextureFree(color_texture);
      color_texture = RDTextureID();
    }
    width = 0;
    height = 0;
  }

  /**
   * @brief 检查渲染目标是否有效
   * @return true 有效
   */
  bool IsValid() const {
    return !color_texture.IsNull() && !depth_texture.IsNull() &&
           !render_pass.IsNull() && !framebuffer.IsNull();
  }
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_RENDER_BUFFERS_H_
