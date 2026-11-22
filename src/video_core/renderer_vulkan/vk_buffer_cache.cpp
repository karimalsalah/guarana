// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <span>
#include <utility>
#include <vector>

#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/nx2_async_readback.h"
#include "video_core/nx2_perf.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_mapped_reads.h"
#include "video_core/nx2_uma.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vertex_repack.h"

#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_texel_buffer_range.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/texture_cache/util.h"

#include <vulkan/utility/vk_format_utils.h>

namespace Vulkan {
namespace {
// Zero-filled space after each buffer on MoltenVK. Shaders declare every constant buffer at its
// full 64 KiB and Metal validates that length from the binding offset.
constexpr VkDeviceSize MOLTENVK_BUFFER_TAIL = 65536;

/// The emu rows of a copy recorded on its own and of an upload batch, by call site: the
/// BufferCopy* and BufferUploadBatch* categories are in BufferCopySite order.
constexpr TraceCategory CopyCategory(VideoCommon::BufferCopySite site) {
    return static_cast<TraceCategory>(static_cast<u32>(TraceCategory::BufferCopyOther) +
                                      static_cast<u32>(site));
}

constexpr TraceCategory UploadBatchCategory(VideoCommon::BufferCopySite site) {
    return static_cast<TraceCategory>(static_cast<u32>(TraceCategory::BufferUploadBatchOther) +
                                      static_cast<u32>(site));
}

static_assert(CopyCategory(VideoCommon::BufferCopySite::ListPage) ==
              TraceCategory::BufferCopyListPage);
static_assert(CopyCategory(VideoCommon::BufferCopySite::Gather) == TraceCategory::BufferCopyGather);
static_assert(CopyCategory(VideoCommon::BufferCopySite::Count) ==
              TraceCategory::BufferUploadBatchOther);
static_assert(UploadBatchCategory(VideoCommon::BufferCopySite::ListPage) ==
              TraceCategory::BufferUploadBatchListPage);
static_assert(UploadBatchCategory(VideoCommon::BufferCopySite::Count) ==
              TraceCategory::BufferUploadBatchMixed);

VkBufferCopy MakeBufferCopy(const VideoCommon::BufferCopy& copy) {
    return VkBufferCopy{
        .srcOffset = copy.src_offset,
        .dstOffset = copy.dst_offset,
        .size = copy.size,
    };
}

VkIndexType IndexTypeFromNumElements(const Device& device, u32 num_elements) {
    if (num_elements <= 0xff && device.IsExtIndexTypeUint8Supported()) {
        return VK_INDEX_TYPE_UINT8_EXT;
    }
    if (num_elements <= 0xffff) {
        return VK_INDEX_TYPE_UINT16;
    }
    return VK_INDEX_TYPE_UINT32;
}

size_t BytesPerIndex(VkIndexType index_type) {
    switch (index_type) {
    case VK_INDEX_TYPE_UINT8_EXT:
        return 1;
    case VK_INDEX_TYPE_UINT16:
        return 2;
    case VK_INDEX_TYPE_UINT32:
        return 4;
    default:
        ASSERT_MSG(false, "Invalid index type={}", index_type);
        return 1;
    }
}

vk::Buffer CreateBuffer(const Device& device, const MemoryAllocator& memory_allocator,
                        StagingBufferPool& staging_pool, u64 size, VkDeviceSize sparse_alignment,
                        bool host_mapped) {
    VkBufferUsageFlags flags =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (device.IsExtTransformFeedbackSupported()) {
        flags |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsExtConditionalRendering()) {
        flags |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        flags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    const VkBufferCreateInfo buffer_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = size + (device.IsMoltenVK() ? MOLTENVK_BUFFER_TAIL : 0),
        .usage = flags,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    // The staging pool holds most of the memory that can be freed now (#262: 926 MiB of an Intel
    // UHD's 3.2 GB heap when a 128 KiB buffer had no room after a scene change).
    return CreateAfterRecovery(
        [&] {
            if (sparse_alignment > 1) {
                return memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::DeviceLocal,
                                                     sparse_alignment);
            }
            // host_mapped (EDEN_NX2_UMA bit 2): on an integrated GPU every memory type is system
            // memory, so a mapped one costs the GPU nothing and takes uploads without a staging
            // copy. EDEN_NX2_MAPPED_READS uses the same mapping so a download can read it in place.
            const bool mapped_reads =
                VideoCommon::Nx2MappedReads::Active(device.IsIntegrated(), device.IsMoltenVK());
            const MemoryUsage usage = device.IsMoltenVK() || host_mapped || mapped_reads
                                           ? MemoryUsage::Stream
                                           : MemoryUsage::DeviceLocal;
            return memory_allocator.CreateBuffer(buffer_ci, usage);
        },
        [&] {
            static u32 logged{};
            if (logged++ < 32) {
                LOG_WARNING(Render_Vulkan,
                            "No memory for a {} byte buffer; freeing staging buffers and retrying",
                            size);
            }
            staging_pool.ReleaseMemory();
        });
}

/// Buffers larger than this are cleared on the GPU even when mapped: zeroing them here would
/// stall the GPU thread.
constexpr u64 MAX_DIRECT_CLEAR_BYTES = 32ULL << 20;
} // Anonymous namespace

Buffer::Buffer(BufferCacheRuntime& runtime, VideoCommon::NullBufferParams null_params)
    : VideoCommon::BufferBase(null_params), arena{&runtime.arena}, tracker{4096},
      old_tracker{4096} {
    if (runtime.device.HasNullDescriptor()) {
        return;
    }
    device = &runtime.device;
    buffer = runtime.CreateNullBuffer();
    is_null = true;
    if (device->IsBufferDeviceAddressSupported()) {
        device_address = device->GetLogical().GetBufferDeviceAddress(*buffer);
    }
}

Buffer::Buffer(BufferCacheRuntime& runtime, DAddr cpu_addr_, u64 size_bytes_,
               bool sparse_compatible_)
    : VideoCommon::BufferBase(cpu_addr_, size_bytes_), device{&runtime.device},
      arena{&runtime.arena},
      buffer{CreateBuffer(*device, runtime.memory_allocator, runtime.staging_pool, SizeBytes(),
                          runtime.SparseAlignmentFor(sparse_compatible_),
                          runtime.uma_direct && cpu_addr_ != 0)},
      tracker{SizeBytes()}, old_tracker{runtime.uma_generations ? SizeBytes() : 0} {
    sparse_compatible = sparse_compatible_;
    if (device->IsMoltenVK()) {
        runtime.arena.OutsideRenderPass();
        runtime.arena.Fill(*buffer, SizeBytes(), MOLTENVK_BUFFER_TAIL, VK_ACCESS_UNIFORM_READ_BIT);
    }
    // Not the inline index buffer (address 0): it is freed at once when it grows, while recorded
    // draws still read it, and a mapped successor zeroed here could take its memory.
    if (runtime.uma_direct && cpu_addr_ != 0 &&
        runtime.SparseAlignmentFor(sparse_compatible_) <= 1) {
        mapped_for_uploads = true;
        if (buffer.IsHostCoherent()) {
            direct_upload_span = buffer.Mapped();
        }
    }
    if (runtime.device.NamesObjects()) {
        buffer.SetObjectNameEXT(fmt::format("Buffer {:#x}", CpuAddr()).c_str());
    }
    if (device->IsBufferDeviceAddressSupported()) {
        device_address = device->GetLogical().GetBufferDeviceAddress(*buffer);
    }
}

void Buffer::MarkUsage(u64 offset, u64 size) noexcept {
    tracker.Track(offset, size);
    last_usage_tick = arena->Now();
    new_usage = true;
    if (offset + size > SizeBytes()) [[unlikely]] {
        // The tracker drops a range that ends past the buffer: nothing vouches for its bytes.
        direct_upload_span = {};
    }
}

void Buffer::AgeUsageTracking() noexcept {
    // One tick past the last use: its command is recorded after the mark, and a submission in
    // between would move it to the next tick.
    if (old_usage && arena->IsFree(old_usage_tick + 1)) {
        old_tracker.Reset();
        old_usage = false;
    }
    if (!old_usage && new_usage) {
        // old_tracker is empty here: it becomes the tracker of the uses to come.
        std::swap(tracker, old_tracker);
        old_usage_tick = last_usage_tick;
        old_usage = true;
        new_usage = false;
    }
}

VkBufferView Buffer::View(VkDeviceSize offset, VkDeviceSize size, VkFormat format) {
    const auto it{std::ranges::find_if(views, [offset, size, format](const BufferView& view) {
        return offset == view.offset && size == view.size && format == view.format;
    })};
    if (it != views.end()) {
        return *it->handle;
    }
    try {
        auto view = device->GetLogical().CreateBufferView({
            .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .buffer = *buffer,
            .format = format,
            .offset = offset,
            .range = size,
        });
        views.push_back({offset, size, format, std::move(view)});
        return *views.back().handle;
    } catch (const vk::Exception& exception) {
        static std::atomic<u32> logged{};
        const auto result = exception.GetResult();
        if (logged.fetch_add(1, std::memory_order_relaxed) < 32 || result == VK_ERROR_DEVICE_LOST) {
            LOG_ERROR(Render_Vulkan,
                      "vkCreateBufferView failed: VkResult={} buffer={:#x} format={} offset={} range={}",
                      static_cast<s32>(result), reinterpret_cast<u64>(*buffer), format,
                      offset, size);
        }
        if (result == VK_ERROR_DEVICE_LOST) {
            device->ReportLoss();
        }
        return VK_NULL_HANDLE;
    }
}

class QuadIndexBuffer {
public:
    QuadIndexBuffer(const Device& device_, MemoryAllocator& memory_allocator_,
                    Scheduler& scheduler_, StagingBufferPool& staging_pool_, BufferArena& arena_)
        : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_},
          staging_pool{staging_pool_}, arena{arena_} {}

    virtual ~QuadIndexBuffer() = default;

    void UpdateBuffer(u32 num_indices_) {
        while (!retired_buffers.empty() && scheduler.IsFree(retired_buffers.front().first)) {
            retired_buffers.erase(retired_buffers.begin());
        }
        if (num_indices_ <= num_indices) {
            return;
        }

        // Commands recorded so far may still use the old buffer: it stays until the GPU is past
        // them. Waiting for the GPU here stalled the thread ~150 ms in the middle of a load.
        if (buffer) {
            retired_buffers.emplace_back(scheduler.CurrentTick(), std::move(buffer));
        }

        // Grown by at least half, so that a draw count creeping up doesn't redo this every time.
        num_indices = (std::max)(num_indices_, num_indices + num_indices / 2);
        index_type = IndexTypeFromNumElements(device, num_indices);

        const u32 num_quads = GetQuadsNum(num_indices);
        const u32 num_triangle_indices = num_quads * 6;
        const u32 num_first_offset_copies = 4;
        const size_t bytes_per_index = BytesPerIndex(index_type);
        const size_t size_bytes = num_triangle_indices * bytes_per_index * num_first_offset_copies;
        const VkBufferCreateInfo buffer_ci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = size_bytes,
            .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        buffer = memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::DeviceLocal);
        if (device.NamesObjects()) {
            buffer.SetObjectNameEXT("Quad LUT");
        }

        const bool host_visible = buffer.IsHostVisible();
        const StagingBufferRef staging = [&] {
            if (host_visible) {
                return StagingBufferRef{};
            }
            return staging_pool.Request(size_bytes, MemoryUsage::Upload);
        }();

        u8* staging_data = host_visible ? buffer.Mapped().data() : staging.mapped_span.data();
        const size_t quad_size = bytes_per_index * 6;

        for (u32 first = 0; first < num_first_offset_copies; ++first) {
            for (u32 quad = 0; quad < num_quads; ++quad) {
                MakeAndUpdateIndices(staging_data, quad_size, quad, first);
                staging_data += quad_size;
            }
        }

        if (!host_visible) {
            arena.OutsideRenderPass();
            arena.IndexTable(staging.buffer, staging.offset, *buffer, size_bytes);
        } else {
            buffer.Flush();
        }
    }

    void BindBuffer(u32 first) {
        const VkIndexType index_type_ = index_type;
        const size_t sub_first_offset = static_cast<size_t>(first % 4) * GetQuadsNum(num_indices);
        const size_t offset =
            (sub_first_offset + GetQuadsNum(first)) * 6ULL * BytesPerIndex(index_type);
        arena.BindIndex(*buffer, offset, index_type_, false);
    }

protected:
    virtual u32 GetQuadsNum(u32 num_indices) const = 0;

    virtual void MakeAndUpdateIndices(u8* staging_data, size_t quad_size, u32 quad, u32 first) = 0;

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    StagingBufferPool& staging_pool;
    BufferArena& arena;

    vk::Buffer buffer{};
    std::vector<std::pair<u64, vk::Buffer>> retired_buffers; ///< GPU tick that frees each one
    MemoryCommit memory_commit{};
    VkIndexType index_type{};
    u32 num_indices = 0;
};

class QuadArrayIndexBuffer : public QuadIndexBuffer {
public:
    QuadArrayIndexBuffer(const Device& device_, MemoryAllocator& memory_allocator_,
                         Scheduler& scheduler_, StagingBufferPool& staging_pool_,
                         BufferArena& arena_)
        : QuadIndexBuffer(device_, memory_allocator_, scheduler_, staging_pool_, arena_) {}

    ~QuadArrayIndexBuffer() = default;

private:
    u32 GetQuadsNum(u32 num_indices_) const override {
        return num_indices_ / 4;
    }

    template <typename T>
    static std::array<T, 6> MakeIndices(u32 quad, u32 first) {
        std::array<T, 6> indices{0, 1, 2, 0, 2, 3};
        for (T& index : indices) {
            index = static_cast<T>(first + index + quad * 4);
        }
        return indices;
    }

    void MakeAndUpdateIndices(u8* staging_data, size_t quad_size, u32 quad, u32 first) override {
        switch (index_type) {
        case VK_INDEX_TYPE_UINT8_EXT:
            std::memcpy(staging_data, MakeIndices<u8>(quad, first).data(), quad_size);
            break;
        case VK_INDEX_TYPE_UINT16:
            std::memcpy(staging_data, MakeIndices<u16>(quad, first).data(), quad_size);
            break;
        case VK_INDEX_TYPE_UINT32:
            std::memcpy(staging_data, MakeIndices<u32>(quad, first).data(), quad_size);
            break;
        default:
            ASSERT(false);
            break;
        }
    }
};

class QuadStripIndexBuffer : public QuadIndexBuffer {
public:
    QuadStripIndexBuffer(const Device& device_, MemoryAllocator& memory_allocator_,
                         Scheduler& scheduler_, StagingBufferPool& staging_pool_,
                         BufferArena& arena_)
        : QuadIndexBuffer(device_, memory_allocator_, scheduler_, staging_pool_, arena_) {}

    ~QuadStripIndexBuffer() = default;

private:
    u32 GetQuadsNum(u32 num_indices_) const override {
        return num_indices_ >= 4 ? (num_indices_ - 2) / 2 : 0;
    }

    template <typename T>
    static std::array<T, 6> MakeIndices(u32 quad, u32 first) {
        std::array<T, 6> indices{0, 3, 1, 0, 2, 3};
        for (T& index : indices) {
            index = static_cast<T>(first + index + quad * 2);
        }
        return indices;
    }

    void MakeAndUpdateIndices(u8* staging_data, size_t quad_size, u32 quad, u32 first) override {
        switch (index_type) {
        case VK_INDEX_TYPE_UINT8_EXT:
            std::memcpy(staging_data, MakeIndices<u8>(quad, first).data(), quad_size);
            break;
        case VK_INDEX_TYPE_UINT16:
            std::memcpy(staging_data, MakeIndices<u16>(quad, first).data(), quad_size);
            break;
        case VK_INDEX_TYPE_UINT32:
            std::memcpy(staging_data, MakeIndices<u32>(quad, first).data(), quad_size);
            break;
        default:
            ASSERT(false);
            break;
        }
    }
};

BufferCacheRuntime::BufferCacheRuntime(const Device& device_, MemoryAllocator& memory_allocator_,
                                       Scheduler& scheduler_, StagingBufferPool& staging_pool_,
                                       GuestDescriptorQueue& guest_descriptor_queue_,
                                       ComputePassDescriptorQueue& compute_pass_descriptor_queue,
                                       DescriptorPool& descriptor_pool)
    : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_},
      master_semaphore{scheduler_.GetMasterSemaphore()}, staging_pool{staging_pool_},
      guest_descriptor_queue{&guest_descriptor_queue_}, arena{device_, scheduler_},
      quad_index_pass(device, scheduler, descriptor_pool, staging_pool,
                      compute_pass_descriptor_queue),
      multi_range_buffers(device_) {
    namespace Uma = VideoCommon::Nx2Uma;
    uma_coalesce = Uma::Has(Uma::Coalesce);
    // MoltenVK's buffers are mapped already and downloaded through their mapping.
    uma_direct = Uma::Has(Uma::DirectUploads) && device.IsIntegrated() && !device.IsMoltenVK();
    uma_generations = Uma::Has(Uma::UsageGenerations);
    uma_list_runs = Uma::Has(Uma::ListPageRuns);
    uma_batch_copies = Uma::Has(Uma::BatchCopies);
    if (Uma::g_flags != 0) {
        LOG_INFO(Render_Vulkan,
                 "EDEN_NX2_UMA={:#x}: copy coalescing {}, direct uploads {}{}, usage generations "
                 "{}, adaptive submissions {}, list page runs {}, batched copies {}",
                 Uma::g_flags, uma_coalesce ? "on" : "off", uma_direct ? "on" : "off",
                 Uma::Has(Uma::DirectUploads) && !uma_direct ? " (not an integrated GPU)" : "",
                 uma_generations ? "on" : "off", Uma::Has(Uma::AdaptiveSubmit) ? "on" : "off",
                 uma_list_runs ? "on" : "off", uma_batch_copies ? "on" : "off");
        if (Uma::Has(Uma::HostImageCopy)) {
            LOG_WARNING(Render_Vulkan,
                        "EDEN_NX2_UMA bit 4 (host image copy) is not implemented");
        }
    }
    if (VideoCommon::Nx2MappedReads::Enabled()) {
        const bool active =
            VideoCommon::Nx2MappedReads::Active(device.IsIntegrated(), device.IsMoltenVK());
        LOG_INFO(Render_Vulkan,
                 "EDEN_NX2_MAPPED_READS: host-visible buffer cache {} (integrated {}, MoltenVK {})",
                 active ? "on" : "off", device.IsIntegrated() ? "yes" : "no",
                 device.IsMoltenVK() ? "yes" : "no");
    }
    const VkDriverIdKHR driver_id = device.GetDriverID();
    const bool enable_vertex_repack = VertexRepackEnabled();
    LOG_INFO(Render_Vulkan, "69-byte vertex layout repacking {} ({})",
             enable_vertex_repack ? "enabled" : "disabled",
             "portable Vulkan layout fallback");
    static_vertex_repack =
        StaticVertexRepackEnabled(device.GetMinVertexInputBindingStrideAlignment());
    LOG_INFO(Render_Vulkan, "Vertex repacking without dynamic vertex input {}",
             static_vertex_repack ? "enabled" : "disabled");
    if (enable_vertex_repack) {
        vertex_repack_pass = std::make_unique<VertexRepackPass>(
            device, scheduler, descriptor_pool, staging_pool, compute_pass_descriptor_queue);
    }
    limit_dynamic_storage_buffers = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY ||
                                    driver_id == VK_DRIVER_ID_ARM_PROPRIETARY;
    if (limit_dynamic_storage_buffers) {
        // Guest storage buffers are bound as plain VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, so the
        // per-stage limit applies, not the much lower dynamic one (16 on Adreno 830, which left
        // Ocarina of Time's compute shaders with half their buffers unbound).
        max_dynamic_storage_buffers = device.GetMaxPerStageDescriptorStorageBuffers();
    }
    if (device.SupportsUint8Indices()) {
        uint8_pass = std::make_unique<Uint8Pass>(device, scheduler, descriptor_pool, staging_pool,
                                                 compute_pass_descriptor_queue);
    }
    if (BufferArena::Enabled()) {
        upload_pool = std::make_unique<StagingBufferPool>(device, memory_allocator, scheduler,
                                                          arena.Clock(), 2);
    }
    quad_array_index_buffer = std::make_shared<QuadArrayIndexBuffer>(
        device_, memory_allocator_, scheduler_, staging_pool_, arena);
    quad_strip_index_buffer = std::make_shared<QuadStripIndexBuffer>(
        device_, memory_allocator_, scheduler_, staging_pool_, arena);
}

u32 BufferCacheRuntime::SetVertexRepackLayout(const Tegra::Engines::Maxwell3D::Regs& regs,
                                            bool dynamic_input) {
    const u32 old_mask = vertex_repack_mask;
    const bool input_supported = VertexRepackInputSupported(dynamic_input, static_vertex_repack);
    vertex_repack_mask = vertex_repack_pass && input_supported ? VertexRepackMask(regs) : 0;
    if (vertex_repack_mask != old_mask) {
        LOG_DEBUG(Render_Vulkan, "Vertex repack selection: mask={:#x}, dynamic_input={}",
                  vertex_repack_mask, dynamic_input);
    }
    return old_mask ^ vertex_repack_mask;
}

StagingBufferRef BufferCacheRuntime::UploadStagingBuffer(size_t size) {
    return UploadPool().Request(size, MemoryUsage::Upload);
}

StagingBufferRef BufferCacheRuntime::DownloadStagingBuffer(size_t size, bool deferred) {
    return staging_pool.Request(size, MemoryUsage::Download, deferred);
}

void BufferCacheRuntime::BindTextureBuffer(Buffer& buffer, u32 offset, u32 size,
                                           VideoCore::Surface::PixelFormat format) {
    const bool is_null = buffer.IsNull();
    if (is_null && device.HasNullDescriptor()) {
        guest_descriptor_queue->AddTexelBuffer(VK_NULL_HANDLE, 0, 0, 0, VK_FORMAT_R8_UNORM);
        return;
    }
    if (format == VideoCore::Surface::PixelFormat::Invalid) {
        format = VideoCore::Surface::PixelFormat::R8_UNORM;
    }
    const auto host_format =
        MaxwellToVK::SurfaceFormat(device, FormatType::Buffer, false, format).format;
    constexpr VkFormatFeatureFlags features =
        VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
    const u32 block_size = vkuFormatTexelBlockSize(host_format);
    const u32 texels_per_block = vkuFormatTexelsPerBlock(host_format);
    const auto alignment = device.GetTexelBufferOffsetAlignment(
        block_size % 3 == 0 ? block_size / 3 : block_size);
    const auto buffer_size = is_null ? NullBufferSize() : buffer.SizeBytes();
    const auto view_offset = is_null ? 0u : offset;
    const bool descriptor_buffer = guest_descriptor_queue->UsesDescriptorBuffer();
    const auto address = buffer.DeviceAddress();
    const auto range = ClampTexelBufferRange(view_offset, is_null ? block_size : size, buffer_size,
                                            descriptor_buffer ? 1 : alignment,
                                            block_size, texels_per_block,
                                            device.GetMaxTexelBufferElements());
    const bool invalid_address =
        descriptor_buffer &&
        (address == 0 || alignment == 0 || (address + view_offset) % alignment != 0);
    if (range == 0 || invalid_address ||
        !device.IsFormatSupported(host_format, features, FormatType::Buffer)) {
        static std::atomic<u32> logged{};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
            LOG_ERROR(Render_Vulkan,
                      "Invalid texel buffer binding: format={} offset={} size={} buffer_size={} "
                      "address={:#x} alignment={} max_elements={}",
                      host_format, offset, size, buffer_size, address, alignment,
                      device.GetMaxTexelBufferElements());
        }
        guest_descriptor_queue->RejectTexelBuffer();
        return;
    }
    if (descriptor_buffer) {
        guest_descriptor_queue->AddTexelBuffer(VK_NULL_HANDLE, address, view_offset, range,
                                               host_format);
        return;
    }
    if (const auto view = buffer.View(view_offset, range, host_format)) {
        guest_descriptor_queue->AddTexelBuffer(view);
    } else {
        guest_descriptor_queue->RejectTexelBuffer();
    }
}

void BufferCacheRuntime::FreeDeferredStagingBuffer(StagingBufferRef& ref) {
    staging_pool.FreeDeferred(ref);
}

u64 BufferCacheRuntime::GetDeviceLocalMemory() const {
    return device.GetDeviceLocalMemory();
}

u64 BufferCacheRuntime::GetDeviceMemoryUsage() const {
    return device.GetDeviceMemoryUsage();
}

bool BufferCacheRuntime::CanReportMemoryUsage() const {
    return device.CanReportMemoryUsage();
}

u64 BufferCacheRuntime::GetBufferAllocationTail() const {
    return device.IsMoltenVK() ? MOLTENVK_BUFFER_TAIL : 0;
}

u32 BufferCacheRuntime::GetUniformBufferAlignment() const {
    return static_cast<u32>(device.GetUniformBufferAlignment());
}

u32 BufferCacheRuntime::GetStorageBufferAlignment() const {
    return static_cast<u32>(device.GetStorageBufferAlignment());
}

u64 BufferCacheRuntime::GetMaxStorageBufferRange() const {
    return device.GetMaxStorageBufferRange();
}

void BufferCacheRuntime::TickFrame(Common::SlotVector<Buffer>& slot_buffers) noexcept {
    multi_range_buffers.TickFrame(scheduler);
    arena.TickFrame();
    if (upload_pool) {
        upload_pool->TickFrame();
    }
    if (uma_direct || uma_generations) {
        // The known GPU tick is otherwise refreshed only when something waits or runs out, and
        // a stale one keeps every range used.
        try {
            scheduler.GetMasterSemaphore().Refresh();
        } catch (const vk::Exception&) {
        }
    }
    for (auto it = slot_buffers.begin(); it != slot_buffers.end(); it++) {
        if (uma_generations) {
            it->AgeUsageTracking();
        } else if (arena.IsFree(it->LastUsageTick() + (uma_direct ? 1 : 0))) {
            // Direct uploads wait one tick more, as AgeUsageTracking does.
            it->ResetUsageTracking();
        }
    }
    if (uma_coalesce || uma_direct || uma_generations) {
        ReportUmaUploads();
    }
}

void BufferCacheRuntime::ReportUmaUploads() noexcept {
    ++uma_uploads.frames;
    const auto now = std::chrono::steady_clock::now();
    if (now - uma_report_time < std::chrono::seconds{1}) {
        return;
    }
    uma_report_time = now;
    const UmaUploads& uploads = uma_uploads;
    LOG_INFO(Render_Vulkan,
             "UMA direct uploads: {} direct, {} staged, bytes {} direct, {} staged ({} reordered "
             "{} bytes, {} in order {} bytes; {} not mapped, {} in use by pending GPU work) in {} "
             "frames",
             uploads.direct, uploads.reordered + uploads.in_order, uploads.direct_bytes,
             uploads.reordered_bytes + uploads.in_order_bytes, uploads.reordered,
             uploads.reordered_bytes, uploads.in_order, uploads.in_order_bytes, uploads.unmapped,
             uploads.busy, uploads.frames);
    uma_uploads = {};
}

bool BufferCacheRuntime::IsFree(u64 tick) {
    return scheduler.IsFree(tick);
}

void BufferCacheRuntime::Wait(u64 tick) {
    const BufferArena::ImmediateScope immediate{arena};
    scheduler.Wait(tick);
}

void BufferCacheRuntime::WaitSubmittedTick(u64 tick) {
    auto& master = scheduler.GetMasterSemaphore();
    if (master.IsFree(tick)) {
        return;
    }
    // The copy's submission may be deferred (Scheduler::DeferFlush): the split's front asks the
    // back for it rather than waiting for the back's next submission. If the back has stopped,
    // nothing else uses the scheduler and Wait submits it.
    if (!scheduler.IsSubmitted(tick) && VideoCommon::GpuSplit::Get().Active() &&
        !VideoCommon::GpuSplit::OnBackThread()) {
        VideoCommon::Nx2Profile::Scope scope{VideoCommon::Nx2Profile::Zone::ListSubmitWait};
        if (!scheduler.WaitSubmitted(tick)) {
            scheduler.Wait(tick);
            return;
        }
    }
    VideoCommon::Nx2Profile::Scope scope{VideoCommon::Nx2Profile::Zone::ListGpuWait};
    master.Wait(tick);
}

void BufferCacheRuntime::Finish(std::source_location location) {
    const BufferArena::ImmediateScope immediate{arena};
    scheduler.Finish({}, {}, location);
}

void BufferCacheRuntime::FinishForHostRead(std::source_location location) {
    const BufferArena::ImmediateScope immediate{arena};
    arena.OutsideRenderPass();
    arena.CopyBarrier(BufferArena::Barrier::HostRead);
    scheduler.Finish({}, {}, location);
}

u64 BufferCacheRuntime::SubmitForHostRead() {
    const BufferArena::ImmediateScope immediate{arena};
    arena.OutsideRenderPass();
    arena.CopyBarrier(BufferArena::Barrier::HostRead);
    return scheduler.Flush();
}

u64 BufferCacheRuntime::QueueForHostRead() {
    const BufferArena::ImmediateScope immediate{arena};
    arena.OutsideRenderPass();
    arena.CopyBarrier(BufferArena::Barrier::HostRead);
    const u64 tick = scheduler.CurrentTick();
    // EDEN_NX2_EARLY_SUBMIT: the early download goes to the GPU right behind the write that
    // fills its pages. Deferred, it waits for the next submission (up to the DeferFlush window,
    // longer while the back records a long list), and the guest's read waits for that too.
    if (VideoCore::AsyncReadback::EarlySubmit() != 0) {
        scheduler.Flush();
        VideoCore::AsyncReadback::Add(VideoCore::AsyncReadback::g_counters.early_flushes, 1);
    } else if (!scheduler.DeferFlush()) {
        scheduler.Flush();
    }
    return tick;
}

void BufferCacheRuntime::WaitForMappedRead(u64 write_tick) {
    const auto wait = VideoCommon::Nx2MappedReads::ChooseHostRead(
        scheduler.HostReadsMappedBuffers(), write_tick);
    if (wait == VideoCommon::Nx2MappedReads::HostReadWait::FinishAll) {
        FinishForHostRead();
        return;
    }
    // Every submission ends with a host-read barrier (Scheduler::SubmitExecution).
    WaitForHostRead(write_tick);
}

void BufferCacheRuntime::WaitForHostRead(u64 tick) {
    if (scheduler.IsFree(tick)) {
        return;
    }
    // A queued tick may not be submitted yet. Only the split's back submits: other threads ask it
    // to, as for fences (InnerFence::Wait). On the back, and once it has stopped, Wait submits it.
    if (!scheduler.IsSubmitted(tick) && VideoCommon::GpuSplit::Get().Active() &&
        !VideoCommon::GpuSplit::OnBackThread()) {
        const u64 submit_start = VideoCore::AsyncReadback::Now();
        const bool submitted = scheduler.WaitSubmitted(tick);
        VideoCore::AsyncReadback::CountSubmitWait(VideoCore::AsyncReadback::Now() - submit_start);
        if (submitted) {
            scheduler.GetMasterSemaphore().Wait(tick);
            return;
        }
    }
    scheduler.Wait(tick);
}

bool BufferCacheRuntime::IsFreeNow(u64 tick) {
    if (scheduler.IsFree(tick)) {
        return true;
    }
    if (!scheduler.IsSubmitted(tick)) {
        return false;
    }
    scheduler.GetMasterSemaphore().Refresh();
    return scheduler.IsFree(tick);
}

bool BufferCacheRuntime::CanReorderUpload(Buffer& buffer,
                                          std::span<const VideoCommon::BufferCopy> copies) {
    bool can_use_upload_cmdbuf = false;
    if (Settings::values.disable_buffer_reorder) {
        can_use_upload_cmdbuf = false;
    } else {
        can_use_upload_cmdbuf =
            std::ranges::all_of(copies, [&](const VideoCommon::BufferCopy& copy) {
                return !buffer.IsRegionUsed(copy.dst_offset, copy.size);
            });
    }
    if (uma_coalesce || uma_direct || uma_generations) {
        u64 bytes = 0;
        for (const VideoCommon::BufferCopy& copy : copies) {
            bytes += copy.size;
        }
        if (can_use_upload_cmdbuf) {
            ++uma_uploads.reordered;
            uma_uploads.reordered_bytes += bytes;
        } else {
            ++uma_uploads.in_order;
            uma_uploads.in_order_bytes += bytes;
        }
    }
    if (!buffer.DirectUploadSpan().empty()) {
        // The staged copy is a GPU write still to run, in this submission's upload command
        // buffer or in order: a later direct write of these bytes has to wait for it.
        for (const VideoCommon::BufferCopy& copy : copies) {
            buffer.MarkUsage(copy.dst_offset, copy.size);
        }
    }
    return can_use_upload_cmdbuf;
}

u8* BufferCacheRuntime::DirectUploadTarget(Buffer& buffer,
                                           std::span<const VideoCommon::BufferCopy> copies) {
    if (!uma_direct) {
        return nullptr;
    }
    const std::span<u8> mapped = buffer.DirectUploadSpan();
    if (mapped.empty()) {
        ++uma_uploads.unmapped;
        return nullptr;
    }
    // Safe when no command recorded and not yet run by the host GPU reads or writes the bytes:
    // every such use marks its range (Buffer::MarkUsage), and a range is forgotten only once the
    // GPU is past the last use of its generation (TickFrame). The write then lands before any
    // command that can see it, as the staged copy would.
    u64 bytes = 0;
    for (const VideoCommon::BufferCopy& copy : copies) {
        if (copy.dst_offset > mapped.size() || copy.size > mapped.size() - copy.dst_offset ||
            buffer.IsRegionUsed(copy.dst_offset, copy.size)) {
            ++uma_uploads.busy;
            return nullptr;
        }
        bytes += copy.size;
    }
    ++uma_uploads.direct;
    uma_uploads.direct_bytes += bytes;
    return mapped.data();
}

bool BufferCacheRuntime::ClearNewBufferDirect(Buffer& buffer) {
    const std::span<u8> mapped = buffer.DirectUploadSpan();
    if (mapped.empty() || mapped.size() > MAX_DIRECT_CLEAR_BYTES) {
        return false;
    }
    std::memset(mapped.data(), 0, mapped.size());
    return true;
}

void BufferCacheRuntime::CopyBuffer(VkBuffer dst_buffer, VkBuffer src_buffer,
                                    std::span<const VideoCommon::BufferCopy> copies, bool barrier,
                                    bool can_reorder_upload) {
    const VideoCommon::BufferCopySite site =
        std::exchange(copy_site, VideoCommon::BufferCopySite::Other);
    if (dst_buffer == VK_NULL_HANDLE || src_buffer == VK_NULL_HANDLE) {
        return;
    }
    // Measuring a popular game, this number never exceeds the specified size once data is warmed up
    boost::container::small_vector<VkBufferCopy, 8> vk_copies(copies.size());
    std::ranges::transform(copies, vk_copies.begin(), MakeBufferCopy);
    if (src_buffer == UploadPool().StreamBuf() && can_reorder_upload) {
        if (uma_coalesce && !vk_copies.empty()) {
            scheduler.RecordReorderedUpload(
                src_buffer, dst_buffer,
                std::span<const VkBufferCopy>{vk_copies.data(), vk_copies.size()});
            return;
        }
        scheduler.RecordWithUploadBuffer([src_buffer, dst_buffer, vk_copies](
                                             vk::CommandBuffer, vk::CommandBuffer upload_cmdbuf) {
            upload_cmdbuf.CopyBuffer(src_buffer, dst_buffer, vk_copies);
        });
        return;
    }

    scheduler.RequestOutsideRenderPassOperationContext();
    if (barrier && src_buffer == UploadPool().StreamBuf() && !vk_copies.empty()) {
        // Only in-order stream uploads share barriers. Dedicated staging buffers retain their
        // normal recording path and tick-based reuse.
        scheduler.RecordUpload(src_buffer, dst_buffer,
                               std::span<const VkBufferCopy>{vk_copies.data(), vk_copies.size()},
                               UploadBatchCategory(site));
        return;
    }
    if (uma_coalesce && !vk_copies.empty()) {
        // EDEN_NX2_UMA bit 1: downloads, DMA copies, gathers and dedicated staging uploads join
        // the batch too. It is recorded before the next command and within this tick, between
        // the same barriers as below, and holds no copy that depends on another of its copies.
        scheduler.RecordUpload(src_buffer, dst_buffer,
                               std::span<const VkBufferCopy>{vk_copies.data(), vk_copies.size()},
                               UploadBatchCategory(site));
        return;
    }
    if (uma_batch_copies && barrier && !vk_copies.empty()) {
        // EDEN_NX2_UMA bit 64: bit 1's batching of the copies that take their own barriers, as
        // above. Copies bracketed by PreCopyBarrier and PostCopyBarrier share theirs already.
        scheduler.RecordUpload(src_buffer, dst_buffer,
                               std::span<const VkBufferCopy>{vk_copies.data(), vk_copies.size()},
                               UploadBatchCategory(site));
        return;
    }
    scheduler.RecordEmu(CopyCategory(site), {static_cast<u32>(vk_copies.size()), 0, 0},
        [src_buffer, dst_buffer, vk_copies, barrier](vk::CommandBuffer cmdbuf) {
        if (barrier) {
            cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, READ_BARRIER);
        }
        cmdbuf.CopyBuffer(src_buffer, dst_buffer, vk_copies);
        if (barrier) {
            // Later transfers too: a reordered upload of the same bytes in a later submission
            // has no barrier of its own before it.
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, 0, WRITE_BARRIER);
        }
    });
}

void BufferCacheRuntime::PreCopyBarrier() {
    arena.OutsideRenderPass();
    arena.CopyBarrier(BufferArena::Barrier::PreCopy);
}

void BufferCacheRuntime::PostCopyBarrier() {
    arena.OutsideRenderPass();
    arena.CopyBarrier(BufferArena::Barrier::PostCopy);
}

void BufferCacheRuntime::ClearBuffer(VkBuffer dest_buffer, u32 offset, size_t size, u32 value) {
    if (dest_buffer == VK_NULL_HANDLE) {
        return;
    }
    arena.OutsideRenderPass();
    arena.Clear(dest_buffer, offset, size, value);
}

bool BufferCacheRuntime::BindMultiRangeStorageBuffer(u64 key, bool is_written) {
    if (multi_range_sources.empty() || multi_range_total == 0) {
        return false;
    }
    const MultiRangeRef ref = multi_range_buffers.Get(device, scheduler, memory_allocator, key,
                                                     multi_range_sources, multi_range_total);
    if (ref.handle == VK_NULL_HANDLE) {
        return false;
    }
    if (is_written && !ref.sparse) {
        return false;
    }
    if (ref.needs_gather) {
        PreCopyBarrier();
        VkDeviceSize dst_offset = 0;
        for (const MultiRangeSource& source : multi_range_sources) {
            const std::array<VideoCommon::BufferCopy, 1> copy{VideoCommon::BufferCopy{
                .src_offset = u64(source.offset),
                .dst_offset = u64(dst_offset),
                .size = size_t(source.size),
            }};
            SetCopySite(VideoCommon::BufferCopySite::Gather);
            CopyBuffer(ref.handle, source.handle, copy, false);
            dst_offset += source.size;
        }
        PostCopyBarrier();
        multi_range_buffers.MarkGathered(key);
    }
    guest_descriptor_queue->AddBuffer(ref.handle, ref.address, 0, ref.size);
    return true;
}

void BufferCacheRuntime::BindIndexBuffer(PrimitiveTopology topology, IndexFormat index_format,
                                         u32 base_vertex, u32 num_indices, VkBuffer buffer,
                                         u32 offset, [[maybe_unused]] u32 size) {
    VkIndexType vk_index_type = MaxwellToVK::IndexFormat(index_format);
    VkDeviceSize vk_offset = offset;
    VkBuffer vk_buffer = buffer;
    if (topology == PrimitiveTopology::Quads || topology == PrimitiveTopology::QuadStrip) {
        vk_index_type = VK_INDEX_TYPE_UINT32;
        const BufferArena::ImmediateScope immediate{arena};
        std::tie(vk_buffer, vk_offset) =
            quad_index_pass.Assemble(index_format, num_indices, base_vertex, buffer, offset,
                                     topology == PrimitiveTopology::QuadStrip);
    } else if (vk_index_type == VK_INDEX_TYPE_UINT8_EXT && !device.IsExtIndexTypeUint8Supported()) {
        vk_index_type = VK_INDEX_TYPE_UINT16;
        if (uint8_pass) {
            const BufferArena::ImmediateScope immediate{arena};
            std::tie(vk_buffer, vk_offset) = uint8_pass->Assemble(num_indices, buffer, offset);
        } else if (device.GetDriverID() == VK_DRIVER_ID_QUALCOMM_PROPRIETARY) {
            ReserveNullBuffer();
            vk_buffer = *null_buffer;
            vk_offset = 0;
        }
    }
    if (vk_buffer == VK_NULL_HANDLE) {
        // Vulkan doesn't support null index buffers. Replace it with our own null buffer.
        ReserveNullBuffer();
        vk_buffer = *null_buffer;
    }
    arena.BindIndex(vk_buffer, vk_offset, vk_index_type, true);
}

void BufferCacheRuntime::BindHostUint8IndexBuffer(std::span<const u8> indices) {
    const auto staging = UploadPool().Request(indices.size() * sizeof(u16), MemoryUsage::Upload);
    u16* const widened = reinterpret_cast<u16*>(staging.mapped_span.data());
    std::ranges::transform(indices, widened, [](u8 index) -> u16 { return index == 0xff ? 0xffff : index; });
    arena.BindIndex(staging.buffer, staging.offset, VK_INDEX_TYPE_UINT16, false);
}

void BufferCacheRuntime::BindQuadIndexBuffer(PrimitiveTopology topology, u32 first, u32 count) {
    if (count == 0) {
        ReserveNullBuffer();
        arena.BindIndex(*null_buffer, 0, VK_INDEX_TYPE_UINT32, false);
        return;
    }

    if (topology == PrimitiveTopology::Quads) {
        quad_array_index_buffer->UpdateBuffer(first + count);
        quad_array_index_buffer->BindBuffer(first);
    } else if (topology == PrimitiveTopology::QuadStrip) {
        quad_strip_index_buffer->UpdateBuffer(first + count);
        quad_strip_index_buffer->BindBuffer(first);
    }
}

void BufferCacheRuntime::BindVertexBuffer(u32 index, VkBuffer buffer, u32 offset, u32 size, u32 stride) {
    if (index >= device.GetMaxVertexInputBindings()) {
        return;
    }
    if (buffer != VK_NULL_HANDLE && size != 0 && IsVertexRepacked(index)) {
        const BufferArena::ImmediateScope immediate{arena};
        const auto repacked =
            vertex_repack_pass->Assemble(buffer, offset, size, u64{offset} + size);
        if (!repacked) {
            return;
        }
        buffer = repacked->first;
        offset = static_cast<u32>(repacked->second);
        size = static_cast<u32>((u64{size} + 68) / 69 * 72);
        stride = 72;
    }
    if (device.IsExtExtendedDynamicStateSupported()) {
        arena.BindVertex(index, buffer, offset, size, stride, true);
        return;
    }
    if (!device.HasNullDescriptor() && buffer == VK_NULL_HANDLE) {
        ReserveNullBuffer();
        buffer = *null_buffer;
        offset = 0;
    }
    arena.BindVertex(index, buffer, offset, 0, 0, false);
}

void BufferCacheRuntime::BindVertexBuffers(VideoCommon::HostBindings<Buffer>& bindings) {
    boost::container::static_vector<VkBuffer, VideoCommon::NUM_VERTEX_BUFFERS> buffer_handles(
        bindings.buffers.size(), boost::container::default_init);
    for (u32 i = 0; i < bindings.buffers.size(); ++i) {
        auto handle = bindings.buffers[i]->Handle();
        if (handle == VK_NULL_HANDLE) {
            bindings.offsets[i] = 0;
            bindings.sizes[i] = VK_WHOLE_SIZE;
            if (!device.HasNullDescriptor()) {
                ReserveNullBuffer();
                handle = *null_buffer;
            }
        }
        buffer_handles[i] = handle;
        const u32 binding = bindings.min_index + i;
        if (handle != VK_NULL_HANDLE && IsVertexRepacked(binding) && bindings.strides[i] == 69 &&
            bindings.sizes[i] > 0) {
            const BufferArena::ImmediateScope immediate{arena};
            const auto repacked = vertex_repack_pass->Assemble(
                handle, static_cast<u32>(bindings.offsets[i]), static_cast<u32>(bindings.sizes[i]),
                bindings.buffers[i]->SizeBytes());
            if (!repacked) {
                continue;
            }
            buffer_handles[i] = repacked->first;
            bindings.offsets[i] = repacked->second;
            bindings.sizes[i] = (bindings.sizes[i] + 68) / 69 * 72;
            bindings.strides[i] = 72;
            static bool reported = false;
            if (!reported) {
                LOG_INFO(Render_Vulkan, "GPU vertex repacking active: 69 -> 72 bytes");
                reported = true;
            }
        }
    }
    const u32 device_max = device.GetMaxVertexInputBindings();
    const u32 min_binding = (std::min)(bindings.min_index, device_max);
    const u32 max_binding = (std::min)(bindings.max_index, device_max);
    const u32 binding_count = max_binding - min_binding;
    if (binding_count == 0) {
        return;
    }
    boost::container::static_vector<BufferArena::VertexBinding, VideoCommon::NUM_VERTEX_BUFFERS>
        vertex_bindings;
    for (u32 i = 0; i < binding_count; ++i) {
        vertex_bindings.push_back({buffer_handles[i], bindings.offsets[i], bindings.sizes[i],
                                   bindings.strides[i]});
    }
    arena.BindVertices(bindings.min_index, vertex_bindings,
                       device.IsExtExtendedDynamicStateSupported());
}

void BufferCacheRuntime::BindTransformFeedbackBuffer(u32 index, VkBuffer buffer, u32 offset,
                                                     u32 size) {
    if (!device.IsExtTransformFeedbackSupported()) {
        // Already logged in the rasterizer
        return;
    }
    if (buffer == VK_NULL_HANDLE) {
        // Vulkan doesn't support null transform feedback buffers.
        // Replace it with our own null buffer.
        ReserveNullBuffer();
        buffer = *null_buffer;
        offset = 0;
        size = 0;
    }
    const std::array<BufferArena::VertexBinding, 1> binding{{{buffer, offset, size, 0}}};
    arena.BindTransformFeedback(index, binding);
}

void BufferCacheRuntime::BindTransformFeedbackBuffers(VideoCommon::HostBindings<Buffer>& bindings) {
    if (!device.IsExtTransformFeedbackSupported()) {
        // Already logged in the rasterizer
        return;
    }
    boost::container::static_vector<VkBuffer, VideoCommon::NUM_VERTEX_BUFFERS> buffer_handles(bindings.buffers.size());
    for (u32 i = 0; i < bindings.buffers.size(); ++i) {
        auto handle = bindings.buffers[i]->Handle();
        if (handle == VK_NULL_HANDLE) {
            ReserveNullBuffer();
            handle = *null_buffer;
            bindings.offsets[i] = 0;
            bindings.sizes[i] = 0;
        }
        buffer_handles[i] = handle;
    }
    boost::container::static_vector<BufferArena::VertexBinding, VideoCommon::NUM_VERTEX_BUFFERS>
        feedback_bindings;
    for (size_t i = 0; i < buffer_handles.size(); ++i) {
        feedback_bindings.push_back({buffer_handles[i], bindings.offsets[i], bindings.sizes[i], 0});
    }
    arena.BindTransformFeedback(0, feedback_bindings);
}

void BufferCacheRuntime::ReserveNullBuffer() {
    if (!null_buffer) {
        null_buffer = CreateNullBuffer();
    }
}

VkDeviceSize BufferCacheRuntime::NullBufferSize() const {
    return device.IsMoltenVK() ? MOLTENVK_BUFFER_TAIL : 16;
}

vk::Buffer BufferCacheRuntime::CreateNullBuffer() {
    VkBufferCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = NullBufferSize(),
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        create_info.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        create_info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    vk::Buffer ret = memory_allocator.CreateBuffer(create_info, MemoryUsage::DeviceLocal);
    if (device.NamesObjects()) {
        ret.SetObjectNameEXT("Null buffer");
    }

    arena.OutsideRenderPass();
    arena.Fill(*ret, 0, VK_WHOLE_SIZE,
               VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                   VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);

    return ret;
}

} // namespace Vulkan
