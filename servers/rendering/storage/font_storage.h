/**
 * @file font_storage.h
 * @brief Font resource storage with SDF atlas support
 *
 * 参考 Godot 4.6 FontStorage / DynamicFont 设计，适配 AR HUD 的
 * SDF 字体渲染需求。Phase 3 实现预生成 SDF 图集加载，
 * FreeType 运行时 SDF 生成留到 Phase 4。
 *
 * @par 核心概念：
 *   - FontInfo：字体资源簿记，持有 SDF 图集纹理 RID + 字形度量
 *   - GlyphInfo：单个字形的 UV 矩形 + 推进量 + 偏移 + 尺寸
 *   - FontStorage：字体资源管理器，RID → FontInfo 映射
 *
 * @par SDF 图集格式：
 *   Phase 3 使用预生成的 SDF 纹理图集（单张纹理，多字形排列），
 *   每个字形在图集中的位置由 UV 坐标指定。
 *   图集纹理格式：R8Unorm（SDF 距离值存储在 R 通道）。
 *
 * @par 度量文件格式（JSON 简化版）：
 *   @code
 *   {
 *     "height": 32.0,
 *     "ascent": 24.0,
 *     "descent": -8.0,
 *     "glyphs": [
 *       {
 *         "charcode": 65,       // 'A'
 *         "uv_x": 0.0, "uv_y": 0.0, "uv_w": 0.1, "uv_h": 0.15,
 *         "advance": 18.0,
 *         "offset_x": 1.0, "offset_y": 0.0,
 *         "width": 16.0, "height": 24.0
 *       },
 *       ...
 *     ]
 *   }
 *   @endcode
 *
 * @par 数据分层：
 *   - FontStorage（本文件）：业务数据（字形度量、图集引用）
 *   - TextureStorage：SDF 图集纹理的 GPU 资源管理
 *   - RD Texture：API 无关的 GPU 元数据
 *   - RDD GLTextureInfo：API 特有数据
 *
 * @par 架构层次：
 *   FontStorage（本文件，字体资源存储）
 *     └── TextureStorage          ← SDF 图集纹理管理
 *           └── RenderingDevice   ← GPU 资源生命周期管理
 *
 * @par 线程安全约束（参考 Godot RenderingServer 线程模型）：
 *   | 方法 | GPU 操作 | 线程约束 |
 *   |------|---------|---------|
 *   | Initialize/Finalize | TextureStorage 操作 | 🔴 渲染线程 |
 *   | FontLoadFromSdfAtlas | TextureStorage::TextureCreateFromData | 🔴
 * 渲染线程 | | FontFree | TextureStorage::TextureFree | 🔴 渲染线程 | |
 * FontGetInfo/GetGlyph | 仅读 CPU 数据 | 🟢 任意线程 | | FontIsValid | 仅读 CPU
 * 数据 | 🟢 任意线程 | | FontGetTextureRID | 仅读 CPU 数据 | 🟢 任意线程 |
 *
 * @see TextureStorage
 * @see arch_skill.md §2.8 Compositor/Storage/Renderer
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-26
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_SERVERS_RENDERING_STORAGE_FONT_STORAGE_H_
#define ARHUD_SERVERS_RENDERING_STORAGE_FONT_STORAGE_H_

#include "io/logger.h"
#include "os/memory.h"
#include "rendering_device.h"
#include "string/ustring.h"
#include "template/hash_map.h"
#include "template/local_vector.h"

#if ARHUD_FREETYPE_ENABLED
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace arhud {

using UString = String;

class TextureStorage;

// ═══════════════════════════════════════════════════════════════════════
// GlyphInfo — 单个字形的度量数据
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 字形度量数据（参考 Godot GlyphInfo）
 *
 * 记录单个字形在 SDF 图集中的 UV 坐标和排版度量。
 * UV 坐标为归一化值 [0,1]，相对于图集纹理。
 *
 * @par 与 Godot 字形度量的对应关系：
 *   | Godot | ARHud | 说明 |
 *   |-------|-------|------|
 *   | Rect2 uv_rect | uv_x/y/w/h | 图集中的 UV 矩形 |
 *   | float advance | advance | 水平推进量 |
 *   | Vector2 offset | offset_x/y | 绘制偏移（相对基线） |
 *   | Size2 size | width/height | 字形像素尺寸 |
 *   | int charcode | charcode | Unicode 码点 |
 */
struct GlyphInfo {
  uint32 charcode = 0;  ///< Unicode 码点

  float uv_x = 0.0f;  ///< 图集 UV 左上角 X（归一化）
  float uv_y = 0.0f;  ///< 图集 UV 左上角 Y（归一化）
  float uv_w = 0.0f;  ///< 图集 UV 宽度（归一化）
  float uv_h = 0.0f;  ///< 图集 UV 高度（归一化）

  float advance = 0.0f;   ///< 水平推进量（像素）
  float offset_x = 0.0f;  ///< 绘制偏移 X（相对基线左侧）
  float offset_y = 0.0f;  ///< 绘制偏移 Y（相对基线向上）

  float width = 0.0f;   ///< 字形像素宽度
  float height = 0.0f;  ///< 字形像素高度
};

// ═══════════════════════════════════════════════════════════════════════
// FontInfo — 字体资源簿记
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 字体资源簿记（参考 Godot Font）
 *
 * 持有 SDF 图集纹理的 RID 和字形度量数据。
 * GPU 纹理资源由 TextureStorage 管理，FontInfo 仅持有 RID 引用。
 *
 * @par 与 Godot Font 的对应关系：
 *   | Godot | ARHud | 说明 |
 *   |-------|-------|------|
 *   | RID texture | texture_rid | SDF 图集纹理 |
 *   | float height | height | 字体高度（EM 方块） |
 *   | float ascent | ascent | 基线以上高度 |
 *   | float descent | descent | 基线以下深度（负值） |
 *   | HashMap<charcode, Glyph> glyphs | glyphs | 字形查找表 |
 */
struct FontInfo {
  RID rid;          ///< 由 FontStorage 分配的资源 ID
  RID texture_rid;  ///< SDF 图集纹理 RID（TextureStorage 管理）
  UString name;     ///< 字体名称（调试/日志）

  float height = 0.0f;   ///< 字体高度（EM 方块，像素）
  float ascent = 0.0f;   ///< 基线以上高度（像素）
  float descent = 0.0f;  ///< 基线以下深度（像素，负值）

  HashMap<uint32, GlyphInfo> glyphs;  ///< 字形查找表（charcode → GlyphInfo）

  bool is_valid = false;  ///< 字体是否有效
};

/**
 * @brief FontInitializeFromFreeType 的参数包（标量部分；名称/路径/码点走 blob
 * 通道）
 *
 * blob 布局：`[名称 name_size][路径 path_size][码点 charcode_count×4B]`。
 */
struct FontCreateParams {
  float font_size = 0.0f;     ///< 字体大小（像素）
  float sdf_spread = 8.0f;    ///< SDF 扩散距离（像素）
  uint32 charcode_count = 0;  ///< 码点数量
  uint32 name_size = 0;       ///< blob 段 1：名称字节数（含 NUL）
  uint32 path_size = 0;       ///< blob 段 2：路径字节数（含 NUL）
};

// ═══════════════════════════════════════════════════════════════════════
// FontStorage
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 字体资源存储（参考 Godot FontStorage）
 *
 * 管理 SDF 字体图集的加载、字形查询和资源释放。
 * 通过 TextureStorage 管理 SDF 图集纹理的 GPU 资源。
 *
 * @par 核心职责：
 *   1. **SDF 图集加载** — 从预生成图集 + 度量文件创建字体
 *   2. **字形查询** — 通过 charcode 查找字形的 UV 矩形和度量
 *   3. **资源释放** — 释放 FontInfo 并委托 TextureStorage 释放图集纹理
 *
 * @par Phase 3 限制：
 *   - 仅支持预生成 SDF 图集加载（不支持 FreeType 运行时生成）
 *   - 不支持动态图集扩展（字形数量固定）
 *   - 不支持多图集（单纹理）
 *
 * @par Phase 4 扩展计划：
 *   - FreeType 运行时 SDF 生成
 *   - 动态图集扩展（字形按需添加）
 *   - 多图集支持（字形溢出时自动创建新图集）
 *   - 字形缓存淘汰（LRU）
 *
 * @see TextureStorage
 * @see GlyphInfo
 * @see FontInfo
 */
class FontStorage {
 public:
  /**
   * @brief 初始化 FontStorage 系统
   *
   * 初始化流程：
   *   1. 保存 TextureStorage 指针（用于图集纹理管理）
   *   2. 若启用 FreeType，初始化 FT_Library
   * 实例（全局单例，供多次字体加载复用）
   *
   * @param[in] p_texture_storage TextureStorage 指针，用于管理 SDF 图集纹理
   *
   * @return Error::kOK            初始化成功
   * @return Error::kAlreadyExists 重复初始化
   * @return Error::kFailed        参数无效或 FreeType 初始化失败
   *
   * @pre TextureStorage 必须已初始化
   * @post ft_library_ 在 FreeType 启用时有效
   */
  Error Initialize(TextureStorage* p_texture_storage);

  /**
   * @brief 清理 FontStorage 系统
   *
   * 释放流程：
   *   1. 遍历并释放所有 FontInfo（委托 TextureStorage 释放图集纹理）
   *   2. 若启用 FreeType，释放 FT_Library 实例
   *   3. 清空 TextureStorage 指针
   */
  void Finalize();

  /**
   * @brief 从预生成 SDF 图集加载字体
   *
   * 加载 SDF 图集纹理和字形度量数据，创建 FontInfo。
   * 图集纹理通过 TextureStorage::TextureCreateFromData 创建。
   *
   * @param[in] p_name          字体名称
   * @param[in] p_atlas_data    SDF 图集纹理数据（R8 格式，每像素 1 字节）
   * @param[in] p_atlas_data_size 图集数据大小（字节）
   * @param[in] p_atlas_width   图集宽度（像素）
   * @param[in] p_atlas_height  图集高度（像素）
   * @param[in] p_height        字体高度（EM 方块，像素）
   * @param[in] p_ascent        基线以上高度（像素）
   * @param[in] p_descent       基线以下深度（像素，负值）
   * @param[in] p_glyphs        字形度量数组
   * @param[in] p_glyph_count   字形数量
   *
   * @return 字体 RID，失败时返回空 RID
   */
  RID FontLoadFromSdfAtlas(const char* p_name, const uint8* p_atlas_data,
                           uint32 p_atlas_data_size, uint32 p_atlas_width,
                           uint32 p_atlas_height, float p_height,
                           float p_ascent, float p_descent,
                           const GlyphInfo* p_glyphs, uint32 p_glyph_count);

  /**
   * @brief 从 FreeType 字体文件加载字体（运行时 SDF 生成）
   *
   * 内部流程：
   *   1. FreeType 打开 .ttf/.otf 文件
   *   2. 遍历 charcodes，加载字形轮廓
   *   3. FreeType 内置 SDF 渲染器生成 SDF 像素
   *   4. 打包到图集纹理
   *   5. 计算字形度量
   *   6. 调用 FontLoadFromSdfAtlas 上传图集
   *
   * @param[in] p_name          字体名称
   * @param[in] p_font_path     字体文件路径（.ttf/.otf）
   * @param[in] p_font_size     字体大小（像素）
   * @param[in] p_charcodes     需要渲染的字符码点数组
   * @param[in] p_charcode_count 字符数量
   * @param[in] p_sdf_spread    SDF 扩散距离（像素，默认 8）
   *
   * @return 字体 RID，失败时返回空 RID
   *
   * @pre ARHUD_FREETYPE_ENABLED=1（编译时启用 FreeType）
   */
  RID FontLoadFromFreeType(const char* p_name, const char* p_font_path,
                           float p_font_size, const uint32* p_charcodes,
                           uint32 p_charcode_count, float p_sdf_spread = 8.0f);

  /**
   * @brief RID 分裂分配（对齐 §7.11 F / Godot FUNCRIDSPLIT）
   *
   * **任意线程**：仅分配线程安全的空壳 FontInfo（is_valid=false），无 GPU
   * 操作。
   *
   * @return 空壳字体 RID
   */
  RID FontAllocate();

  /**
   * @brief RID 分裂初始化：渲染线程为已分配的空壳 RID 生成 SDF 图集并填充
   *
   * 失败时释放该 RID（FontFree）。名称/路径/码点经内联 blob 传递。
   *
   * @param[in] p_font   FontAllocate() 返回的空壳 RID
   * @param[in] p_params 字号 / SDF spread / 码点数 / 段大小
   * @param[in] p_blob   blob 基址，布局 `[名称][路径][码点]`
   *
   * @pre 必须在渲染线程上调用
   */
  void FontInitializeFromFreeType(RID p_font, const FontCreateParams& p_params,
                                  const uint8* p_blob);

  /**
   * @brief 释放字体资源
   *
   * 释放 FontInfo，并委托 TextureStorage 释放 SDF 图集纹理。
   *
   * @param[in] p_font 字体 RID
   */
  void FontFree(RID p_font);

  /**
   * @brief 获取字体信息
   *
   * @param[in] p_font 字体 RID
   * @return FontInfo 指针，RID 无效时返回 nullptr
   */
  const FontInfo* FontGetInfo(RID p_font) const;

  /**
   * @brief 获取指定码点的字形信息
   *
   * @param[in] p_font     字体 RID
   * @param[in] p_charcode Unicode 码点
   *
   * @return GlyphInfo 指针，字形不存在时返回 nullptr
   */
  const GlyphInfo* FontGetGlyph(RID p_font, uint32 p_charcode) const;

  /**
   * @brief 获取字体的 SDF 图集纹理 RID
   *
   * @param[in] p_font 字体 RID
   * @return 图集纹理 RID，RID 无效时返回空 RID
   */
  RID FontGetTextureRID(RID p_font) const;

  /**
   * @brief 检查字体是否有效
   *
   * @param[in] p_font 字体 RID
   * @return 字体有效返回 true，否则返回 false
   */
  bool FontIsValid(RID p_font) const;

 private:
  /**
   * @brief RID 分裂内部：FreeType SDF 生成（渲染线程），写入已分配的 RID
   * @note FontLoadFromFreeType（同步）与 FontInitializeFromFreeType（分裂）共用
   */
  Error FontBuildFromFreeType(RID p_font, const char* p_name,
                              const char* p_font_path, float p_font_size,
                              const uint32* p_charcodes,
                              uint32 p_charcode_count, float p_sdf_spread);

  /**
   * @brief RID 分裂内部：为已分配的 RID 上传 SDF 图集纹理并填充 FontInfo
   * @note 渲染线程；FontLoadFromSdfAtlas（同步）与 FontBuildFromFreeType 共用
   */
  Error FontInitializeFromSdfAtlas(RID p_font, const char* p_name,
                                   const uint8* p_atlas_data,
                                   uint32 p_atlas_data_size,
                                   uint32 p_atlas_width, uint32 p_atlas_height,
                                   float p_height, float p_ascent,
                                   float p_descent, const GlyphInfo* p_glyphs,
                                   uint32 p_glyph_count);

  TextureStorage* texture_storage_ =
      nullptr;                           ///< TextureStorage（不拥有所有权）
  RIDOwner<FontInfo, true> font_owner_;  ///< 线程安全的 RID → FontInfo 映射

#if ARHUD_FREETYPE_ENABLED
  FT_Library ft_library_ =
      nullptr;  ///< FreeType 库实例（Initialize 时创建，Finalize 时释放）
#endif
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_STORAGE_FONT_STORAGE_H_
