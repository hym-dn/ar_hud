/**
 * @file rendering_device_driver_gl.cpp
 * @brief OpenGL 渲染设备驱动实现
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-08
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include "rendering_device_driver_gl.h"

#include "os/memory.h"
#include "io/logger.h"
#include <glad/gl.h>

namespace arhud
{

    GLStateCache::GLStateCache()
    {
        for (uint32_t i = 0; i < 16; ++i)
        {
            bound_textures_[i] = 0;
            bound_ubos_[i] = 0;
            bound_samplers_[i] = 0;
        }
    }

    void GLStateCache::Reset()
    {
        program_ = 0;
        vao_ = 0;
        fbo_ = 0;
        vertex_buffer_ = 0;
        index_buffer_ = 0;
        active_texture_unit_ = 0;

        for (uint32_t i = 0; i < 16; ++i)
        {
            bound_textures_[i] = 0;
            bound_ubos_[i] = 0;
        }

        depth_test_ = false;
        depth_write_ = true;
        depth_func_ = GL_LESS;
        blend_ = false;
        blend_src_rgb_ = GL_ONE;
        blend_dst_rgb_ = GL_ZERO;
        blend_src_alpha_ = GL_ONE;
        blend_dst_alpha_ = GL_ZERO;
        cull_face_ = false;
        cull_mode_ = GL_BACK;
        front_face_ = GL_CW;

        viewport_dirty_ = true;
        scissor_dirty_ = true;
    }

    void GLStateCache::UseProgram(uint32_t p_program)
    {
        if (program_ != p_program)
        {
            program_ = p_program;
            glUseProgram(p_program);
        }
    }

    void GLStateCache::BindVertexArray(uint32_t p_vao)
    {
        if (vao_ != p_vao)
        {
            vao_ = p_vao;
            glBindVertexArray(p_vao);
        }
    }

    void GLStateCache::BindFramebuffer(uint32_t p_fbo)
    {
        if (fbo_ != p_fbo)
        {
            fbo_ = p_fbo;
            glBindFramebuffer(GL_FRAMEBUFFER, p_fbo);
        }
    }

    void GLStateCache::BindVertexBuffer(uint32_t p_buffer)
    {
        if (vertex_buffer_ != p_buffer)
        {
            vertex_buffer_ = p_buffer;
            glBindBuffer(GL_ARRAY_BUFFER, p_buffer);
        }
    }

    void GLStateCache::BindIndexBuffer(uint32_t p_buffer)
    {
        if (index_buffer_ != p_buffer)
        {
            index_buffer_ = p_buffer;
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p_buffer);
        }
    }

    void GLStateCache::BindUniformBuffer(uint32_t p_index, uint32_t p_buffer)
    {
        if (p_index < 16 && bound_ubos_[p_index] != p_buffer)
        {
            bound_ubos_[p_index] = p_buffer;
            glBindBufferBase(GL_UNIFORM_BUFFER, p_index, p_buffer);
        }
    }

    void GLStateCache::ActiveTexture(uint32_t p_unit)
    {
        if (active_texture_unit_ != p_unit)
        {
            active_texture_unit_ = p_unit;
            glActiveTexture(GL_TEXTURE0 + p_unit);
        }
    }

    void GLStateCache::BindTexture2D(uint32_t p_unit, uint32_t p_texture)
    {
        if (p_unit < 16 && bound_textures_[p_unit] != p_texture)
        {
            bound_textures_[p_unit] = p_texture;
            ActiveTexture(p_unit);
            glBindTexture(GL_TEXTURE_2D, p_texture);
        }
    }

    void GLStateCache::BindTexture(uint32_t p_target, uint32_t p_texture)
    {
        glBindTexture(p_target, p_texture);
        if (active_texture_unit_ < 16)
        {
            bound_textures_[active_texture_unit_] = p_texture;
        }
    }

    void GLStateCache::BindSampler(uint32_t p_unit, uint32_t p_sampler)
    {
        if (p_unit < 16 && bound_samplers_[p_unit] != p_sampler)
        {
            bound_samplers_[p_unit] = p_sampler;
            glBindSampler(p_unit, p_sampler);
        }
    }

    void GLStateCache::SetDepthTest(bool p_enable)
    {
        if (depth_test_ != p_enable)
        {
            depth_test_ = p_enable;
            if (p_enable)
            {
                glEnable(GL_DEPTH_TEST);
            }
            else
            {
                glDisable(GL_DEPTH_TEST);
            }
        }
    }

    void GLStateCache::SetDepthWrite(bool p_enable)
    {
        if (depth_write_ != p_enable)
        {
            depth_write_ = p_enable;
            glDepthMask(p_enable ? GL_TRUE : GL_FALSE);
        }
    }

    void GLStateCache::SetDepthFunc(uint32_t p_func)
    {
        if (depth_func_ != p_func)
        {
            depth_func_ = p_func;
            glDepthFunc(p_func);
        }
    }

    void GLStateCache::SetBlend(bool p_enable)
    {
        if (blend_ != p_enable)
        {
            blend_ = p_enable;
            if (p_enable)
            {
                glEnable(GL_BLEND);
            }
            else
            {
                glDisable(GL_BLEND);
            }
        }
    }

    void GLStateCache::SetBlendFunc(uint32_t p_src_rgb, uint32_t p_dst_rgb,
                                    uint32_t p_src_alpha, uint32_t p_dst_alpha)
    {
        if (blend_src_rgb_ != p_src_rgb || blend_dst_rgb_ != p_dst_rgb ||
            blend_src_alpha_ != p_src_alpha || blend_dst_alpha_ != p_dst_alpha)
        {
            blend_src_rgb_ = p_src_rgb;
            blend_dst_rgb_ = p_dst_rgb;
            blend_src_alpha_ = p_src_alpha;
            blend_dst_alpha_ = p_dst_alpha;
            glBlendFuncSeparate(p_src_rgb, p_dst_rgb, p_src_alpha, p_dst_alpha);
        }
    }

    void GLStateCache::SetCullFace(bool p_enable)
    {
        if (cull_face_ != p_enable)
        {
            cull_face_ = p_enable;
            if (p_enable)
            {
                glEnable(GL_CULL_FACE);
            }
            else
            {
                glDisable(GL_CULL_FACE);
            }
        }
    }

    void GLStateCache::SetCullMode(uint32_t p_mode)
    {
        if (cull_mode_ != p_mode)
        {
            cull_mode_ = p_mode;
            glCullFace(p_mode);
        }
    }

    void GLStateCache::SetFrontFace(uint32_t p_face)
    {
        if (front_face_ != p_face)
        {
            front_face_ = p_face;
            glFrontFace(p_face);
        }
    }

    void GLStateCache::SetViewport(int32_t p_x, int32_t p_y,
                                   uint32_t p_width, uint32_t p_height)
    {
        if (viewport_dirty_ || viewport_x_ != p_x || viewport_y_ != p_y ||
            viewport_w_ != p_width || viewport_h_ != p_height)
        {
            viewport_x_ = p_x;
            viewport_y_ = p_y;
            viewport_w_ = p_width;
            viewport_h_ = p_height;
            viewport_dirty_ = false;
            glViewport(p_x, p_y, static_cast<GLsizei>(p_width), static_cast<GLsizei>(p_height));
        }
    }

    void GLStateCache::SetScissor(int32_t p_x, int32_t p_y,
                                  uint32_t p_width, uint32_t p_height)
    {
        if (scissor_dirty_ || scissor_x_ != p_x || scissor_y_ != p_y ||
            scissor_w_ != p_width || scissor_h_ != p_height)
        {
            scissor_x_ = p_x;
            scissor_y_ = p_y;
            scissor_w_ = p_width;
            scissor_h_ = p_height;
            scissor_dirty_ = false;
            glScissor(p_x, p_y, static_cast<GLsizei>(p_width), static_cast<GLsizei>(p_height));
        }
    }

    int32_t GLShaderInfo::FindLocation(uint32_t p_binding) const
    {
        for (uint32_t i = 0; i < uniform_locations.Size(); ++i)
        {
            if (uniform_locations[i].binding == p_binding)
            {
                return uniform_locations[i].location;
            }
        }
        return -1;
    }

    // ─── 生命周期 ───────────────────────────────────────────────────────

    /**
     * @brief 构造函数
     *
     * @param[in] p_context_driver 渲染上下文驱动
     */
    GLRenderingDeviceDriver::GLRenderingDeviceDriver(IRenderingContextDriver *p_context_driver,
                                                     IGLManager *p_gl_manager)
      : context_driver_(p_context_driver)
      , gl_manager_(p_gl_manager)
    {
    }

    /**
     * @brief 析构函数
     */
    GLRenderingDeviceDriver::~GLRenderingDeviceDriver()
    {
        if (initialized_)
        {
            Shutdown();
        }
    }

    /**
     * @brief 初始化 OpenGL 渲染设备驱动
     *
     * @param[in] p_device_index  设备索引
     * @param[in] p_frame_count   帧缓冲数量
     *
     * @retval Error::kOK 初始化成功
     * @retval Error::kFailed 初始化失败
     * @retval Error::kNotSupported 不支持的 GL 版本
     *
     * @pre GL 上下文已 MakeCurrent
     */
    Error GLRenderingDeviceDriver::Initialize(uint32_t p_device_index, uint32_t p_frame_count)
    {
        (void)p_device_index;
        (void)p_frame_count;

        if (initialized_)
        {
            return Error::kOK;
        }

        int gl_version = gladLoadGL(reinterpret_cast<GLADloadfunc>(gl_manager_->GetGLProcAddress()));
        if (gl_version == 0)
        {
            ARHUD_LOG_ERROR("kFailed", "GLRenderingDeviceDriver: Failed to load GL functions via glad");
            return Error::kFailed;
        }

        QueryLimits();

        capabilities_.device_family = DeviceFamily::kOpenGL;

        GLint major = 3, minor = 3;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        capabilities_.version_major = static_cast<uint32_t>(major);
        capabilities_.version_minor = static_cast<uint32_t>(minor);

        snprintf(api_name_, sizeof(api_name_), "OpenGL %u.%u",
                 capabilities_.version_major, capabilities_.version_minor);

        snprintf(pipeline_cache_uuid_, sizeof(pipeline_cache_uuid_),
                 "00000000-0000-0000-0000-000000000000");

        initialized_ = true;
        ARHUD_LOG_INFO("GLRenderingDeviceDriver: Initialized (%s)", api_name_);
        return Error::kOK;
    }

    void GLRenderingDeviceDriver::Shutdown()
    {
        if (!initialized_)
        {
            return;
        }

        shader_allocator_.Reset(true);
        pipeline_allocator_.Reset(true);
        framebuffer_allocator_.Reset(true);
        render_pass_allocator_.Reset(true);
        uniform_set_allocator_.Reset(true);
        vertex_format_allocator_.Reset(true);

        state_cache_.Reset();

        initialized_ = false;
        ARHUD_LOG_INFO("GLRenderingDeviceDriver: Shutdown complete");
    }

    BufferID GLRenderingDeviceDriver::BufferCreate(
      uint64_t p_size,
      BitField<BufferUsageBits> p_usage,
      MemoryAllocationType p_allocation_type)
    {
        (void)p_usage;

        GLuint gl_buffer = 0;
        glGenBuffers(1, &gl_buffer);
        if (gl_buffer == 0)
        {
            return BufferID();
        }

        state_cache_.BindVertexBuffer(gl_buffer);

        GLenum gl_usage = GL_STATIC_DRAW;
        if (p_allocation_type == MemoryAllocationType::kCpu)
        {
            gl_usage = GL_DYNAMIC_DRAW;
        }

        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(p_size),
                        nullptr, gl_usage);

        return BufferID(static_cast<uint64_t>(gl_buffer));
    }

    void GLRenderingDeviceDriver::BufferFree(BufferID p_buffer)
    {
        if (!p_buffer.IsValid())
        {
            return;
        }

        GLuint gl_buffer = static_cast<GLuint>(p_buffer.GetId());
        glDeleteBuffers(1, &gl_buffer);
    }

    uint8_t *GLRenderingDeviceDriver::BufferMap(BufferID p_buffer)
    {
        if (!p_buffer.IsValid())
        {
            return nullptr;
        }

        GLuint gl_buffer = static_cast<GLuint>(p_buffer.GetId());
        state_cache_.BindVertexBuffer(gl_buffer);

        uint8_t *ptr = static_cast<uint8_t *>(glMapBuffer(GL_ARRAY_BUFFER, GL_READ_WRITE));

        return ptr;
    }

    void GLRenderingDeviceDriver::BufferUnmap(BufferID p_buffer)
    {
        if (!p_buffer.IsValid())
        {
            return;
        }

        GLuint gl_buffer = static_cast<GLuint>(p_buffer.GetId());
        state_cache_.BindVertexBuffer(gl_buffer);

        glUnmapBuffer(GL_ARRAY_BUFFER);
    }

    uint64_t GLRenderingDeviceDriver::BufferGetAllocationSize(BufferID p_buffer)
    {
        if (!p_buffer.IsValid())
        {
            return 0;
        }

        GLuint gl_buffer = static_cast<GLuint>(p_buffer.GetId());
        state_cache_.BindVertexBuffer(gl_buffer);

        GLint size = 0;
        glGetIntegerv(GL_BUFFER_SIZE, &size);

        return static_cast<uint64_t>(size);
    }

    TextureID GLRenderingDeviceDriver::TextureCreate(
      const TextureFormat &p_format,
      const TextureView &p_view)
    {
        (void)p_view;

        GLuint gl_texture = 0;
        glGenTextures(1, &gl_texture);
        if (gl_texture == 0)
        {
            return TextureID();
        }

        GLenum target = GL_TEXTURE_2D;
        switch (p_format.texture_type)
        {
        case TextureType::k2D:
            target = GL_TEXTURE_2D;
            break;
        case TextureType::k2DArray:
            target = GL_TEXTURE_2D_ARRAY;
            break;
        case TextureType::k3D:
            target = GL_TEXTURE_3D;
            break;
        case TextureType::kCube:
            target = GL_TEXTURE_CUBE_MAP;
            break;
        default:
            target = GL_TEXTURE_2D;
            break;
        }

        state_cache_.BindTexture(target, gl_texture);

        uint32_t internal_format = DataFormatToGLInternalFormat(p_format.format);
        uint32_t gl_format = DataFormatToGLFormat(p_format.format);
        uint32_t gl_type = DataFormatToGLType(p_format.format);

        if (p_format.texture_type == TextureType::k2D)
        {
            glTexImage2D(target, 0, static_cast<GLint>(internal_format),
                            static_cast<GLsizei>(p_format.width),
                            static_cast<GLsizei>(p_format.height),
                            0, gl_format, gl_type, nullptr);
        }
        else if (p_format.texture_type == TextureType::k3D ||
                 p_format.texture_type == TextureType::k2DArray)
        {
            glTexImage3D(target, 0, static_cast<GLint>(internal_format),
                            static_cast<GLsizei>(p_format.width),
                            static_cast<GLsizei>(p_format.height),
                            static_cast<GLsizei>(p_format.depth),
                            0, gl_format, gl_type, nullptr);
        }
        else if (p_format.texture_type == TextureType::kCube)
        {
            for (uint32_t i = 0; i < 6; ++i)
            {
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, static_cast<GLint>(internal_format),
                                static_cast<GLsizei>(p_format.width),
                                static_cast<GLsizei>(p_format.height),
                                0, gl_format, gl_type, nullptr);
            }
        }

        if (p_format.mipmaps > 1)
        {
            glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(p_format.mipmaps - 1));
        }

        TextureID tex_id(static_cast<uint64_t>(gl_texture));

        GLTextureInfo info;
        info.target = target;
        info.format = p_format.format;
        info.width = p_format.width;
        info.height = p_format.height;
        info.depth = p_format.depth;
        info.mipmaps = p_format.mipmaps;
        info.layers = p_format.array_layers;
        texture_infos_.Insert(tex_id.GetId(), info);

        return tex_id;
    }

    void GLRenderingDeviceDriver::TextureFree(TextureID p_texture)
    {
        if (!p_texture.IsValid())
        {
            return;
        }

        GLuint gl_texture = static_cast<GLuint>(p_texture.GetId());
        glDeleteTextures(1, &gl_texture);

        texture_infos_.Erase(p_texture.GetId());
    }

    uint64_t GLRenderingDeviceDriver::TextureGetAllocationSize(TextureID p_texture)
    {
        (void)p_texture;
        return 0;
    }

    BitField<TextureUsageBits> GLRenderingDeviceDriver::TextureGetUsagesSupportedByFormat(
      DataFormat p_format, bool p_cpu_readable)
    {
        (void)p_format;
        (void)p_cpu_readable;

        BitField<TextureUsageBits> usages;
        usages.SetFlag(TextureUsageBits::kSampling);
        usages.SetFlag(TextureUsageBits::kColorAttachment);
        usages.SetFlag(TextureUsageBits::kCanCopyFrom);
        usages.SetFlag(TextureUsageBits::kCanCopyTo);
        usages.SetFlag(TextureUsageBits::kCanUpdate);
        return usages;
    }

    SamplerID GLRenderingDeviceDriver::SamplerCreate(const SamplerState &p_state)
    {
        GLuint gl_sampler = 0;
        glGenSamplers(1, &gl_sampler);
        if (gl_sampler == 0)
        {
            return SamplerID();
        }

        uint32_t min_filter = GL_NEAREST_MIPMAP_NEAREST;
        if (p_state.mip_filter == SamplerFilter::kLinear)
        {
            if (p_state.min_filter == SamplerFilter::kLinear)
            {
                min_filter = GL_LINEAR_MIPMAP_LINEAR;
            }
            else
            {
                min_filter = GL_NEAREST_MIPMAP_LINEAR;
            }
        }
        else
        {
            if (p_state.min_filter == SamplerFilter::kLinear)
            {
                min_filter = GL_LINEAR_MIPMAP_NEAREST;
            }
        }

        uint32_t mag_filter = SamplerFilterToGL(p_state.mag_filter);

        glSamplerParameteri(gl_sampler, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(min_filter));
        glSamplerParameteri(gl_sampler, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(mag_filter));
        glSamplerParameteri(gl_sampler, GL_TEXTURE_WRAP_S, static_cast<GLint>(SamplerRepeatModeToGL(p_state.repeat_u)));
        glSamplerParameteri(gl_sampler, GL_TEXTURE_WRAP_T, static_cast<GLint>(SamplerRepeatModeToGL(p_state.repeat_v)));
        glSamplerParameteri(gl_sampler, GL_TEXTURE_WRAP_R, static_cast<GLint>(SamplerRepeatModeToGL(p_state.repeat_w)));

        if (p_state.use_anisotropy)
        {
            glSamplerParameterf(gl_sampler, GL_MAX_TEXTURE_MAX_ANISOTROPY, p_state.anisotropy_max);
        }

        if (p_state.enable_compare)
        {
            glSamplerParameteri(gl_sampler, GL_TEXTURE_COMPARE_MODE, GL_TRUE);
            glSamplerParameteri(gl_sampler, GL_TEXTURE_COMPARE_FUNC, static_cast<GLint>(CompareOperatorToGL(p_state.compare_op)));
        }

        glSamplerParameterf(gl_sampler, GL_TEXTURE_LOD_BIAS, p_state.lod_bias);
        glSamplerParameterf(gl_sampler, GL_TEXTURE_MIN_LOD, p_state.min_lod);
        glSamplerParameterf(gl_sampler, GL_TEXTURE_MAX_LOD, p_state.max_lod);

        return SamplerID(static_cast<uint64_t>(gl_sampler));
    }

    void GLRenderingDeviceDriver::SamplerFree(SamplerID p_sampler)
    {
        if (!p_sampler.IsValid())
        {
            return;
        }

        GLuint gl_sampler = static_cast<GLuint>(p_sampler.GetId());
        glDeleteSamplers(1, &gl_sampler);
    }

    VertexFormatID GLRenderingDeviceDriver::VertexFormatCreate(
      VectorView<VertexAttribute> p_vertex_attribs)
    {
        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        if (vao == 0)
        {
            return VertexFormatID();
        }

        state_cache_.BindVertexArray(vao);

        for (uint32_t i = 0; i < p_vertex_attribs.Size(); ++i)
        {
            const VertexAttribute &attr = p_vertex_attribs[i];

            glEnableVertexAttribArray(attr.location);

            GLint component_count = 4;
            uint32_t gl_type = GL_FLOAT;
            GLboolean normalized = GL_FALSE;

            switch (attr.format)
            {
            case DataFormat::kR8G8B8A8Unorm:
            case DataFormat::kR8G8B8A8Snorm:
            case DataFormat::kB8G8R8A8Unorm:
            case DataFormat::kR8G8B8A8Srgb:
            case DataFormat::kR8G8B8A8Uint:
            case DataFormat::kR8G8B8A8Sint:
                component_count = 4;
                gl_type = DataFormatToGLType(attr.format);
                break;
            case DataFormat::kR8G8Unorm:
            case DataFormat::kR8G8Snorm:
            case DataFormat::kR8G8Uint:
            case DataFormat::kR8G8Sint:
                component_count = 2;
                gl_type = DataFormatToGLType(attr.format);
                break;
            case DataFormat::kR8Unorm:
            case DataFormat::kR8Snorm:
            case DataFormat::kR8Uint:
            case DataFormat::kR8Sint:
                component_count = 1;
                gl_type = DataFormatToGLType(attr.format);
                break;
            case DataFormat::kR32G32B32A32Sfloat:
                component_count = 4;
                gl_type = GL_FLOAT;
                break;
            case DataFormat::kR32G32Sfloat:
                component_count = 2;
                gl_type = GL_FLOAT;
                break;
            case DataFormat::kR32Sfloat:
                component_count = 1;
                gl_type = GL_FLOAT;
                break;
            case DataFormat::kR16G16B16A16Sfloat:
                component_count = 4;
                gl_type = GL_HALF_FLOAT;
                break;
            case DataFormat::kR16G16Sfloat:
                component_count = 2;
                gl_type = GL_HALF_FLOAT;
                break;
            case DataFormat::kR16Sfloat:
                component_count = 1;
                gl_type = GL_HALF_FLOAT;
                break;
            default:
                component_count = 4;
                gl_type = GL_FLOAT;
                break;
            }

            if (attr.format == DataFormat::kR8G8B8A8Unorm ||
                attr.format == DataFormat::kR8G8Unorm ||
                attr.format == DataFormat::kR8Unorm ||
                attr.format == DataFormat::kB8G8R8A8Unorm ||
                attr.format == DataFormat::kR8G8B8A8Srgb)
            {
                normalized = GL_TRUE;
            }

            glVertexAttribPointer(
              attr.location,
              component_count,
              gl_type,
              normalized,
              static_cast<GLsizei>(attr.stride),
              reinterpret_cast<const void *>(static_cast<uintptr_t>(attr.offset)));

            if (attr.frequency == VertexFrequency::kInstance)
            {
                glVertexAttribDivisor(attr.location, 1);
            }
        }

        GLVertexFormatInfo *info = vertex_format_allocator_.Alloc();
        info->vao = vao;
        for (uint32_t i = 0; i < p_vertex_attribs.Size(); ++i)
        {
            info->attributes.PushBack(p_vertex_attribs[i]);
        }

        return VertexFormatID(reinterpret_cast<uint64_t>(info));
    }

    void GLRenderingDeviceDriver::VertexFormatFree(VertexFormatID p_vertex_format)
    {
        if (!p_vertex_format.IsValid())
        {
            return;
        }

        GLVertexFormatInfo *info = reinterpret_cast<GLVertexFormatInfo *>(
          p_vertex_format.GetId());
        if (!info)
        {
            return;
        }

        if (info->vao != 0)
        {
            glDeleteVertexArrays(1, &info->vao);
        }

        vertex_format_allocator_.Free(info);
    }

    ShaderID GLRenderingDeviceDriver::ShaderCreateFromGLSL(
      VectorView<ShaderStageSource> p_stage_sources,
      VectorView<ShaderUniform> p_uniforms,
      uint32_t p_push_constant_size)
    {
        (void)p_push_constant_size;

        if (p_stage_sources.Size() == 0)
        {
            return ShaderID();
        }

        LocalVector<uint32_t> compiled_shaders;

        for (uint32_t i = 0; i < p_stage_sources.Size(); ++i)
        {
            const ShaderStageSource &stage_src = p_stage_sources[i];
            if (stage_src.source == nullptr)
            {
                continue;
            }

            uint32_t gl_type = 0;
            switch (stage_src.stage)
            {
            case ShaderStage::kVertex:
                gl_type = GL_VERTEX_SHADER;
                break;
            case ShaderStage::kFragment:
                gl_type = GL_FRAGMENT_SHADER;
                break;
            case ShaderStage::kTessellationControl:
                gl_type = GL_TESS_CONTROL_SHADER;
                break;
            case ShaderStage::kTessellationEvaluation:
                gl_type = GL_TESS_EVALUATION_SHADER;
                break;
            case ShaderStage::kCompute:
                gl_type = GL_COMPUTE_SHADER;
                break;
            default:
                continue;
            }

            uint32_t shader_obj = CompileShader(stage_src.source, gl_type);
            if (shader_obj == 0)
            {
                for (uint32_t j = 0; j < compiled_shaders.Size(); ++j)
                {
                    glDeleteShader(compiled_shaders[j]);
                }
                return ShaderID();
            }

            compiled_shaders.PushBack(shader_obj);
        }

        if (compiled_shaders.Size() == 0)
        {
            return ShaderID();
        }

        uint32_t program = LinkProgram(compiled_shaders);
        if (program == 0)
        {
            for (uint32_t i = 0; i < compiled_shaders.Size(); ++i)
            {
                glDeleteShader(compiled_shaders[i]);
            }
            return ShaderID();
        }

        GLShaderInfo *info = shader_allocator_.Alloc();
        info->program = program;
        info->shader_objects = compiled_shaders;

        for (uint32_t i = 0; i < p_uniforms.Size(); ++i)
        {
            const ShaderUniform &uniform = p_uniforms[i];
            GLShaderInfo::UniformLocation loc;
            loc.binding = uniform.binding;
            loc.type = uniform.type;
            loc.location = -1;
            char name_buf[64];
            snprintf(name_buf, sizeof(name_buf), "u_%u", uniform.binding);
            loc.location = glGetUniformLocation(program, name_buf);
            info->uniform_locations.PushBack(loc);
        }

        return ShaderID(reinterpret_cast<uint64_t>(info));
    }

    void GLRenderingDeviceDriver::ShaderFree(ShaderID p_shader)
    {
        if (!p_shader.IsValid())
        {
            return;
        }

        GLShaderInfo *info = reinterpret_cast<GLShaderInfo *>(
          p_shader.GetId());
        if (!info)
        {
            return;
        }

        if (info->program != 0)
        {
            glDeleteProgram(info->program);
        }
        for (uint32_t i = 0; i < info->shader_objects.Size(); ++i)
        {
            glDeleteShader(info->shader_objects[i]);
        }

        shader_allocator_.Free(info);
    }

    UniformSetID GLRenderingDeviceDriver::UniformSetCreate(
      VectorView<BoundUniform> p_uniforms,
      ShaderID p_shader,
      uint32_t p_set_index)
    {
        GLUniformSetInfo *info = uniform_set_allocator_.Alloc();
        info->shader = p_shader;
        info->set_index = p_set_index;

        for (uint32_t i = 0; i < p_uniforms.Size(); ++i)
        {
            const BoundUniform &uniform = p_uniforms[i];
            GLUniformSetInfo::BoundResource res;
            res.type = uniform.type;
            res.binding = uniform.binding;

            if (uniform.ids.Size() > 0)
            {
                res.gl_name = static_cast<uint32_t>(uniform.ids[0].GetId());
            }

            info->resources.PushBack(res);
        }

        return UniformSetID(reinterpret_cast<uint64_t>(info));
    }

    void GLRenderingDeviceDriver::UniformSetFree(UniformSetID p_uniform_set)
    {
        if (!p_uniform_set.IsValid())
        {
            return;
        }

        GLUniformSetInfo *info = reinterpret_cast<GLUniformSetInfo *>(
          p_uniform_set.GetId());
        if (!info)
        {
            return;
        }

        uniform_set_allocator_.Free(info);
    }

    RenderPassID GLRenderingDeviceDriver::RenderPassCreate(
      VectorView<Attachment> p_attachments,
      VectorView<Subpass> p_subpasses,
      VectorView<SubpassDependency> p_subpass_dependencies)
    {
        (void)p_subpasses;
        (void)p_subpass_dependencies;

        GLRenderPassInfo *info = render_pass_allocator_.Alloc();
        for (uint32_t i = 0; i < p_attachments.Size(); ++i)
        {
            info->attachments.PushBack(p_attachments[i]);
        }

        return RenderPassID(reinterpret_cast<uint64_t>(info));
    }

    void GLRenderingDeviceDriver::RenderPassFree(RenderPassID p_render_pass)
    {
        if (!p_render_pass.IsValid())
        {
            return;
        }

        GLRenderPassInfo *info = reinterpret_cast<GLRenderPassInfo *>(
          p_render_pass.GetId());
        if (!info)
        {
            return;
        }

        render_pass_allocator_.Free(info);
    }

    FramebufferID GLRenderingDeviceDriver::FramebufferCreate(
      RenderPassID p_render_pass,
      VectorView<TextureID> p_attachments,
      uint32_t p_width,
      uint32_t p_height)
    {
        (void)p_render_pass;

        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        if (fbo == 0)
        {
            return FramebufferID();
        }

        state_cache_.BindFramebuffer(fbo);

        GLFramebufferInfo *info = framebuffer_allocator_.Alloc();
        info->fbo = fbo;
        info->width = p_width;
        info->height = p_height;

        uint32_t color_index = 0;
        for (uint32_t i = 0; i < p_attachments.Size(); ++i)
        {
            GLuint tex = static_cast<GLuint>(p_attachments[i].GetId());
            if (tex == 0)
            {
                continue;
            }

            GLenum attachment = GL_COLOR_ATTACHMENT0 + color_index;
            glFramebufferTexture2D(GL_FRAMEBUFFER, attachment,
                                      GL_TEXTURE_2D, tex, 0);
            info->color_attachments.PushBack(tex);
            ++color_index;
        }

        if (color_index > 0)
        {
            GLenum *draw_buffers = static_cast<GLenum *>(
              memory::Alloc(sizeof(GLenum) * color_index));
            for (uint32_t i = 0; i < color_index; ++i)
            {
                draw_buffers[i] = GL_COLOR_ATTACHMENT0 + i;
            }
            glDrawBuffers(static_cast<GLsizei>(color_index), draw_buffers);
            memory::Free(draw_buffers);
        }

        return FramebufferID(reinterpret_cast<uint64_t>(info));
    }

    void GLRenderingDeviceDriver::FramebufferFree(FramebufferID p_framebuffer)
    {
        if (!p_framebuffer.IsValid())
        {
            return;
        }

        GLFramebufferInfo *info = reinterpret_cast<GLFramebufferInfo *>(
          p_framebuffer.GetId());
        if (!info)
        {
            return;
        }

        if (info->fbo != 0)
        {
            glDeleteFramebuffers(1, &info->fbo);
        }

        framebuffer_allocator_.Free(info);
    }

    PipelineID GLRenderingDeviceDriver::RenderPipelineCreate(
      ShaderID p_shader,
      VertexFormatID p_vertex_format,
      RenderPrimitive p_render_primitive,
      const PipelineRasterizationState &p_rasterization_state,
      const PipelineMultisampleState &p_multisample_state,
      const PipelineDepthStencilState &p_depth_stencil_state,
      const PipelineColorBlendState &p_blend_state,
      BitField<PipelineDynamicStateFlags> p_dynamic_state,
      RenderPassID p_render_pass)
    {
        (void)p_vertex_format;
        (void)p_multisample_state;
        (void)p_render_pass;

        GLPipelineState *pipeline = pipeline_allocator_.Alloc();

        GLShaderInfo *shader_info = reinterpret_cast<GLShaderInfo *>(
          p_shader.GetId());
        pipeline->shader_program = shader_info ? shader_info->program : 0;

        pipeline->render_primitive = p_render_primitive;

        pipeline->cull_enable = (p_rasterization_state.cull_mode != PolygonCullMode::kDisabled);
        switch (p_rasterization_state.cull_mode)
        {
        case PolygonCullMode::kFront:
            pipeline->cull_mode = GL_FRONT;
            break;
        case PolygonCullMode::kBack:
            pipeline->cull_mode = GL_BACK;
            break;
        default:
            pipeline->cull_mode = GL_BACK;
            break;
        }

        pipeline->front_face = (p_rasterization_state.front_face == PolygonFrontFace::kClockwise)
                                   ? GL_CW
                                   : GL_CCW;

        pipeline->depth_test_enable = p_depth_stencil_state.enable_depth_test;
        pipeline->depth_write_enable = p_depth_stencil_state.enable_depth_write;
        pipeline->depth_func = CompareOperatorToGL(p_depth_stencil_state.depth_compare_operator);

        pipeline->blend_enable = false;
        if (p_blend_state.attachments.Size() > 0)
        {
            const PipelineColorBlendAttachment &blend_att = p_blend_state.attachments[0];
            pipeline->blend_enable = blend_att.enable_blend;
            pipeline->blend_src_rgb = BlendFactorToGL(blend_att.src_color_blend_factor);
            pipeline->blend_dst_rgb = BlendFactorToGL(blend_att.dst_color_blend_factor);
            pipeline->blend_src_alpha = BlendFactorToGL(blend_att.src_alpha_blend_factor);
            pipeline->blend_dst_alpha = BlendFactorToGL(blend_att.dst_alpha_blend_factor);
        }

        pipeline->wireframe = p_rasterization_state.wireframe;
        pipeline->dynamic_state = p_dynamic_state;

        return PipelineID(reinterpret_cast<uint64_t>(pipeline));
    }

    void GLRenderingDeviceDriver::PipelineFree(PipelineID p_pipeline)
    {
        if (!p_pipeline.IsValid())
        {
            return;
        }

        GLPipelineState *pipeline = reinterpret_cast<GLPipelineState *>(
          p_pipeline.GetId());
        if (!pipeline)
        {
            return;
        }

        pipeline_allocator_.Free(pipeline);
    }

    ICommandBuffer *GLRenderingDeviceDriver::CommandBufferCreate()
    {
        void *mem = command_buffer_allocator_.Alloc(this);
        if (!mem)
        {
            return nullptr;
        }

        return static_cast<ICommandBuffer *>(mem);
    }

    void GLRenderingDeviceDriver::CommandBufferFree(ICommandBuffer *p_command_buffer)
    {
        if (!p_command_buffer)
        {
            return;
        }

        GLCommandBuffer *gl_cmd = static_cast<GLCommandBuffer *>(p_command_buffer);
        gl_cmd->~GLCommandBuffer();
        command_buffer_allocator_.Free(gl_cmd);
    }

    void GLRenderingDeviceDriver::CommandBeginRenderPass(
      RenderPassID p_render_pass,
      FramebufferID p_framebuffer,
      VectorView<RenderPassClearValue> p_clear_values,
      int32_t p_rect_x, int32_t p_rect_y,
      uint32_t p_rect_w, uint32_t p_rect_h)
    {
        (void)p_render_pass;

        GLFramebufferInfo *fb_info = nullptr;
        if (p_framebuffer.IsValid())
        {
            fb_info = reinterpret_cast<GLFramebufferInfo *>(
              p_framebuffer.GetId());
        }

        if (fb_info)
        {
            state_cache_.BindFramebuffer(fb_info->fbo);
        }
        else
        {
            state_cache_.BindFramebuffer(0);
        }

        state_cache_.SetViewport(p_rect_x, p_rect_y, p_rect_w, p_rect_h);

        GLbitfield clear_mask = 0;

        GLRenderPassInfo *rp_info = nullptr;
        if (p_render_pass.IsValid())
        {
            rp_info = reinterpret_cast<GLRenderPassInfo *>(
              p_render_pass.GetId());
        }

        uint32_t clear_index = 0;
        uint32_t attachment_count = rp_info ? rp_info->attachments.Size() : 1;

        for (uint32_t i = 0; i < attachment_count && clear_index < p_clear_values.Size(); ++i)
        {
            bool is_depth = false;
            if (rp_info)
            {
                const Attachment &att = rp_info->attachments[i];
                is_depth = (att.format == DataFormat::kD16Unorm ||
                            att.format == DataFormat::kD32Sfloat ||
                            att.format == DataFormat::kD24UnormS8Uint ||
                            att.format == DataFormat::kD32SfloatS8Uint);
            }

            if (!is_depth)
            {
                if (rp_info && rp_info->attachments[i].load_op == AttachmentLoadOp::kClear)
                {
                    const RenderPassClearValue &cv = p_clear_values[clear_index];
                    glClearColor(cv.color[0], cv.color[1], cv.color[2], cv.color[3]);
                    clear_mask |= GL_COLOR_BUFFER_BIT;
                }
            }
            else
            {
                if (rp_info && rp_info->attachments[i].load_op == AttachmentLoadOp::kClear)
                {
                    const RenderPassClearValue &cv = p_clear_values[clear_index];
                    glClearDepth(static_cast<GLdouble>(cv.depth));
                    clear_mask |= GL_DEPTH_BUFFER_BIT;
                }
                if (rp_info && rp_info->attachments[i].stencil_load_op == AttachmentLoadOp::kClear)
                {
                    clear_mask |= GL_STENCIL_BUFFER_BIT;
                }
            }
            ++clear_index;
        }

        if (clear_mask != 0)
        {
            glClear(clear_mask);
        }
    }

    void GLRenderingDeviceDriver::CommandEndRenderPass()
    {
    }

    void GLRenderingDeviceDriver::CommandBindRenderPipeline(PipelineID p_pipeline)
    {
        if (!p_pipeline.IsValid())
        {
            return;
        }

        GLPipelineState *pipeline = reinterpret_cast<GLPipelineState *>(
          p_pipeline.GetId());
        if (!pipeline)
        {
            return;
        }

        state_cache_.UseProgram(pipeline->shader_program);

        state_cache_.SetCullFace(pipeline->cull_enable);
        if (pipeline->cull_enable)
        {
            state_cache_.SetCullMode(pipeline->cull_mode);
            state_cache_.SetFrontFace(pipeline->front_face);
        }

        state_cache_.SetDepthTest(pipeline->depth_test_enable);
        state_cache_.SetDepthWrite(pipeline->depth_write_enable);
        state_cache_.SetDepthFunc(pipeline->depth_func);

        state_cache_.SetBlend(pipeline->blend_enable);
        if (pipeline->blend_enable)
        {
            state_cache_.SetBlendFunc(pipeline->blend_src_rgb, pipeline->blend_dst_rgb,
                                      pipeline->blend_src_alpha, pipeline->blend_dst_alpha);
        }
    }

    void GLRenderingDeviceDriver::CommandBindUniformSet(
      UniformSetID p_uniform_set, uint32_t p_set_index)
    {
        (void)p_set_index;

        if (!p_uniform_set.IsValid())
        {
            return;
        }

        GLUniformSetInfo *info = reinterpret_cast<GLUniformSetInfo *>(
          p_uniform_set.GetId());
        if (!info)
        {
            return;
        }

        uint32_t texture_unit = 0;

        for (uint32_t i = 0; i < info->resources.Size(); ++i)
        {
            const GLUniformSetInfo::BoundResource &res = info->resources[i];

            switch (res.type)
            {
            case UniformType::kSamplerWithTexture:
            case UniformType::kTexture:
            case UniformType::kInputAttachment:
                if (texture_unit < kMaxTextureUnits)
                {
                    state_cache_.BindTexture2D(texture_unit, res.gl_name);
                    if (info->shader.IsValid())
                    {
                        GLShaderInfo *shader_info = reinterpret_cast<GLShaderInfo *>(
                          info->shader.GetId());
                        if (shader_info)
                        {
                            int32_t loc = shader_info->FindLocation(res.binding);
                            if (loc >= 0)
                            {
                                glUniform1i(loc, static_cast<GLint>(texture_unit));
                            }
                        }
                    }
                    state_cache_.BindSampler(texture_unit, 0);
                    ++texture_unit;
                }
                break;

            case UniformType::kUniformBuffer:
            case UniformType::kUniformBufferDynamic:
                state_cache_.BindUniformBuffer(res.binding, res.gl_name);
                break;

            default:
                break;
            }
        }
    }

    void GLRenderingDeviceDriver::CommandBindVertexBuffers(
      const BufferID *p_buffers,
      const uint64_t *p_offsets,
      uint32_t p_count)
    {
        for (uint32_t i = 0; i < p_count; ++i)
        {
            if (!p_buffers[i].IsValid())
            {
                continue;
            }

            GLuint gl_buffer = static_cast<GLuint>(p_buffers[i].GetId());
            state_cache_.BindVertexBuffer(gl_buffer);

            (void)p_offsets;
        }
    }

    void GLRenderingDeviceDriver::CommandBindIndexBuffer(
      BufferID p_buffer,
      IndexBufferFormat p_format,
      uint64_t p_offset)
    {
        (void)p_format;
        (void)p_offset;

        if (!p_buffer.IsValid())
        {
            return;
        }

        GLuint gl_buffer = static_cast<GLuint>(p_buffer.GetId());
        state_cache_.BindIndexBuffer(gl_buffer);
    }

    void GLRenderingDeviceDriver::CommandDraw(
      uint32_t p_vertex_count,
      uint32_t p_instance_count,
      uint32_t p_base_vertex,
      uint32_t p_first_instance)
    {
        (void)p_first_instance;

        if (p_instance_count <= 1)
        {
            glDrawArrays(GL_TRIANGLES, static_cast<GLint>(p_base_vertex),
                            static_cast<GLsizei>(p_vertex_count));
        }
        else
        {
            glDrawArraysInstanced(GL_TRIANGLES, static_cast<GLint>(p_base_vertex),
                                     static_cast<GLsizei>(p_vertex_count),
                                     static_cast<GLsizei>(p_instance_count));
        }
    }

    void GLRenderingDeviceDriver::CommandDrawIndexed(
      uint32_t p_index_count,
      uint32_t p_instance_count,
      uint32_t p_first_index,
      int32_t p_vertex_offset,
      uint32_t p_first_instance)
    {
        (void)p_instance_count;
        (void)p_first_instance;

        GLenum gl_type = GL_UNSIGNED_INT;
        const void *indices = reinterpret_cast<const void *>(
          static_cast<uintptr_t>(p_first_index * sizeof(uint32_t)));

        glDrawElements(GL_TRIANGLES,
                          static_cast<GLsizei>(p_index_count),
                          gl_type, indices);
    }

    void GLRenderingDeviceDriver::CommandSetViewport(
      int32_t p_x, int32_t p_y,
      uint32_t p_width, uint32_t p_height)
    {
        state_cache_.SetViewport(p_x, p_y, p_width, p_height);
    }

    void GLRenderingDeviceDriver::CommandSetScissor(
      int32_t p_x, int32_t p_y,
      uint32_t p_width, uint32_t p_height)
    {
        state_cache_.SetScissor(p_x, p_y, p_width, p_height);
    }

    void GLRenderingDeviceDriver::CommandSetBlendConstants(
      float p_r, float p_g, float p_b, float p_a)
    {
        glBlendColor(p_r, p_g, p_b, p_a);
    }

    void GLRenderingDeviceDriver::CommandClearBuffer(
      BufferID p_buffer,
      uint64_t p_offset,
      uint64_t p_size)
    {
        if (!p_buffer.IsValid())
        {
            return;
        }

        GLuint gl_buffer = static_cast<GLuint>(p_buffer.GetId());
        state_cache_.BindVertexBuffer(gl_buffer);

        if (p_size > 0)
        {
            uint8_t *zero_data = static_cast<uint8_t *>(memory::Alloc(static_cast<uint64_t>(p_size)));
            if (zero_data)
            {
                for (uint64_t i = 0; i < p_size; ++i)
                {
                    zero_data[i] = 0;
                }
                glBufferSubData(GL_ARRAY_BUFFER,
                                    static_cast<GLintptr>(p_offset),
                                    static_cast<GLsizeiptr>(p_size),
                                    zero_data);
                memory::Free(zero_data);
            }
        }
    }

    void GLRenderingDeviceDriver::CommandCopyBuffer(
      BufferID p_src_buffer,
      BufferID p_dst_buffer,
      VectorView<BufferCopyRegion> p_regions)
    {
        if (!p_src_buffer.IsValid() || !p_dst_buffer.IsValid())
        {
            return;
        }

        GLuint src_gl = static_cast<GLuint>(p_src_buffer.GetId());
        GLuint dst_gl = static_cast<GLuint>(p_dst_buffer.GetId());

        for (uint32_t i = 0; i < p_regions.Size(); ++i)
        {
            const BufferCopyRegion &region = p_regions[i];

            state_cache_.BindVertexBuffer(src_gl);

            uint8_t *src_data = static_cast<uint8_t *>(
              glMapBuffer(GL_ARRAY_BUFFER, GL_READ_WRITE));

            if (src_data)
            {
                state_cache_.BindVertexBuffer(dst_gl);
                glBufferSubData(GL_ARRAY_BUFFER,
                                    static_cast<GLintptr>(region.dst_offset),
                                    static_cast<GLsizeiptr>(region.size),
                                    src_data + region.src_offset);
            }

            if (src_data)
            {
                glUnmapBuffer(GL_ARRAY_BUFFER);
            }
        }
    }

    void GLRenderingDeviceDriver::CommandCopyBufferToTexture(
      BufferID p_src_buffer,
      TextureID p_dst_texture,
      VectorView<BufferTextureCopyRegion> p_regions)
    {
        if (!p_src_buffer.IsValid() || !p_dst_texture.IsValid())
        {
            return;
        }

        GLTextureInfo *tex_info = nullptr;
        if (texture_infos_.Has(p_dst_texture.GetId()))
        {
            tex_info = &texture_infos_[p_dst_texture.GetId()];
        }
        if (tex_info == nullptr)
        {
            return;
        }

        GLuint src_gl = static_cast<GLuint>(p_src_buffer.GetId());
        GLuint dst_gl = static_cast<GLuint>(p_dst_texture.GetId());

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, src_gl);

        state_cache_.BindTexture(tex_info->target, dst_gl);

        uint32_t gl_format = DataFormatToGLFormat(tex_info->format);
        uint32_t gl_type = DataFormatToGLType(tex_info->format);

        for (uint32_t i = 0; i < p_regions.Size(); ++i)
        {
            const BufferTextureCopyRegion &region = p_regions[i];

            if (tex_info->target == GL_TEXTURE_2D ||
                tex_info->target == GL_TEXTURE_CUBE_MAP)
            {
                GLenum tex_target = tex_info->target;
                if (tex_info->target == GL_TEXTURE_CUBE_MAP)
                {
                    tex_target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + region.texture_subresource.layer;
                }

                glTexSubImage2D(tex_target,
                                static_cast<GLint>(region.texture_subresource.mipmap),
                                region.texture_offset_x,
                                region.texture_offset_y,
                                region.texture_region_width,
                                region.texture_region_height,
                                gl_format,
                                gl_type,
                                reinterpret_cast<const void *>(static_cast<uintptr_t>(region.buffer_offset)));
            }
            else if (tex_info->target == GL_TEXTURE_2D_ARRAY ||
                     tex_info->target == GL_TEXTURE_3D)
            {
                glTexSubImage3D(tex_info->target,
                                static_cast<GLint>(region.texture_subresource.mipmap),
                                region.texture_offset_x,
                                region.texture_offset_y,
                                region.texture_offset_z,
                                region.texture_region_width,
                                region.texture_region_height,
                                region.texture_region_depth,
                                gl_format,
                                gl_type,
                                reinterpret_cast<const void *>(static_cast<uintptr_t>(region.buffer_offset)));
            }
        }

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }

    void GLRenderingDeviceDriver::CommandClearColorTexture(
      TextureID p_texture,
      float p_color_r, float p_color_g,
      float p_color_b, float p_color_a,
      const TextureSubresourceRange &p_subresources)
    {
        (void)p_subresources;

        if (!p_texture.IsValid())
        {
            return;
        }

        GLuint gl_texture = static_cast<GLuint>(p_texture.GetId());

        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        if (fbo == 0)
        {
            return;
        }

        state_cache_.BindFramebuffer(fbo);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_TEXTURE_2D, gl_texture, 0);

        GLenum draw_buf = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &draw_buf);

        glClearColor(p_color_r, p_color_g, p_color_b, p_color_a);

        glEnable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);

        glClear(GL_COLOR_BUFFER_BIT);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_TEXTURE_2D, 0, 0);

        glDeleteFramebuffers(1, &fbo);
    }

    void GLRenderingDeviceDriver::CommandClearDepthStencilTexture(
      TextureID p_texture,
      float p_depth, uint32_t p_stencil,
      const TextureSubresourceRange &p_subresources)
    {
        (void)p_stencil;
        (void)p_subresources;

        if (!p_texture.IsValid())
        {
            return;
        }

        GLuint gl_texture = static_cast<GLuint>(p_texture.GetId());

        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        if (fbo == 0)
        {
            return;
        }

        state_cache_.BindFramebuffer(fbo);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_TEXTURE_2D, gl_texture, 0);

        glClearDepth(static_cast<GLdouble>(p_depth));

        glEnable(GL_DEPTH_TEST);

        glClear(GL_DEPTH_BUFFER_BIT);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_TEXTURE_2D, 0, 0);

        glDeleteFramebuffers(1, &fbo);
    }

    void GLRenderingDeviceDriver::BeginSegment(
      uint32_t p_frame_index,
      uint32_t p_frames_drawn)
    {
        frame_index_ = p_frame_index;
        frames_drawn_ = p_frames_drawn;
        state_cache_.Reset();
    }

    void GLRenderingDeviceDriver::EndSegment()
    {
    }

    uint64_t GLRenderingDeviceDriver::LimitGet(Limit p_limit)
    {
        uint32_t idx = static_cast<uint32_t>(p_limit);
        if (idx < static_cast<uint32_t>(Limit::kMax))
        {
            return limits_[idx];
        }
        return 0;
    }

    bool GLRenderingDeviceDriver::HasFeature(Features p_feature)
    {
        (void)p_feature;
        return false;
    }

    const Capabilities &GLRenderingDeviceDriver::GetCapabilities() const
    {
        return capabilities_;
    }

    const char *GLRenderingDeviceDriver::GetApiName() const
    {
        return api_name_;
    }

    const char *GLRenderingDeviceDriver::GetPipelineCacheUuid() const
    {
        return pipeline_cache_uuid_;
    }

    void GLRenderingDeviceDriver::QueryLimits()
    {
        GLint val = 0;

        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &val);
        limits_[static_cast<uint32_t>(Limit::kMaxTextureSize2D)] = static_cast<uint64_t>(val);

        glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE, &val);
        limits_[static_cast<uint32_t>(Limit::kMaxTextureSizeCube)] = static_cast<uint64_t>(val);

        glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &val);
        limits_[static_cast<uint32_t>(Limit::kMaxTexturesPerShaderStage)] = static_cast<uint64_t>(val);
        limits_[static_cast<uint32_t>(Limit::kMaxSamplersPerShaderStage)] = static_cast<uint64_t>(val);

        glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &val);
        limits_[static_cast<uint32_t>(Limit::kMaxUniformBufferSize)] = static_cast<uint64_t>(val);

        glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &val);
        limits_[static_cast<uint32_t>(Limit::kMaxVertexInputAttributes)] = static_cast<uint64_t>(val);
        limits_[static_cast<uint32_t>(Limit::kMaxVertexInputBindings)] = static_cast<uint64_t>(val);

        glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &val);
        limits_[static_cast<uint32_t>(Limit::kMaxTextureArrayLayers)] = static_cast<uint64_t>(val);

        glGetIntegerv(GL_MAX_FRAMEBUFFER_WIDTH, &val);
        limits_[static_cast<uint32_t>(Limit::kMaxFramebufferWidth)] = static_cast<uint64_t>(val);

        glGetIntegerv(GL_MAX_FRAMEBUFFER_HEIGHT, &val);
        limits_[static_cast<uint32_t>(Limit::kMaxFramebufferHeight)] = static_cast<uint64_t>(val);

        glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &val);
        limits_[static_cast<uint32_t>(Limit::kMinUniformBufferOffsetAlignment)] = static_cast<uint64_t>(val);

        limits_[static_cast<uint32_t>(Limit::kMaxBoundUniformSets)] = 4;
        limits_[static_cast<uint32_t>(Limit::kMaxFramebufferColorAttachments)] = 4;
        limits_[static_cast<uint32_t>(Limit::kMaxTexturesPerUniformSet)] = 16;
        limits_[static_cast<uint32_t>(Limit::kMaxSamplersPerUniformSet)] = 16;
        limits_[static_cast<uint32_t>(Limit::kMaxUniformBuffersPerUniformSet)] = 12;
        limits_[static_cast<uint32_t>(Limit::kMaxStorageBuffersPerUniformSet)] = 8;
        limits_[static_cast<uint32_t>(Limit::kMaxStorageImagesPerUniformSet)] = 8;
        limits_[static_cast<uint32_t>(Limit::kMaxDrawIndexedIndex)] = 0x00FFFFFF;
        limits_[static_cast<uint32_t>(Limit::kMaxPushConstantSize)] = 128;
        limits_[static_cast<uint32_t>(Limit::kMaxVertexInputAttributeOffset)] = 2048;
        limits_[static_cast<uint32_t>(Limit::kMaxVertexInputBindingStride)] = 2048;
        limits_[static_cast<uint32_t>(Limit::kMaxViewportDimensionsX)] = 4096;
        limits_[static_cast<uint32_t>(Limit::kMaxViewportDimensionsY)] = 4096;
        limits_[static_cast<uint32_t>(Limit::kMaxShaderVaryings)] = 60;
    }

    uint32_t GLRenderingDeviceDriver::CompileShader(
      const char *p_source, uint32_t p_type)
    {
        if (!p_source)
        {
            return 0;
        }

        GLuint shader = glCreateShader(p_type);
        if (shader == 0)
        {
            ARHUD_LOG_ERROR("kFailed", "GLRenderingDeviceDriver: Failed to create shader object");
            return 0;
        }

        glShaderSource(shader, 1, &p_source, nullptr);

        glCompileShader(shader);

        GLint compile_status = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compile_status);

        if (compile_status == 0)
        {
            GLint log_length = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);

            if (log_length > 0)
            {
                char *log = static_cast<char *>(memory::Alloc(static_cast<uint64_t>(log_length + 1)));
                if (log)
                {
                    glGetShaderInfoLog(shader, log_length, nullptr, log);
                    ARHUD_LOG_ERROR("GLRenderingDeviceDriver: Shader compile error: %s", log);
                    memory::Free(log);
                }
            }

            glDeleteShader(shader);
            return 0;
        }

        return shader;
    }

    uint32_t GLRenderingDeviceDriver::LinkProgram(
      const LocalVector<uint32_t> &p_shader_objects)
    {
        GLuint program = glCreateProgram();
        if (program == 0)
        {
            ARHUD_LOG_ERROR("kFailed", "GLRenderingDeviceDriver: Failed to create program object");
            return 0;
        }

        for (uint32_t i = 0; i < p_shader_objects.Size(); ++i)
        {
            glAttachShader(program, p_shader_objects[i]);
        }

        glLinkProgram(program);

        GLint link_status = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &link_status);

        if (link_status == 0)
        {
            GLint log_length = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);

            if (log_length > 0)
            {
                char *log = static_cast<char *>(memory::Alloc(static_cast<uint64_t>(log_length + 1)));
                if (log)
                {
                    glGetProgramInfoLog(program, log_length, nullptr, log);
                    ARHUD_LOG_ERROR("GLRenderingDeviceDriver: Program link error: %s", log);
                    memory::Free(log);
                }
            }

            glDeleteProgram(program);
            return 0;
        }

        return program;
    }

    uint32_t GLRenderingDeviceDriver::DataFormatToGLInternalFormat(DataFormat p_format)
    {
        switch (p_format)
        {
        case DataFormat::kR8Unorm:         return GL_R8;
        case DataFormat::kR8G8Unorm:       return GL_RG8;
        case DataFormat::kR8G8B8A8Unorm:   return GL_RGBA8;
        case DataFormat::kR8G8B8A8Srgb:    return GL_SRGB8_ALPHA8;
        case DataFormat::kB8G8R8A8Unorm:   return GL_RGBA8;
        case DataFormat::kR16Sfloat:       return GL_R16F;
        case DataFormat::kR16G16Sfloat:    return GL_RG16F;
        case DataFormat::kR16G16B16A16Sfloat: return GL_RGBA16F;
        case DataFormat::kR32Sfloat:       return GL_R32F;
        case DataFormat::kR32G32Sfloat:    return GL_RG32F;
        case DataFormat::kR32G32B32A32Sfloat: return GL_RGBA32F;
        case DataFormat::kD16Unorm:        return GL_DEPTH_COMPONENT16;
        case DataFormat::kD32Sfloat:       return GL_DEPTH_COMPONENT32F;
        case DataFormat::kD24UnormS8Uint:  return GL_DEPTH24_STENCIL8;
        case DataFormat::kD32SfloatS8Uint: return GL_DEPTH32F_STENCIL8;
        default:                           return GL_RGBA8;
        }
    }

    uint32_t GLRenderingDeviceDriver::DataFormatToGLType(DataFormat p_format)
    {
        switch (p_format)
        {
        case DataFormat::kR8Unorm:
        case DataFormat::kR8G8Unorm:
        case DataFormat::kR8G8B8A8Unorm:
        case DataFormat::kB8G8R8A8Unorm:
        case DataFormat::kR8G8B8A8Srgb:
            return GL_UNSIGNED_BYTE;
        case DataFormat::kR8Snorm:
        case DataFormat::kR8G8Snorm:
        case DataFormat::kR8G8B8A8Snorm:
            return GL_BYTE;
        case DataFormat::kR8Uint:
        case DataFormat::kR8G8Uint:
        case DataFormat::kR8G8B8A8Uint:
            return GL_UNSIGNED_INT;
        case DataFormat::kR8Sint:
        case DataFormat::kR8G8Sint:
        case DataFormat::kR8G8B8A8Sint:
            return GL_INT;
        case DataFormat::kR16Sfloat:
        case DataFormat::kR16G16Sfloat:
        case DataFormat::kR16G16B16A16Sfloat:
            return GL_HALF_FLOAT;
        case DataFormat::kR32Sfloat:
        case DataFormat::kR32G32Sfloat:
        case DataFormat::kR32G32B32A32Sfloat:
            return GL_FLOAT;
        case DataFormat::kD16Unorm:
            return GL_UNSIGNED_SHORT;
        case DataFormat::kD32Sfloat:
            return GL_FLOAT;
        case DataFormat::kD24UnormS8Uint:
            return GL_UNSIGNED_INT;
        case DataFormat::kD32SfloatS8Uint:
            return GL_FLOAT;
        default:
            return GL_UNSIGNED_BYTE;
        }
    }

    uint32_t GLRenderingDeviceDriver::DataFormatToGLFormat(DataFormat p_format)
    {
        switch (p_format)
        {
        case DataFormat::kR8Unorm:
        case DataFormat::kR8Snorm:
        case DataFormat::kR8Uint:
        case DataFormat::kR8Sint:
        case DataFormat::kR16Sfloat:
        case DataFormat::kR32Sfloat:
            return GL_RED;
        case DataFormat::kR8G8Unorm:
        case DataFormat::kR8G8Snorm:
        case DataFormat::kR8G8Uint:
        case DataFormat::kR8G8Sint:
        case DataFormat::kR16G16Sfloat:
        case DataFormat::kR32G32Sfloat:
            return GL_RG;
        case DataFormat::kR8G8B8A8Unorm:
        case DataFormat::kR8G8B8A8Snorm:
        case DataFormat::kR8G8B8A8Uint:
        case DataFormat::kR8G8B8A8Sint:
        case DataFormat::kR8G8B8A8Srgb:
        case DataFormat::kR16G16B16A16Sfloat:
        case DataFormat::kR32G32B32A32Sfloat:
            return GL_RGBA;
        case DataFormat::kB8G8R8A8Unorm:
            return GL_BGRA;
        case DataFormat::kD16Unorm:
        case DataFormat::kD32Sfloat:
            return GL_DEPTH_COMPONENT;
        case DataFormat::kD24UnormS8Uint:
        case DataFormat::kD32SfloatS8Uint:
            return GL_DEPTH_STENCIL;
        default:
            return GL_RGBA;
        }
    }

    uint32_t GLRenderingDeviceDriver::RenderPrimitiveToGLMode(RenderPrimitive p_primitive)
    {
        switch (p_primitive)
        {
        case RenderPrimitive::kPoints:                      return GL_POINTS;
        case RenderPrimitive::kLines:                       return GL_LINES;
        case RenderPrimitive::kLinesWithAdjacency:          return GL_LINES_ADJACENCY;
        case RenderPrimitive::kLineStrips:                  return GL_LINE_STRIP;
        case RenderPrimitive::kLinesStripsWithAdjacency:    return GL_LINE_STRIP_ADJACENCY;
        case RenderPrimitive::kTriangles:                   return GL_TRIANGLES;
        case RenderPrimitive::kTrianglesWithAdjacency:      return GL_TRIANGLES_ADJACENCY;
        case RenderPrimitive::kTriangleStrips:              return GL_TRIANGLE_STRIP;
        case RenderPrimitive::kTriangleStripsWithAdjacency: return GL_TRIANGLE_STRIP_ADJACENCY;
        default:                                            return GL_TRIANGLES;
        }
    }

    uint32_t GLRenderingDeviceDriver::BlendFactorToGL(BlendFactor p_factor)
    {
        switch (p_factor)
        {
        case BlendFactor::kZero:                    return GL_ZERO;
        case BlendFactor::kOne:                     return GL_ONE;
        case BlendFactor::kSrcColor:                return GL_SRC_COLOR;
        case BlendFactor::kOneMinusSrcColor:        return GL_ONE_MINUS_SRC_COLOR;
        case BlendFactor::kDstColor:                return GL_DST_COLOR;
        case BlendFactor::kOneMinusDstColor:        return GL_ONE_MINUS_DST_COLOR;
        case BlendFactor::kSrcAlpha:                return GL_SRC_ALPHA;
        case BlendFactor::kOneMinusSrcAlpha:        return GL_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::kDstAlpha:                return GL_DST_ALPHA;
        case BlendFactor::kOneMinusDstAlpha:        return GL_ONE_MINUS_DST_ALPHA;
        case BlendFactor::kConstantColor:           return GL_CONSTANT_COLOR;
        case BlendFactor::kOneMinusConstantColor:   return GL_ONE_MINUS_CONSTANT_COLOR;
        case BlendFactor::kConstantAlpha:           return GL_CONSTANT_ALPHA;
        case BlendFactor::kOneMinusConstantAlpha:   return GL_ONE_MINUS_CONSTANT_ALPHA;
        case BlendFactor::kSrcAlphaSaturate:        return GL_SRC_ALPHA_SATURATE;
        default:                                    return GL_ZERO;
        }
    }

    uint32_t GLRenderingDeviceDriver::CompareOperatorToGL(CompareOperator p_op)
    {
        switch (p_op)
        {
        case CompareOperator::kNever:            return GL_NEVER;
        case CompareOperator::kLess:             return GL_LESS;
        case CompareOperator::kEqual:            return GL_EQUAL;
        case CompareOperator::kLessOrEqual:      return GL_LEQUAL;
        case CompareOperator::kGreater:          return GL_GREATER;
        case CompareOperator::kNotEqual:         return GL_NOTEQUAL;
        case CompareOperator::kGreaterOrEqual:   return GL_GEQUAL;
        case CompareOperator::kAlways:           return GL_ALWAYS;
        default:                                 return GL_ALWAYS;
        }
    }

    uint32_t GLRenderingDeviceDriver::SamplerFilterToGL(SamplerFilter p_filter)
    {
        switch (p_filter)
        {
        case SamplerFilter::kNearest: return GL_NEAREST;
        case SamplerFilter::kLinear:  return GL_LINEAR;
        default:                      return GL_NEAREST;
        }
    }

    uint32_t GLRenderingDeviceDriver::SamplerRepeatModeToGL(SamplerRepeatMode p_mode)
    {
        switch (p_mode)
        {
        case SamplerRepeatMode::kRepeat:           return GL_REPEAT;
        case SamplerRepeatMode::kMirroredRepeat:   return GL_MIRRORED_REPEAT;
        case SamplerRepeatMode::kClampToEdge:      return GL_CLAMP_TO_EDGE;
        case SamplerRepeatMode::kClampToBorder:    return GL_CLAMP_TO_BORDER;
        case SamplerRepeatMode::kMirrorClampToEdge: return GL_MIRROR_CLAMP_TO_EDGE;
        default:                                   return GL_CLAMP_TO_EDGE;
        }
    }

    uint32_t GLRenderingDeviceDriver::DataFormatGetPixelSize(DataFormat p_format)
    {
        switch (p_format)
        {
        case DataFormat::kR8Unorm:         return 1;
        case DataFormat::kR8G8Unorm:       return 2;
        case DataFormat::kR8G8B8A8Unorm:   return 4;
        case DataFormat::kR8G8B8A8Snorm:   return 4;
        case DataFormat::kB8G8R8A8Unorm:   return 4;
        case DataFormat::kR8G8B8A8Srgb:    return 4;
        case DataFormat::kR16Sfloat:       return 2;
        case DataFormat::kR16G16Sfloat:    return 4;
        case DataFormat::kR16G16B16A16Sfloat: return 8;
        case DataFormat::kR32Sfloat:       return 4;
        case DataFormat::kR32G32Sfloat:    return 8;
        case DataFormat::kR32G32B32A32Sfloat: return 16;
        case DataFormat::kD16Unorm:        return 2;
        case DataFormat::kD32Sfloat:       return 4;
        case DataFormat::kD24UnormS8Uint:  return 4;
        case DataFormat::kD32SfloatS8Uint: return 8;
        default:                           return 4;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // SwapChain 管理
    // ═══════════════════════════════════════════════════════════════════════

    IRenderingDeviceDriver::SwapChainID GLRenderingDeviceDriver::SwapChainCreate(
      IRenderingContextDriver::SurfaceID p_surface,
      IRenderingContextDriver::ContextID p_context)
    {
        if (p_surface == IRenderingContextDriver::kInvalidSurfaceId ||
            p_context == IRenderingContextDriver::kInvalidContextId)
        {
            return kInvalidSwapChainId;
        }

        if (!context_driver_)
        {
            return kInvalidSwapChainId;
        }

        IGLManager::SurfaceID gl_surf = context_driver_->SurfaceGetAPISurface(p_surface);
        if (gl_surf == IGLManager::kInvalidSurfaceId)
        {
            return kInvalidSwapChainId;
        }

        GLSwapChain sc;
        sc.rcd_surface = p_surface;
        sc.gl_context = static_cast<IGLManager::ContextID>(p_context);
        sc.gl_surface = gl_surf;

        swap_chains_.PushBack(sc);

        return static_cast<SwapChainID>(swap_chains_.Size());
    }

    void GLRenderingDeviceDriver::SwapChainDestroy(SwapChainID p_swapchain)
    {
        if (p_swapchain == kInvalidSwapChainId || p_swapchain > swap_chains_.Size())
        {
            return;
        }

        uint32_t index = static_cast<uint32_t>(p_swapchain) - 1;
        if (index < swap_chains_.Size())
        {
            swap_chains_[index].rcd_surface = IRenderingContextDriver::kInvalidSurfaceId;
            swap_chains_[index].gl_context = IGLManager::kInvalidContextId;
            swap_chains_[index].gl_surface = IGLManager::kInvalidSurfaceId;
        }
    }

    Error GLRenderingDeviceDriver::SwapChainAcquire(SwapChainID p_swapchain)
    {
        if (p_swapchain == kInvalidSwapChainId || p_swapchain > swap_chains_.Size())
        {
            return Error::kInvalidParameter;
        }

        uint32_t index = static_cast<uint32_t>(p_swapchain) - 1;
        const GLSwapChain &sc = swap_chains_[index];

        if (sc.gl_context == IGLManager::kInvalidContextId ||
            sc.gl_surface == IGLManager::kInvalidSurfaceId)
        {
            return Error::kInvalidParameter;
        }

        if (!gl_manager_)
        {
            return Error::kFailed;
        }

        return gl_manager_->MakeCurrent(sc.gl_context, sc.gl_surface);
    }

    void GLRenderingDeviceDriver::SwapChainPresent(SwapChainID p_swapchain)
    {
        if (p_swapchain == kInvalidSwapChainId || p_swapchain > swap_chains_.Size())
        {
            return;
        }

        uint32_t index = static_cast<uint32_t>(p_swapchain) - 1;
        const GLSwapChain &sc = swap_chains_[index];

        if (sc.gl_surface == IGLManager::kInvalidSurfaceId)
        {
            return;
        }

        if (!gl_manager_)
        {
            return;
        }

        gl_manager_->SwapBuffers(sc.gl_surface);
    }

} // namespace arhud
