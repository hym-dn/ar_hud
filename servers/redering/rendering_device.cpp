/**
 * @file rendering_device.cpp
 * @brief RenderingDevice 实现 — 资源生命周期管理、Staging Buffer、帧循环
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-10
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include "rendering_device.h"
#include "rendering_device_driver.h"
#include "os/memory.h"

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // 生命周期
    // ═══════════════════════════════════════════════════════════════════════

    RenderingDevice::~RenderingDevice()
    {
        if (initialized_)
        {
            Finalize();
        }
    }

    Error RenderingDevice::Initialize(IRenderingContextDriver *p_context_driver,
                                      uint32_t p_frame_count,
                                      uint32_t p_staging_buffer_size)
    {
        ARHUD_ASSERT(p_context_driver != nullptr, "RenderingContextDriver must not be null");
        ARHUD_ASSERT(p_frame_count >= 2 && p_frame_count <= kMaxFrameCount,
                     "Frame count must be between 2 and kMaxFrameCount");

        if (initialized_)
        {
            return Error::kAlreadyExists;
        }

        render_thread_id_ = Thread::GetCallerId();

        context_driver_ = p_context_driver;

        device_driver_ = context_driver_->CreateDeviceDriver();
        if (device_driver_ == nullptr)
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice: failed to create device driver");
            context_driver_ = nullptr;
            return Error::kFailed;
        }

        Error err = device_driver_->Initialize(0, p_frame_count);
        if (err != Error::kOK)
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice: failed to initialize device driver");
            context_driver_->DriverFree(device_driver_);
            device_driver_ = nullptr;
            context_driver_ = nullptr;
            return Error::kFailed;
        }

        frame_count_ = p_frame_count;
        staging_buffer_size_ = p_staging_buffer_size;
        frame_number_ = 0;

        err = InitializeStagingBuffer();
        if (err != Error::kOK)
        {
            context_driver_->DriverFree(device_driver_);
            device_driver_ = nullptr;
            context_driver_ = nullptr;
            return err;
        }

        err = InitializeCommandBufferPool();
        if (err != Error::kOK)
        {
            DestroyStagingBuffer();
            context_driver_->DriverFree(device_driver_);
            device_driver_ = nullptr;
            context_driver_ = nullptr;
            return err;
        }

        for (uint32_t i = 0; i < frame_count_; ++i)
        {
            frames_[i].Clear();
        }

        initialized_ = true;
        return Error::kOK;
    }

    void RenderingDevice::Finalize()
    {
        if (!initialized_)
        {
            return;
        }

        ARHUD_ASSERT(IsRenderThread(),
                     "Finalize() must be called on the render thread (Initialize was called on a different thread)");

        for (uint32_t i = 0; i < independent_device_infos_.Size(); ++i)
        {
            IndependentDeviceInfo &info = independent_device_infos_[i];
            RenderingDevice *indep_rd = info.device;

            ARHUD_ASSERT(!indep_rd->initialized_,
                         "Independent device must be finalized on its own rendering thread before parent Finalize()");

            if (indep_rd->device_driver_ != nullptr)
            {
                if (info.swapchain_id != IRenderingDeviceDriver::kInvalidSwapChainId)
                {
                    indep_rd->device_driver_->SwapChainDestroy(info.swapchain_id);
                }
                context_driver_->DriverFree(indep_rd->device_driver_);
            }

            if (info.surface_id != IRenderingContextDriver::kInvalidSurfaceId)
            {
                context_driver_->SurfaceDestroy(info.surface_id);
            }
            if (info.context_id != IRenderingContextDriver::kInvalidContextId)
            {
                context_driver_->ContextDestroy(info.context_id);
            }

            ARHUD_DELETE(indep_rd);
        }
        independent_devices_.Clear();
        independent_device_infos_.Clear();

        FreeAllResources();

        DestroyCommandBufferPool();

        DestroyStagingBuffer();

        dependency_map_.Reset();
        reverse_dependency_map_.Reset();

        if (device_driver_ != nullptr && context_driver_ != nullptr)
        {
            context_driver_->DriverFree(device_driver_);
        }
        device_driver_ = nullptr;
        context_driver_ = nullptr;
        initialized_ = false;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 独立渲染设备
    // ═══════════════════════════════════════════════════════════════════════

    RenderingDevice *RenderingDevice::CreateIndependentDevice(IScreen::ScreenID p_screen_id,
                                                              void *p_native_window,
                                                              uint32_t p_width,
                                                              uint32_t p_height)
    {
        if (!initialized_)
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::CreateIndependentDevice() not initialized");
            return nullptr;
        }

        if (!is_main_device_)
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::CreateIndependentDevice() only main device can create independent devices");
            return nullptr;
        }

        IRenderingContextDriver::SurfaceID surf_id = context_driver_->SurfaceCreate(
            p_screen_id, p_native_window, p_width, p_height);
        if (surf_id == IRenderingContextDriver::kInvalidSurfaceId)
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::CreateIndependentDevice() failed to create surface");
            return nullptr;
        }

        IRenderingContextDriver::ContextID ctx_id = context_driver_->ContextCreate(
            surf_id);
        if (ctx_id == IRenderingContextDriver::kInvalidContextId)
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::CreateIndependentDevice() failed to create context");
            context_driver_->SurfaceDestroy(surf_id);
            return nullptr;
        }

        auto *rd = ARHUD_NEW(RenderingDevice)();
        rd->is_main_device_ = false;

        SwapChainInfo sc_info;
        sc_info.context_id = ctx_id;
        sc_info.surface_id = surf_id;
        sc_info.swapchain_id = IRenderingDeviceDriver::kInvalidSwapChainId;
        rd->swapchain_infos_.PushBack(sc_info);

        independent_devices_.PushBack(rd);
        IndependentDeviceInfo device_info;
        device_info.device = rd;
        device_info.context_id = ctx_id;
        device_info.surface_id = surf_id;
        device_info.swapchain_id = IRenderingDeviceDriver::kInvalidSwapChainId;
        independent_device_infos_.PushBack(device_info);

        ARHUD_LOG_INFO("RenderingDevice: Independent device created (ContextID=%u, SurfaceID=%u)",
                       static_cast<uint32_t>(ctx_id), static_cast<uint32_t>(surf_id));
        return rd;
    }

    Error RenderingDevice::InitializeIndependentDevice(RenderingDevice *p_device)
    {
        ARHUD_ASSERT(p_device != nullptr, "Device must not be null");
        ARHUD_ASSERT(!p_device->initialized_, "Device already initialized");
        ARHUD_ASSERT(!p_device->is_main_device_, "Not an independent device");

        if (p_device->swapchain_infos_.IsEmpty())
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::InitializeIndependentDevice() no swapchain info");
            return Error::kFailed;
        }

        Error err = p_device->Initialize(context_driver_, frame_count_);
        if (err != Error::kOK)
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::InitializeIndependentDevice() failed to initialize RD");
            return err;
        }

        const SwapChainInfo &sc_info = p_device->swapchain_infos_[0];
        err = p_device->MakeCurrent(sc_info.context_id, sc_info.surface_id);
        if (err != Error::kOK)
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::InitializeIndependentDevice() failed to make current");
            p_device->Finalize();
            return err;
        }

        IndependentDeviceInfo *found_info = nullptr;
        for (uint32_t i = 0; i < independent_device_infos_.Size(); ++i)
        {
            if (independent_device_infos_[i].device == p_device)
            {
                found_info = &independent_device_infos_[i];
                break;
            }
        }

        if (found_info != nullptr && !p_device->swapchain_infos_.IsEmpty())
        {
            found_info->swapchain_id = p_device->swapchain_infos_[0].swapchain_id;
        }

        ARHUD_LOG_INFO("RenderingDevice: Independent device initialized on render thread");
        return Error::kOK;
    }

    void RenderingDevice::DestroyIndependentDevice(RenderingDevice *p_device)
    {
        if (p_device == nullptr)
        {
            return;
        }

        ARHUD_ASSERT(!p_device->initialized_,
                     "Independent device must be finalized on its own rendering thread before DestroyIndependentDevice()");

        IRenderingContextDriver::ContextID ctx_id = IRenderingContextDriver::kInvalidContextId;
        IRenderingContextDriver::SurfaceID surf_id = IRenderingContextDriver::kInvalidSurfaceId;
        IRenderingDeviceDriver::SwapChainID swapchain_id = IRenderingDeviceDriver::kInvalidSwapChainId;

        for (uint32_t i = 0; i < independent_device_infos_.Size(); ++i)
        {
            if (independent_device_infos_[i].device == p_device)
            {
                ctx_id = independent_device_infos_[i].context_id;
                surf_id = independent_device_infos_[i].surface_id;
                swapchain_id = independent_device_infos_[i].swapchain_id;
                independent_device_infos_.RemoveAt(i);
                break;
            }
        }

        for (uint32_t i = 0; i < independent_devices_.Size(); ++i)
        {
            if (independent_devices_[i] == p_device)
            {
                independent_devices_.RemoveAt(i);
                break;
            }
        }

        if (p_device->device_driver_ != nullptr)
        {
            if (swapchain_id != IRenderingDeviceDriver::kInvalidSwapChainId)
            {
                p_device->device_driver_->SwapChainDestroy(swapchain_id);
            }
        }

        ARHUD_LOG_INFO("RenderingDevice: Independent device destroyed");
        ARHUD_DELETE(p_device);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Surface 操作
    // ═══════════════════════════════════════════════════════════════════════

    Error RenderingDevice::MakeCurrent(IRenderingContextDriver::ContextID p_context_id,
                                       IRenderingContextDriver::SurfaceID p_surface)
    {
        if (!initialized_ || device_driver_ == nullptr)
        {
            return Error::kFailed;
        }

        IRenderingDeviceDriver::SwapChainID sc_id = IRenderingDeviceDriver::kInvalidSwapChainId;

        for (uint32_t i = 0; i < swapchain_infos_.Size(); ++i)
        {
            if (swapchain_infos_[i].context_id == p_context_id &&
                swapchain_infos_[i].surface_id == p_surface)
            {
                sc_id = swapchain_infos_[i].swapchain_id;
                break;
            }
        }

        if (sc_id == IRenderingDeviceDriver::kInvalidSwapChainId)
        {
            sc_id = device_driver_->SwapChainCreate(p_surface, p_context_id);
            if (sc_id == IRenderingDeviceDriver::kInvalidSwapChainId)
            {
                return Error::kFailed;
            }

            SwapChainInfo sc_info;
            sc_info.context_id = p_context_id;
            sc_info.surface_id = p_surface;
            sc_info.swapchain_id = sc_id;
            swapchain_infos_.PushBack(sc_info);
        }

        return device_driver_->SwapChainAcquire(sc_id);
    }

    void RenderingDevice::SwapBuffers(IRenderingContextDriver::SurfaceID p_surface)
    {
        if (!initialized_ || device_driver_ == nullptr)
        {
            return;
        }

        for (uint32_t i = 0; i < swapchain_infos_.Size(); ++i)
        {
            if (swapchain_infos_[i].surface_id == p_surface)
            {
                device_driver_->SwapChainPresent(swapchain_infos_[i].swapchain_id);
                return;
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 帧循环
    // ═══════════════════════════════════════════════════════════════════════

    void RenderingDevice::BeginFrame()
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");
        ARHUD_ASSERT(IsRenderThread(),
                     "BeginFrame() must be called on the render thread");

        FreeResourcesFromPreviousFrames();

        frame_number_++;

        small_staging_block_.fill_amount = 0;

        staging_buffer_block_index_ = 0;
        for (uint32_t i = 0; i < staging_buffer_blocks_.Size(); ++i)
        {
            StagingBufferBlock &block = staging_buffer_blocks_[i];
            if (block.frame_used == 0 ||
                block.frame_used <= frame_number_ - frame_count_)
            {
                block.fill_amount = 0;
                if (staging_buffer_block_index_ == 0 ||
                    block.fill_amount < staging_buffer_blocks_[staging_buffer_block_index_].fill_amount)
                {
                    staging_buffer_block_index_ = i;
                }
            }
        }
    }

    void RenderingDevice::EndFrame()
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");
        ARHUD_ASSERT(IsRenderThread(),
                     "EndFrame() must be called on the render thread");

        uint32_t frame_index = static_cast<uint32_t>(frame_number_ % frame_count_);
        if (staging_buffer_block_index_ < staging_buffer_blocks_.Size())
        {
            staging_buffer_blocks_[staging_buffer_block_index_].frame_used = frame_number_;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Buffer 管理
    // ═══════════════════════════════════════════════════════════════════════

    RDBufferID RenderingDevice::BufferCreate(uint64_t p_size,
                                             BitField<BufferUsageBits> p_usage,
                                             MemoryAllocationType p_allocation_type)
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");
        ARHUD_ASSERT(p_size > 0, "Buffer size must be > 0");

        BufferID driver_id = device_driver_->BufferCreate(p_size, p_usage, p_allocation_type);
        if (!driver_id.IsValid())
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::BufferCreate() driver failed, size=%llu", p_size);
            return RDBufferID();
        }

        auto *buf = ARHUD_NEW(Buffer);
        buf->driver_id = driver_id;
        buf->size = p_size;
        buf->usage = p_usage;
        buf->allocation_type = p_allocation_type;
        buf->frame_used = frame_number_;

        RID rid = buffer_owner_.MakeRid(buf);
        return RDBufferID(rid);
    }

    void RenderingDevice::BufferFree(RDBufferID p_buffer)
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

        if (p_buffer.IsNull())
        {
            return;
        }

        uint32_t frame_index = static_cast<uint32_t>(frame_number_ % frame_count_);
        frames_[frame_index].buffers_to_free.PushBack(p_buffer.GetRid());
    }

    BufferID RenderingDevice::BufferGetDriverId(RDBufferID p_buffer) const
    {
        if (p_buffer.IsNull())
        {
            return BufferID();
        }

        const Buffer *buf = buffer_owner_.GetOrNull(p_buffer.GetRid());
        if (buf == nullptr)
        {
            return BufferID();
        }

        return buf->driver_id;
    }

    uint64_t RenderingDevice::BufferGetSize(RDBufferID p_buffer) const
    {
        if (p_buffer.IsNull())
        {
            return 0;
        }

        const Buffer *buf = buffer_owner_.GetOrNull(p_buffer.GetRid());
        return buf != nullptr ? buf->size : 0;
    }

    uint8_t *RenderingDevice::BufferMap(RDBufferID p_buffer)
    {
        if (p_buffer.IsNull())
        {
            return nullptr;
        }

        BufferID driver_id = BufferGetDriverId(p_buffer);
        if (!driver_id.IsValid())
        {
            return nullptr;
        }

        return device_driver_->BufferMap(driver_id);
    }

    void RenderingDevice::BufferUnmap(RDBufferID p_buffer)
    {
        if (p_buffer.IsNull())
        {
            return;
        }

        BufferID driver_id = BufferGetDriverId(p_buffer);
        if (!driver_id.IsValid())
        {
            return;
        }

        device_driver_->BufferUnmap(driver_id);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Texture 管理
    // ═══════════════════════════════════════════════════════════════════════

    RDTextureID RenderingDevice::TextureCreate(const TextureFormat &p_format,
                                               const TextureView &p_view)
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

        TextureID driver_id = device_driver_->TextureCreate(p_format, p_view);
        if (!driver_id.IsValid())
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::TextureCreate() driver failed");
            return RDTextureID();
        }

        auto *tex = ARHUD_NEW(Texture);
        tex->driver_id = driver_id;
        tex->format = p_format;
        tex->frame_used = frame_number_;

        RID rid = texture_owner_.MakeRid(tex);
        return RDTextureID(rid);
    }

    void RenderingDevice::TextureFree(RDTextureID p_texture)
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

        if (p_texture.IsNull())
        {
            return;
        }

        uint32_t frame_index = static_cast<uint32_t>(frame_number_ % frame_count_);
        frames_[frame_index].textures_to_free.PushBack(p_texture.GetRid());
    }

    TextureID RenderingDevice::TextureGetDriverId(RDTextureID p_texture) const
    {
        if (p_texture.IsNull())
        {
            return TextureID();
        }

        const Texture *tex = texture_owner_.GetOrNull(p_texture.GetRid());
        if (tex == nullptr)
        {
            return TextureID();
        }

        return tex->driver_id;
    }

    const TextureFormat &RenderingDevice::TextureGetFormat(RDTextureID p_texture) const
    {
        ARHUD_ASSERT(p_texture.IsValid(), "Invalid RDTextureID");
        const Texture *tex = texture_owner_.GetOrNull(p_texture.GetRid());
        ARHUD_ASSERT(tex != nullptr, "Texture not found in owner");
        return tex->format;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Sampler 管理
    // ═══════════════════════════════════════════════════════════════════════

    RDSamplerID RenderingDevice::SamplerCreate(const SamplerState &p_state)
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

        SamplerID driver_id = device_driver_->SamplerCreate(p_state);
        if (!driver_id.IsValid())
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::SamplerCreate() driver failed");
            return RDSamplerID();
        }

        auto *sampler = ARHUD_NEW(Sampler);
        sampler->driver_id = driver_id;
        sampler->state = p_state;

        RID rid = sampler_owner_.MakeRid(sampler);
        return RDSamplerID(rid);
    }

    void RenderingDevice::SamplerFree(RDSamplerID p_sampler)
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

        if (p_sampler.IsNull())
        {
            return;
        }

        uint32_t frame_index = static_cast<uint32_t>(frame_number_ % frame_count_);
        frames_[frame_index].samplers_to_free.PushBack(p_sampler.GetRid());
    }

    SamplerID RenderingDevice::SamplerGetDriverId(RDSamplerID p_sampler) const
    {
        if (p_sampler.IsNull())
        {
            return SamplerID();
        }

        const Sampler *sampler = sampler_owner_.GetOrNull(p_sampler.GetRid());
        if (sampler == nullptr)
        {
            return SamplerID();
        }

        return sampler->driver_id;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Shader 管理
    // ═══════════════════════════════════════════════════════════════════════

    RDShaderID RenderingDevice::ShaderCreateFromGLSL(VectorView<ShaderStageSource> p_stage_sources,
                                                     VectorView<ShaderUniform> p_uniforms,
                                                     uint32_t p_push_constant_size)
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

        ShaderID driver_id = device_driver_->ShaderCreateFromGLSL(
            p_stage_sources, p_uniforms, p_push_constant_size);

        auto *shader = ARHUD_NEW(Shader);
        shader->driver_id = driver_id;
        shader->is_valid = driver_id.IsValid();

        if (shader->is_valid)
        {
            for (uint32_t i = 0; i < p_uniforms.Size(); ++i)
            {
                shader->uniforms.PushBack(p_uniforms[i]);
            }
            shader->push_constant_size = p_push_constant_size;

            bool has_compute = false;
            for (uint32_t i = 0; i < p_stage_sources.Size(); ++i)
            {
                shader->stage_bits.SetFlag(p_stage_sources[i].stage);
                if (p_stage_sources[i].stage == ShaderStage::kCompute)
                {
                    has_compute = true;
                }
            }
            shader->is_compute = has_compute;
        }

        RID rid = shader_owner_.MakeRid(shader);
        return RDShaderID(rid);
    }

    void RenderingDevice::ShaderFree(RDShaderID p_shader)
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

        if (p_shader.IsNull())
        {
            return;
        }

        uint32_t frame_index = static_cast<uint32_t>(frame_number_ % frame_count_);
        frames_[frame_index].shaders_to_free.PushBack(p_shader.GetRid());
    }

    ShaderID RenderingDevice::ShaderGetDriverId(RDShaderID p_shader) const
    {
        if (p_shader.IsNull())
        {
            return ShaderID();
        }

        const Shader *shader = shader_owner_.GetOrNull(p_shader.GetRid());
        if (shader == nullptr)
        {
            return ShaderID();
        }

        return shader->driver_id;
    }

    const LocalVector<ShaderUniform> &RenderingDevice::ShaderGetUniforms(RDShaderID p_shader) const
    {
        static const LocalVector<ShaderUniform> kEmptyUniforms;

        if (p_shader.IsNull())
        {
            return kEmptyUniforms;
        }

        const Shader *shader = shader_owner_.GetOrNull(p_shader.GetRid());
        if (shader == nullptr)
        {
            return kEmptyUniforms;
        }

        return shader->uniforms;
    }

    uint32_t RenderingDevice::ShaderGetPushConstantSize(RDShaderID p_shader) const
    {
        if (p_shader.IsNull())
        {
            return 0;
        }

        const Shader *shader = shader_owner_.GetOrNull(p_shader.GetRid());
        if (shader == nullptr)
        {
            return 0;
        }

        return shader->push_constant_size;
    }

    bool RenderingDevice::ShaderIsValid(RDShaderID p_shader) const
    {
        if (p_shader.IsNull())
        {
            return false;
        }

        const Shader *shader = shader_owner_.GetOrNull(p_shader.GetRid());
        if (shader == nullptr)
        {
            return false;
        }

        return shader->is_valid;
    }

    bool RenderingDevice::ShaderIsCompute(RDShaderID p_shader) const
    {
        if (p_shader.IsNull())
        {
            return false;
        }

        const Shader *shader = shader_owner_.GetOrNull(p_shader.GetRid());
        if (shader == nullptr)
        {
            return false;
        }

        return shader->is_compute;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // UniformSet 管理
    // ═══════════════════════════════════════════════════════════════════════

    RDUniformSetID RenderingDevice::UniformSetCreate(VectorView<BoundUniform> p_uniforms,
                                                     RDShaderID p_shader,
                                                     uint32_t p_set_index)
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

        ShaderID shader_driver_id = ShaderGetDriverId(p_shader);
        if (!shader_driver_id.IsValid())
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::UniformSetCreate() invalid shader RID");
            return RDUniformSetID();
        }

        UniformSetID driver_id = device_driver_->UniformSetCreate(
            p_uniforms, shader_driver_id, p_set_index);
        if (!driver_id.IsValid())
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::UniformSetCreate() driver failed");
            return RDUniformSetID();
        }

        auto *uniform_set = ARHUD_NEW(UniformSet);
        uniform_set->driver_id = driver_id;
        uniform_set->shader_id = shader_driver_id;
        uniform_set->set_index = p_set_index;

        RID rid = uniform_set_owner_.MakeRid(uniform_set);
        return RDUniformSetID(rid);
    }

    void RenderingDevice::UniformSetFree(RDUniformSetID p_uniform_set)
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

        if (p_uniform_set.IsNull())
        {
            return;
        }

        uint32_t frame_index = static_cast<uint32_t>(frame_number_ % frame_count_);
        frames_[frame_index].uniform_sets_to_free.PushBack(p_uniform_set.GetRid());
    }

    UniformSetID RenderingDevice::UniformSetGetDriverId(RDUniformSetID p_uniform_set) const
    {
        if (p_uniform_set.IsNull())
        {
            return UniformSetID();
        }

        const UniformSet *us = uniform_set_owner_.GetOrNull(p_uniform_set.GetRid());
        if (us == nullptr)
        {
            return UniformSetID();
        }

        return us->driver_id;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // VertexFormat 管理
    // ═══════════════════════════════════════════════════════════════════════

    RDVertexFormatID RenderingDevice::VertexFormatCreate(VectorView<VertexAttribute> p_vertex_attribs)
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

        VertexFormatID driver_id = device_driver_->VertexFormatCreate(p_vertex_attribs);
        if (!driver_id.IsValid())
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::VertexFormatCreate() driver failed");
            return RDVertexFormatID();
        }

        auto *vf = ARHUD_NEW(VertexFormat);
        vf->driver_id = driver_id;

        RID rid = vertex_format_owner_.MakeRid(vf);
        return RDVertexFormatID(rid);
    }

    void RenderingDevice::VertexFormatFree(RDVertexFormatID p_vertex_format)
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

        if (p_vertex_format.IsNull())
        {
            return;
        }

        uint32_t frame_index = static_cast<uint32_t>(frame_number_ % frame_count_);
        frames_[frame_index].vertex_formats_to_free.PushBack(p_vertex_format.GetRid());
    }

    VertexFormatID RenderingDevice::VertexFormatGetDriverId(RDVertexFormatID p_vertex_format) const
    {
        if (p_vertex_format.IsNull())
        {
            return VertexFormatID();
        }

        const VertexFormat *vf = vertex_format_owner_.GetOrNull(p_vertex_format.GetRid());
        if (vf == nullptr)
        {
            return VertexFormatID();
        }

        return vf->driver_id;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // RenderPass 管理
    // ═══════════════════════════════════════════════════════════════════════

    RDRenderPassID RenderingDevice::RenderPassCreate(VectorView<Attachment> p_attachments,
                                                     VectorView<Subpass> p_subpasses,
                                                     VectorView<SubpassDependency> p_subpass_dependencies)
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

        RenderPassID driver_id = device_driver_->RenderPassCreate(
            p_attachments, p_subpasses, p_subpass_dependencies);
        if (!driver_id.IsValid())
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::RenderPassCreate() driver failed");
            return RDRenderPassID();
        }

        auto *rp = ARHUD_NEW(RenderPass);
        rp->driver_id = driver_id;

        RID rid = render_pass_owner_.MakeRid(rp);
        return RDRenderPassID(rid);
    }

    void RenderingDevice::RenderPassFree(RDRenderPassID p_render_pass)
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

        if (p_render_pass.IsNull())
        {
            return;
        }

        uint32_t frame_index = static_cast<uint32_t>(frame_number_ % frame_count_);
        frames_[frame_index].render_passes_to_free.PushBack(p_render_pass.GetRid());
    }

    RenderPassID RenderingDevice::RenderPassGetDriverId(RDRenderPassID p_render_pass) const
    {
        if (p_render_pass.IsNull())
        {
            return RenderPassID();
        }

        const RenderPass *rp = render_pass_owner_.GetOrNull(p_render_pass.GetRid());
        if (rp == nullptr)
        {
            return RenderPassID();
        }

        return rp->driver_id;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Framebuffer 管理
    // ═══════════════════════════════════════════════════════════════════════

    RDFramebufferID RenderingDevice::FramebufferCreate(RDRenderPassID p_render_pass,
                                                       VectorView<RDTextureID> p_attachments,
                                                       uint32_t p_width,
                                                       uint32_t p_height)
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

        RenderPassID rp_driver_id = RenderPassGetDriverId(p_render_pass);
        if (!rp_driver_id.IsValid())
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::FramebufferCreate() invalid render pass RID");
            return RDFramebufferID();
        }

        LocalVector<TextureID> driver_attachments;
        driver_attachments.Reserve(p_attachments.Size());
        for (uint32_t i = 0; i < p_attachments.Size(); ++i)
        {
            TextureID tex_driver_id = TextureGetDriverId(p_attachments[i]);
            if (!tex_driver_id.IsValid())
            {
                ARHUD_LOG_ERROR("kFailed", "RenderingDevice::FramebufferCreate() invalid texture attachment at index %u", i);
                return RDFramebufferID();
            }
            driver_attachments.PushBack(tex_driver_id);
        }

        FramebufferID driver_id = device_driver_->FramebufferCreate(
            rp_driver_id,
            VectorView<TextureID>(driver_attachments.Data(), driver_attachments.Size()),
            p_width, p_height);
        if (!driver_id.IsValid())
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::FramebufferCreate() driver failed");
            return RDFramebufferID();
        }

        auto *fb = ARHUD_NEW(Framebuffer);
        fb->driver_id = driver_id;
        fb->width = p_width;
        fb->height = p_height;

        RID rid = framebuffer_owner_.MakeRid(fb);
        return RDFramebufferID(rid);
    }

    void RenderingDevice::FramebufferFree(RDFramebufferID p_framebuffer)
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

        if (p_framebuffer.IsNull())
        {
            return;
        }

        uint32_t frame_index = static_cast<uint32_t>(frame_number_ % frame_count_);
        frames_[frame_index].framebuffers_to_free.PushBack(p_framebuffer.GetRid());
    }

    FramebufferID RenderingDevice::FramebufferGetDriverId(RDFramebufferID p_framebuffer) const
    {
        if (p_framebuffer.IsNull())
        {
            return FramebufferID();
        }

        const Framebuffer *fb = framebuffer_owner_.GetOrNull(p_framebuffer.GetRid());
        if (fb == nullptr)
        {
            return FramebufferID();
        }

        return fb->driver_id;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Pipeline 管理
    // ═══════════════════════════════════════════════════════════════════════

    RDPipelineID RenderingDevice::RenderPipelineCreate(
        RDShaderID p_shader,
        RDVertexFormatID p_vertex_format,
        RenderPrimitive p_render_primitive,
        const PipelineRasterizationState &p_rasterization_state,
        const PipelineMultisampleState &p_multisample_state,
        const PipelineDepthStencilState &p_depth_stencil_state,
        const PipelineColorBlendState &p_blend_state,
        BitField<PipelineDynamicStateFlags> p_dynamic_state,
        RDRenderPassID p_render_pass)
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

        ShaderID shader_driver_id = ShaderGetDriverId(p_shader);
        VertexFormatID vf_driver_id = VertexFormatGetDriverId(p_vertex_format);
        RenderPassID rp_driver_id = RenderPassGetDriverId(p_render_pass);

        if (!shader_driver_id.IsValid())
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::RenderPipelineCreate() invalid shader RID");
            return RDPipelineID();
        }
        if (!vf_driver_id.IsValid())
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::RenderPipelineCreate() invalid vertex format RID");
            return RDPipelineID();
        }
        if (!rp_driver_id.IsValid())
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::RenderPipelineCreate() invalid render pass RID");
            return RDPipelineID();
        }

        PipelineID driver_id = device_driver_->RenderPipelineCreate(
            shader_driver_id, vf_driver_id, p_render_primitive,
            p_rasterization_state, p_multisample_state,
            p_depth_stencil_state, p_blend_state,
            p_dynamic_state, rp_driver_id);
        if (!driver_id.IsValid())
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::RenderPipelineCreate() driver failed");
            return RDPipelineID();
        }

        auto *pipeline = ARHUD_NEW(Pipeline);
        pipeline->driver_id = driver_id;

        RID rid = pipeline_owner_.MakeRid(pipeline);
        return RDPipelineID(rid);
    }

    void RenderingDevice::PipelineFree(RDPipelineID p_pipeline)
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

        if (p_pipeline.IsNull())
        {
            return;
        }

        uint32_t frame_index = static_cast<uint32_t>(frame_number_ % frame_count_);
        frames_[frame_index].pipelines_to_free.PushBack(p_pipeline.GetRid());
    }

    PipelineID RenderingDevice::PipelineGetDriverId(RDPipelineID p_pipeline) const
    {
        if (p_pipeline.IsNull())
        {
            return PipelineID();
        }

        const Pipeline *pipe = pipeline_owner_.GetOrNull(p_pipeline.GetRid());
        if (pipe == nullptr)
        {
            return PipelineID();
        }

        return pipe->driver_id;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // CommandBuffer 池管理
    // ═══════════════════════════════════════════════════════════════════════

    RDCommandBufferID RenderingDevice::CommandBufferAcquire()
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

        RID found_rid;
        bool found = false;
        command_buffer_owner_.ForEach([&](RID p_rid, CommandBuffer *p_obj)
                                      {
            if (!found && p_obj->pool_state == CommandBufferPoolState::kFree)
            {
                found_rid = p_rid;
                found = true;
            } });

        if (!found)
        {
            return RDCommandBufferID();
        }

        CommandBuffer *cmd = command_buffer_owner_.GetOrNull(found_rid);
        ARHUD_ASSERT(cmd != nullptr, "CommandBuffer must not be null after ForEach");

        cmd->pool_state = CommandBufferPoolState::kAcquired;
        cmd->frame_used = frame_number_;

        return RDCommandBufferID(found_rid);
    }

    void RenderingDevice::CommandBufferRelease(RDCommandBufferID p_command_buffer)
    {
        ARHUD_ASSERT(initialized_, "RenderingDevice not initialized");

        if (p_command_buffer.IsNull())
        {
            return;
        }

        CommandBuffer *cmd = command_buffer_owner_.GetOrNull(p_command_buffer.GetRid());
        if (cmd == nullptr)
        {
            return;
        }

        if (cmd->driver_cmd != nullptr)
        {
            cmd->driver_cmd->Reset();
        }

        cmd->pool_state = CommandBufferPoolState::kFree;
    }

    ICommandBuffer *RenderingDevice::CommandBufferGetDriverCmd(RDCommandBufferID p_command_buffer) const
    {
        if (p_command_buffer.IsNull())
        {
            return nullptr;
        }

        const CommandBuffer *cmd = command_buffer_owner_.GetOrNull(p_command_buffer.GetRid());
        return cmd != nullptr ? cmd->driver_cmd : nullptr;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // DrawList
    // ═══════════════════════════════════════════════════════════════════════

    DrawListID RenderingDevice::DrawListBegin()
    {
        if (!initialized_)
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::DrawListBegin() not initialized");
            return kInvalidDrawListId;
        }

        if (draw_list_.active)
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::DrawListBegin() draw list already active");
            return kInvalidDrawListId;
        }

        RDCommandBufferID cmd_id = CommandBufferAcquire();
        if (cmd_id.IsNull())
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::DrawListBegin() no free command buffer");
            return kInvalidDrawListId;
        }

        ICommandBuffer *driver_cmd = CommandBufferGetDriverCmd(cmd_id);
        if (driver_cmd == nullptr)
        {
            CommandBufferRelease(cmd_id);
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice::DrawListBegin() null driver cmd");
            return kInvalidDrawListId;
        }

        driver_cmd->Begin();

        draw_list_.active = true;
        draw_list_.command_buffer = cmd_id;
        draw_list_.driver_cmd = driver_cmd;

        return 0;
    }

    void RenderingDevice::DrawListSetViewport(DrawListID p_list,
                                              int32_t p_x, int32_t p_y,
                                              uint32_t p_width, uint32_t p_height)
    {
        (void)p_list;
        if (!draw_list_.active || draw_list_.driver_cmd == nullptr)
        {
            return;
        }
        draw_list_.driver_cmd->SetViewport(p_x, p_y, p_width, p_height);
    }

    void RenderingDevice::DrawListSetScissor(DrawListID p_list,
                                             int32_t p_x, int32_t p_y,
                                             uint32_t p_width, uint32_t p_height)
    {
        (void)p_list;
        if (!draw_list_.active || draw_list_.driver_cmd == nullptr)
        {
            return;
        }
        draw_list_.driver_cmd->SetScissor(p_x, p_y, p_width, p_height);
    }

    void RenderingDevice::DrawListSetBlendConstants(DrawListID p_list,
                                                    float p_r, float p_g,
                                                    float p_b, float p_a)
    {
        (void)p_list;
        if (!draw_list_.active || draw_list_.driver_cmd == nullptr)
        {
            return;
        }
        draw_list_.driver_cmd->SetBlendConstants(p_r, p_g, p_b, p_a);
    }

    void RenderingDevice::DrawListBindRenderPipeline(DrawListID p_list, RDPipelineID p_pipeline)
    {
        (void)p_list;
        if (!draw_list_.active || draw_list_.driver_cmd == nullptr)
        {
            return;
        }
        Pipeline *pipeline = pipeline_owner_.GetOrNull(p_pipeline.GetRid());
        if (pipeline == nullptr)
        {
            ARHUD_LOG_ERROR("kInvalidParameter", "DrawListBindRenderPipeline: invalid pipeline RID");
            return;
        }
        draw_list_.driver_cmd->BindRenderPipeline(pipeline->driver_id);
    }

    void RenderingDevice::DrawListBindUniformSet(DrawListID p_list,
                                                 RDUniformSetID p_uniform_set,
                                                 uint32_t p_set_index)
    {
        (void)p_list;
        if (!draw_list_.active || draw_list_.driver_cmd == nullptr)
        {
            return;
        }
        UniformSet *uniform_set = uniform_set_owner_.GetOrNull(p_uniform_set.GetRid());
        if (uniform_set == nullptr)
        {
            ARHUD_LOG_ERROR("kInvalidParameter", "DrawListBindUniformSet: invalid uniform set RID");
            return;
        }
        draw_list_.driver_cmd->BindUniformSet(uniform_set->driver_id, p_set_index);
    }

    void RenderingDevice::DrawListBindVertexBuffers(DrawListID p_list,
                                                    const RDBufferID *p_buffers,
                                                    const uint64_t *p_offsets,
                                                    uint32_t p_count)
    {
        (void)p_list;
        if (!draw_list_.active || draw_list_.driver_cmd == nullptr)
        {
            return;
        }
        BufferID driver_ids[CmdBindVertexBuffers::kMaxBindings] = {};
        uint64_t driver_offsets[CmdBindVertexBuffers::kMaxBindings] = {};
        uint32_t actual_count = p_count > CmdBindVertexBuffers::kMaxBindings
                                    ? CmdBindVertexBuffers::kMaxBindings
                                    : p_count;
        for (uint32_t i = 0; i < actual_count; ++i)
        {
            Buffer *buf = buffer_owner_.GetOrNull(p_buffers[i].GetRid());
            driver_ids[i] = buf != nullptr ? buf->driver_id : BufferID();
            driver_offsets[i] = p_offsets != nullptr ? p_offsets[i] : 0;
        }
        draw_list_.driver_cmd->BindVertexBuffers(driver_ids, driver_offsets, actual_count);
    }

    void RenderingDevice::DrawListBindIndexBuffer(DrawListID p_list,
                                                  RDBufferID p_buffer,
                                                  IndexBufferFormat p_format,
                                                  uint64_t p_offset)
    {
        (void)p_list;
        if (!draw_list_.active || draw_list_.driver_cmd == nullptr)
        {
            return;
        }
        Buffer *buf = buffer_owner_.GetOrNull(p_buffer.GetRid());
        if (buf == nullptr)
        {
            ARHUD_LOG_ERROR("kInvalidParameter", "DrawListBindIndexBuffer: invalid buffer RID");
            return;
        }
        draw_list_.driver_cmd->BindIndexBuffer(buf->driver_id, p_format, p_offset);
    }

    void RenderingDevice::DrawListDraw(DrawListID p_list,
                                       uint32_t p_vertex_count,
                                       uint32_t p_instance_count)
    {
        (void)p_list;
        if (!draw_list_.active || draw_list_.driver_cmd == nullptr)
        {
            return;
        }
        draw_list_.driver_cmd->Draw(p_vertex_count, p_instance_count, 0, 0);
    }

    void RenderingDevice::DrawListDrawIndexed(DrawListID p_list,
                                              uint32_t p_index_count,
                                              uint32_t p_instance_count)
    {
        (void)p_list;
        if (!draw_list_.active || draw_list_.driver_cmd == nullptr)
        {
            return;
        }
        draw_list_.driver_cmd->DrawIndexed(p_index_count, p_instance_count, 0, 0, 0);
    }

    void RenderingDevice::DrawListEnd()
    {
        if (!draw_list_.active)
        {
            ARHUD_LOG_WARN("RenderingDevice::DrawListEnd() no active draw list");
            return;
        }

        if (draw_list_.driver_cmd != nullptr)
        {
            draw_list_.driver_cmd->End();
            draw_list_.driver_cmd->Execute();
        }

        if (!draw_list_.command_buffer.IsNull())
        {
            CommandBufferRelease(draw_list_.command_buffer);
        }

        draw_list_.active = false;
        draw_list_.command_buffer = RDCommandBufferID();
        draw_list_.driver_cmd = nullptr;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 依赖追踪
    // ═══════════════════════════════════════════════════════════════════════

    void RenderingDevice::DependencyAdd(RID p_base, RID p_dependent)
    {
        ARHUD_ASSERT(p_base.IsValid(), "Base RID must be valid");
        ARHUD_ASSERT(p_dependent.IsValid(), "Dependent RID must be valid");

        dependency_map_[p_base].Insert(p_dependent);
        reverse_dependency_map_[p_dependent].Insert(p_base);
    }

    void RenderingDevice::DependencyRemove(RID p_base, RID p_dependent)
    {
        HashSet<RID> *deps = dependency_map_.GetPtr(p_base);
        if (deps != nullptr)
        {
            deps->Erase(p_dependent);
        }

        HashSet<RID> *rev_deps = reverse_dependency_map_.GetPtr(p_dependent);
        if (rev_deps != nullptr)
        {
            rev_deps->Erase(p_base);
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 内部方法 — 延迟销毁
    // ═══════════════════════════════════════════════════════════════════════

    void RenderingDevice::FreeResourcesFromPreviousFrames()
    {
        uint64_t safe_frame = (frame_number_ >= frame_count_) ? frame_number_ - frame_count_ : 0;
        uint32_t frame_index = static_cast<uint32_t>(safe_frame % frame_count_);

        Frame &frame = frames_[frame_index];

        for (uint32_t i = 0; i < frame.pipelines_to_free.Size(); ++i)
        {
            FreePipelineDriverResource(frame.pipelines_to_free[i]);
        }
        for (uint32_t i = 0; i < frame.command_buffers_to_release.Size(); ++i)
        {
            CommandBuffer *cmd = command_buffer_owner_.GetOrNull(frame.command_buffers_to_release[i]);
            if (cmd != nullptr && cmd->driver_cmd != nullptr)
            {
                cmd->driver_cmd->Reset();
            }
            if (cmd != nullptr)
            {
                cmd->pool_state = CommandBufferPoolState::kFree;
            }
        }
        for (uint32_t i = 0; i < frame.framebuffers_to_free.Size(); ++i)
        {
            FreeFramebufferDriverResource(frame.framebuffers_to_free[i]);
        }
        for (uint32_t i = 0; i < frame.render_passes_to_free.Size(); ++i)
        {
            FreeRenderPassDriverResource(frame.render_passes_to_free[i]);
        }
        for (uint32_t i = 0; i < frame.uniform_sets_to_free.Size(); ++i)
        {
            FreeUniformSetDriverResource(frame.uniform_sets_to_free[i]);
        }
        for (uint32_t i = 0; i < frame.shaders_to_free.Size(); ++i)
        {
            FreeShaderDriverResource(frame.shaders_to_free[i]);
        }
        for (uint32_t i = 0; i < frame.vertex_formats_to_free.Size(); ++i)
        {
            FreeVertexFormatDriverResource(frame.vertex_formats_to_free[i]);
        }
        for (uint32_t i = 0; i < frame.samplers_to_free.Size(); ++i)
        {
            FreeSamplerDriverResource(frame.samplers_to_free[i]);
        }
        for (uint32_t i = 0; i < frame.textures_to_free.Size(); ++i)
        {
            FreeTextureDriverResource(frame.textures_to_free[i]);
        }
        for (uint32_t i = 0; i < frame.buffers_to_free.Size(); ++i)
        {
            FreeBufferDriverResource(frame.buffers_to_free[i]);
        }

        frame.Clear();
    }

    void RenderingDevice::FreeBufferDriverResource(RID p_rid)
    {
        Buffer *buf = buffer_owner_.GetOrNull(p_rid);
        if (buf == nullptr)
        {
            return;
        }

        device_driver_->BufferFree(buf->driver_id);
        ARHUD_DELETE(buf);
        buffer_owner_.Free(p_rid);
    }

    void RenderingDevice::FreeTextureDriverResource(RID p_rid)
    {
        Texture *tex = texture_owner_.GetOrNull(p_rid);
        if (tex == nullptr)
        {
            return;
        }

        device_driver_->TextureFree(tex->driver_id);
        ARHUD_DELETE(tex);
        texture_owner_.Free(p_rid);
    }

    void RenderingDevice::FreeSamplerDriverResource(RID p_rid)
    {
        Sampler *sampler = sampler_owner_.GetOrNull(p_rid);
        if (sampler == nullptr)
        {
            return;
        }

        device_driver_->SamplerFree(sampler->driver_id);
        ARHUD_DELETE(sampler);
        sampler_owner_.Free(p_rid);
    }

    void RenderingDevice::FreeShaderDriverResource(RID p_rid)
    {
        Shader *shader = shader_owner_.GetOrNull(p_rid);
        if (shader == nullptr)
        {
            return;
        }

        device_driver_->ShaderFree(shader->driver_id);
        ARHUD_DELETE(shader);
        shader_owner_.Free(p_rid);
    }

    void RenderingDevice::FreeUniformSetDriverResource(RID p_rid)
    {
        UniformSet *us = uniform_set_owner_.GetOrNull(p_rid);
        if (us == nullptr)
        {
            return;
        }

        device_driver_->UniformSetFree(us->driver_id);
        ARHUD_DELETE(us);
        uniform_set_owner_.Free(p_rid);
    }

    void RenderingDevice::FreeVertexFormatDriverResource(RID p_rid)
    {
        VertexFormat *vf = vertex_format_owner_.GetOrNull(p_rid);
        if (vf == nullptr)
        {
            return;
        }

        device_driver_->VertexFormatFree(vf->driver_id);
        ARHUD_DELETE(vf);
        vertex_format_owner_.Free(p_rid);
    }

    void RenderingDevice::FreeRenderPassDriverResource(RID p_rid)
    {
        RenderPass *rp = render_pass_owner_.GetOrNull(p_rid);
        if (rp == nullptr)
        {
            return;
        }

        device_driver_->RenderPassFree(rp->driver_id);
        ARHUD_DELETE(rp);
        render_pass_owner_.Free(p_rid);
    }

    void RenderingDevice::FreeFramebufferDriverResource(RID p_rid)
    {
        Framebuffer *fb = framebuffer_owner_.GetOrNull(p_rid);
        if (fb == nullptr)
        {
            return;
        }

        device_driver_->FramebufferFree(fb->driver_id);
        ARHUD_DELETE(fb);
        framebuffer_owner_.Free(p_rid);
    }

    void RenderingDevice::FreePipelineDriverResource(RID p_rid)
    {
        Pipeline *pipe = pipeline_owner_.GetOrNull(p_rid);
        if (pipe == nullptr)
        {
            return;
        }
        device_driver_->PipelineFree(pipe->driver_id);
        ARHUD_DELETE(pipe);
        pipeline_owner_.Free(p_rid);
    }

    void RenderingDevice::FreeCommandBufferDriverResource(RID p_rid)
    {
        CommandBuffer *cmd = command_buffer_owner_.GetOrNull(p_rid);
        if (cmd == nullptr)
        {
            return;
        }

        if (cmd->driver_cmd != nullptr)
        {
            device_driver_->CommandBufferFree(cmd->driver_cmd);
            cmd->driver_cmd = nullptr;
        }

        ARHUD_DELETE(cmd);
        command_buffer_owner_.Free(p_rid);
    }

    Error RenderingDevice::InitializeCommandBufferPool()
    {
        for (uint32_t i = 0; i < frame_count_; ++i)
        {
            ICommandBuffer *driver_cmd = device_driver_->CommandBufferCreate();
            if (driver_cmd == nullptr)
            {
                ARHUD_LOG_ERROR("kFailed", "RenderingDevice: failed to create command buffer %u", i);
                DestroyCommandBufferPool();
                return Error::kFailed;
            }

            auto *cmd = ARHUD_NEW(CommandBuffer);
            cmd->driver_cmd = driver_cmd;
            cmd->pool_state = CommandBufferPoolState::kFree;
            cmd->frame_used = 0;

            command_buffer_owner_.MakeRid(cmd);
        }

        return Error::kOK;
    }

    void RenderingDevice::DestroyCommandBufferPool()
    {
        command_buffer_owner_.ForEach([this](RID p_rid, CommandBuffer *p_obj)
                                      {
            if (p_obj->driver_cmd != nullptr)
            {
                device_driver_->CommandBufferFree(p_obj->driver_cmd);
                p_obj->driver_cmd = nullptr;
            }
            ARHUD_DELETE(p_obj); });
        command_buffer_owner_.Clear();
    }

    void RenderingDevice::FreeAllResources()
    {
        for (uint32_t i = 0; i < frame_count_; ++i)
        {
            Frame &frame = frames_[i];

            for (uint32_t j = 0; j < frame.pipelines_to_free.Size(); ++j)
            {
                FreePipelineDriverResource(frame.pipelines_to_free[j]);
            }

            for (uint32_t j = 0; j < frame.command_buffers_to_release.Size(); ++j)
            {
                CommandBuffer *cmd = command_buffer_owner_.GetOrNull(frame.command_buffers_to_release[j]);
                if (cmd != nullptr && cmd->driver_cmd != nullptr)
                {
                    cmd->driver_cmd->Reset();
                }
                if (cmd != nullptr)
                {
                    cmd->pool_state = CommandBufferPoolState::kFree;
                }
            }

            for (uint32_t j = 0; j < frame.framebuffers_to_free.Size(); ++j)
            {
                FreeFramebufferDriverResource(frame.framebuffers_to_free[j]);
            }

            for (uint32_t j = 0; j < frame.render_passes_to_free.Size(); ++j)
            {
                FreeRenderPassDriverResource(frame.render_passes_to_free[j]);
            }

            for (uint32_t j = 0; j < frame.uniform_sets_to_free.Size(); ++j)
            {
                FreeUniformSetDriverResource(frame.uniform_sets_to_free[j]);
            }

            for (uint32_t j = 0; j < frame.shaders_to_free.Size(); ++j)
            {
                FreeShaderDriverResource(frame.shaders_to_free[j]);
            }

            for (uint32_t j = 0; j < frame.vertex_formats_to_free.Size(); ++j)
            {
                FreeVertexFormatDriverResource(frame.vertex_formats_to_free[j]);
            }

            for (uint32_t j = 0; j < frame.samplers_to_free.Size(); ++j)
            {
                FreeSamplerDriverResource(frame.samplers_to_free[j]);
            }

            for (uint32_t j = 0; j < frame.textures_to_free.Size(); ++j)
            {
                FreeTextureDriverResource(frame.textures_to_free[j]);
            }

            for (uint32_t j = 0; j < frame.buffers_to_free.Size(); ++j)
            {
                FreeBufferDriverResource(frame.buffers_to_free[j]);
            }

            frame.Clear();
        }

        pipeline_owner_.ForEach([this](RID p_rid, Pipeline *p_obj)
                                {
            device_driver_->PipelineFree(p_obj->driver_id);
            ARHUD_DELETE(p_obj); });
        pipeline_owner_.Clear();

        command_buffer_owner_.ForEach([this](RID p_rid, CommandBuffer *p_obj)
                                      {
            if (p_obj->driver_cmd != nullptr)
            {
                device_driver_->CommandBufferFree(p_obj->driver_cmd);
                p_obj->driver_cmd = nullptr;
            }
            ARHUD_DELETE(p_obj); });
        command_buffer_owner_.Clear();

        framebuffer_owner_.ForEach([this](RID p_rid, Framebuffer *p_obj)
                                   {
            device_driver_->FramebufferFree(p_obj->driver_id);
            ARHUD_DELETE(p_obj); });
        framebuffer_owner_.Clear();

        render_pass_owner_.ForEach([this](RID p_rid, RenderPass *p_obj)
                                   {
            device_driver_->RenderPassFree(p_obj->driver_id);
            ARHUD_DELETE(p_obj); });
        render_pass_owner_.Clear();

        uniform_set_owner_.ForEach([this](RID p_rid, UniformSet *p_obj)
                                   {
            device_driver_->UniformSetFree(p_obj->driver_id);
            ARHUD_DELETE(p_obj); });
        uniform_set_owner_.Clear();

        shader_owner_.ForEach([this](RID p_rid, Shader *p_obj)
                              {
            device_driver_->ShaderFree(p_obj->driver_id);
            ARHUD_DELETE(p_obj); });
        shader_owner_.Clear();

        vertex_format_owner_.ForEach([this](RID p_rid, VertexFormat *p_obj)
                                     {
            device_driver_->VertexFormatFree(p_obj->driver_id);
            ARHUD_DELETE(p_obj); });
        vertex_format_owner_.Clear();

        sampler_owner_.ForEach([this](RID p_rid, Sampler *p_obj)
                               {
            device_driver_->SamplerFree(p_obj->driver_id);
            ARHUD_DELETE(p_obj); });
        sampler_owner_.Clear();

        texture_owner_.ForEach([this](RID p_rid, Texture *p_obj)
                               {
            device_driver_->TextureFree(p_obj->driver_id);
            ARHUD_DELETE(p_obj); });
        texture_owner_.Clear();

        buffer_owner_.ForEach([this](RID p_rid, Buffer *p_obj)
                              {
            device_driver_->BufferFree(p_obj->driver_id);
            ARHUD_DELETE(p_obj); });
        buffer_owner_.Clear();
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Staging Buffer
    // ═══════════════════════════════════════════════════════════════════════

    Error RenderingDevice::InitializeStagingBuffer()
    {
        for (uint32_t i = 0; i < frame_count_; ++i)
        {
            StagingBufferBlock block;
            block.driver_id = device_driver_->BufferCreate(
                staging_buffer_size_,
                BitField<BufferUsageBits>(BufferUsageBits::kTransferFrom) |
                    BitField<BufferUsageBits>(BufferUsageBits::kTransferTo),
                MemoryAllocationType::kCpu);
            if (!block.driver_id.IsValid())
            {
                ARHUD_LOG_ERROR("kFailed", "RenderingDevice: failed to create staging buffer block %u", i);
                DestroyStagingBuffer();
                return Error::kFailed;
            }

            block.data_ptr = device_driver_->BufferMap(block.driver_id);
            if (block.data_ptr == nullptr)
            {
                ARHUD_LOG_ERROR("kFailed", "RenderingDevice: failed to map staging buffer block %u", i);
                device_driver_->BufferFree(block.driver_id);
                DestroyStagingBuffer();
                return Error::kFailed;
            }

            block.frame_used = 0;
            block.fill_amount = 0;

            staging_buffer_blocks_.PushBack(block);
        }

        staging_buffer_total_size_ = static_cast<uint64_t>(frame_count_) * staging_buffer_size_;

        small_staging_block_.driver_id = device_driver_->BufferCreate(
            kSmallStagingBlockSize,
            BitField<BufferUsageBits>(BufferUsageBits::kTransferFrom) |
                BitField<BufferUsageBits>(BufferUsageBits::kTransferTo),
            MemoryAllocationType::kCpu);
        if (!small_staging_block_.driver_id.IsValid())
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice: failed to create small staging buffer block");
            DestroyStagingBuffer();
            return Error::kFailed;
        }

        small_staging_block_.data_ptr = device_driver_->BufferMap(small_staging_block_.driver_id);
        if (small_staging_block_.data_ptr == nullptr)
        {
            ARHUD_LOG_ERROR("kFailed", "RenderingDevice: failed to map small staging buffer block");
            device_driver_->BufferFree(small_staging_block_.driver_id);
            small_staging_block_.driver_id = BufferID();
            DestroyStagingBuffer();
            return Error::kFailed;
        }

        small_staging_block_.frame_used = 0;
        small_staging_block_.fill_amount = 0;

        return Error::kOK;
    }

    void RenderingDevice::DestroyStagingBuffer()
    {
        for (uint32_t i = 0; i < staging_buffer_blocks_.Size(); ++i)
        {
            StagingBufferBlock &block = staging_buffer_blocks_[i];
            block.data_ptr = nullptr;
            if (block.driver_id.IsValid())
            {
                device_driver_->BufferFree(block.driver_id);
                block.driver_id = BufferID();
            }
        }
        staging_buffer_blocks_.Clear();
        staging_buffer_total_size_ = 0;

        small_staging_block_.data_ptr = nullptr;
        if (small_staging_block_.driver_id.IsValid())
        {
            device_driver_->BufferFree(small_staging_block_.driver_id);
            small_staging_block_.driver_id = BufferID();
        }
    }

    Error RenderingDevice::StagingBufferAllocate(uint32_t p_size,
                                                  StagingBufferAllocation &r_allocation)
    {
        if (p_size == 0)
        {
            return Error::kFailed;
        }

        if (p_size <= kSmallUploadMax)
        {
            static constexpr uint32_t kSmallAlignment = 32;
            uint32_t aligned_size = (p_size + kSmallAlignment - 1) & ~(kSmallAlignment - 1);

            if (aligned_size <= kSmallStagingBlockSize - small_staging_block_.fill_amount)
            {
                r_allocation.data_ptr = small_staging_block_.data_ptr + small_staging_block_.fill_amount;
                r_allocation.driver_id = small_staging_block_.driver_id;
                r_allocation.offset = small_staging_block_.fill_amount;
                small_staging_block_.fill_amount += aligned_size;
                return Error::kOK;
            }
        }

        if (p_size <= kLargeUploadThreshold)
        {
            static constexpr uint32_t kMediumAlignment = 256;
            uint32_t aligned_size = (p_size + kMediumAlignment - 1) & ~(kMediumAlignment - 1);

            if (staging_buffer_block_index_ < staging_buffer_blocks_.Size())
            {
                StagingBufferBlock &block = staging_buffer_blocks_[staging_buffer_block_index_];

                if (aligned_size <= staging_buffer_size_ - block.fill_amount)
                {
                    r_allocation.data_ptr = block.data_ptr + block.fill_amount;
                    r_allocation.driver_id = block.driver_id;
                    r_allocation.offset = block.fill_amount;
                    block.fill_amount += aligned_size;
                    return Error::kOK;
                }
            }

            for (uint32_t i = 0; i < staging_buffer_blocks_.Size(); ++i)
            {
                StagingBufferBlock &block = staging_buffer_blocks_[i];
                if (block.frame_used == 0 ||
                    block.frame_used <= frame_number_ - frame_count_)
                {
                    if (aligned_size <= staging_buffer_size_ - block.fill_amount)
                    {
                        staging_buffer_block_index_ = i;
                        r_allocation.data_ptr = block.data_ptr + block.fill_amount;
                        r_allocation.driver_id = block.driver_id;
                        r_allocation.offset = block.fill_amount;
                        block.fill_amount += aligned_size;
                        return Error::kOK;
                    }
                }
            }
        }

        uint32_t new_block_size = p_size > staging_buffer_size_ ? p_size : staging_buffer_size_;

        if (staging_buffer_total_size_ + new_block_size > kMaxStagingTotalSize)
        {
            ARHUD_LOG_ERROR("kFailed",
                "StagingBufferAllocate: total staging size exceeded limit (%llu > %llu), size=%u",
                static_cast<unsigned long long>(staging_buffer_total_size_ + new_block_size),
                static_cast<unsigned long long>(kMaxStagingTotalSize),
                p_size);
            return Error::kFailed;
        }

        StagingBufferBlock new_block;
        new_block.driver_id = device_driver_->BufferCreate(
            new_block_size,
            BitField<BufferUsageBits>(BufferUsageBits::kTransferFrom) |
                BitField<BufferUsageBits>(BufferUsageBits::kTransferTo),
            MemoryAllocationType::kCpu);
        if (!new_block.driver_id.IsValid())
        {
            ARHUD_LOG_ERROR("kFailed", "StagingBufferAllocate: failed to create new block, size=%u", new_block_size);
            return Error::kFailed;
        }

        new_block.data_ptr = device_driver_->BufferMap(new_block.driver_id);
        if (new_block.data_ptr == nullptr)
        {
            ARHUD_LOG_ERROR("kFailed", "StagingBufferAllocate: failed to map new block");
            device_driver_->BufferFree(new_block.driver_id);
            return Error::kFailed;
        }

        new_block.frame_used = frame_number_;
        new_block.fill_amount = 0;

        staging_buffer_blocks_.PushBack(new_block);
        staging_buffer_total_size_ += new_block_size;
        staging_buffer_block_index_ = staging_buffer_blocks_.Size() - 1;

        StagingBufferBlock &block = staging_buffer_blocks_[staging_buffer_block_index_];
        r_allocation.data_ptr = block.data_ptr + block.fill_amount;
        r_allocation.driver_id = block.driver_id;
        r_allocation.offset = block.fill_amount;
        block.fill_amount += p_size;

        return Error::kOK;
    }

    Error RenderingDevice::BufferUpdate(RDBufferID p_buffer, uint32_t p_offset,
                                         uint32_t p_size, const void *p_data)
    {
        if (p_buffer.IsNull() || p_data == nullptr || p_size == 0)
        {
            return Error::kFailed;
        }

        BufferID dst_driver_id = BufferGetDriverId(p_buffer);
        if (!dst_driver_id.IsValid())
        {
            ARHUD_LOG_ERROR("kFailed", "BufferUpdate: invalid buffer RID");
            return Error::kFailed;
        }

        StagingBufferAllocation alloc;
        Error err = StagingBufferAllocate(p_size, alloc);
        if (err != Error::kOK)
        {
            return err;
        }

        memcpy(alloc.data_ptr, p_data, p_size);

        BufferCopyRegion region;
        region.src_offset = alloc.offset;
        region.dst_offset = p_offset;
        region.size = p_size;

        device_driver_->CommandCopyBuffer(
            alloc.driver_id, dst_driver_id,
            VectorView<BufferCopyRegion>(&region, 1));

        return Error::kOK;
    }

    Error RenderingDevice::TextureUpdate(RDTextureID p_texture, uint32_t p_layer,
                                          uint32_t p_mipmap, const void *p_data,
                                          uint32_t p_data_size)
    {
        if (p_texture.IsNull() || p_data == nullptr || p_data_size == 0)
        {
            return Error::kFailed;
        }

        TextureID dst_driver_id = TextureGetDriverId(p_texture);
        if (!dst_driver_id.IsValid())
        {
            ARHUD_LOG_ERROR("kFailed", "TextureUpdate: invalid texture RID");
            return Error::kFailed;
        }

        const TextureFormat &format = TextureGetFormat(p_texture);

        StagingBufferAllocation alloc;
        Error err = StagingBufferAllocate(p_data_size, alloc);
        if (err != Error::kOK)
        {
            return err;
        }

        memcpy(alloc.data_ptr, p_data, p_data_size);

        BufferTextureCopyRegion region;
        region.buffer_offset = alloc.offset;
        region.row_pitch = 0;
        region.texture_subresource.aspect = TextureAspect::kColor;
        region.texture_subresource.layer = p_layer;
        region.texture_subresource.mipmap = p_mipmap;
        region.texture_offset_x = 0;
        region.texture_offset_y = 0;
        region.texture_offset_z = 0;
        region.texture_region_width = format.width;
        region.texture_region_height = format.height;
        region.texture_region_depth = 1;

        device_driver_->CommandCopyBufferToTexture(
            alloc.driver_id, dst_driver_id,
            VectorView<BufferTextureCopyRegion>(&region, 1));

        return Error::kOK;
    }

} // namespace arhud
