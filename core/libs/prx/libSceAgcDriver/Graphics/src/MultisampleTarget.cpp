#include <shared_mutex>
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTarget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleInitialContents.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleDcc.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTextureLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <limits>
#include <mutex>
#include <vector>

namespace AgcDriver::Graphics {
namespace {

bool sameTarget(const ColorTarget& a, const ColorTarget& b) {
    return a.address == b.address && a.surfaceAddress == b.surfaceAddress && a.bytes == b.bytes && a.format == b.format && a.extent.width == b.extent.width && a.extent.height == b.extent.height && a.samples == b.samples && a.elementBytes == b.elementBytes && a.tileMode == b.tileMode && a.pipeBankXor == b.pipeBankXor && a.dccAddress == b.dccAddress && a.dccControl == b.dccControl && a.dccAlphaOnMsb == b.dccAlphaOnMsb && a.dccPipeAligned == b.dccPipeAligned && a.cmaskAddress == b.cmaskAddress && a.cmaskBytes == b.cmaskBytes && a.cmaskFastClear == b.cmaskFastClear && a.clearWords == b.clearWords && a.fmaskAddress == b.fmaskAddress && a.fmaskSwizzle == b.fmaskSwizzle && a.arrayLayers == b.arrayLayers && a.arraySlice == b.arraySlice && a.depth == b.depth && a.depthSlice == b.depthSlice && a.mipCount == b.mipCount && a.mip == b.mip && a.mipTail == b.mipTail;
}

Recorder* recorderFor(const Context& context) {
    auto* recorder = Recorder::Active();
    return recorder != nullptr && recorder->Device() == context.device ? recorder : nullptr;
}

template<typename Record>
void recordCommands(const Context& context, Record&& record) {
    auto* recorder = recorderFor(context);
    std::unique_ptr<CommandBatch> batch;
    if (recorder == nullptr) batch = std::make_unique<CommandBatch>(context);
    record(recorder != nullptr ? recorder->Commands() : batch->Handle(), recorder);
    if (batch) batch->SubmitAndWait();
}

class MultisampleTarget {
public:
    MultisampleTarget(const Context& context, const ColorTarget& target) : context(context), target(target) {
        Require(context.device != VK_NULL_HANDLE && context.formatProperties != nullptr && context.imageFormatProperties != nullptr && context.detiler != nullptr, "multisample target requires a device, format queries and a detiler");
        Require(target.samples == 2u || target.samples == 4u || target.samples == 8u, "unsupported initial multisample fragment count");
        Require(target.tileMode == ColorTileMode::RenderTarget && target.arrayLayers == 1u && target.arraySlice == 0u && target.depth == 1u && target.depthSlice == 0u && target.mipCount == 1u && target.mip == 0u && !target.mipTail, "initial multisample array, volume, mip or non-R_X contents are not implemented");
        const MultisampleTextureLayout layout(TextureTileMode::kR64KBX, target.elementBytes, target.samples, target.extent.width, target.extent.height, 1u, target.pipeBankXor);
        Require(target.bytes == layout.Bytes(), "initial multisample target footprint mismatch");
        Require(context.storageImageMultisample && context.storageImageWriteWithoutFormat && context.storageImageReadWithoutFormat, "multisample target requires enabled formatless image transfers");
        this->context.bufferPool.reset();
        const auto samples = static_cast<VkSampleCountFlagBits>(target.samples);
        Require((context.limits.framebufferColorSampleCounts & samples) != 0, "the device cannot render color with the target's sample count");
        VkFormatProperties properties{};
        context.formatProperties(context.physical, target.format, &properties);
        Require((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0, "multisampled render-target format cannot be a color attachment");
        constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        constexpr VkImageCreateFlags flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        VkImageFormatProperties supported{};
        Check(context.imageFormatProperties(context.physical, target.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, flags, &supported), "vkGetPhysicalDeviceImageFormatProperties multisampled target");
        Require((supported.sampleCounts & samples) != 0 && target.extent.width <= supported.maxExtent.width && target.extent.height <= supported.maxExtent.height, "multisampled render target exceeds device image limits");
        auto color = readInitialRange(target.address, target.bytes, MultisampleTextureLayout::Alignment);
        auto dcc = target.dccAddress != 0u ? readInitialRange(target.dccAddress, MultisampleDccBytes_nid_no_patch(target.elementBytes, target.samples, target.extent.width, target.extent.height, 1u), 256u) : std::vector<std::byte>{};
        auto cmask = target.cmaskAddress != 0u ? readInitialRange(target.cmaskAddress, target.cmaskBytes, CmaskLayout::Alignment) : std::vector<std::byte>{};
        auto fmask = target.fmaskAddress != 0u ? readInitialRange(target.fmaskAddress & ~std::uint64_t{65535u}, MultisampleFmaskBytes(target), 65536u) : std::vector<std::byte>{};
        const auto initial = DecodeMultisampleInitialContents(target, color, dcc, cmask, fmask);
        try {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.flags = flags;
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = target.format;
            info.extent = {target.extent.width, target.extent.height, 1};
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = samples;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = usage;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &image), "vkCreateImage multisampled target");
            VkMemoryRequirements requirements{};
            context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &memory), "vkAllocateMemory multisampled target");
            Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, memory, 0), "vkBindImageMemory multisampled target");
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            VkImageViewUsageCreateInfo attachmentUsage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
            attachmentUsage.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            viewInfo.pNext = &attachmentUsage;
            viewInfo.image = image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = target.format;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView multisampled target");
            VkImageViewUsageCreateInfo transferUsage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
            transferUsage.usage = VK_IMAGE_USAGE_STORAGE_BIT;
            auto transferInfo = viewInfo;
            transferInfo.pNext = &transferUsage;
            transferInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            transferInfo.format = TextureDetiler::ImageElementFormat(target.elementBytes);
            Require(transferInfo.format != VK_FORMAT_UNDEFINED, "multisample target element format is not implemented");
            VkFormatProperties transferProperties{};
            context.formatProperties(context.physical, transferInfo.format, &transferProperties);
            Require((transferProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0u, "multisample target transfer view cannot be a storage image");
            Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &transferInfo, nullptr, &transferView), "vkCreateImageView multisampled target transfer");
            std::shared_ptr<Buffer> upload;
            if (!initial.clear) {
                upload = std::make_shared<Buffer>(context, (initial.samples.size() + 3u) & ~std::size_t{3u}, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
                std::memcpy(upload->Bytes().data(), initial.samples.data(), initial.samples.size());
                context.detiler->BeginBatch();
            }
            recordCommands(context, [&](VkCommandBuffer commands, Recorder* recorder) {
                VkImageMemoryBarrier toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                toGeneral.dstAccessMask = initial.clear ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_WRITE_BIT;
                toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toGeneral.image = image;
                toGeneral.subresourceRange = viewInfo.subresourceRange;
                context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, initial.clear ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toGeneral);
                if (initial.clear) context.Resolved(&DeviceFunctions::cmdClearColorImage, "vkCmdClearColorImage")(commands, image, VK_IMAGE_LAYOUT_GENERAL, &*initial.clear, 1, &toGeneral.subresourceRange);
                else {
                    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
                    transfer(commands, *upload, false);
                    if (recorder != nullptr) recorder->Keep(upload, upload->Bytes().size());
                }
                RecordMemoryBarrier(context, commands, initial.clear ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, initial.clear ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);
                if (recorder != nullptr) Recorder::CountBarriers(Recorder::CommandClass::Draw, 2);
            });
        } catch (...) {
            release();
            throw;
        }
    }
    ~MultisampleTarget() { release(); }
    MultisampleTarget(const MultisampleTarget&) = delete;
    MultisampleTarget& operator=(const MultisampleTarget&) = delete;

    void CheckCurrent(const ColorTarget& use) {
        Require(sameTarget(target, use), "multisample target layout or metadata changed; residency transition is not implemented");
        Require(refusal.empty(), refusal);
        for (const auto& range : initialRanges) {
            Require(range.generation != 0u && GuestMemory::Watched(range.address, range.bytes), "multisample target reuse requires watched guest memory; untracked writes are not implemented");
            if (auto* recorder = recorderFor(context); recorder != nullptr) Require(!recorder->PendingWriteOverlaps(range.address, range.bytes), "external GPU writes to a resident multisample target or its metadata are not implemented");
            GuestMemory::CollectWritesUncached(range.address, range.bytes);
            Require(GuestMemory::UnchangedSince(range.address, range.bytes, range.generation), "guest writes to a resident multisample target or its metadata are not implemented");
        }
    }

    void NoteFill(std::uint64_t address, std::size_t bytes) {
        Require(bytes <= std::numeric_limits<std::uint64_t>::max() - address, "multisample target fill range overflows");
        for (const auto& range : initialRanges) {
            if (address >= range.address + range.bytes || range.address >= address + bytes) continue;
            refusal = "a fill of a resident multisample target or its metadata needs an unimplemented residency transition";
        }
    }

    std::span<const GuestMemory::UnchangedQuery> Ranges() const { return initialRanges; }
    void MarkDirty() { dirty = true; }

    bool Overlaps(std::uint64_t address, std::size_t bytes) const {
        return std::ranges::any_of(initialRanges, [&](const auto& range) { return address < range.address + range.bytes && range.address < address + bytes; });
    }

    void PublishGuest() {
        CheckCurrent(target);
        if (!dirty) return;
        for (const auto& range : initialRanges) GuestMemory::CheckRange(reinterpret_cast<const void*>(range.address), range.bytes, 1u, true);
        for (std::size_t first = 0; first < initialRanges.size(); ++first) {
            for (std::size_t second = first + 1u; second < initialRanges.size(); ++second) {
                const auto& a = initialRanges[first];
                const auto& b = initialRanges[second];
                Require(a.address >= b.address + b.bytes || b.address >= a.address + a.bytes, "multisample guest publication with overlapping color and metadata ranges is not implemented");
            }
        }
        const auto samples = ReadSamples();
        CheckCurrent(target);
        const MultisampleTextureLayout layout(TextureTileMode::kR64KBX, target.elementBytes, target.samples, target.extent.width, target.extent.height, 1u, target.pipeBankXor);
        std::vector<std::vector<std::byte>> payloads;
        payloads.reserve(initialRanges.size());
        for (const auto& range : initialRanges) {
            auto& payload = payloads.emplace_back(range.bytes);
            if (range.address == target.address) {
                std::memcpy(payload.data(), reinterpret_cast<const void*>(target.address), payload.size());
                layout.Tile(samples, payload);
            } else if (range.address == target.dccAddress) std::fill(payload.begin(), payload.end(), std::byte{0xff});
            else if (range.address == target.cmaskAddress) std::fill(payload.begin(), payload.end(), target.samples == 2u ? std::byte{0xdd} : target.samples == 4u ? std::byte{0xee} : std::byte{0xff});
            else {
                Require(target.fmaskAddress != 0u && range.address == (target.fmaskAddress & ~std::uint64_t{65535u}), "unknown multisample publication range");
                const auto pattern = MultisampleExpandedFmask(target.samples);
                for (std::size_t byte = 0; byte < payload.size(); ++byte) payload[byte] = static_cast<std::byte>((pattern >> (byte % 4u * 8u)) & 0xffu);
            }
        }
        CheckCurrent(target);
        for (std::size_t index = 0; index < initialRanges.size(); ++index) GuestMemory::StoreOwnBytes(initialRanges[index].address, payloads[index]);
        for (auto& range : initialRanges) range.generation = GuestMemory::CollectWritesUncached(range.address, range.bytes);
        dirty = false;
    }

    std::vector<std::byte> ReadSamples() {
        Require(context.storageImageReadWithoutFormat, "device does not enable multisampled image reads without format");
        if (auto* recorder = recorderFor(context); recorder != nullptr) recorder->Sync();
        const auto count = static_cast<std::size_t>(target.extent.width) * target.extent.height * target.samples * target.elementBytes;
        Buffer readback(context, (count + 3u) & ~std::size_t{3u}, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        context.detiler->BeginBatch();
        CommandBatch batch(context);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        transfer(batch.Handle(), readback, true);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        batch.SubmitAndWait();
        readback.Invalidate();
        return {readback.Bytes().begin(), readback.Bytes().begin() + count};
    }

    VkRenderPass ResolvePass() {
        if (resolvePass != VK_NULL_HANDLE) return resolvePass;
        std::array<VkAttachmentDescription, 2> attachments{};
        attachments[0].format = target.format;
        attachments[0].samples = static_cast<VkSampleCountFlagBits>(target.samples);
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[1].format = target.format;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        for (auto& attachment : attachments) {
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
            attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_GENERAL};
        const VkAttachmentReference resolve{1, VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;
        subpass.pResolveAttachments = &resolve;
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        Check(context.Function<PFN_vkCreateRenderPass>("vkCreateRenderPass")(context.device, &info, nullptr, &resolvePass), "vkCreateRenderPass resolve");
        return resolvePass;
    }

    Context context;
    ColorTarget target;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView transferView = VK_NULL_HANDLE;
    VkRenderPass resolvePass = VK_NULL_HANDLE;

private:
    std::vector<GuestMemory::UnchangedQuery> initialRanges;
    std::string refusal;
    bool dirty = true;

    std::vector<std::byte> readInitialRange(std::uint64_t address, std::size_t bytes, std::size_t alignment) {
        Require(bytes != 0u, "initial multisample snapshot range is empty");
        RequireNoMultisampleTarget(address, bytes, "an initial target snapshot");
        GuestMemory::CheckRange(reinterpret_cast<const void*>(address), bytes, alignment);
        if (auto* recorder = recorderFor(context); recorder != nullptr) {
            recorder->FlushKeyStoresOverlapping(address, bytes);
            recorder->FlushStoresOverlapping(address, bytes);
        }
        GuestMemory::FlushGpuWrites(address, bytes);
        if (StorageTexture::FlushPending(address, bytes)) {
            if (auto* recorder = recorderFor(context); recorder != nullptr) recorder->SyncThrough(address, bytes);
        }
        const auto generation = GuestMemory::CollectWritesUncached(address, bytes);
        std::vector<std::byte> data(bytes);
        std::memcpy(data.data(), reinterpret_cast<const void*>(address), bytes);
        if (generation != 0u) {
            GuestMemory::CollectWritesUncached(address, bytes);
            Require(GuestMemory::UnchangedSince(address, bytes, generation), "guest memory changed during initial multisample import");
        }
        initialRanges.push_back({address, bytes, generation});
        return data;
    }

    void transfer(VkCommandBuffer commands, Buffer& buffer, bool readback) {
        const auto total = static_cast<std::uint64_t>(target.extent.width) * target.extent.height * target.samples;
        Require(total <= std::numeric_limits<std::uint32_t>::max(), "multisample target transfer element count overflows");
        const auto alignment = std::max<std::uint64_t>({4u, target.elementBytes, context.limits.minStorageBufferOffsetAlignment});
        const auto stride = static_cast<std::uint64_t>(context.limits.maxStorageBufferRange) / alignment * alignment;
        Require(stride != 0u, "multisample target transfer buffer limit is too small");
        const auto count = total * target.elementBytes;
        for (std::uint64_t offset = 0; offset < count; offset += stride) {
            const auto bytes = std::min(stride, count - offset);
            const auto first = static_cast<std::uint32_t>(offset / target.elementBytes);
            const auto elements = static_cast<std::uint32_t>(bytes / target.elementBytes);
            if (readback) context.detiler->DispatchMultisampleReadback(commands, target.elementBytes, buffer.Handle(), offset, (bytes + 3u) & ~3ull, transferView, target.extent.width, target.extent.height, 1u, target.samples, first, elements);
            else context.detiler->DispatchMultisampleImage(commands, target.elementBytes, buffer.Handle(), offset, (bytes + 3u) & ~3ull, transferView, target.extent.width, target.extent.height, 1u, target.samples, first, elements);
        }
    }

    void release() noexcept {
        if (resolvePass) context.Function<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(context.device, resolvePass, nullptr);
        if (view) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, view, nullptr);
        if (transferView) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, transferView, nullptr);
        if (image) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
        if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
    }
};

struct GuardedRange {
    VkDevice device;
    std::uint64_t address;
    std::uint64_t bytes;
    std::uint32_t samples;
};

std::shared_mutex& guardedMutex() {
    static std::shared_mutex mutex;
    return mutex;
}

std::vector<GuardedRange>& guarded() {
    static std::vector<GuardedRange> ranges;
    return ranges;
}

std::atomic<bool>& anyGuarded() {
    static std::atomic<bool> any{false};
    return any;
}

void guardReads(std::uint64_t address, std::size_t bytes) {
    SynchronizeMultisampleTargetRead(address, bytes);
}

void guard(const Context& context, const MultisampleTarget& target) {
    std::unique_lock lock(guardedMutex());
    for (const auto& range : target.Ranges()) guarded().push_back({context.device, range.address, range.bytes, target.target.samples});
    anyGuarded().store(true, std::memory_order_release);
    static std::once_flag installed;
    std::call_once(installed, [] { GuestMemory::SetReadGuard(&guardReads); });
}

std::mutex& targetsMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::unique_ptr<MultisampleTarget>>& targets() {
    static std::vector<std::unique_ptr<MultisampleTarget>> list;
    return list;
}

MultisampleTarget& acquire(const Context& context, const ColorTarget& use) {
    for (const auto& target : targets()) {
        if (target->context.device != context.device) continue;
        if (target->target.address >= use.address + use.bytes || use.address >= target->target.address + target->target.bytes) continue;
        target->CheckCurrent(use);
        return *target;
    }
    targets().push_back(std::make_unique<MultisampleTarget>(context, use));
    guard(context, *targets().back());
    auto& created = *targets().back();
    return created;
}

}

void MultisampledCmask::NoteFill(std::uint64_t fillAddress, std::size_t fillBytes, std::uint32_t pattern) {
    if (address == 0 || fillAddress >= address + bytes || address >= fillAddress + fillBytes) return;
    if (fillAddress > address || fillAddress + fillBytes < address + bytes) {
        char text[192];
        std::snprintf(text, sizeof(text), "a fill of 0x%llx+0x%zx partly covers the CMASK 0x%llx+0x%llx of a multisampled color target, leaving its tiles mixed", static_cast<unsigned long long>(fillAddress), fillBytes, static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
        refusal = text;
        return;
    }
    refusal.clear();
    if (pattern == 0) {
        keys = DccKeys::Clear0000;
    } else if (pattern == 0xffffffffu) {
        keys = DccKeys::Uncompressed;
    } else {
        char text[192];
        std::snprintf(text, sizeof(text), "a fill of the CMASK 0x%llx+0x%llx of a multisampled color target with 0x%08x leaves its tiles mixed (neither fast-cleared 0 nor expanded 0xffffffff)", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), pattern);
        refusal = text;
    }
}

void MultisampledCmask::SeedKeys(DccKeys seeded) {
    if (seeded == DccKeys::Clear0000 || seeded == DccKeys::Uncompressed) {
        keys = seeded;
        return;
    }
    char text[192];
    std::snprintf(text, sizeof(text), "the CMASK 0x%llx of a multisampled color target reads as %s, which is not modeled", static_cast<unsigned long long>(address), DccKeysName(seeded));
    refusal = text;
}

void MultisampledCmask::CheckAddress(std::uint64_t cmaskAddress) const {
    if (cmaskAddress == address) return;
    char text[192];
    std::snprintf(text, sizeof(text), "a multisampled color target used CMASK 0x%llx after its first use with CMASK 0x%llx, which is not modeled", static_cast<unsigned long long>(cmaskAddress), static_cast<unsigned long long>(address));
    Require(false, std::string(text));
}

bool MultisampledCmask::TakeClear() {
    Require(refusal.empty(), refusal);
    if (keys != DccKeys::Clear0000) return false;
    keys = DccKeys::Uncompressed;
    return true;
}

VkImageView MultisampleTargetView(const Context& context, const ColorTarget& target) {
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::lock_guard lock(targetsMutex());
    return acquire(context, target).view;
}

std::vector<std::byte> ReadMultisampleTargetSamples(const Context& context, const ColorTarget& target) {
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::lock_guard lock(targetsMutex());
    for (const auto& resident : targets()) {
        if (resident->context.device == context.device && sameTarget(resident->target, target)) return resident->ReadSamples();
    }
    Require(false, "multisample target sample read requires an existing resident");
    return {};
}

void MarkMultisampleTargetDirty(const Context& context, const ColorTarget& target) {
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::lock_guard lock(targetsMutex());
    for (const auto& resident : targets()) {
        if (resident->context.device == context.device && sameTarget(resident->target, target)) {
            resident->MarkDirty();
            return;
        }
    }
    Require(false, "marking multisample writes requires an existing resident");
}

void SynchronizeMultisampleTargetRead(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0u || !anyGuarded().load(std::memory_order_acquire)) return;
    Require(bytes <= std::numeric_limits<std::uint64_t>::max() - address, "multisample read range overflows");
    {
        std::shared_lock lock(guardedMutex());
        if (std::ranges::none_of(guarded(), [&](const auto& range) { return address < range.address + range.bytes && range.address < address + bytes; })) return;
    }
    static thread_local bool publishing = false;
    Require(!publishing, "recursive multisample guest publication is not implemented");
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::lock_guard lock(targetsMutex());
    struct Publishing {
        bool& flag;
        explicit Publishing(bool& flag) : flag(flag) { flag = true; }
        ~Publishing() { flag = false; }
    } publication(publishing);
    for (const auto& resident : targets()) if (resident->Overlaps(address, bytes)) resident->PublishGuest();
}

void ResolveMultisampleTarget(const Context& context, const ColorTarget& source, StorageTexture& destination, VkImageView destinationView) {
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::lock_guard lock(targetsMutex());
    auto& target = acquire(context, source);
    const std::array<VkImageView, 2> views{target.view, destinationView};
    auto framebuffer = std::make_shared<Framebuffer>(context, target.ResolvePass(), views, source.extent);
    recordCommands(context, [&](VkCommandBuffer commands, Recorder* recorder) {
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = target.ResolvePass();
        begin.framebuffer = framebuffer->Handle();
        begin.renderArea = {{0, 0}, source.extent};
        context.Resolved(&DeviceFunctions::cmdBeginRenderPass, "vkCmdBeginRenderPass")(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
        context.Resolved(&DeviceFunctions::cmdEndRenderPass, "vkCmdEndRenderPass")(commands);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
        if (recorder != nullptr) {
            Recorder::CountBarriers(Recorder::CommandClass::Draw, 2);
            recorder->Keep(framebuffer);
        }
    });
    destination.MarkDirty();
}

void NoteColorMetadataFill(std::uint64_t address, std::size_t bytes, std::uint32_t pattern) {
    std::lock_guard lock(targetsMutex());
    for (const auto& target : targets()) target->NoteFill(address, bytes);
}

void ClearMultisampleTargets(VkDevice device) {
    std::lock_guard lock(targetsMutex());
    std::erase_if(targets(), [&](const auto& target) { return target->context.device == device; });
    std::unique_lock guardLock(guardedMutex());
    std::erase_if(guarded(), [&](const auto& range) { return range.device == device; });
    anyGuarded().store(!guarded().empty(), std::memory_order_release);
}

void RequireNoMultisampleTarget(std::uint64_t address, std::size_t bytes, const char* access) {
    if (bytes == 0 || !anyGuarded().load(std::memory_order_acquire)) return;
    std::shared_lock lock(guardedMutex());
    for (const auto& range : guarded()) {
        if (address >= range.address + range.bytes || range.address >= address + bytes) continue;
        char text[256];
        std::snprintf(text, sizeof(text), "AGC graphics: %s of 0x%llx+0x%zx overlaps multisampled color target 0x%llx+0x%llx (%u samples), whose samples are only on the host: guest access to it is not implemented", access, static_cast<unsigned long long>(address), bytes, static_cast<unsigned long long>(range.address), static_cast<unsigned long long>(range.bytes), range.samples);
        throw std::runtime_error(text);
    }
}

}
