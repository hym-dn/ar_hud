/**
 * @file texture_storage.h
 * @brief Texture resource storage with metadata and lifecycle management
 *
 * TextureStorage sits above RenderingDevice (RD) and manages texture resources
 * with metadata like dimensions, format, and mipmap information.
 *
 * @par 设计原则：
 *   1. 在 RD 之上提供 Texture 资源的元数据管理
 *   2. 通过 RD 的公共 API 创建/更新/释放纹理
 *   3. 不持有 GPU 资源所有权，GPU 端资源由 RD 管理
 *   4. API 无关，仅调 RD 方法，无需 GL/Vulkan 子类
 *
 * @par 数据分层：
 *   - TextureStorage（本文件）：业务数据（尺寸、格式、mipmap 级数）
 *   - RD Texture：API 无关的 GPU 元数据（TextureFormat、driver_id）
 *   - RDD GLTextureInfo：API 特有数据（GL target、format 枚举）
 *
 * @par 架构层次：
 *   TextureStorage（本文件，纹理资源存储）
 *     └── RenderingDevice          ← GPU 资源生命周期管理
 *           └── RenderingDeviceDriver   ← 驱动层接口
 *                 └── RenderingContextDriver ← 上下文层接口
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-24
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_SERVERS_RENDERING_STORAGE_TEXTURE_STORAGE_H_
#define ARHUD_SERVERS_RENDERING_STORAGE_TEXTURE_STORAGE_H_

#include "io/logger.h"
#include "os/memory.h"
#include "rendering_device.h"
#include "string/ustring.h"
#include "template/hash_map.h"
#include "template/local_vector.h"

namespace arhud {

using UString = String;

// ═══════════════════════════════════════════════════════════════════════
// Texture 资源簿记
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 纹理资源簿记，仅存业务数据
 *
 * GPU 元数据（driver_id、TextureFormat）由 RD 层的 Texture 结构持有，
 * 本结构仅存储业务层数据：尺寸、格式、mipmap 级数等。
 *
 * @par 数据分层：
 *   - TextureStorage（本结构）：业务数据（尺寸、格式、mipmap）
 *   - RD Texture：API 无关的 GPU 元数据（TextureFormat、driver_id）
 *   - RDD GLTextureInfo：API 特有数据（GL target、format 枚举）
 */
struct TextureInfo {
  RID rid;            ///< 由 TextureStorage 分配的资源 ID
  RDTextureID rd_id;  ///< RD 层的纹理 ID
  UString name;       ///< 纹理名称（用于调试/日志）
  uint32 width = 0;   ///< 纹理宽度（像素）
  uint32 height = 0;  ///< 纹理高度（像素）
  DataFormat format = DataFormat::kR8G8B8A8Unorm;  ///< 数据格式
  uint32 mip_levels = 1;                           ///< Mipmap 级数
  bool is_valid = false;                           ///< 纹理是否有效
};

// ═══════════════════════════════════════════════════════════════════════
// TextureCreateParams — 创建参数包（H1：复杂创建用 param struct）
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 从数据创建纹理的参数包（对齐 §7.11 H1）
 *
 * 配合 RID 分裂（`TextureAllocate` + `TextureInitializeFromData`）使用：
 * 标量字段随命令内联，变长字节走 blob 通道，布局固定为
 * `[ level0 像素 data_size 字节 ][ 名称 name_size 字节（含 NUL）]`。
 */
struct TextureCreateParams {
  uint32 width = 0;                                ///< 纹理宽度（像素）
  uint32 height = 0;                               ///< 纹理高度（像素）
  DataFormat format = DataFormat::kR8G8B8A8Unorm;  ///< 数据格式
  uint32 mip_levels = 1;                           ///< Mipmap 级数
  uint32 data_size = 0;  ///< blob 段 1：level 0 像素字节数（可 0）
  uint32 name_size = 0;  ///< blob 段 2：名称字节数（含 NUL）
};

// ═══════════════════════════════════════════════════════════════════════
// TextureStorage
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 纹理资源存储，管理纹理元数据和生命周期
 *
 * 在 RenderingDevice 层之上管理纹理的业务数据，
 * 通过 RD 的公共 API 创建/更新/释放纹理。
 *
 * @par 核心职责：
 *   1. **资源创建** — 创建空纹理或从数据创建纹理
 *   2. **数据更新** — 通过 RD 的 Staging Buffer 更新纹理数据
 *   3. **Mipmap 生成** — 请求 GPU 生成 Mipmap 链
 *   4. **资源查询** — 通过 RID 提供 O(1) 的 TextureInfo 查找
 *   5. **资源释放** — 释放 TextureInfo 并委托 RD 释放 GPU 端资源
 *
 * @par 典型使用流程：
 *   @code
 *   TextureStorage storage;
 *   storage.Initialize(rd);
 *
 *   // 创建空纹理
 *   RID tex = storage.TextureCreate("hud_bg", 1920, 720,
 *                                    DataFormat::kR8G8B8A8Unorm);
 *
 *   // 上传数据
 *   storage.TextureUpdate(tex, 0, 0, pixels, sizeof(pixels));
 *
 *   // 生成 Mipmap
 *   storage.TextureGenerateMipmaps(tex);
 *
 *   // 释放
 *   storage.TextureFree(tex);
 *   storage.Finalize();
 *   @endcode
 *
 * @par 线程安全：
 *   - RIDOwner<TextureInfo, true> 提供线程安全的 RID → 对象映射
 *     （防御性加锁，为未来跨线程查询预留）
 *   - Initialize/Finalize 应在渲染线程调用
 *   - 创建/释放/更新（TextureCreate 系列 / TextureUpdate / TextureFree）
 *     必须在渲染线程调用（GL 上下文亲和，
 *     内部 ARHUD_ASSERT_ON_RENDER_THREAD 校验）
 *   - 查询方法（TextureGet*）为 const 只读，可在任意线程调用；
 *     返回的引用/指针生命周期由渲染线程的创建/释放节奏保证，
 *     不应跨帧长期持有
 *
 * @see RenderingDevice
 * @see TextureInfo
 */
class TextureStorage {
 public:
  /**
   * @brief 初始化纹理存储
   *
   * @param[in] p_rd RenderingDevice 指针（不拥有所有权，调用者管理生命周期）
   *
   * @retval Error::kOK 初始化成功
   * @retval Error::kAlreadyExists 已经初始化
   * @retval Error::kFailed 无效的 RenderingDevice
   *
   * @pre p_rd 已初始化（IsInitialized() == true）
   */
  Error Initialize(RenderingDevice* p_rd);

  /**
   * @brief 结束纹理存储并释放所有资源
   *
   * 释放所有 TextureInfo 条目并委托 RenderingDevice 释放 GPU 资源。
   * 必须在 RenderingDevice 结束之前调用。
   *
   * @pre 所有外部纹理引用已不再使用
   */
  void Finalize();

  /**
   * @brief 创建空纹理
   *
   * 创建指定尺寸和格式的空纹理，不填充数据。
   * 可用于 RenderTarget 附件或后续 TextureUpdate 填充。
   *
   * @param[in] p_name      纹理名称（用于调试/日志）
   * @param[in] p_width     纹理宽度（像素）
   * @param[in] p_height    纹理高度（像素）
   * @param[in] p_format    数据格式
   * @param[in] p_mip_levels Mipmap 级数（默认 1）
   *
   * @return 纹理 RID，失败时返回空 RID
   *
   * @pre Initialize() 已成功调用
   */
  RID TextureCreate(const char* p_name, uint32 p_width, uint32 p_height,
                    DataFormat p_format, uint32 p_mip_levels = 1);

  /**
   * @brief 从数据创建纹理（自动生成 mipmap）
   *
   * 创建纹理并上传 level 0 数据。若 p_mip_levels > 1，
   * 自动调用 glGenerateMipmap 生成其余层级。
   *
   * @par 适用场景
   *   运行时动态纹理（HUD 图标、动态渲染结果等），
   *   对 mipmap 质量无特殊要求，接受 GPU 双线性降采样。
   *
   * @param[in] p_name      纹理名称（用于调试/日志）
   * @param[in] p_data      level 0 像素数据指针
   * @param[in] p_data_size level 0 数据大小（字节）
   * @param[in] p_width     纹理宽度（像素）
   * @param[in] p_height    纹理高度（像素）
   * @param[in] p_format    数据格式
   * @param[in] p_mip_levels Mipmap 级数（默认 1）
   *
   * @return 纹理 RID，失败时返回空 RID
   *
   * @pre Initialize() 已成功调用
   */
  RID TextureCreateFromData(const char* p_name, const uint8* p_data,
                            uint32 p_data_size, uint32 p_width, uint32 p_height,
                            DataFormat p_format, uint32 p_mip_levels = 1);

  /**
   * @brief 从多级 mipmap 数据创建纹理（预计算 mipmap chain）
   *
   * 创建纹理并逐级上传 mipmap 数据，不调用 glGenerateMipmap。
   * 调用者需提供完整的 mipmap chain 数据。
   *
   * @par 适用场景
   *   - 离线烘焙的 mipmap chain（Kaiser 滤波、SDF 字体图集）
   *   - 对 mipmap 质量有严格要求，不接受 GPU 自动降采样
   *   - 压缩格式纹理（BC/ETC），GPU 无法自动生成 mipmap
   *
   * @param[in] p_name       纹理名称（用于调试/日志）
   * @param[in] p_mip_spans  各 mipmap 层级的数据跨度数组
   *                         数组长度必须等于 p_mip_levels
   *                         p_mip_spans[0] = level 0 数据
   *                         p_mip_spans[1] = level 1 数据 ...
   * @param[in] p_width      纹理宽度（像素，level 0）
   * @param[in] p_height     纹理高度（像素，level 0）
   * @param[in] p_format     数据格式
   * @param[in] p_mip_levels Mipmap 级数
   *
   * @return 纹理 RID，失败时返回空 RID
   *
   * @pre Initialize() 已成功调用
   * @pre p_mip_spans 数组长度 == p_mip_levels
   */
  RID TextureCreateFromMipmaps(const char* p_name,
                               const MipmapDataSpan* p_mip_spans,
                               uint32 p_width, uint32 p_height,
                               DataFormat p_format, uint32 p_mip_levels);

  /**
   * @brief RID 分裂分配（对齐 §7.11 F / Godot FUNCRIDSPLIT）
   *
   * **任意线程**可调用：仅从线程安全的 RIDOwner 分配一个空壳条目
   * （`is_valid=false`、`rd_id` 为空），不做任何 GPU 操作，立即返回。
   * 必须与 TextureInitializeFromData() 配对使用。
   *
   * @return 空壳纹理 RID
   */
  RID TextureAllocate();

  /**
   * @brief RID 分裂初始化：渲染线程为已分配的空壳 RID 建 GPU 纹理并上传 level 0
   *
   * 对齐 Godot `texture_2d_initialize`。失败时释放该 RID（TextureFree）。
   *
   * @param[in] p_texture TextureAllocate() 返回的空壳 RID
   * @param[in] p_params  创建参数（含 blob 两段大小）
   * @param[in] p_blob    内联 blob 基址，布局 `[像素][名称]`（blob
   * 载荷，执行期有效）
   *
   * @pre 必须在渲染线程上调用
   */
  void TextureInitializeFromData(RID p_texture,
                                 const TextureCreateParams& p_params,
                                 const uint8* p_blob);

  /**
   * @brief 更新纹理数据
   *
   * 通过 RD 的 Staging Buffer 将 CPU 数据上传到 GPU 纹理。
   *
   * @param[in] p_texture   纹理 RID
   * @param[in] p_layer     目标数组层（默认 0）
   * @param[in] p_mipmap    目标 Mipmap 层级（默认 0）
   * @param[in] p_data      CPU 端数据指针
   * @param[in] p_data_size 数据大小（字节）
   *
   * @retval Error::kOK 更新成功
   * @retval Error::kFailed 更新失败
   */
  Error TextureUpdate(RID p_texture, uint32 p_layer, uint32 p_mipmap,
                      const void* p_data, uint32 p_data_size);

  /**
   * @brief 生成 Mipmap 链
   *
   * 请求 GPU 从基础层级自动生成完整 Mipmap 链。
   * 纹理创建时必须指定 mip_levels > 1。
   *
   * @param[in] p_texture 纹理 RID
   *
   * @retval Error::kOK 生成成功
   * @retval Error::kFailed 生成失败
   */
  Error TextureGenerateMipmaps(RID p_texture);

  /**
   * @brief 释放纹理资源
   *
   * 释放 TextureInfo 条目并委托 RenderingDevice 释放 GPU 资源
   * （延迟销毁）。
   *
   * @param[in] p_texture 纹理 RID
   */
  void TextureFree(RID p_texture);

  /**
   * @brief 获取完整的纹理信息结构
   *
   * @param[in] p_texture 纹理 RID
   *
   * @return TextureInfo 指针，RID 无效时返回 nullptr
   */
  const TextureInfo* TextureGetInfo(RID p_texture) const;

  /**
   * @brief 获取 RD 层的纹理 ID
   *
   * @param[in] p_texture 纹理 RID
   *
   * @return RDTextureID，RID 无效时返回空 ID
   */
  RDTextureID TextureGetRDId(RID p_texture) const;

  /**
   * @brief 获取纹理宽度
   *
   * @param[in] p_texture 纹理 RID
   *
   * @return 宽度（像素），RID 无效时返回 0
   */
  uint32 TextureGetWidth(RID p_texture) const;

  /**
   * @brief 获取纹理高度
   *
   * @param[in] p_texture 纹理 RID
   *
   * @return 高度（像素），RID 无效时返回 0
   */
  uint32 TextureGetHeight(RID p_texture) const;

  /**
   * @brief 检查纹理是否有效
   *
   * @param[in] p_texture 纹理 RID
   *
   * @return 纹理有效返回 true，否则返回 false
   */
  bool TextureIsValid(RID p_texture) const;

  /**
   * @brief 获取 RenderingDevice 指针
   *
   * @return RenderingDevice 指针，未初始化时返回 nullptr
   */
  RenderingDevice* GetRD() const {
    return rd_;
  }

 private:
  /**
   * @brief RID 分裂内部：为已分配的 RID 创建 GPU 纹理并填充元数据
   *
   * TextureCreate()（同步路径）与 TextureInitializeFromData()（分裂路径）共用。
   *
   * @pre 必须在渲染线程上调用
   *
   * @return Error::kOK 成功；Error::kFailed RID 无效或 RD 创建失败
   */
  Error TextureInitializeRid(RID p_texture, const char* p_name, uint32 p_width,
                             uint32 p_height, DataFormat p_format,
                             uint32 p_mip_levels);

  RenderingDevice* rd_ = nullptr;  ///< RenderingDevice（不拥有所有权）
  RIDOwner<TextureInfo, true>
      texture_owner_;  ///< 线程安全的 RID → TextureInfo 映射
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_STORAGE_TEXTURE_STORAGE_H_
