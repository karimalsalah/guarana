// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <utility>

#include <boost/container/small_vector.hpp>

#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/buffer_cache/usage_tracker.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_multi_range_buffer.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/surface.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class DescriptorPool;
class Scheduler;
struct HostVertexBinding;

class BufferCacheRuntime;

class Buffer : public VideoCommon::BufferBase {
public:
    explicit Buffer(BufferCacheRuntime&, VideoCommon::NullBufferParams null_params);
    explicit Buffer(BufferCacheRuntime& runtime, VAddr cpu_addr_, u64 size_bytes_,
                    bool sparse_compatible_);

    [[nodiscard]] VkBufferView View(u32 offset, u32 size, VideoCore::Surface::PixelFormat format);

    [[nodiscard]] VkBuffer Handle() const noexcept {
        return *buffer;
    }

    [[nodiscard]] VkDeviceAddress DeviceAddress() const noexcept {
        return device_address;
    }

    [[nodiscard]] bool IsSparseCompatible() const noexcept {
        return sparse_compatible;
    }

    [[nodiscard]] vk::MemoryLocation Location() const noexcept {
        return buffer.Location();
    }

    [[nodiscard]] bool IsRegionUsed(u64 offset, u64 size) const noexcept {
        return tracker.IsUsed(offset, size) || (old_usage && old_tracker.IsUsed(offset, size));
    }

    void MarkUsage(u64 offset, u64 size) noexcept;

    void ResetUsageTracking() noexcept {
        tracker.Reset();
    }

    /// EDEN_NX2_UMA bit 16, once a frame instead of ResetUsageTracking. The ranges used before
    /// the previous turn are forgotten once the host GPU is past the last of those uses, and
    /// the ranges used since take their place.
    void AgeUsageTracking() noexcept;

    [[nodiscard]] u64 LastUsageTick() const noexcept {
        return last_usage_tick;
    }

    /// Empty for a buffer mapped only for direct uploads: it is downloaded as an unmapped one.
    [[nodiscard]] std::span<const u8> Mapped() const noexcept {
        return buffer.IsHostCoherent() && !mapped_for_uploads ? buffer.Mapped()
                                                              : std::span<const u8>{};
    }

    /// EDEN_NX2_UMA bit 2: the buffer's memory while uploads may be written straight into it.
    [[nodiscard]] std::span<u8> DirectUploadSpan() const noexcept {
        return direct_upload_span;
    }

    /// For a GPU use the usage tracker cannot follow: uploads go through the GPU from now on.
    void DisableDirectUploads() noexcept {
        direct_upload_span = {};
    }

    operator VkBuffer() const noexcept {
        return *buffer;
    }

private:
    struct BufferView {
        u32 offset;
        u32 size;
        VideoCore::Surface::PixelFormat format;
        vk::BufferView handle;
    };

    const Device* device{};
    Scheduler* scheduler{};
    vk::Buffer buffer;
    std::vector<BufferView> views;
    VideoCommon::UsageTracker tracker;
    VideoCommon::UsageTracker old_tracker; ///< The uses before AgeUsageTracking's last turn
    std::span<u8> direct_upload_span;
    VkDeviceAddress device_address{};
    u64 last_usage_tick{};
    u64 old_usage_tick{}; ///< The last use old_tracker holds
    bool old_usage{};     ///< old_tracker holds uses the host GPU may not have run yet
    bool new_usage{};     ///< Used since AgeUsageTracking's last turn
    bool mapped_for_uploads{};
    bool is_null{};
    bool sparse_compatible{};
};

class QuadArrayIndexBuffer;
class QuadStripIndexBuffer;

class BufferCacheRuntime {
    friend Buffer;

    using PrimitiveTopology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
    using IndexFormat = Tegra::Engines::Maxwell3D::Regs::IndexFormat;

public:
    explicit BufferCacheRuntime(const Device& device_, MemoryAllocator& memory_manager_,
                                Scheduler& scheduler_, StagingBufferPool& staging_pool_,
                                GuestDescriptorQueue& guest_descriptor_queue,
                                ComputePassDescriptorQueue& compute_pass_descriptor_queue,
                                DescriptorPool& descriptor_pool);

    void TickFrame(Common::SlotVector<Buffer>& slot_buffers) noexcept;

    /// Returns the mask of bindings whose repacked state changed.
    u32 SetVertexRepackLayout(const Tegra::Engines::Maxwell3D::Regs& regs, bool dynamic_input);

    bool IsVertexRepacked(u32 binding) const {
        return (vertex_repack_mask & (1U << binding)) != 0;
    }

    bool AnyVertexRepacked() const {
        return vertex_repack_mask != 0;
    }

    u64 CurrentTick();

    bool IsFree(u64 tick);

    void Wait(u64 tick);

    /// Waits for a submitted tick without flushing: safe on threads other than the GPU thread.
    void WaitSubmittedTick(u64 tick);

    void Finish();

    void FinishForHostRead();

    /// FinishForHostRead without the wait: submits, and returns the tick to wait for.
    u64 SubmitForHostRead();

    /// SubmitForHostRead without the submission: the copies go with the GPU thread's next one, as
    /// a fence's do (Scheduler::DeferFlush). Returns the tick they go in.
    u64 QueueForHostRead();

    /// Waits for a SubmitForHostRead or QueueForHostRead tick, on any thread.
    void WaitForHostRead(u64 tick);

    /// Whether the host GPU completed `tick`, asking it when the tick is submitted but not known
    /// to be done.
    [[nodiscard]] bool IsFreeNow(u64 tick);

    u64 GetDeviceLocalMemory() const;

    u64 GetDeviceMemoryUsage() const;

    bool CanReportMemoryUsage() const;

    u32 GetUniformBufferAlignment() const;

    u32 GetStorageBufferAlignment() const;

    /// Largest range a storage buffer binding may have
    u64 GetMaxStorageBufferRange() const;

    [[nodiscard]] StagingBufferRef UploadStagingBuffer(size_t size);

    [[nodiscard]] StagingBufferRef DownloadStagingBuffer(size_t size, bool deferred = false);

    /// Called for every staged upload that may be reordered. With direct uploads on, it also
    /// marks the destination used: the copy is a GPU write still to run.
    bool CanReorderUpload(Buffer& buffer, std::span<const VideoCommon::BufferCopy> copies);

    /// EDEN_NX2_UMA bit 2. The memory of `buffer` when these copies can be written straight
    /// into it, null when they have to go through a staging buffer: the buffer is not mapped,
    /// or work recorded and not yet run by the host GPU uses some of the bytes.
    [[nodiscard]] u8* DirectUploadTarget(Buffer& buffer,
                                         std::span<const VideoCommon::BufferCopy> copies);

    /// Zeroes a buffer no command uses yet through its mapping. False when it has none (or is
    /// too large to fill on this thread): the caller clears it on the GPU.
    [[nodiscard]] bool ClearNewBufferDirect(Buffer& buffer);

    /// Whether every GPU use of a buffer has to reach its usage tracker: direct uploads go by
    /// it across submissions, reordered ones only within the command buffer.
    [[nodiscard]] bool TracksEveryUse() const noexcept {
        return uma_direct;
    }

    /// EDEN_NX2_UMA bit 32 (BufferCache::Nx2CopyListPages).
    [[nodiscard]] bool MergesListPageRuns() const noexcept {
        return uma_list_runs;
    }

    /// The call site the next CopyBuffer is timed under (gpu_costs emu rows buffer_copy.<site>
    /// and buffer_upload_batch.<site>). CopyBuffer goes back to Other.
    void SetCopySite(VideoCommon::BufferCopySite site) noexcept {
        copy_site = site;
    }

    void FreeDeferredStagingBuffer(StagingBufferRef& ref);

    void PreCopyBarrier();

    void CopyBuffer(VkBuffer dst_buffer, VkBuffer src_buffer,
                    std::span<const VideoCommon::BufferCopy> copies, bool barrier,
                    bool can_reorder_upload = false);

    void PostCopyBarrier();

    void ClearBuffer(VkBuffer dest_buffer, u32 offset, size_t size, u32 value);

    void BindIndexBuffer(PrimitiveTopology topology, IndexFormat index_format, u32 num_indices,
                         u32 base_vertex, VkBuffer buffer, u32 offset, u32 size);

    void BindQuadIndexBuffer(PrimitiveTopology topology, u32 first, u32 count);

    bool WidensUint8IndicesOnHost() const {
        return device.IsMoltenVK() && !device.IsExtIndexTypeUint8Supported();
    }

    void BindHostUint8IndexBuffer(std::span<const u8> indices);

    void BindVertexBuffer(u32 index, VkBuffer buffer, u32 offset, u32 size, u32 stride);

    void BindVertexBuffers(VideoCommon::HostBindings<Buffer>& bindings);

    void BindTransformFeedbackBuffer(u32 index, VkBuffer buffer, u32 offset, u32 size);

    void BindTransformFeedbackBuffers(VideoCommon::HostBindings<Buffer>& bindings);

    /// This and the two members below are the hooks of VideoCommon::StreamUniformReusable
    /// (buffer_cache/uniform_stream_reuse.h).
    struct StreamUniformRef {
        VkBuffer buffer;
        VkDeviceAddress device_address; // base of `buffer`, 0 without buffer device address
        u32 offset;
    };

    std::span<u8> BindMappedUniformBuffer([[maybe_unused]] size_t stage,
                                          [[maybe_unused]] u32 binding_index,
                                          u32 size) {
        const StagingBufferRef ref = staging_pool.Request(size, MemoryUsage::Upload);
        last_stream_uniform = StreamUniformRef{
            .buffer = ref.buffer,
            .device_address = ref.device_address,
            .offset = static_cast<u32>(ref.offset),
        };
        RebindStreamUniform(last_stream_uniform, size);
        return ref.mapped_span;
    }

    /// The allocation of the latest BindMappedUniformBuffer call.
    [[nodiscard]] StreamUniformRef LastStreamUniformRef() const noexcept {
        return last_stream_uniform;
    }

    /// Adds `ref` to the descriptor payload exactly as BindMappedUniformBuffer did when it made
    /// the allocation (it is the very same call). The caller guarantees the allocation is still
    /// live: made on the scheduler tick that is current now, so the staging pool has not recycled
    /// it and the GPU has not even started on it.
    void RebindStreamUniform(const StreamUniformRef& ref, u32 size) {
        guest_descriptor_queue.AddBuffer(ref.buffer, ref.device_address, ref.offset, size);
    }

    [[nodiscard]] VkDeviceSize SparseAlignmentFor(bool sparse_compatible) const noexcept {
        if (!sparse_compatible || !multi_range_buffers.use_sparse) {
            return 0;
        }
        return multi_range_buffers.block_size;
    }

    [[nodiscard]] bool PrefersSparseSources() const noexcept {
        return multi_range_buffers.use_sparse;
    }

    void ResetMultiRange() noexcept {
        multi_range_sources.clear();
        multi_range_total = 0;
    }

    void PushMultiRangeSource(const Buffer& buffer, u32 offset, u32 size) {
        const vk::MemoryLocation location = buffer.Location();
        multi_range_sources.push_back(MultiRangeSource{
            .handle = buffer.Handle(),
            .memory = location.memory,
            .memory_offset = location.offset,
            .offset = offset,
            .size = size,
            .write_tick = buffer.getWriteTick(),
            .memory_type = location.memory_type,
        });
        multi_range_total += size;
    }

    bool BindMultiRangeStorageBuffer(u64 key, bool is_written);

    void InvalidateMultiRange(u64 key) {
        multi_range_buffers.Invalidate(key);
    }

    void OnBufferDeleted(const Buffer& buffer) {
        multi_range_buffers.DropOwner(scheduler, buffer.Handle());
    }

    void BindUniformBuffer(const Buffer& buffer, u32 offset, u32 size) {
        BindBuffer(buffer, offset, size);
    }

    void BindStorageBuffer(const Buffer& buffer, u32 offset, u32 size,
                           [[maybe_unused]] bool is_written) {
        BindBuffer(buffer, offset, size);
    }

    void BindTextureBuffer(Buffer& buffer, u32 offset, u32 size,
                           VideoCore::Surface::PixelFormat format) {
        // An unmapped texture buffer has no format; its view is the null buffer's anyway.
        if (format == VideoCore::Surface::PixelFormat::Invalid) {
            format = VideoCore::Surface::PixelFormat::R8_UNORM;
        }
        guest_descriptor_queue.AddTexelBuffer(buffer.View(offset, size, format),
                                              buffer.DeviceAddress(), offset, size,
                                              TexelBufferFormat(format));
    }

    bool ShouldLimitDynamicStorageBuffers() const {
        return limit_dynamic_storage_buffers;
    }

    u32 GetMaxDynamicStorageBuffers() const {
        return max_dynamic_storage_buffers;
    }

    // List-fed grid macros (nx2_list_grid.h): copies of a command list's three words made at the
    // list's place in the stream, by keep id. The front reads them back when it needs the values,
    // uploads that carry their markers get them copied over, and dispatches take their grid there.
    struct ListKeep {
        u64 keep_id{};
        u64 tick{}; ///< The submission that copies them
        bool verified{};
        u64 hash{};                ///< If verified: the macro the list fed
        std::array<u32, 3> real{}; ///< If verified: the words the front read the old way
    };
    /// Records, outside the render pass, the copy of the three words at `offset` of `source`.
    /// Verify mode: `real` are the words the front read the old way for the macro `hash`, else
    /// empty.
    void KeepListWords(u64 keep_id, VkBuffer source, VkDeviceSize offset, u64 hash,
                       std::span<const u32> real);
    /// The keep id ending in `keep_bits` whose slot still holds its words.
    [[nodiscard]] std::optional<u64> FindKeptList(u32 keep_bits) const;
    /// The kept words (or with `arguments`, the dispatch arguments made from them), after waiting
    /// for their copy. False if the slot holds another list.
    [[nodiscard]] bool ReadKeptListWords(u64 keep_id, std::span<u32> words, bool arguments = false);
    /// Verify mode: the kept list `keep_id`, if its words were also read the old way.
    [[nodiscard]] const ListKeep* KeptVerify(u64 keep_id) const;
    /// Records the copy of kept word `index` over the word at `offset` of `destination`.
    void CopyKeptWord(u64 keep_id, u32 index, VkBuffer destination, VkDeviceSize offset);
    /// Where the kept list's dispatch arguments are: x, then y and z as 16 bits, as
    /// KeplerCompute::DispatchNativeCompute takes them from a QMD.
    [[nodiscard]] std::pair<VkBuffer, VkDeviceSize> KeptGrid(u64 keep_id) const;

private:
    void BindBuffer(const Buffer& buffer, u32 offset, u32 size) {
        const VkBuffer handle = buffer.Handle();
        if (handle == VK_NULL_HANDLE) {
            guest_descriptor_queue.AddBuffer(handle, 0, 0, VK_WHOLE_SIZE);
        } else {
            guest_descriptor_queue.AddBuffer(handle, buffer.DeviceAddress(), offset, size);
        }
    }

    VkFormat TexelBufferFormat(VideoCore::Surface::PixelFormat format) const;

    void ReserveNullBuffer();
    vk::Buffer CreateNullBuffer();

    /// EDEN_NX2_UMA: logs the uploads of the last second.
    void ReportUmaUploads() noexcept;

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    StagingBufferPool& staging_pool;
    GuestDescriptorQueue& guest_descriptor_queue;

    /// EDEN_NX2_UMA bits, latched when the renderer starts.
    bool uma_coalesce{};
    bool uma_direct{};
    bool uma_generations{};
    bool uma_list_runs{};
    bool uma_batch_copies{};
    VideoCommon::BufferCopySite copy_site{}; ///< SetCopySite's, for the next CopyBuffer
    struct UmaUploads {
        u64 direct{};
        u64 direct_bytes{};
        u64 reordered{};
        u64 reordered_bytes{};
        u64 in_order{};
        u64 in_order_bytes{};
        u64 unmapped{}; ///< Staged because the buffer has no mapping for direct uploads
        u64 busy{};     ///< Staged because pending GPU work uses the destination
        u64 frames{};
    };
    UmaUploads uma_uploads;
    std::chrono::steady_clock::time_point uma_report_time{};

    std::shared_ptr<QuadArrayIndexBuffer> quad_array_index_buffer;
    std::shared_ptr<QuadStripIndexBuffer> quad_strip_index_buffer;

    vk::Buffer null_buffer;

    /// Kept list words: a host-readable slot per keep id (modulo the slot count) holding the
    /// three words at 0 and the dispatch arguments at LIST_KEEP_GRID (ListGridArgumentsPass).
    static constexpr u32 LIST_KEEP_SLOTS = 1024;
    static constexpr u32 LIST_KEEP_SLOT_BYTES = 32;
    static constexpr VkDeviceSize LIST_KEEP_GRID = 16;
    void EnsureListKeepBuffer();
    [[nodiscard]] static VkDeviceSize ListKeepOffset(u64 keep_id) {
        return VkDeviceSize{keep_id % LIST_KEEP_SLOTS} * LIST_KEEP_SLOT_BYTES;
    }
    vk::Buffer list_keep_buffer;
    std::unique_ptr<ListGridArgumentsPass> list_grid_arguments_pass;
    DescriptorPool* list_pass_descriptor_pool{};
    ComputePassDescriptorQueue* list_pass_descriptor_queue{};
    std::array<ListKeep, LIST_KEEP_SLOTS> list_keeps{};

    std::unique_ptr<Uint8Pass> uint8_pass;
    std::unique_ptr<VertexRepackPass> vertex_repack_pass;
    u32 vertex_repack_mask{};
    bool static_vertex_repack{};
    QuadIndexedPass quad_index_pass;

    MultiRangeBufferCache multi_range_buffers;
    boost::container::small_vector<MultiRangeSource, 16> multi_range_sources;
    VkDeviceSize multi_range_total{};

    StreamUniformRef last_stream_uniform{};

    bool limit_dynamic_storage_buffers = false;
    u32 max_dynamic_storage_buffers = (std::numeric_limits<u32>::max)();

    /// The index buffer BindIndexBuffer recorded last, at scheduler recording epoch bound_index_epoch.
    VkBuffer bound_index_buffer = VK_NULL_HANDLE;
    VkDeviceSize bound_index_offset = 0;
    VkIndexType bound_index_type = VK_INDEX_TYPE_UINT32;
    u64 bound_index_epoch = 0;
    bool bound_index_valid = false;
};

struct BufferCacheParams {
    using Runtime = Vulkan::BufferCacheRuntime;
    using Buffer = Vulkan::Buffer;
    using Async_Buffer = Vulkan::StagingBufferRef;
    using MemoryTracker = VideoCommon::MemoryTrackerBase<Tegra::MaxwellDeviceMemoryManager>;

    static constexpr bool IS_OPENGL = false;
    static constexpr bool HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS = false;
    static constexpr bool HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT = false;
    static constexpr bool NEEDS_BIND_UNIFORM_INDEX = false;
    static constexpr bool NEEDS_BIND_STORAGE_INDEX = false;
    static constexpr bool USE_MEMORY_MAPS = true;
    static constexpr bool SEPARATE_IMAGE_BUFFER_BINDINGS = false;
    static constexpr bool USE_MEMORY_MAPS_FOR_UPLOADS = true;
};

using BufferCache = VideoCommon::BufferCache<BufferCacheParams>;

} // namespace Vulkan
