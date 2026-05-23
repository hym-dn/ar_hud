/**
 * @file resource_tracker.cpp
 * @brief 轻量级资源追踪器实现 — 资源使用记录与冲突检测
 *
 * 本文件实现了 ResourceTracker 类的所有方法，提供：
 * - 帧级生命周期管理（BeginFrame/EndFrame）
 * - 资源注册/注销（显式生命周期）
 * - 使用记录（命令级别的使用快照）
 * - 冲突检测（Debug 断言模式）
 *
 * @par 实现策略（Phase 1）
 *
 * **设计原则：**
 *   1. **最小开销**：所有操作 O(1) 或 O(n)（n = 活跃资源数）
 *   2. **防御性编程**：非法输入静默忽略，不崩溃
 *   3. **渐进增强**：保留扩展点供 Phase 2 屏障推导使用
 *
 * **内存管理：**
 *   - 使用 HashMap 存储资源追踪信息，避免动态数组扩容
 *   - 不做内存池化（Phase 1 资源数量有限，HashMap 足够）
 *   - Phase 2 可考虑改用 PagedAllocator + 自定义哈希表
 *
 * @see resource_tracker.h          ResourceTracker 接口定义
 * @see command_buffer_gl.h         GLCommandBuffer 集成点
 * @see rendering_device_commons.h 资源 ID 类型定义
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-09
 * @copyright Copyright (c) 2025 ARHud Project
 */

#include "resource_tracker.h"
#include "io/logger.h"

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // 生命周期管理
    // ═══════════════════════════════════════════════════════════════════════

    void ResourceTracker::BeginFrame()
    {
        current_frame_++;
        active_count_ = 0;

        for (auto it = tracks_.Begin(); it != tracks_.End(); ++it)
        {
            (*it).value.is_active_ = false;
#ifdef ARHUD_DEBUG
            (*it).value.access_count_ = 0;
#endif
        }
    }

    void ResourceTracker::EndFrame()
    {
        // Phase 1: 无额外清理操作
        // Phase 2 可能:
        //   - 检测未释放的资源（泄漏检测）
        //   - 输出帧统计信息（峰值资源数、平均使用次数等）
        //   - 清理已注销但仍在 HashMap 中的条目（lazy cleanup）
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 资源注册与注销
    // ═══════════════════════════════════════════════════════════════════════

    void ResourceTracker::Register(uint64_t p_id, ResourceType p_type, const char *p_debug_name)
    {
        if (p_id == 0)
        {
            return;
        }

        ResourceTrack *track = tracks_.GetPtr(p_id);
        if (track != nullptr)
        {
            track->type_ = p_type;
            track->last_usage_ = ResourceUsage::kNone;
            track->last_cmd_index_ = UINT32_MAX;
            track->is_active_ = true;
#ifdef ARHUD_DEBUG
            track->debug_name_ = p_debug_name;
            track->access_count_ = 0;
#endif
        }
        else
        {
            ResourceTrack new_track;
            new_track.type_ = p_type;
            new_track.last_usage_ = ResourceUsage::kNone;
            new_track.last_cmd_index_ = UINT32_MAX;
            new_track.is_active_ = true;
#ifdef ARHUD_DEBUG
            new_track.debug_name_ = p_debug_name;
            new_track.access_count_ = 0;
#endif
            tracks_.Insert(p_id, new_track);
        }

        active_count_++;
    }

    void ResourceTracker::Unregister(uint64_t p_id)
    {
        if (p_id == 0)
        {
            return;
        }

        ResourceTrack *track = tracks_.GetPtr(p_id);
        if (track != nullptr)
        {
            if (track->is_active_)
            {
                active_count_--;
            }
            tracks_.Erase(p_id);
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 使用记录
    // ═══════════════════════════════════════════════════════════════════════

    void ResourceTracker::RecordUsage(uint64_t p_id, ResourceUsage p_usage, uint32_t p_cmd_index)
    {
        if (p_id == 0 || p_usage == ResourceUsage::kNone)
        {
            return;
        }

        ResourceTrack *track = tracks_.GetPtr(p_id);
        if (track == nullptr)
        {
#ifdef ARHUD_DEBUG
            ARHUD_LOG_WARN("ResourceTracker: RecordUsage on unregistered resource id={}", p_id);
#endif
            return;
        }

        if (!track->is_active_)
        {
#ifdef ARHUD_DEBUG
            ARHUD_LOG_WARN("ResourceTracker: RecordUsage on inactive resource id={}", p_id);
#endif
            return;
        }

        track->last_usage_ = p_usage;
        track->last_cmd_index_ = p_cmd_index;
#ifdef ARHUD_DEBUG
        track->access_count_++;
#endif
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 冲突检测
    // ═══════════════════════════════════════════════════════════════════════

    bool ResourceTracker::ValidateUsage(uint64_t p_id, ResourceUsage p_usage) const
    {
        if (p_id == 0)
        {
            return true;
        }

        const ResourceTrack *track = tracks_.GetPtr(p_id);
        if (track == nullptr)
        {
#ifdef ARHUD_DEBUG
            ARHUD_LOG_WARN("ResourceTracker: ValidateUsage on unregistered resource id={} usage={}",
                           static_cast<uint32_t>(p_usage), p_id);
#endif
            return false;
        }

        if (!track->is_active_)
        {
#ifdef ARHUD_DEBUG
            ARHUD_LOG_WARN("ResourceTracker: ValidateUsage on inactive resource id={}", p_id);
#endif
            return false;
        }

        bool prev_is_write = IsWriteOnlyUsage(track->last_usage_) ||
                             track->last_usage_ == ResourceUsage::kAttachmentColorReadWrite ||
                             track->last_usage_ == ResourceUsage::kAttachmentDepthStencilReadWrite ||
                             track->last_usage_ == ResourceUsage::kStorageImageReadWrite;

        bool curr_is_write = IsWriteOnlyUsage(p_usage) ||
                             p_usage == ResourceUsage::kAttachmentColorReadWrite ||
                             p_usage == ResourceUsage::kAttachmentDepthStencilReadWrite ||
                             p_usage == ResourceUsage::kStorageImageReadWrite;

        bool prev_is_read = IsReadOnlyUsage(track->last_usage_) ||
                            track->last_usage_ == ResourceUsage::kAttachmentColorReadWrite ||
                            track->last_usage_ == ResourceUsage::kAttachmentDepthStencilReadWrite ||
                            track->last_usage_ == ResourceUsage::kStorageImageReadWrite;

        bool curr_is_read = IsReadOnlyUsage(p_usage) ||
                            p_usage == ResourceUsage::kAttachmentColorReadWrite ||
                            p_usage == ResourceUsage::kAttachmentDepthStencilReadWrite ||
                            p_usage == ResourceUsage::kStorageImageReadWrite;

        if (prev_is_write && curr_is_write)
        {
#ifdef ARHUD_DEBUG
            ARHUD_LOG_WARN("ResourceTracker: W-W hazard detected resource id={} "
                           "prev_usage={}({}) curr_usage={}({})",
                           p_id,
                           static_cast<uint32_t>(track->last_usage_), track->last_cmd_index_,
                           static_cast<uint32_t>(p_usage), "current");
#endif
            return false;
        }

        if (prev_is_read && curr_is_write)
        {
#ifdef ARHUD_DEBUG
            ARHUD_LOG_WARN("ResourceTracker: R-W (RAW) hazard detected resource id={} "
                           "prev_usage={}({}) curr_usage={}({})",
                           p_id,
                           static_cast<uint32_t>(track->last_usage_), track->last_cmd_index_,
                           static_cast<uint32_t>(p_usage), "current");
#endif
            return false;
        }

        return true;
    }

    bool ResourceTracker::ValidateAll() const
    {
        bool all_valid = true;

        for (auto it = tracks_.Begin(); it != tracks_.End(); ++it)
        {
            const ResourceTrack &track = (*it).value;

            if (!track.is_active_)
            {
                continue;
            }

            if (track.last_usage_ == ResourceUsage::kNone)
            {
#ifdef ARHUD_DEBUG
                ARHUD_LOG_VERBOSE("ResourceTracker: Zombie resource registered but never used id={} type={}",
                                  (*it).key, static_cast<uint32_t>(track.type_));
#endif
            }
        }

        return all_valid;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 查询接口
    // ═══════════════════════════════════════════════════════════════════════

    bool ResourceTracker::IsActive(uint64_t p_id) const
    {
        if (p_id == 0)
        {
            return false;
        }

        const ResourceTrack *track = tracks_.GetPtr(p_id);
        return track != nullptr && track->is_active_;
    }

    const ResourceTrack *ResourceTracker::GetTrack(uint64_t p_id) const
    {
        if (p_id == 0)
        {
            return nullptr;
        }

        return tracks_.GetPtr(p_id);
    }

#ifdef ARHUD_DEBUG
    void ResourceTracker::DumpState() const
    {
        ARHUD_LOG_INFO("[ResourceTracker] Frame={} Active={}", current_frame_, active_count_);

        for (auto it = tracks_.Begin(); it != tracks_.End(); ++it)
        {
            const ResourceTrack &track = (*it).value;

            if (!track.is_active_)
            {
                continue;
            }

            const char *type_name = "Unknown";
            switch (track.type_)
            {
            case ResourceType::kBuffer:
                type_name = "Buffer";
                break;
            case ResourceType::kTexture:
                type_name = "Texture";
                break;
            case ResourceType::kSampler:
                type_name = "Sampler";
                break;
            case ResourceType::kUniformSet:
                type_name = "UniformSet";
                break;
            case ResourceType::kPipeline:
                type_name = "Pipeline";
                break;
            case ResourceType::kRenderPass:
                type_name = "RenderPass";
                break;
            case ResourceType::kFramebuffer:
                type_name = "Framebuffer";
                break;
            default:
                break;
            }

            const char *usage_name = "None";
            switch (track.last_usage_)
            {
            case ResourceUsage::kCopyFrom:
                usage_name = "copy_from";
                break;
            case ResourceUsage::kCopyTo:
                usage_name = "copy_to";
                break;
            case ResourceUsage::kTextureSample:
                usage_name = "sampled";
                break;
            case ResourceUsage::kAttachmentColorReadWrite:
                usage_name = "color_attach_rw";
                break;
            case ResourceUsage::kAttachmentDepthStencilReadWrite:
                usage_name = "depth_stencil_rw";
                break;
            case ResourceUsage::kStorageImageReadWrite:
                usage_name = "storage_rw";
                break;
            default:
                break;
            }

            ARHUD_LOG_INFO("  [{:#x}] {:<12} {:<16} cmd={:<4} count={:<4} \"{}\"",
                           (*it).key,
                           type_name,
                           usage_name,
                           track.last_cmd_index_ == UINT32_MAX ? -1 : static_cast<int32_t>(track.last_cmd_index_),
                           track.access_count_,
                           track.debug_name_ ? track.debug_name_ : "(null)");
        }
    }
#endif

} // namespace arhud
