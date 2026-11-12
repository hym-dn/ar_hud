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
  bool owns_color_texture = true;  ///< 颜色附件是否本结构拥有（外部导入=false）

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

    // 尺寸相关：两张附件纹理
    if (AllocAttachments(p_rd, p_width, p_height) != Error::kOK) {
      return Error::kFailed;
    }

    // 尺寸无关：RenderPass（附件只带格式/采样/加载存储/布局）
    render_pass = AllocRenderPass(p_rd);
    if (render_pass.IsNull()) {
      FreeOwnAttachments(p_rd);
      return Error::kFailed;
    }

    // 尺寸相关：Framebuffer（引用 render_pass + 当前附件纹理）
    if (AllocFramebuffer(p_rd, p_width, p_height) != Error::kOK) {
      p_rd->RenderPassFree(render_pass);
      render_pass = RDRenderPassID();
      FreeOwnAttachments(p_rd);
      return Error::kFailed;
    }

    return Error::kOK;
  }

  /**
   * @brief 以外部导入纹理为颜色附件创建渲染目标（Mode X 零拷贝上屏）
   *
   * @par 与 Create 的三处差别
   *   - 颜色附件由调用方导入（显存归平台帧池），本结构不拥有 →
   *     `owns_color_texture = false`，Destroy 不释放它；
   *   - 不建深度附件：本路径只承载后处理 composite 全屏 pass
   *     （depth test 关闭），无深度需求；
   *   - RenderPass 只有 1 个颜色附件，与 FBO 附件表一一对应。
   *
   * @param[in] p_rd             RenderingDevice 指针
   * @param[in] p_external_color 已导入的外部纹理 RID
   * @param[in] p_width          宽度（须与外部资源一致）
   * @param[in] p_height         高度（须与外部资源一致）
   * @param[in] p_color_format   颜色格式（须与外部资源一致）
   *
   * @retval Error::kOK     创建成功
   * @retval Error::kFailed 参数无效或资源创建失败
   */
  Error CreateExternal(RenderingDevice* p_rd, RDTextureID p_external_color,
                       uint32 p_width, uint32 p_height,
                       DataFormat p_color_format) {
    if (p_rd == nullptr || p_external_color.IsNull() || p_width == 0 ||
        p_height == 0) {
      return Error::kFailed;
    }

    width = p_width;
    height = p_height;
    color_format = p_color_format;
    color_texture = p_external_color;
    owns_color_texture = false;

    Attachment color_attachment;
    color_attachment.format = p_color_format;
    color_attachment.samples = TextureSamples::k1;
    color_attachment.load_op = AttachmentLoadOp::kClear;
    color_attachment.store_op = AttachmentStoreOp::kStore;
    color_attachment.initial_layout = TextureLayout::kColorAttachmentOptimal;
    color_attachment.final_layout = TextureLayout::kColorAttachmentOptimal;

    Subpass subpass;
    AttachmentReference color_ref;
    color_ref.attachment = 0;
    color_ref.layout = TextureLayout::kColorAttachmentOptimal;
    subpass.color_references.PushBack(color_ref);

    render_pass =
        p_rd->RenderPassCreate(VectorView<Attachment>(&color_attachment, 1),
                               VectorView<Subpass>(&subpass, 1),
                               VectorView<SubpassDependency>(nullptr, 0));
    if (render_pass.IsNull()) {
      return Error::kFailed;
    }

    framebuffer = p_rd->FramebufferCreate(
        render_pass, VectorView<RDTextureID>(&color_texture, 1), p_width,
        p_height);
    if (framebuffer.IsNull()) {
      p_rd->RenderPassFree(render_pass);
      render_pass = RDRenderPassID();
      color_texture = RDTextureID();
      return Error::kFailed;
    }

    return Error::kOK;
  }

  /**
   * @brief 重建渲染目标（窗口 resize 时调用）
   *
   * @par 外部导入形态不适用
   *   颜色附件由平台帧池拥有，resize 必须走"销毁导入 → 池 Resize →
   *   重新导入 → CreateExternal"（见 RenderViewport::ApplyPendingResize），
   *   直接 Recreate 会错误地自分配一张普通纹理。
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
    // 只重建**尺寸相关**对象：附件纹理 + Framebuffer。
    // RenderPass 与尺寸无关（附件只带格式/采样/加载存储/布局），必须保留：
    // Pipeline 经 RD 的 `DependencyAdd(render_pass, pipeline)` 依赖它，
    // 若在 resize 里销毁 render_pass，会触发 FreeResourceDependencies
    // **级联释放 composite pipeline**，resize 后 DrawListBindRenderPipeline
    // 立刻报 invalid pipeline RID（实测每屏每帧一次，resize 期间数千次）。
    if (render_pass.IsNull()) {
      // 防御：无既有 pass 可复用（异常/半初始化态）→ 退回完整重建
      return Create(p_rd, p_width, p_height, color_format);
    }

    // 先释放并重建尺寸相关对象；render_pass 原地保留
    FreeOwnAttachments(p_rd);
    if (AllocAttachments(p_rd, p_width, p_height) != Error::kOK) {
      width = 0;
      height = 0;
      return Error::kFailed;
    }
    // 旧 framebuffer 同样引用已失效的旧纹理，须随尺寸重建
    if (!framebuffer.IsNull()) {
      p_rd->FramebufferFree(framebuffer);
      framebuffer = RDFramebufferID();
    }
    if (AllocFramebuffer(p_rd, p_width, p_height) != Error::kOK) {
      FreeOwnAttachments(p_rd);
      width = 0;
      height = 0;
      return Error::kFailed;
    }

    width = p_width;
    height = p_height;
    return Error::kOK;
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
      if (owns_color_texture) {
        p_rd->TextureFree(color_texture);
      } else {
        // 外部导入：别名必须在平台侧归还该显存之前解除，
        // 不能用帧槽延迟释放（见 RD::TextureFreeImmediate）
        p_rd->TextureFreeImmediate(color_texture);
      }
      color_texture = RDTextureID();
    }
    width = 0;
    height = 0;
    owns_color_texture = true;
  }

  /**
   * @brief 检查渲染目标是否有效
   *
   * @return true 有效（外部导入形态不要求深度附件）
   */
  bool IsValid() const {
    if (color_texture.IsNull() || render_pass.IsNull() ||
        framebuffer.IsNull()) {
      return false;
    }
    return owns_color_texture ? !depth_texture.IsNull() : true;
  }

 private:
  // ── owned 路径（Create / Recreate）的构造原语 ──
  // 拆分目的：Create 全量构造、Recreate 只重建尺寸相关部分并复用
  // render_pass，两处共用同一份参数定义，避免尺寸相关逻辑漂移。

  /**
   * @brief 分配尺寸相关的两张附件纹理（颜色 + 深度/模板）
   *
   * @par 所有权
   *   置 `owns_color_texture = true`；失败时回滚已分配的颜色纹理。
   *
   * @param[in] p_rd    RenderingDevice 指针
   * @param[in] p_width 宽（像素）
   * @param[in] p_height 高（像素）
   *
   * @retval Error::kOK     两张纹理就绪
   * @retval Error::kFailed 任一纹理创建失败（已回滚）
   */
  Error AllocAttachments(RenderingDevice* p_rd, uint32 p_width,
                         uint32 p_height) {
    TextureFormat color_fmt;
    color_fmt.width = p_width;
    color_fmt.height = p_height;
    color_fmt.format = color_format;
    color_fmt.mipmaps = 1;
    color_fmt.usage.SetFlag(TextureUsageBits::kColorAttachment);
    color_fmt.usage.SetFlag(TextureUsageBits::kSampling);
    TextureView color_view;
    color_texture = p_rd->TextureCreate(color_fmt, color_view);
    if (color_texture.IsNull()) {
      return Error::kFailed;
    }

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

    owns_color_texture = true;
    return Error::kOK;
  }

  /**
   * @brief 分配 render_pass（**尺寸无关**：只由格式/采样/加载存储/布局定义）
   *
   * @param[in] p_rd RenderingDevice 指针
   * @return RenderPass RID；失败返回空 RID
   */
  RDRenderPassID AllocRenderPass(RenderingDevice* p_rd) {
    Attachment attachments[2];
    attachments[0].format = color_format;
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

    return p_rd->RenderPassCreate(VectorView<Attachment>(attachments, 2),
                                  VectorView<Subpass>(&subpass, 1),
                                  VectorView<SubpassDependency>(nullptr, 0));
  }

  /**
   * @brief 分配 framebuffer（尺寸相关，引用既有 render_pass + 当前附件纹理）
   *
   * @param[in] p_rd     RenderingDevice 指针
   * @param[in] p_width  宽（像素）
   * @param[in] p_height 高（像素）
   *
   * @retval Error::kOK     framebuffer 就绪
   * @retval Error::kFailed 创建失败
   */
  Error AllocFramebuffer(RenderingDevice* p_rd, uint32 p_width,
                         uint32 p_height) {
    RDTextureID fb_attachments[2] = {color_texture, depth_texture};
    framebuffer = p_rd->FramebufferCreate(
        render_pass, VectorView<RDTextureID>(fb_attachments, 2), p_width,
        p_height);
    return framebuffer.IsNull() ? Error::kFailed : Error::kOK;
  }

  /**
   * @brief 释放本结构拥有的附件纹理（owns_color_texture 才释放颜色）
   *
   * @param[in] p_rd RenderingDevice 指针
   */
  void FreeOwnAttachments(RenderingDevice* p_rd) {
    if (p_rd == nullptr) {
      return;
    }
    if (!depth_texture.IsNull()) {
      p_rd->TextureFree(depth_texture);
      depth_texture = RDTextureID();
    }
    if (!color_texture.IsNull() && owns_color_texture) {
      p_rd->TextureFree(color_texture);
      color_texture = RDTextureID();
    }
  }
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_RENDER_BUFFERS_H_
