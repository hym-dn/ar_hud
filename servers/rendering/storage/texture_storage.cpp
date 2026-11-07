/**
 * @file texture_storage.cpp
 * @brief TextureStorage implementation
 *
 * API 无关的实现，仅通过 RenderingDevice 公共接口操作纹理。
 * 无需 GL/Vulkan 子类。
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-24
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include "texture_storage.h"

#include "io/logger.h"
#include "os/memory.h"

namespace arhud {

/**
 * @brief 初始化纹理存储
 *
 * 校验 RenderingDevice 有效性后建立绑定。本方法不创建任何纹理，
 * 纹理由调用方通过 TextureCreate 系列方法显式创建。
 *
 * @param[in] p_rd RenderingDevice 指针（不拥有所有权）
 *
 * @retval Error::kOK            初始化成功，rd_ 绑定到 p_rd
 * @retval Error::kAlreadyExists 重复初始化（rd_ 已绑定）
 * @retval Error::kFailed        p_rd 为空或尚未初始化
 */
Error TextureStorage::Initialize(RenderingDevice* p_rd) {
  if (rd_ != nullptr) {
    return Error::kAlreadyExists;
  }

  if (p_rd == nullptr || !p_rd->IsInitialized()) {
    ARHUD_LOG_ERROR("kFailed",
                    "TextureStorage::Initialize() invalid RenderingDevice");
    return Error::kFailed;
  }

  rd_ = p_rd;
  return Error::kOK;
}

/**
 * @brief 结束纹理存储并释放所有资源
 *
 * 遍历全部 TextureInfo 条目，逐个委托 RD::TextureFree 延迟释放
 * GPU 端纹理，随后释放簿记内存并清空映射。
 *
 * @pre 必须在 RenderingDevice 结束之前调用（GPU 资源需经 RD 释放）
 *
 * @post texture_owner_ 已清空，rd_ == nullptr
 */
void TextureStorage::Finalize() {
  texture_owner_.ForEach([this](RID p_rid, TextureInfo* p_info) {
    rd_->TextureFree(p_info->rd_id);
    ARHUD_DELETE(p_info);
  });

  texture_owner_.Clear();
  rd_ = nullptr;
}

/**
 * @brief 创建空纹理
 *
 * 构造 TextureFormat（usage 预置 kSampling | kColorAttachment |
 * kCanUpdate，供采样、作渲染目标附件、后续 TextureUpdate 使用）
 * 与 TextureView（swizzle 保持默认 RGBA 直通），委托 RD 创建纹理，
 * 成功后登记 TextureInfo 簿记条目。不填充任何像素数据。
 *
 * @param[in] p_name      纹理名称（用于调试/日志）
 * @param[in] p_width     纹理宽度（像素）
 * @param[in] p_height    纹理高度（像素）
 * @param[in] p_format    数据格式
 * @param[in] p_mip_levels Mipmap 级数（默认 1）
 *
 * @return 纹理 RID；RD 创建失败时返回空 RID
 *
 * @pre Initialize() 已成功调用
 * @pre 必须在渲染线程上调用（ARHUD_ASSERT_ON_RENDER_THREAD 校验）
 */
RID TextureStorage::TextureAllocate() {
  // 任意线程：仅分配线程安全的 RID 槽位（空壳），不做 GPU 操作
  auto* info = ARHUD_NEW(TextureInfo);
  info->is_valid = false;
  RID rid = texture_owner_.MakeRid(info);
  info->rid = rid;
  return rid;
}

Error TextureStorage::TextureInitializeRid(RID p_texture, const char* p_name,
                                           uint32 p_width, uint32 p_height,
                                           DataFormat p_format,
                                           uint32 p_mip_levels) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  TextureInfo* info = texture_owner_.GetOrNull(p_texture);
  if (info == nullptr) {
    ARHUD_LOG_ERROR("kFailed",
                    "TextureStorage::TextureInitializeRid() invalid RID");
    return Error::kFailed;
  }

  TextureFormat tex_format;
  tex_format.width = p_width;
  tex_format.height = p_height;
  tex_format.format = p_format;
  tex_format.mipmaps = p_mip_levels;
  tex_format.usage.SetFlag(TextureUsageBits::kSampling);
  tex_format.usage.SetFlag(TextureUsageBits::kColorAttachment);
  tex_format.usage.SetFlag(TextureUsageBits::kCanUpdate);

  TextureView tex_view;
  tex_view.format = p_format;
  tex_view.swizzle_r = TextureSwizzle::kR;
  tex_view.swizzle_g = TextureSwizzle::kG;
  tex_view.swizzle_b = TextureSwizzle::kB;
  tex_view.swizzle_a = TextureSwizzle::kA;

  RDTextureID rd_id = rd_->TextureCreate(tex_format, tex_view);
  if (rd_id.IsNull()) {
    ARHUD_LOG_ERROR("kFailed",
                    "TextureStorage::TextureInitializeRid() RD texture "
                    "creation failed for '%s'",
                    p_name ? p_name : "(null)");
    return Error::kFailed;
  }

  info->name = UString(p_name);
  info->width = p_width;
  info->height = p_height;
  info->format = p_format;
  info->mip_levels = p_mip_levels;
  info->rd_id = rd_id;
  info->is_valid = true;
  return Error::kOK;
}

void TextureStorage::TextureInitializeFromData(
    RID p_texture, const TextureCreateParams& p_params, const uint8* p_blob) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  // blob 布局：[ level0 像素 data_size 字节 ][ 名称 name_size 字节（含 NUL）]
  const uint8* data = p_blob;
  const char* name = reinterpret_cast<const char*>(p_blob + p_params.data_size);

  if (TextureInitializeRid(p_texture, name, p_params.width, p_params.height,
                           p_params.format,
                           p_params.mip_levels) != Error::kOK) {
    TextureFree(p_texture);
    return;
  }

  if (p_params.data_size > 0) {
    Error err = TextureUpdate(p_texture, 0, 0, data, p_params.data_size);
    if (err != Error::kOK) {
      ARHUD_LOG_ERROR(
          "kFailed",
          "TextureStorage::TextureInitializeFromData() upload failed");
      TextureFree(p_texture);
      return;
    }
  }

  if (p_params.mip_levels > 1) {
    TextureGenerateMipmaps(p_texture);
  }
}

RID TextureStorage::TextureCreate(const char* p_name, uint32 p_width,
                                  uint32 p_height, DataFormat p_format,
                                  uint32 p_mip_levels) {
  RID rid = TextureAllocate();
  if (!rid.IsValid()) {
    return RID();
  }

  if (TextureInitializeRid(rid, p_name, p_width, p_height, p_format,
                           p_mip_levels) != Error::kOK) {
    TextureFree(rid);
    return RID();
  }

  return rid;
}

/**
 * @brief 从数据创建纹理（自动生成 mipmap）
 *
 * 流程：TextureCreate 建纹理 → TextureUpdate 上传 level 0 数据
 * （p_data 为空则跳过）→ p_mip_levels > 1 时 TextureGenerateMipmaps
 * 自动生成其余层级。任一步失败则回滚 TextureFree 并返回空 RID。
 *
 * @param[in] p_name      纹理名称（用于调试/日志）
 * @param[in] p_data      level 0 像素数据指针（可空）
 * @param[in] p_data_size level 0 数据大小（字节）
 * @param[in] p_width     纹理宽度（像素）
 * @param[in] p_height    纹理高度（像素）
 * @param[in] p_format    数据格式
 * @param[in] p_mip_levels Mipmap 级数（默认 1）
 *
 * @return 纹理 RID；创建/上传失败时返回空 RID
 *
 * @pre Initialize() 已成功调用
 * @pre 必须在渲染线程上调用（ARHUD_ASSERT_ON_RENDER_THREAD 校验）
 */
RID TextureStorage::TextureCreateFromData(const char* p_name,
                                          const uint8* p_data,
                                          uint32 p_data_size, uint32 p_width,
                                          uint32 p_height, DataFormat p_format,
                                          uint32 p_mip_levels) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  RID rid = TextureCreate(p_name, p_width, p_height, p_format, p_mip_levels);
  if (!rid.IsValid()) {
    return RID();
  }

  if (p_data != nullptr && p_data_size > 0) {
    Error err = TextureUpdate(rid, 0, 0, p_data, p_data_size);
    if (err != Error::kOK) {
      ARHUD_LOG_ERROR(
          "kFailed",
          "TextureStorage::TextureCreateFromData() upload failed for '%s'",
          p_name ? p_name : "(null)");
      TextureFree(rid);
      return RID();
    }
  }

  if (p_mip_levels > 1) {
    TextureGenerateMipmaps(rid);
  }

  return rid;
}

/**
 * @brief 从多级 mipmap 数据创建纹理（预计算 mipmap chain）
 *
 * 先校验参数合法性（p_mip_spans 非空且 p_mip_levels > 0），
 * TextureCreate 建纹理后逐级调用 TextureUpdate 上传各层数据，
 * 全程不调用 glGenerateMipmap（调用方负责提供完整 mipmap chain）。
 * 某一层的 data 为空或 size 为 0 时跳过该层；任一层上传失败则
 * 回滚 TextureFree 并返回空 RID。
 *
 * @param[in] p_name       纹理名称（用于调试/日志）
 * @param[in] p_mip_spans  各 mipmap 层级的数据跨度数组（长度 == p_mip_levels）
 * @param[in] p_width      纹理宽度（像素，level 0）
 * @param[in] p_height     纹理高度（像素，level 0）
 * @param[in] p_format     数据格式
 * @param[in] p_mip_levels Mipmap 级数
 *
 * @return 纹理 RID；参数非法或上传失败时返回空 RID
 *
 * @pre Initialize() 已成功调用
 * @pre 必须在渲染线程上调用（ARHUD_ASSERT_ON_RENDER_THREAD 校验）
 *
 * @note 适用压缩格式（BC/ETC）与对降采样质量敏感的图集
 *       （如 SDF 字体图集）——GPU 无法为压缩格式生成 mipmap。
 */
RID TextureStorage::TextureCreateFromMipmaps(const char* p_name,
                                             const MipmapDataSpan* p_mip_spans,
                                             uint32 p_width, uint32 p_height,
                                             DataFormat p_format,
                                             uint32 p_mip_levels) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  if (p_mip_spans == nullptr || p_mip_levels == 0) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "TextureStorage::TextureCreateFromMipmaps() invalid params for '%s'",
        p_name ? p_name : "(null)");
    return RID();
  }

  RID rid = TextureCreate(p_name, p_width, p_height, p_format, p_mip_levels);
  if (!rid.IsValid()) {
    return RID();
  }

  for (uint32 level = 0; level < p_mip_levels; ++level) {
    if (p_mip_spans[level].data == nullptr || p_mip_spans[level].size == 0) {
      continue;
    }

    Error err = TextureUpdate(rid, 0, level, p_mip_spans[level].data,
                              p_mip_spans[level].size);
    if (err != Error::kOK) {
      ARHUD_LOG_ERROR("kFailed",
                      "TextureStorage::TextureCreateFromMipmaps() upload "
                      "failed at level %u for '%s'",
                      level, p_name ? p_name : "(null)");
      TextureFree(rid);
      return RID();
    }
  }

  return rid;
}

/**
 * @brief 更新纹理数据
 *
 * 查簿记条目并校验 is_valid，委托 RD::TextureUpdate 经 Staging
 * Buffer 将 CPU 数据上传到 GPU 纹理。
 *
 * @param[in] p_texture   纹理 RID
 * @param[in] p_layer     目标数组层（默认 0）
 * @param[in] p_mipmap    目标 Mipmap 层级（默认 0）
 * @param[in] p_data      CPU 端数据指针
 * @param[in] p_data_size 数据大小（字节）
 *
 * @retval Error::kOK    更新成功
 * @retval Error::kFailed 纹理 RID 无效或纹理已失效
 *
 * @pre 必须在渲染线程上调用（ARHUD_ASSERT_ON_RENDER_THREAD 校验）
 */
Error TextureStorage::TextureUpdate(RID p_texture, uint32 p_layer,
                                    uint32 p_mipmap, const void* p_data,
                                    uint32 p_data_size) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  TextureInfo* info = texture_owner_.GetOrNull(p_texture);
  if (info == nullptr || !info->is_valid) {
    return Error::kFailed;
  }

  return rd_->TextureUpdate(info->rd_id, p_layer, p_mipmap, p_data,
                            p_data_size);
}

/**
 * @brief 生成 Mipmap 链
 *
 * 查簿记条目并校验 is_valid；纹理 mip_levels <= 1 时无需生成，
 * 直接返回 kOK（空操作）。否则委托 RD::TextureGenerateMipmaps
 * 请求 GPU 从基础层级自动生成完整 Mipmap 链。
 *
 * @param[in] p_texture 纹理 RID
 *
 * @retval Error::kOK    生成成功（或 mip_levels <= 1 无需生成）
 * @retval Error::kFailed 纹理 RID 无效或纹理已失效
 *
 * @pre 必须在渲染线程上调用（ARHUD_ASSERT_ON_RENDER_THREAD 校验）
 */
Error TextureStorage::TextureGenerateMipmaps(RID p_texture) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  TextureInfo* info = texture_owner_.GetOrNull(p_texture);
  if (info == nullptr || !info->is_valid) {
    return Error::kFailed;
  }

  if (info->mip_levels <= 1) {
    return Error::kOK;
  }

  return rd_->TextureGenerateMipmaps(info->rd_id);
}

/**
 * @brief 释放纹理资源
 *
 * 查簿记条目（无效 RID 静默返回，幂等），委托 RD::TextureFree
 * 延迟销毁 GPU 端纹理（Fence 背书，frame_count_ 帧后生效），
 * 随即释放簿记条目。
 *
 * @param[in] p_texture 纹理 RID
 *
 * @pre 必须在渲染线程上调用（ARHUD_ASSERT_ON_RENDER_THREAD 校验）
 *
 * @note 传入无效/空 RID 时安全返回（no-op）
 */
void TextureStorage::TextureFree(RID p_texture) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  TextureInfo* info = texture_owner_.GetOrNull(p_texture);
  if (info == nullptr) {
    return;
  }

  rd_->TextureFree(info->rd_id);
  texture_owner_.Free(p_texture);
  ARHUD_DELETE(info);
}

/**
 * @brief 获取完整的纹理信息结构（本地簿记查询）
 *
 * @param[in] p_texture 纹理 RID
 *
 * @return TextureInfo 指针（尺寸、格式、mipmap 级数、rd_id）；
 *         RID 无效时返回 nullptr
 */
const TextureInfo* TextureStorage::TextureGetInfo(RID p_texture) const {
  return texture_owner_.GetOrNull(p_texture);
}

/**
 * @brief 获取 RD 层的纹理 ID（本地簿记查询）
 *
 * 用于调用方直接访问 RD API（如 RenderPass 附件绑定）时的
 * RID → RDTextureID 转换。
 *
 * @param[in] p_texture 纹理 RID
 *
 * @return RDTextureID；RID 无效时返回空 ID
 */
RDTextureID TextureStorage::TextureGetRDId(RID p_texture) const {
  const TextureInfo* info = texture_owner_.GetOrNull(p_texture);
  if (info == nullptr) {
    return RDTextureID();
  }
  return info->rd_id;
}

/**
 * @brief 获取纹理宽度（本地簿记查询）
 *
 * @param[in] p_texture 纹理 RID
 *
 * @return 宽度（像素）；RID 无效时返回 0
 */
uint32 TextureStorage::TextureGetWidth(RID p_texture) const {
  const TextureInfo* info = texture_owner_.GetOrNull(p_texture);
  return info ? info->width : 0;
}

/**
 * @brief 获取纹理高度（本地簿记查询）
 *
 * @param[in] p_texture 纹理 RID
 *
 * @return 高度（像素）；RID 无效时返回 0
 */
uint32 TextureStorage::TextureGetHeight(RID p_texture) const {
  const TextureInfo* info = texture_owner_.GetOrNull(p_texture);
  return info ? info->height : 0;
}

/**
 * @brief 检查纹理是否有效（本地簿记查询）
 *
 * @param[in] p_texture 纹理 RID
 *
 * @return 纹理有效返回 true；RID 无效返回 false
 */
bool TextureStorage::TextureIsValid(RID p_texture) const {
  const TextureInfo* info = texture_owner_.GetOrNull(p_texture);
  return info ? info->is_valid : false;
}

}  // namespace arhud
