#include "rendering_device.h"
#include "storage/shader_storage.h"
#include "os/memory.h"
#include "io/logger.h"

namespace arhud
{

    Error ShaderStorage::Initialize(RenderingDevice *p_rd)
    {
        if (rd_ != nullptr)
        {
            return Error::kAlreadyExists;
        }

        if (p_rd == nullptr || !p_rd->IsInitialized())
        {
            ARHUD_LOG_ERROR("kFailed", "ShaderStorage::Initialize() invalid RenderingDevice");
            return Error::kFailed;
        }

        rd_ = p_rd;
        return Error::kOK;
    }

    void ShaderStorage::Finalize()
    {
        shader_owner_.ForEach([this](RID p_rid, ShaderInfo *p_info)
                              {
            rd_->ShaderFree(p_info->rd_id);
            ARHUD_DELETE(p_info); });

        shader_owner_.Clear();
        rd_ = nullptr;
    }

    RID ShaderStorage::ShaderCreateFromSource(const char *p_name,
                                              const LocalVector<ShaderStageSource> &p_stage_sources,
                                              const LocalVector<ShaderUniform> &p_uniforms,
                                              uint32_t p_push_constant_size)
    {
        RDShaderID rd_id = rd_->ShaderCreateFromGLSL(
            VectorView<ShaderStageSource>(p_stage_sources),
            VectorView<ShaderUniform>(p_uniforms),
            p_push_constant_size);

        if (rd_id.IsNull() || !rd_->ShaderIsValid(rd_id))
        {
            ARHUD_LOG_ERROR("kFailed", "ShaderStorage::ShaderCreateFromSource() RD shader creation failed for '%s'",
                            p_name ? p_name : "(null)");
            return RID();
        }

        auto *info = ARHUD_NEW(ShaderInfo);
        info->name = UString(p_name);
        for (uint32_t i = 0; i < p_stage_sources.Size(); ++i)
        {
            ShaderStageSourceCache cached;
            cached.stage = p_stage_sources[i].stage;
            cached.source = UString(p_stage_sources[i].source);
            info->sources.PushBack(cached);
        }
        info->rd_id = rd_id;

        RID rid = shader_owner_.MakeRid(info);
        info->rid = rid;

        return rid;
    }

    void ShaderStorage::ShaderFree(RID p_shader)
    {
        ShaderInfo *info = shader_owner_.GetOrNull(p_shader);
        if (info == nullptr)
        {
            return;
        }

        rd_->ShaderFree(info->rd_id);
        shader_owner_.Free(p_shader);
        ARHUD_DELETE(info);
    }

    const ShaderInfo *ShaderStorage::ShaderGetInfo(RID p_shader) const
    {
        return shader_owner_.GetOrNull(p_shader);
    }

    RDShaderID ShaderStorage::ShaderGetRDId(RID p_shader) const
    {
        const ShaderInfo *info = shader_owner_.GetOrNull(p_shader);
        if (info == nullptr)
        {
            return RDShaderID();
        }
        return info->rd_id;
    }

    const LocalVector<ShaderUniform> &ShaderStorage::ShaderGetUniforms(RID p_shader) const
    {
        static const LocalVector<ShaderUniform> kEmptyUniforms;

        const ShaderInfo *info = shader_owner_.GetOrNull(p_shader);
        if (info == nullptr)
        {
            return kEmptyUniforms;
        }

        return rd_->ShaderGetUniforms(info->rd_id);
    }

    uint32_t ShaderStorage::ShaderGetPushConstantSize(RID p_shader) const
    {
        const ShaderInfo *info = shader_owner_.GetOrNull(p_shader);
        if (info == nullptr)
        {
            return 0;
        }

        return rd_->ShaderGetPushConstantSize(info->rd_id);
    }

    bool ShaderStorage::ShaderIsValid(RID p_shader) const
    {
        const ShaderInfo *info = shader_owner_.GetOrNull(p_shader);
        if (info == nullptr)
        {
            return false;
        }

        return rd_->ShaderIsValid(info->rd_id);
    }

}
