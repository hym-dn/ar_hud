/**
 * @file shader_storage.cpp
 * @brief ShaderStorage 实现 — 着色器业务数据 + 源码缓存 + 变体管理
 *
 * @par 实现要点
 *   - 编译缓存：源码 + uniform 反射 + push constant 组合哈希去重，
 *     相同着色器重复创建零编译成本
 *   - 变体参与度探测：仅当源码声明 VARIANT_DEPTH_PASS 分支才编译
 *     深度变体（无分支的 shader 注入宏后产物与基础版逐字节等价，
 *     编译纯浪费——如 hud_canvas / hud_text / hud_composite 均跳过）
 *   - 变体机制：kDepthPass 通过在 #version 行后注入
 *     #define VARIANT_DEPTH_PASS 1 派生独立编译单元（GLSL 要求
 *     #version 必须位于首行，故不能简单前缀拼接）
 *   - 所有权分层：ShaderInfo（业务簿记）归本类；GPU program 归 RD
 *     （延迟销毁）；变体 ShaderInfo 是独立登记的兄弟条目
 *
 * @author yameng.he
 * @version 2.0
 * @date 2026-09-13
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include "storage/shader_storage.h"

#include "io/logger.h"
#include "os/memory.h"
#include "rendering_device.h"

namespace arhud {

Error ShaderStorage::Initialize(RenderingDevice* p_rd) {
  if (rd_ != nullptr) {
    return Error::kAlreadyExists;
  }

  if (p_rd == nullptr || !p_rd->IsInitialized()) {
    ARHUD_LOG_ERROR("kFailed",
                    "ShaderStorage::Initialize() invalid RenderingDevice");
    return Error::kFailed;
  }

  rd_ = p_rd;
  return Error::kOK;
}

void ShaderStorage::Finalize() {
  shader_owner_.ForEach([this](RID p_rid, ShaderInfo* p_info) {
    rd_->ShaderFree(p_info->rd_id);
    ARHUD_DELETE(p_info);
  });
  shader_owner_.Clear();
  shader_cache_.Clear();
  rd_ = nullptr;
}

/**
 * @brief 计算着色器源码缓存键
 *
 * 将各阶段的阶段枚举与源码内容哈希、每个 uniform 的
 * type/binding/writable/length 以及 push constant 大小依次混合为
 * 单个 64 位键。字符串哈希使用 djb2（种子 5381），字段组合使用
 * boost 风格 hash_combine（黄金分割常数 0x9e3779b9）。
 *
 * @param[in] p_stage_sources       着色器阶段源码数组
 * @param[in] p_uniforms            Uniform 描述数组
 * @param[in] p_push_constant_size  Push Constant 大小（字节）
 *
 * @return 64 位组合哈希缓存键（相同输入产生相同键）
 *
 * @note 64 位哈希理论存在碰撞可能；命中缓存后仍会经
 *       ShaderIsValid 验证条目有效性，碰撞导致错误复用的
 *       概率可忽略。
 */
ShaderSourceKey ShaderStorage::ComputeShaderSourceKey(
    const LocalVector<ShaderStageSource>& p_stage_sources,
    const LocalVector<ShaderUniform>& p_uniforms,
    uint32 p_push_constant_size) const {
  uint64 h = 0;

  auto hash_combine = [&h](uint64 value) {
    h ^= value + 0x9e3779b9 + (h << 6) + (h >> 2);
  };

  // Hash stage sources
  for (uint32 i = 0; i < p_stage_sources.Size(); ++i) {
    hash_combine(static_cast<uint64>(p_stage_sources[i].stage));
    // Hash source string content
    const char* src = p_stage_sources[i].source;
    if (src != nullptr) {
      uint64 str_hash = 5381;
      while (*src != '\0') {
        str_hash = ((str_hash << 5) + str_hash) + static_cast<uint64>(*src);
        src++;
      }
      hash_combine(str_hash);
    }
  }

  // Hash uniforms
  for (uint32 i = 0; i < p_uniforms.Size(); ++i) {
    hash_combine(static_cast<uint64>(p_uniforms[i].type));
    hash_combine(static_cast<uint64>(p_uniforms[i].binding));
    hash_combine(static_cast<uint64>(p_uniforms[i].writable));
    hash_combine(static_cast<uint64>(p_uniforms[i].length));
  }

  // Hash push constant size
  hash_combine(static_cast<uint64>(p_push_constant_size));

  ShaderSourceKey key;
  key.hash = h;
  return key;
}

/**
 * @brief 从 GLSL 源码创建着色器（契约语义见头文件声明）
 *
 * @par 实现流程
 *   1. **渲染线程断言**：GL 编译归渲染线程（CommandQueue 代理
 *      场景下由 executor 在渲染线程内调用）
 *   2. **缓存查询**：键命中且 RD::ShaderIsValid 通过 → 返回已有
 *      RID（零编译）；命中但 GPU 端已失效（曾被 Free）→ 移除
 *      陈旧条目后走全新编译
 *   3. **基础编译**：委托 RD::ShaderCreateFromSource（kColorPass，
 *      无宏注入），失败或 is_valid=false → ERROR + 空 RID
 *   4. **源码缓存**：逐阶段深拷贝进 ShaderInfo::sources（UString
 *      副本，供运行时重编译）
 *   5. **变体参与度探测**：任一阶段源码含子串
 *      "VARIANT_DEPTH_PASS" 才进入变体编译；否则跳过（无分支的
 *      shader 注入宏后与基础版逐字节等价，编译纯浪费——如
 *      hud_canvas / hud_text / hud_composite）
 *   6. **变体派生**（仅探测通过）：为每个阶段在 #version 行后插入
 *      `#define VARIANT_DEPTH_PASS 1` 构造合并源码 → 独立编译 →
 *      独立 ShaderInfo 条目（name 后缀 "_depth_pass"）登记。
 *      失败仅 WARN，槽位置空 RID
 *   7. **登记返回**：MakeRid 基础条目 → 回填 kColorPass 变体槽
 *      为自身 → 插入编译缓存
 *
 * @par 变体源码拼接细节
 *   - #version 必须保持首行（GLSL 规范），#define 插在其后：
 *     `[前导空白][#version xxx\n][#define ...][剩余源码]`
 *   - 无 #version 行时直接前缀拼接（安全回退，正常路径不该发生）
 *   - 合并缓冲经 memory::Alloc/Free 手工管理，编译后即释放
 *
 * @param[in] p_name 着色器名称（日志用，nullptr 安全）
 * @param[in] p_stage_sources 阶段源码数组
 * @param[in] p_uniforms Uniform 反射数据
 * @param[in] p_push_constant_size Push Constant 大小（字节）
 *
 * @return 基础着色器 RID；编译失败返回空 RID
 */
RID ShaderStorage::ShaderCreateFromSource(
    const char* p_name, const LocalVector<ShaderStageSource>& p_stage_sources,
    const LocalVector<ShaderUniform>& p_uniforms, uint32 p_push_constant_size) {
  ARHUD_ASSERT(rd_->IsRenderThread(),
               "ShaderCreateFromSource() must be called on the render thread");

  // 检查着色器编译缓存
  ShaderSourceKey cache_key =
      ComputeShaderSourceKey(p_stage_sources, p_uniforms, p_push_constant_size);
  RID* cached = shader_cache_.GetPtr(cache_key);
  if (cached != nullptr) {
    // 验证缓存的着色器仍然有效
    const ShaderInfo* cached_info = shader_owner_.GetOrNull(*cached);
    if (cached_info != nullptr && rd_->ShaderIsValid(cached_info->rd_id)) {
      return *cached;
    }
    // 缓存失效，移除条目
    shader_cache_.Erase(cache_key);
  }

  // --- 编译基础着色器（kColorPass 变体）---
  RDShaderID rd_id = rd_->ShaderCreateFromSource(
      VectorView<ShaderStageSource>(p_stage_sources), ShaderSourceFormat::kGLSL,
      VectorView<ShaderUniform>(p_uniforms), p_push_constant_size);
  if (rd_id.IsNull() || !rd_->ShaderIsValid(rd_id)) {
    ARHUD_LOG_ERROR("kFailed",
                    "ShaderStorage::ShaderCreateFromSource() RD shader "
                    "creation failed for '%s'",
                    p_name ? p_name : "(null)");
    return RID();
  }

  auto* info = ARHUD_NEW(ShaderInfo);
  info->name = UString(p_name);
  for (uint32 i = 0; i < p_stage_sources.Size(); ++i) {
    ShaderStageSourceCache cached;
    cached.stage = p_stage_sources[i].stage;
    cached.source = UString(p_stage_sources[i].source);
    info->sources.PushBack(cached);
  }
  info->rd_id = rd_id;

  // --- 编译变体着色器 ---
  // variant_shaders[0] = kColorPass（基础着色器自身）
  // variant_shaders[1] = kDepthPass
  info->variant_shaders.Resize(static_cast<uint32>(ShaderVariant::kMax));

  // kColorPass 变体 RID 指向自身（稍后赋值 rid）
  // 先占位，rid 在 MakeRid 后回填
  info->variant_shaders[static_cast<uint32>(ShaderVariant::kColorPass)] = RID();

  // ── 变体参与度探测 ──
  // 仅当源码声明了 VARIANT_DEPTH_PASS 分支（#ifdef/#if defined 等，
  // 以子串探测为准）才编译深度变体。无分支的 shader（如 hud_canvas/
  // hud_text/hud_composite）注入宏后与基础版逐字节等价——编译纯浪费
  // （多一个冗余 GPU program + 一份源码拷贝）。探测跳过后槽位置空，
  // ShaderGetVariant() 自动退回基础 RID（既有软退化路径，零额外代码）。
  bool has_depth_variant = false;
  for (uint32 i = 0; i < p_stage_sources.Size() && !has_depth_variant; ++i) {
    const char* src =
        p_stage_sources[i].source ? p_stage_sources[i].source : "";
    if (src != nullptr && strstr(src, "VARIANT_DEPTH_PASS") != nullptr) {
      has_depth_variant = true;
    }
  }

  // 编译 kDepthPass 变体（仅参与度探测通过时）
  if (has_depth_variant) {
    LocalVector<ShaderStageSource> variant_sources;
    variant_sources.Resize(p_stage_sources.Size());
    for (uint32 i = 0; i < p_stage_sources.Size(); ++i) {
      variant_sources[i].stage = p_stage_sources[i].stage;

      // 构建 #define 前缀 + 原始源码
      // 注意：#define 必须插入到 #version 行之后，GLSL 要求 #version
      // 必须是第一行
      const char* define_str = "#define VARIANT_DEPTH_PASS 1\n";
      const char* original =
          p_stage_sources[i].source ? p_stage_sources[i].source : "";

      // 跳过前导空白/换行，找到 #version 的位置
      const char* src = original;
      while (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r') {
        ++src;
      }

      // 查找 #version 行的结尾，将 #define 插入其后
      const char* version_end = nullptr;
      uint32 leading_len = static_cast<uint32>(src - original);  // 前导空白长度
      if (strncmp(src, "#version", 8) == 0) {
        // 找到 #version 行的换行符
        const char* newline = strchr(src, '\n');
        if (newline != nullptr) {
          version_end = newline + 1;
        }
      }

      uint32 define_len = static_cast<uint32>(strlen(define_str));
      uint32 original_len = static_cast<uint32>(strlen(original));

      char* combined = nullptr;
      if (version_end != nullptr) {
        // 前导空白 + #version 行 + #define + 剩余源码
        uint32 before_len = static_cast<uint32>(version_end - original);
        uint32 after_len = original_len - before_len;
        uint32 total_len = before_len + define_len + after_len + 1;

        combined = static_cast<char*>(memory::Alloc(total_len));
        memcpy(combined, original, before_len);  // 前导空白 + #version xxx\n
        memcpy(combined + before_len, define_str,
               define_len);  // #define VARIANT_DEPTH_PASS 1\n
        memcpy(combined + before_len + define_len, version_end,
               after_len);  // 剩余源码
        combined[before_len + define_len + after_len] = '\0';
      } else {
        // 无 #version 行，直接前缀（不应该发生，但安全回退）
        uint32 total_len = define_len + original_len + 1;
        combined = static_cast<char*>(memory::Alloc(total_len));
        memcpy(combined, define_str, define_len);
        memcpy(combined + define_len, original, original_len);
        combined[define_len + original_len] = '\0';
      }

      variant_sources[i].source = combined;
    }

    RDShaderID depth_rd_id = rd_->ShaderCreateFromSource(
        VectorView<ShaderStageSource>(variant_sources),
        ShaderSourceFormat::kGLSL, VectorView<ShaderUniform>(p_uniforms),
        p_push_constant_size);

    if (!depth_rd_id.IsNull() && rd_->ShaderIsValid(depth_rd_id)) {
      // 为变体创建独立的 ShaderInfo 条目
      auto* depth_info = ARHUD_NEW(ShaderInfo);
      depth_info->name = UString(p_name) + "_depth_pass";
      for (uint32 i = 0; i < p_stage_sources.Size(); ++i) {
        ShaderStageSourceCache cached;
        cached.stage = p_stage_sources[i].stage;
        cached.source = UString(variant_sources[i].source);
        depth_info->sources.PushBack(cached);
      }
      depth_info->rd_id = depth_rd_id;

      RID depth_rid = shader_owner_.MakeRid(depth_info);
      depth_info->rid = depth_rid;
      info->variant_shaders[static_cast<uint32>(ShaderVariant::kDepthPass)] =
          depth_rid;
    } else {
      ARHUD_LOG_WARN(
          "ShaderStorage::ShaderCreateFromSource() depth variant compilation "
          "failed for '%s'",
          p_name ? p_name : "(null)");
      info->variant_shaders[static_cast<uint32>(ShaderVariant::kDepthPass)] =
          RID();
    }

    // 释放临时合并的源码字符串
    for (uint32 i = 0; i < variant_sources.Size(); ++i) {
      if (variant_sources[i].source != nullptr &&
          variant_sources[i].source != p_stage_sources[i].source) {
        memory::Free(const_cast<char*>(variant_sources[i].source));
      }
    }
  } else {
    // 未声明 VARIANT_DEPTH_PASS 分支：跳过变体编译，
    // 槽位置空 RID —— ShaderGetVariant(kDepthPass) 退回基础 RID
    info->variant_shaders[static_cast<uint32>(ShaderVariant::kDepthPass)] =
        RID();
    ARHUD_LOG_INFO(
        "ShaderStorage::ShaderCreateFromSource() '%s' declares no "
        "VARIANT_DEPTH_PASS branch, depth variant skipped",
        p_name ? p_name : "(null)");
  }

  RID rid = shader_owner_.MakeRid(info);
  info->rid = rid;

  // 回填 kColorPass 变体 RID 为自身
  info->variant_shaders[static_cast<uint32>(ShaderVariant::kColorPass)] = rid;

  shader_cache_.Insert(cache_key, rid);

  return rid;
}

/**
 * @brief 释放着色器资源（契约语义见头文件声明）
 *
 * 实现分两段：
 *   1. **变体级联**：遍历 variant_shaders，跳过自身槽（kColorPass
 *      回填的 RID == p_shader）与空槽，逐个释放变体条目——
 *      RD::ShaderFree（GPU 延迟销毁）→ owner_.Free → DELETE
 *   2. **本体释放**：同构三步释放基础条目
 *
 * 幂等性：GetOrNull 对无效/悬垂 RID 返回 nullptr 即整体 no-op；
 * 变体条目已被单独释放时（理论不该发生）同样安全跳过。
 *
 * @param[in] p_shader 基础着色器 RID
 */
void ShaderStorage::ShaderFree(RID p_shader) {
  ARHUD_ASSERT(rd_->IsRenderThread(),
               "ShaderFree() must be called on the render thread");

  ShaderInfo* info = shader_owner_.GetOrNull(p_shader);
  if (info == nullptr) {
    return;
  }

  // 释放变体着色器（kDepthPass 等独立条目）
  for (uint32 i = 0; i < info->variant_shaders.Size(); ++i) {
    RID variant_rid = info->variant_shaders[i];
    if (variant_rid.IsValid() && variant_rid != p_shader) {
      ShaderInfo* variant_info = shader_owner_.GetOrNull(variant_rid);
      if (variant_info != nullptr) {
        rd_->ShaderFree(variant_info->rd_id);
        shader_owner_.Free(variant_rid);
        ARHUD_DELETE(variant_info);
      }
    }
  }

  rd_->ShaderFree(info->rd_id);
  shader_owner_.Free(p_shader);
  ARHUD_DELETE(info);
}

/**
 * @brief 获取完整 ShaderInfo（契约语义见头文件声明）
 *
 * O(1) RIDOwner 查找的直通封装，无附加逻辑。
 *
 * @param[in] p_shader 着色器 RID
 *
 * @return 簿记结构指针（含 name/sources/variant_shaders/rd_id）
 */
const ShaderInfo* ShaderStorage::ShaderGetInfo(RID p_shader) const {
  return shader_owner_.GetOrNull(p_shader);
}

/**
 * @brief 获取 RD 层着色器 ID（契约语义见头文件声明）
 *
 * 供 Pipeline 创建等需要 RDShaderID 的场景使用——
 * RD 层 API 以 RDShaderID 为句柄，本方法完成
 * 业务 RID → RD ID 的翻译。
 *
 * @param[in] p_shader 着色器 RID
 *
 * @return RDShaderID；无效 RID 返回空 ID
 */
RDShaderID ShaderStorage::ShaderGetRDId(RID p_shader) const {
  const ShaderInfo* info = shader_owner_.GetOrNull(p_shader);
  if (info == nullptr) {
    return RDShaderID();
  }
  return info->rd_id;
}

/**
 * @brief 获取 uniform 反射数据（代理到 RD 层）
 *
 * 本地查簿记取 rd_id 后转发 RD 查询，返回引用的生命周期归属 RD。
 * 静态空列表保证无效 RID 时返回安全引用。
 *
 * @param[in] p_shader 着色器 RID
 *
 * @return uniform 列表的常量引用；RID 无效时返回空列表
 */
const LocalVector<ShaderUniform>& ShaderStorage::ShaderGetUniforms(
    RID p_shader) const {
  static const LocalVector<ShaderUniform> kEmptyUniforms;

  const ShaderInfo* info = shader_owner_.GetOrNull(p_shader);
  if (info == nullptr) {
    return kEmptyUniforms;
  }

  return rd_->ShaderGetUniforms(info->rd_id);
}

/**
 * @brief 获取 push constant 块大小（代理到 RD 层）
 *
 * @param[in] p_shader 着色器 RID
 *
 * @return push constant 大小（字节）；RID 无效时返回 0
 */
uint32 ShaderStorage::ShaderGetPushConstantSize(RID p_shader) const {
  const ShaderInfo* info = shader_owner_.GetOrNull(p_shader);
  if (info == nullptr) {
    return 0;
  }

  return rd_->ShaderGetPushConstantSize(info->rd_id);
}

/**
 * @brief 检查着色器是否有效（代理到 RD 层）
 *
 * 双重判定：簿记条目存在 且 RD 侧编译产物有效。
 *
 * @param[in] p_shader 着色器 RID
 *
 * @return 有效返回 true；RID 无效或编译失败返回 false
 */
bool ShaderStorage::ShaderIsValid(RID p_shader) const {
  const ShaderInfo* info = shader_owner_.GetOrNull(p_shader);
  if (info == nullptr) {
    return false;
  }

  return rd_->ShaderIsValid(info->rd_id);
}

/**
 * @brief 获取指定变体的 Shader RID（本地簿记查询）
 *
 * @param[in] p_shader  基础着色器 RID
 * @param[in] p_variant 变体类型
 *
 * @return 变体 Shader RID；变体不存在或编译失败时回退返回基础 RID
 *
 * @note kColorPass 槽位恒指向基础着色器自身；回退语义保证返回值
 *       总是可用于渲染的合法着色器，调用方无需判空。
 */
RID ShaderStorage::ShaderGetVariant(RID p_shader,
                                    ShaderVariant p_variant) const {
  const ShaderInfo* info = shader_owner_.GetOrNull(p_shader);
  if (info == nullptr) {
    return RID();
  }

  uint32 idx = static_cast<uint32>(p_variant);
  if (idx < info->variant_shaders.Size() &&
      info->variant_shaders[idx].IsValid()) {
    return info->variant_shaders[idx];
  }

  // 变体不存在时返回基础 Shader RID
  return p_shader;
}

}  // namespace arhud
