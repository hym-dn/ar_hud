/**
 * @file font_storage.cpp
 * @brief FontStorage 字体资源存储实现
 *
 * 本文件实现了 FontStorage 类的全部功能，涵盖两个字体加载路径：
 *   - FontLoadFromSdfAtlas：从预生成的 SDF 图集 + 字形度量数据创建字体
 *   - FontLoadFromFreeType：通过 FreeType 库在运行时加载 .ttf/.otf 字体文件，
 *     利用 FreeType 2.10+ 内置 SDF 渲染器生成 SDF 像素数据，自动打包到图集纹理
 *
 * @par 参考 Godot 4.6 Font / DynamicFont：
 *   - font_load_from_sdf_atlas 对应 Godot 的预生成图集加载路径
 *   - font_load_from_free_type 对应 Godot 的 DynamicFont 运行时字形渲染
 *   - 字形度量数据结构 GlyphInfo 对应 Godot 的 GlyphInfo
 *
 * @par 数据流：
 *   FontLoadFromFreeType（本文件）
 *     → FreeType 逐字形渲染
 *       → 行打包算法计算图集布局
 *         → 渲染到临时像素缓冲区
 *           → FontLoadFromSdfAtlas（本文件）
 *             → TextureStorage::TextureCreateFromData
 *               → RenderingDevice 创建 GPU 纹理
 *
 * @par 线程安全约束：
 *   - 渲染线程：FontLoadFromSdfAtlas、FontLoadFromFreeType、FontFree（涉及 GPU
 * 操作）
 *   - 任意线程：FontGetInfo、FontGetGlyph、FontGetTextureRID、FontIsValid（纯
 * CPU 读取）
 *
 * @see font_storage.h
 * @see texture_storage.h
 * @see arch_skill.md §2.8 Compositor/Storage/Renderer
 */

#include "font_storage.h"

#include "texture_storage.h"

#if ARHUD_FREETYPE_ENABLED
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#endif

namespace arhud {

// ═══════════════════════════════════════════════════════════════════════
// Initialize / Finalize
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 初始化 FontStorage 系统
 *
 * 持有 TextureStorage 指针用于 SDF 图集纹理的 GPU 资源管理。
 * FontStorage 不拥有 TextureStorage，仅持有非拥有指针。
 *
 * 若编译时启用 FreeType（ARHUD_FREETYPE_ENABLED=1），同时初始化
 * FT_Library 实例。该实例在 FontStorage 生命周期内复用，避免每次
 * FontLoadFromFreeType 调用时重复初始化/释放 FreeType 库。
 *
 * @param[in] p_texture_storage  TextureStorage 实例指针，不能为 nullptr
 *
 * @return Error::kOK            初始化成功
 * @return Error::kAlreadyExists 重复初始化
 * @return Error::kFailed        p_texture_storage 为 nullptr 或 FreeType
 * 初始化失败
 *
 * @pre 全局 TextureStorage 必须已初始化
 * @post texture_storage_ 指向传入的 TextureStorage 实例
 * @post ft_library_ 在 FreeType 启用时有效
 */
Error FontStorage::Initialize(TextureStorage* p_texture_storage) {
  if (texture_storage_ != nullptr) {
    ARHUD_LOG_ERROR("kAlreadyExists",
                    "FontStorage::Initialize() already initialized");
    return Error::kAlreadyExists;
  }

  if (p_texture_storage == nullptr) {
    ARHUD_LOG_ERROR("kFailed", "FontStorage::Initialize() invalid parameters");
    return Error::kFailed;
  }

  texture_storage_ = p_texture_storage;

#if ARHUD_FREETYPE_ENABLED
  FT_Error ft_error = FT_Init_FreeType(&ft_library_);
  if (ft_error != 0) {
    ARHUD_LOG_ERROR("kFailed",
                    "FontStorage::Initialize() FT_Init_FreeType failed: %d",
                    ft_error);
    texture_storage_ = nullptr;
    return Error::kFailed;
  }
#endif

  return Error::kOK;
}

/**
 * @brief 清理 FontStorage 系统，释放所有字体资源
 *
 * 遍历 font_owner_ 中所有已注册的 FontInfo，依次执行：
 *   1. 委托 TextureStorage 释放图集纹理的 GPU 资源
 *   2. 清空字形查找表
 *   3. 释放 FontInfo 自身占用的 RID 槽位
 *
 * 若编译时启用 FreeType，释放 FT_Library 实例。
 * 最后将 texture_storage_ 置空，标记为未初始化状态。
 *
 * @note 本方法不释放 TextureStorage 实例本身（不拥有所有权）
 *
 * @pre texture_storage_ 不为 nullptr（即已初始化）
 * @post texture_storage_ == nullptr
 * @post ft_library_ == nullptr（FreeType 启用时）
 * @post 所有 FontInfo 的 glyphs 均已清空
 * @post 所有关联的图集纹理已通过 TextureStorage 释放
 */
void FontStorage::Finalize() {
  if (texture_storage_ == nullptr) {
    return;
  }

  for (uint32 i = 0; i < font_owner_.Size(); ++i) {
    FontInfo* info = font_owner_.GetOrNull(RID(i));
    if (info == nullptr) {
      continue;
    }

    if (info->texture_rid.IsValid()) {
      texture_storage_->TextureFree(info->texture_rid);
    }

    info->glyphs.Clear();
    font_owner_.Free(info->rid);
  }

#if ARHUD_FREETYPE_ENABLED
  if (ft_library_ != nullptr) {
    FT_Done_FreeType(ft_library_);
    ft_library_ = nullptr;
  }
#endif

  texture_storage_ = nullptr;
}

// ═══════════════════════════════════════════════════════════════════════
// FontLoadFromSdfAtlas — 从预生成 SDF 图集加载字体
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 从预生成 SDF 图集 + 字形度量数据加载字体
 *
 * 本方法是字体加载的核心入口之一，执行以下流程：
 *   1. 参数校验（图集数据、尺寸、字体高度）
 *   2. 通过 TextureStorage::TextureCreateFromData 将 SDF 像素数据上传为 GPU
 * 纹理
 *      - 纹理格式：DataFormat::kR8Unorm（单通道 SDF 距离值）
 *      - 纹理名称：取 p_name 或默认 "sdf_atlas"
 *   3. 分配 RID，创建 FontInfo 并填入字体元数据
 *   4. 将字形度量数组插入字形查找表（charcode → GlyphInfo）
 *
 * @param[in] p_name             字体名称，用于调试和日志标识
 * @param[in] p_atlas_data       SDF 图集纹理像素数据（R8 格式，每像素 1 字节）
 * @param[in] p_atlas_data_size  图集数据总字节数（= atlas_width *
 * atlas_height）
 * @param[in] p_atlas_width      图集纹理宽度（像素），必须 > 0
 * @param[in] p_atlas_height     图集纹理高度（像素），必须 > 0
 * @param[in] p_height           字体高度（EM 方块，像素），必须 > 0
 * @param[in] p_ascent           基线以上高度（像素），正数
 * @param[in] p_descent          基线以下深度（像素），通常为负数
 * @param[in] p_glyphs           字形度量数组（charcode + UV 矩形 + 排版度量）
 * @param[in] p_glyph_count      字形数量，为 0 时不插入字形
 *
 * @return 字体 RID（成功），空 RID（失败）
 *
 * @pre 必须在渲染线程调用
 * @pre texture_storage_ 必须已初始化
 * @pre p_atlas_data 不能为 nullptr，图集宽高必须大于 0
 *
 * @post 返回的 RID 有效时，对应的 FontInfo 已注册到 font_owner_
 * @post 图集纹理已通过 TextureStorage 上传到 GPU
 *
 * @see FontLoadFromFreeType 本方法的调用方（FreeType
 * 路径最终也走此方法上传图集）
 */
RID FontStorage::FontAllocate() {
  // 任意线程：仅分配线程安全的 RID 槽位（空壳），不做 GPU 操作
  auto* info = ARHUD_NEW(FontInfo);
  RID rid = font_owner_.MakeRid(info);
  info->rid = rid;
  info->is_valid = false;
  return rid;
}

Error FontStorage::FontInitializeFromSdfAtlas(
    RID p_font, const char* p_name, const uint8* p_atlas_data,
    uint32 p_atlas_data_size, uint32 p_atlas_width, uint32 p_atlas_height,
    float p_height, float p_ascent, float p_descent, const GlyphInfo* p_glyphs,
    uint32 p_glyph_count) {
  ARHUD_ASSERT_ON_RENDER_THREAD(texture_storage_->GetRD());

  if (texture_storage_ == nullptr) {
    ARHUD_LOG_ERROR(
        "kFailed", "FontStorage::FontInitializeFromSdfAtlas() not initialized");
    return Error::kFailed;
  }

  FontInfo* info = font_owner_.GetOrNull(p_font);
  if (info == nullptr) {
    ARHUD_LOG_ERROR("kFailed",
                    "FontStorage::FontInitializeFromSdfAtlas() invalid RID");
    return Error::kFailed;
  }

  if (p_atlas_data == nullptr || p_atlas_width == 0 || p_atlas_height == 0) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "FontStorage::FontInitializeFromSdfAtlas() invalid atlas for '%s'",
        p_name ? p_name : "(null)");
    return Error::kFailed;
  }

  if (p_height <= 0.0f) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "FontStorage::FontInitializeFromSdfAtlas() invalid height for '%s'",
        p_name ? p_name : "(null)");
    return Error::kFailed;
  }

  /**
   * 上传 SDF 图集像素数据到 GPU。
   * TextureStorage 负责创建 OpenGL 纹理对象并设置过滤/包裹参数。
   * 数据格式为 kR8Unorm，SDF 距离值存储在 R 通道（0~255 映射到 [-1, 1]）。
   */
  RID texture_rid = texture_storage_->TextureCreateFromData(
      p_name ? p_name : "sdf_atlas", p_atlas_data, p_atlas_data_size,
      p_atlas_width, p_atlas_height, DataFormat::kR8Unorm);

  if (!texture_rid.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "FontStorage::FontInitializeFromSdfAtlas() atlas texture "
                    "creation failed for '%s'",
                    p_name ? p_name : "(null)");
    return Error::kFailed;
  }

  info->texture_rid = texture_rid;
  info->name = p_name ? UString(p_name) : UString();
  info->height = p_height;
  info->ascent = p_ascent;
  info->descent = p_descent;
  info->is_valid = true;

  /**
   * 将字形度量数据逐个插入 HashMap。
   * 键为 Unicode 码点（charcode），值为 GlyphInfo 结构体副本。
   * 后续渲染时通过 FontGetGlyph(charcode) 快速查询 UV 和排版度量。
   */
  if (p_glyphs != nullptr && p_glyph_count > 0) {
    for (uint32 i = 0; i < p_glyph_count; ++i) {
      info->glyphs.Insert(p_glyphs[i].charcode, p_glyphs[i]);
    }
  }

  return Error::kOK;
}

RID FontStorage::FontLoadFromSdfAtlas(
    const char* p_name, const uint8* p_atlas_data, uint32 p_atlas_data_size,
    uint32 p_atlas_width, uint32 p_atlas_height, float p_height, float p_ascent,
    float p_descent, const GlyphInfo* p_glyphs, uint32 p_glyph_count) {
  RID rid = FontAllocate();
  if (!rid.IsValid()) {
    return RID();
  }

  if (FontInitializeFromSdfAtlas(rid, p_name, p_atlas_data, p_atlas_data_size,
                                 p_atlas_width, p_atlas_height, p_height,
                                 p_ascent, p_descent, p_glyphs,
                                 p_glyph_count) != Error::kOK) {
    FontFree(rid);
    return RID();
  }
  return rid;
}

// ═══════════════════════════════════════════════════════════════════════
// FontLoadFromFreeType — 运行时 FreeType SDF 字体加载
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 通过 FreeType 库在运行时加载 TrueType/OpenType 字体并生成 SDF 图集
 *
 * 本方法是字体加载的核心实现，完成从 .ttf/.otf 文件到 GPU SDF 图集的完整流程。
 * 分为以下 9 个步骤：
 *
 * @par Step 1：获取 FreeType 库实例
 *   复用 FontStorage::Initialize 中已初始化的 FT_Library 实例（ft_library_）。
 *   同一库实例可支持多次不同字号的字体加载，避免重复初始化开销。
 *
 * @par Step 2：配置 SDF 扩散属性
 *   通过 FT_Property_Set 设置 "sdf" 模块的 "spread" 参数（需要 ≥ 2.0 像素）。
 *   SDF spread 控制字形边缘的过渡带宽度，影响渲染质量。
 *
 * @par Step 3：加载字体面（Font Face）
 *   调用 FT_New_Face 从文件路径加载字体。
 *   调用 FT_Set_Pixel_Sizes 设置渲染像素尺寸。
 *   FreeType 内部使用 26.6 定点数表示像素尺寸（即 pixels × 64）。
 *
 * @par Step 4：逐字形渲染与度量收集
 *   遍历 p_charcodes 数组，对每个码点执行：
 *     - FT_Get_Char_Index 获取字形索引
 *     - FT_Load_Glyph + FT_Render_Glyph 渲染字形
 *     - 优先尝试 FT_RENDER_MODE_SDF（FreeType 2.10+ 内置 SDF），
 *       失败时回退到 FT_RENDER_MODE_NORMAL（普通灰度位图）
 *     - 收集 advance、offset、width/height 等排版度量
 *  统计总像素面积用于后续图集尺寸估算。
 *
 * @par Step 5：行打包算法计算图集布局
 *   - 起始图集尺寸 64×64（2 的幂）
 *   - 按总像素面积 × 2 倍（留出字形间距）估算初始尺寸
 *   - 逐字形从左到右排列，超出宽度时换行
 *   - 每行记录行高，超出高度时加倍图集尺寸后重试
 *   - 最多重试 6 次，超出后认为打包失败
 *   - 计算每个字形在图集中的 UV 坐标（归一化值）
 *
 * @par Step 6：渲染字形到图集像素缓冲区
 *   由于 FT_Bitmap 属于 FT_GlyphSlot 的临时内存，第一遍遍历后 bitmap
 *   数据会被后续 FT_Load_Glyph 覆盖，因此需要第二次遍历重新渲染字形，
 *   按 Step 5 计算好的位置逐行复制像素到图集缓冲区。
 *   背景像素初始化为 0（SDF 值对应"字形外部"）。
 *
 * @par Step 7：获取字体度量
 *   从 face->size->metrics 读取 height、ascender、descender。
 *   返回的是 26.6 定点数，需 ÷64 转为浮点数。
 *   修复值：当 FreeType 返回异常值时使用默认估算值。
 *
 * @par Step 8：构建 GlyphInfo 数组并调用 FontLoadFromSdfAtlas
 *   将 PackedGlyph 数据转换为 GlyphInfo 结构体数组。
 *   调用 FontLoadFromSdfAtlas 将图集像素上传到 GPU 并注册 FontInfo。
 *
 * @par Step 9：清理
 *   释放临时图集缓冲区和 FT_Face。FT_Library 由 FontStorage 持有，
 *   在 Finalize 中统一释放，此处不释放以便后续调用复用。
 *
 * @param[in] p_name             字体名称，用于调试日志和纹理命名
 * @param[in] p_font_path        字体文件路径（支持 .ttf 和 .otf 格式）
 * @param[in] p_font_size        字体大小（像素），必须 > 0
 * @param[in] p_charcodes        需要渲染的 Unicode 码点数组
 * @param[in] p_charcode_count   码点数组长度，必须 > 0
 * @param[in] p_sdf_spread       SDF 扩散距离（像素，默认 8.0），< 2.0 时使用
 * FreeType 默认值
 *
 * @return 字体 RID（成功），空 RID（失败）
 *
 * @pre ARHUD_FREETYPE_ENABLED 编译时宏必须为 1
 * @pre 必须在渲染线程调用
 * @pre texture_storage_ 必须已初始化
 * @pre p_font_path、p_charcodes 不能为 nullptr，p_charcode_count > 0
 *
 * @post 返回的 RID 有效时，SDF 图集纹理已上传到 GPU
 * @post 所有字形度量已注册到 FontInfo 的 glyphs HashMap
 *
 * @note 本方法依赖 FreeType 2.10+ 的 FT_RENDER_MODE_SDF 功能。
 *       如果编译时 FreeType 版本低于 2.10，SDF 渲染回退为普通灰度位图。
 *
 * @note 第二次渲染字形（Step 6）是必要步骤：
 *       FT_Bitmap 数据结构直接引用 FT_GlyphSlot 内部缓冲区，
 *       不能跨 FT_Load_Glyph 调用保留。
 *
 * @see FontLoadFromSdfAtlas Step 8 中调用此方法完成 GPU 上传
 */
Error FontStorage::FontBuildFromFreeType(
    RID p_font, const char* p_name, const char* p_font_path, float p_font_size,
    const uint32* p_charcodes, uint32 p_charcode_count, float p_sdf_spread) {
  ARHUD_ASSERT_ON_RENDER_THREAD(texture_storage_->GetRD());

#if !ARHUD_FREETYPE_ENABLED
  ARHUD_LOG_ERROR("kFailed",
                  "FontStorage::FontLoadFromFreeType() FreeType not enabled at "
                  "compile time");
  (void)p_name;
  (void)p_font_path;
  (void)p_font_size;
  (void)p_charcodes;
  (void)p_charcode_count;
  (void)p_sdf_spread;
  return Error::kFailed;
#else
  if (texture_storage_ == nullptr) {
    ARHUD_LOG_ERROR("kFailed",
                    "FontStorage::FontLoadFromFreeType() not initialized");
    return Error::kFailed;
  }

  if (p_font_path == nullptr || p_charcodes == nullptr ||
      p_charcode_count == 0) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "FontStorage::FontLoadFromFreeType() invalid parameters for '%s'",
        p_name ? p_name : "(null)");
    return Error::kFailed;
  }

  if (p_font_size <= 0.0f) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "FontStorage::FontLoadFromFreeType() invalid font size for '%s'",
        p_name ? p_name : "(null)");
    return Error::kFailed;
  }

  // ── Step 1: 获取 FreeType 库实例 ──
  /**
   * FT_Library 在 FontStorage::Initialize 中已初始化（全局单例），
   * 此处直接复用，避免每次加载字体时重复初始化/释放 FreeType 库。
   * 同一库实例可支持多次不同字号的字体加载。
   */
  FT_Library library = ft_library_;
  FT_Error ft_error;

  // ── Step 2: 设置 SDF 扩散属性 ──
  /**
   * SDF spread 参数控制字形边缘到完全内部/外部的过渡距离。
   * 较大的 spread 产生更平滑的边缘但占用更多位图空间（字形整体膨胀）。
   * FreeType 内部将 spread 加在字形宽高的每一侧。
   */
  if (p_sdf_spread >= 2.0f) {
    unsigned int spread_int = static_cast<unsigned int>(p_sdf_spread);
    FT_Property_Set(library, "sdf", "spread", &spread_int);
  }

  // ── Step 3: 加载字体面 ──
  FT_Face face = nullptr;
  ft_error = FT_New_Face(library, p_font_path, 0, &face);
  if (ft_error != 0) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "FontStorage::FontLoadFromFreeType() FT_New_Face failed for '%s': %d",
        p_font_path, ft_error);
    return Error::kFailed;
  }

  /**
   * 设置字符像素尺寸。第二个参数为 0 表示宽度自适应高度。
   * FreeType 使用 26.6 定点数表示像素值，FT_Set_Pixel_Sizes 自动转换。
   */
  FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(p_font_size));

  // ── Step 4: 逐字形渲染与度量收集 ──
  /**
   * GlyphSlot 临时结构体，用于在第一遍遍历中缓存每个字形的
   * bitmap 引用和排版度量。由于 FT_Bitmap 直接引用 FT_GlyphSlot
   * 内部缓冲区，此 bitmap 指针在下次 FT_Load_Glyph 后失效，
   * 因此第一遍仅记录度量数据，不复制像素。
   */
  struct GlyphSlot {
    uint32 charcode;   ///< Unicode 码点
    FT_Bitmap bitmap;  ///< FreeType 渲染输出的位图（临时引用）
    float advance;     ///< 水平推进量（像素）
    float offset_x;    ///< 水平绘制偏移（相对基线左侧）
    float offset_y;    ///< 垂直绘制偏移（相对基线顶部）
    float width;       ///< 位图像素宽度
    float height;      ///< 位图像素高度
  };

  LocalVector<GlyphSlot> glyph_slots;
  glyph_slots.Resize(p_charcode_count);

  uint32 valid_glyph_count = 0;
  uint32 total_pixel_area = 0;

  for (uint32 i = 0; i < p_charcode_count; ++i) {
    uint32 charcode = p_charcodes[i];
    FT_UInt glyph_index = FT_Get_Char_Index(face, charcode);

    /**
     * 优先尝试 SDF 渲染模式（FreeType 2.10+ 内置，通过距离场算法生成）。
     * 如果 FreeType 不支持或字形无法 SDF 渲染，回退到普通灰度位图。
     * SDF 渲染质量更好（缩放无锯齿），但需要额外的 shader 端 smoothstep 处理。
     */
    ft_error = FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);
    if (ft_error != 0) {
      ARHUD_LOG_WARN(
          "FontStorage::FontLoadFromFreeType() FT_Load_Glyph failed for "
          "charcode %u",
          charcode);
      continue;
    }

    ft_error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_SDF);
    if (ft_error != 0) {
      ft_error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
      if (ft_error != 0) {
        ARHUD_LOG_WARN(
            "FontStorage::FontLoadFromFreeType() FT_Render_Glyph failed for "
            "charcode %u",
            charcode);
        continue;
      }
    }

    /**
     * 记录字形度量和位图引用。
     * advance.x 来自 face->glyph->advance，是 26.6 定点数，需 ÷64
     * 转换为浮点像素。 bitmap_left/top 是相对基线的偏移量，单位为像素整数。
     */
    GlyphSlot& slot = glyph_slots[valid_glyph_count];
    slot.charcode = charcode;
    slot.bitmap = face->glyph->bitmap;
    slot.advance = static_cast<float>(face->glyph->advance.x) / 64.0f;
    slot.offset_x = static_cast<float>(face->glyph->bitmap_left);
    slot.offset_y = static_cast<float>(face->glyph->bitmap_top);
    slot.width = static_cast<float>(slot.bitmap.width);
    slot.height = static_cast<float>(slot.bitmap.rows);

    total_pixel_area += slot.bitmap.width * slot.bitmap.rows;
    valid_glyph_count++;
  }

  if (valid_glyph_count == 0) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "FontStorage::FontLoadFromFreeType() no valid glyphs for '%s'",
        p_name ? p_name : "(null)");
    FT_Done_Face(face);
    return Error::kFailed;
  }

  // ── Step 5: 行打包算法计算图集布局 ──

  /**
   * 初始图集尺寸为 64×64（2 的幂，满足 GPU 纹理对齐要求）。
   * 根据总像素面积 × 2 倍间距估算所需图集大小，逐步加倍直到满足。
   */
  uint32 atlas_width = 64;
  uint32 atlas_height = 64;

  uint32 estimated_area = total_pixel_area * 2;
  while (atlas_width * atlas_height < estimated_area) {
    if (atlas_width <= atlas_height) {
      atlas_width *= 2;
    } else {
      atlas_height *= 2;
    }
  }

  /**
   * PackedGlyph 记录每个字形在图集中的最终像素位置和 UV 坐标。
   * UV 坐标通过像素位置除以图集尺寸计算，为归一化值 [0, 1]。
   */
  struct PackedGlyph {
    uint32 charcode;  ///< Unicode 码点
    uint32 x;         ///< 在图集中的像素 X 位置
    uint32 y;         ///< 在图集中的像素 Y 位置
    float uv_x;       ///< 归一化 UV 左边界
    float uv_y;       ///< 归一化 UV 上边界
    float uv_w;       ///< 归一化 UV 宽度
    float uv_h;       ///< 归一化 UV 高度
    float advance;    ///< 水平推进量
    float offset_x;   ///< 水平偏移
    float offset_y;   ///< 垂直偏移
    float width;      ///< 字形像素宽度
    float height;     ///< 字形像素高度
  };

  LocalVector<PackedGlyph> packed_glyphs;
  packed_glyphs.Resize(valid_glyph_count);

  uint32 cursor_x = 0;    ///< 当前行光标 X 位置（像素）
  uint32 cursor_y = 0;    ///< 当前行光标 Y 位置（像素）
  uint32 row_height = 0;  ///< 当前行的最大字形高度（含 1px 间距）
  bool packing_ok = false;

  /**
   * 行打包算法：左到右排列字形，超出图集宽度时换行。
   * 如果所有字形放不下，加倍图集尺寸后最多重试 6 次。
   * 每两个字形之间保留 1 像素间距防止采样溢出。
   */
  for (uint32 attempt = 0; attempt < 6; ++attempt) {
    cursor_x = 0;
    cursor_y = 0;
    row_height = 0;
    packing_ok = true;

    for (uint32 i = 0; i < valid_glyph_count; ++i) {
      uint32 gw = glyph_slots[i].bitmap.width;
      uint32 gh = glyph_slots[i].bitmap.rows;

      if (cursor_x + gw + 1 > atlas_width) {
        cursor_x = 0;
        cursor_y += row_height + 1;
        row_height = 0;
      }

      if (cursor_y + gh > atlas_height) {
        packing_ok = false;
        break;
      }

      packed_glyphs[i].charcode = glyph_slots[i].charcode;
      packed_glyphs[i].x = cursor_x;
      packed_glyphs[i].y = cursor_y;
      packed_glyphs[i].uv_x =
          static_cast<float>(cursor_x) / static_cast<float>(atlas_width);
      packed_glyphs[i].uv_y =
          static_cast<float>(cursor_y) / static_cast<float>(atlas_height);
      packed_glyphs[i].uv_w =
          static_cast<float>(gw) / static_cast<float>(atlas_width);
      packed_glyphs[i].uv_h =
          static_cast<float>(gh) / static_cast<float>(atlas_height);
      packed_glyphs[i].advance = glyph_slots[i].advance;
      packed_glyphs[i].offset_x = glyph_slots[i].offset_x;
      packed_glyphs[i].offset_y = glyph_slots[i].offset_y;
      packed_glyphs[i].width = glyph_slots[i].width;
      packed_glyphs[i].height = glyph_slots[i].height;

      cursor_x += gw + 1;
      if (gh + 1 > row_height) {
        row_height = gh + 1;
      }
    }

    if (packing_ok) {
      break;
    }

    if (atlas_width <= atlas_height) {
      atlas_width *= 2;
    } else {
      atlas_height *= 2;
    }
  }

  if (!packing_ok) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "FontStorage::FontLoadFromFreeType() atlas packing failed for '%s'",
        p_name ? p_name : "(null)");
    FT_Done_Face(face);
    return Error::kFailed;
  }

  // ── Step 6: 二次渲染，将字形像素写入图集缓冲区 ──

  uint32 atlas_data_size = atlas_width * atlas_height;
  uint8* atlas_data = ARHUD_NEW_ARR(uint8, atlas_data_size);

  /**
   * 图集背景初始化为 0。对于 SDF 渲染，0 表示"距离字形边界很远"（完全外部）；
   * 对于普通灰度渲染，0 表示全透明背景。
   * SDF fragment shader 通过 smoothstep(0.5 ± smoothing, distance)
   * 决定像素是否绘制。
   */
  for (uint32 i = 0; i < atlas_data_size; ++i) {
    atlas_data[i] = 0;
  }

  /**
   * 第二次遍历字形：重新加载并渲染，将像素数据复制到图集缓冲区的正确位置。
   * 第一次遍历收集的 FT_Bitmap 数据已因后续 FT_Load_Glyph 调用而失效，
   * 因此必须重新渲染。这里不重新度量，直接使用 Step 4 中记录的 PackedGlyph
   * 位置。
   */
  for (uint32 i = 0; i < valid_glyph_count; ++i) {
    uint32 charcode = packed_glyphs[i].charcode;
    FT_UInt glyph_index = FT_Get_Char_Index(face, charcode);

    ft_error = FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);
    if (ft_error != 0) {
      continue;
    }

    ft_error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_SDF);
    if (ft_error != 0) {
      ft_error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
      if (ft_error != 0) {
        continue;
      }
    }

    const FT_Bitmap& bitmap = face->glyph->bitmap;
    uint32 dst_x = packed_glyphs[i].x;
    uint32 dst_y = packed_glyphs[i].y;

    /**
     * 逐行复制字形位图数据到图集缓冲区。
     * FT_Bitmap::pitch 是每行字节数（可能大于 width，因为有对齐填充），
     * 而 bitmap.width 是实际有效像素宽度。
     * 边界裁剪：如果字形超出图集右/下边缘，截断复制宽度/高度。
     */
    for (uint32 row = 0; row < bitmap.rows; ++row) {
      if (dst_y + row >= atlas_height) {
        break;
      }

      uint32 copy_width = bitmap.width;
      if (dst_x + copy_width > atlas_width) {
        copy_width = atlas_width - dst_x;
      }

      const uint8* src_row = bitmap.buffer + row * bitmap.pitch;
      uint8* dst_row = atlas_data + (dst_y + row) * atlas_width + dst_x;

      for (uint32 col = 0; col < copy_width; ++col) {
        dst_row[col] = src_row[col];
      }
    }
  }

  // ── Step 7: 获取字体度量 ──

  /**
   * FreeType 的 metrics 值是 26.6 定点数（1 = 1/64 像素）。
   * ÷64 转换为浮点像素值。
   * - height：行高（ascender - descender + line_gap 的近似值）
   * - ascender：从基线到最高字形顶部的距离（正数）
   * - descender：从基线到最低字形底部的距离（负数）
   */
  float font_height = static_cast<float>(face->size->metrics.height) / 64.0f;
  float font_ascent = static_cast<float>(face->size->metrics.ascender) / 64.0f;
  float font_descent =
      static_cast<float>(face->size->metrics.descender) / 64.0f;

  /**
   * 修复值：某些字体文件可能返回异常的度量值（为 0 或未设置），
   * 此时使用退化估算值确保后续渲染不崩溃。
   */
  if (font_height <= 0.0f) {
    font_height = p_font_size;
  }
  if (font_ascent <= 0.0f) {
    font_ascent = font_height * 0.8f;
  }

  // ── Step 8: 构建 GlyphInfo 数组并调用 FontLoadFromSdfAtlas ──

  /**
   * 将内部使用的 PackedGlyph 结构体转换为外部的 GlyphInfo 结构体。
   * GlyphInfo 是公有的字体查询接口使用的标准格式。
   */
  LocalVector<GlyphInfo> glyph_infos;
  glyph_infos.Resize(valid_glyph_count);

  for (uint32 i = 0; i < valid_glyph_count; ++i) {
    glyph_infos[i].charcode = packed_glyphs[i].charcode;
    glyph_infos[i].uv_x = packed_glyphs[i].uv_x;
    glyph_infos[i].uv_y = packed_glyphs[i].uv_y;
    glyph_infos[i].uv_w = packed_glyphs[i].uv_w;
    glyph_infos[i].uv_h = packed_glyphs[i].uv_h;
    glyph_infos[i].advance = packed_glyphs[i].advance;
    glyph_infos[i].offset_x = packed_glyphs[i].offset_x;
    glyph_infos[i].offset_y = packed_glyphs[i].offset_y;
    glyph_infos[i].width = packed_glyphs[i].width;
    glyph_infos[i].height = packed_glyphs[i].height;
  }

  Error err = FontInitializeFromSdfAtlas(
      p_font, p_name, atlas_data, atlas_data_size, atlas_width, atlas_height,
      font_height, font_ascent, font_descent, glyph_infos.Data(),
      valid_glyph_count);

  // ── Step 9: 清理临时资源 ──
  /**
   * 仅释放 FT_Face（字体面），不释放 FT_Library。
   * FT_Library 由 FontStorage 持有，在 Finalize 中统一释放。
   * 这样同一库实例可被后续 FontBuildFromFreeType 调用复用。
   */
  ARHUD_DELETE_ARR(atlas_data, uint8);
  FT_Done_Face(face);

  if (err != Error::kOK) {
    ARHUD_LOG_ERROR("kFailed",
                    "FontStorage::FontBuildFromFreeType() "
                    "FontInitializeFromSdfAtlas failed for '%s'",
                    p_name ? p_name : "(null)");
  }

  return err;
#endif  // ARHUD_FREETYPE_ENABLED
}

void FontStorage::FontInitializeFromFreeType(RID p_font,
                                             const FontCreateParams& p_params,
                                             const uint8* p_blob) {
  // blob 布局：[名称 name_size][路径 path_size][码点 charcode_count×4B]
  const char* name = reinterpret_cast<const char*>(p_blob);
  const char* path = reinterpret_cast<const char*>(p_blob + p_params.name_size);
  const uint32* charcodes = reinterpret_cast<const uint32*>(
      p_blob + p_params.name_size + p_params.path_size);

  if (FontBuildFromFreeType(p_font, name, path, p_params.font_size, charcodes,
                            p_params.charcode_count,
                            p_params.sdf_spread) != Error::kOK) {
    FontFree(p_font);
  }
}

RID FontStorage::FontLoadFromFreeType(
    const char* p_name, const char* p_font_path, float p_font_size,
    const uint32* p_charcodes, uint32 p_charcode_count, float p_sdf_spread) {
  RID rid = FontAllocate();
  if (!rid.IsValid()) {
    return RID();
  }

  if (FontBuildFromFreeType(rid, (p_name != nullptr) ? p_name : "", p_font_path,
                            p_font_size, p_charcodes, p_charcode_count,
                            p_sdf_spread) != Error::kOK) {
    FontFree(rid);
    return RID();
  }
  return rid;
}

// ═══════════════════════════════════════════════════════════════════════
// FontFree — 释放字体资源
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 释放字体资源
 *
 * 执行以下清理操作：
 *   1. 委托 TextureStorage::TextureFree 释放 SDF 图集纹理的 GPU 资源
 *   2. 清空字形查找表（HashMap），释放 glyphs 占用的内存
 *   3. 标记 FontInfo 为无效（is_valid = false）
 *   4. 从 RIDOwner 中释放 RID 槽位
 *
 * @param[in] p_font 要释放的字体 RID
 *
 * @pre 必须在渲染线程调用
 * @pre p_font 必须是有效的字体 RID（无效 RID 安全忽略）
 *
 * @post p_font 对应的 RID 槽位已释放，可被后续分配复用
 * @post 关联的图集纹理已从 GPU 删除
 *
 * @note 本方法对无效 RID 安全：p_font 未注册或已释放时直接返回。
 *       重复释放同一 RID 不会造成 double-free。
 */
void FontStorage::FontFree(RID p_font) {
  ARHUD_ASSERT_ON_RENDER_THREAD(texture_storage_->GetRD());

  FontInfo* info = font_owner_.GetOrNull(p_font);
  if (info == nullptr) {
    return;
  }

  if (info->texture_rid.IsValid()) {
    texture_storage_->TextureFree(info->texture_rid);
  }

  info->glyphs.Clear();
  info->is_valid = false;
  font_owner_.Free(p_font);
}

// ═══════════════════════════════════════════════════════════════════════
// 查询方法 — 纯只读，任意线程安全
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 获取字体信息
 *
 * 返回包含字体度量和图集纹理引用的 FontInfo 指针。
 * 此方法为纯只读操作，不涉及任何 GPU 调用，可在任意线程安全调用。
 *
 * @param[in] p_font 字体 RID
 *
 * @return FontInfo 指针（成功），nullptr（RID 无效或字体已标记为无效）
 *
 * @note 返回的指针在字体被释放前有效。调用者不应持有所返回的指针跨帧。
 *       如需跨帧使用字体数据，应在每帧重新调用本方法获取最新指针。
 */
const FontInfo* FontStorage::FontGetInfo(RID p_font) const {
  const FontInfo* info = font_owner_.GetOrNull(p_font);
  if (info == nullptr || !info->is_valid) {
    return nullptr;
  }
  return info;
}

/**
 * @brief 获取指定 Unicode 码点的字形度量信息
 *
 * 在 FontInfo::glyphs 哈希表中查找 p_charcode 对应的 GlyphInfo。
 * 字形信息包含：
 *   - UV 坐标（图集中的归一化矩形，用于 SDF shader 采样）
 *   - 排版度量（advance、offset_x/y、width/height，用于文字布局）
 *
 * @param[in] p_font     字体 RID
 * @param[in] p_charcode Unicode 码点（如 'A' = 65）
 *
 * @return GlyphInfo 指针（找到），nullptr（字体无效或字形不存在）
 *
 * @note 如果指定字体不包含该字符（如中文字体缺少某些生僻字），返回 nullptr。
 *       调用者应检查返回值，对缺失字形使用替代字符（如 '?'）或跳过。
 */
const GlyphInfo* FontStorage::FontGetGlyph(RID p_font,
                                           uint32 p_charcode) const {
  const FontInfo* info = font_owner_.GetOrNull(p_font);
  if (info == nullptr || !info->is_valid) {
    return nullptr;
  }

  return info->glyphs.GetPtr(p_charcode);
}

/**
 * @brief 获取字体的 SDF 图集纹理 RID
 *
 * 返回的纹理 RID 可用于渲染管线的 UniformSet 绑定，
 * 在 SDF fragment shader 中作为字体纹理采样。
 *
 * @param[in] p_font 字体 RID
 *
 * @return 图集纹理 RID（成功），空 RID（字体无效）
 *
 * @note 返回的纹理 RID 由 TextureStorage 管理，调用者不应直接释放。
 *       纹理生命周期与字体 FontInfo 绑定，释放字体时自动释放图集纹理。
 */
RID FontStorage::FontGetTextureRID(RID p_font) const {
  const FontInfo* info = font_owner_.GetOrNull(p_font);
  if (info == nullptr || !info->is_valid) {
    return RID();
  }
  return info->texture_rid;
}

/**
 * @brief 检查字体 RID 是否有效
 *
 * 有效条件：
 *   1. RID 已注册到 font_owner_（即通过 FontLoadFromSdfAtlas 或
 *      FontLoadFromFreeType 成功创建）
 *   2. FontInfo::is_valid 标志位为 true（未被 FontFree 释放）
 *
 * @param[in] p_font 字体 RID
 *
 * @return true  字体有效，可安全用于查询方法
 * @return false RID 未注册或已释放
 *
 * @note 本方法不会验证图集纹理是否仍然有效（TextureStorage
 * 可能在其他地方释放了纹理）。 如需验证纹理有效性，应使用 FontGetTextureRID
 * 获取纹理 RID 后 调用 TextureStorage::TextureIsValid 进一步检查。
 */
bool FontStorage::FontIsValid(RID p_font) const {
  const FontInfo* info = font_owner_.GetOrNull(p_font);
  return info != nullptr && info->is_valid;
}

}  // namespace arhud
