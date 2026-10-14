#include <shared_mutex>
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTarget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleInitialContents.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleDcc.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTextureLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <memory>
#include <map>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {
namespace {

bool sameTarget(const ColorTarget& a, const ColorTarget& b) {
    return a.address == b.address && a.surfaceAddress == b.surfaceAddress && a.bytes == b.bytes && a.format == b.format && a.extent.width == b.extent.width && a.extent.height == b.extent.height && a.samples == b.samples && a.elementBytes == b.elementBytes && a.tileMode == b.tileMode && a.pipeBankXor == b.pipeBankXor && a.dccAddress == b.dccAddress && a.dccControl == b.dccControl && a.dccAlphaOnMsb == b.dccAlphaOnMsb && a.dccPipeAligned == b.dccPipeAligned && a.cmaskAddress == b.cmaskAddress && a.cmaskBytes == b.cmaskBytes && a.cmaskFastClear == b.cmaskFastClear && a.clearWords == b.clearWords && a.fmaskAddress == b.fmaskAddress && a.fmaskSwizzle == b.fmaskSwizzle && a.arrayLayers == b.arrayLayers && a.arraySlice == b.arraySlice && a.depth == b.depth && a.depthSlice == b.depthSlice && a.mipCount == b.mipCount && a.mip == b.mip && a.mipTail == b.mipTail;
}

bool sameAllocation(const ColorTarget& a, const ColorTarget& b) {
    auto view = b;
    view.pipeBankXor = a.pipeBankXor;
    view.fmaskAddress = a.fmaskAddress;
    view.clearWords = a.clearWords;
    view.cmaskFastClear = a.cmaskFastClear;
    return sameTarget(a, view) && (a.fmaskAddress == 0u ? b.fmaskAddress == 0u : b.fmaskAddress != 0u && (a.fmaskAddress & ~std::uint64_t{65535u}) == (b.fmaskAddress & ~std::uint64_t{65535u}));
}

VkClearColorValue fastClearColor(const ColorTarget& target) {
    Require(target.elementBytes != 0 && target.elementBytes <= sizeof(target.clearWords) && 16u % target.elementBytes == 0, "the fast clear of a multisampled target over 64-bit texels is not modeled");
    std::array<std::byte, 16> texels{};
    for (std::size_t offset = 0; offset < texels.size(); offset += target.elementBytes) std::memcpy(texels.data() + offset, target.clearWords.data(), target.elementBytes);
    std::array<std::uint32_t, 4> pattern{};
    std::memcpy(pattern.data(), texels.data(), texels.size());
    VkClearColorValue clear{};
    Require(ClearColorForTexel(target.format, target.elementBytes, pattern, clear), "the fast-clear color of a multisampled target has no exact clear value in its format");
    return clear;
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
    MultisampleTarget(const Context& context, const ColorTarget& target) : context(context), target(target), cmask(target.cmaskAddress, target.cmaskBytes), colorFill(target) {
        Require(context.device != VK_NULL_HANDLE && context.formatProperties != nullptr && context.imageFormatProperties != nullptr && context.detiler != nullptr, "multisample target requires a device, format queries and a detiler");
        Require(target.samples == 2u || target.samples == 4u || target.samples == 8u, "unsupported initial multisample fragment count");
        Require(target.tileMode == ColorTileMode::RenderTarget && target.arrayLayers == 1u && target.arraySlice == 0u && target.depth == 1u && target.depthSlice == 0u && target.mipCount == 1u && target.mip == 0u && !target.mipTail, "initial multisample array, volume, mip or non-R_X contents are not implemented");
        const MultisampleTextureLayout layout(TextureTileMode::kR64KBX, target.elementBytes, target.samples, target.extent.width, target.extent.height, 1u, target.pipeBankXor);
        Require((target.fmaskAddress & 65535u & ~0xcf00u) == 0u, "initial multisample FMASK XOR tag is not implemented");
        backingExtent = {layout.Pitch(), layout.PaddedHeight()};
        Require(target.bytes == layout.Bytes(), "initial multisample target footprint mismatch");
        Require(context.storageImageMultisample && context.storageImageWriteWithoutFormat && context.storageImageReadWithoutFormat, "multisample target requires enabled formatless image transfers");
        this->context.bufferPool.reset();
        const auto samples = static_cast<VkSampleCountFlagBits>(target.samples);
        Require((context.limits.framebufferColorSampleCounts & samples) != 0, "the device cannot render color with the target's sample count");
        VkFormatProperties properties{};
        context.formatProperties(context.physical, target.format, &properties);
        Require((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0, "multisampled render-target format cannot be a color attachment");
        constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        constexpr VkImageCreateFlags flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        VkImageFormatProperties supported{};
        Check(context.imageFormatProperties(context.physical, target.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, flags, &supported), "vkGetPhysicalDeviceImageFormatProperties multisampled target");
        Require((supported.sampleCounts & samples) != 0 && backingExtent.width <= supported.maxExtent.width && backingExtent.height <= supported.maxExtent.height, "multisampled render target exceeds device image limits");
        auto color = readInitialRange(target.address, target.bytes, MultisampleTextureLayout::Alignment);
        auto dcc = target.dccAddress != 0u ? readInitialRange(target.dccAddress, MultisampleDccBytes_nid_no_patch(target.elementBytes, target.samples, target.extent.width, target.extent.height, 1u), 256u) : std::vector<std::byte>{};
        auto cmask = target.cmaskAddress != 0u ? readInitialRange(target.cmaskAddress, target.cmaskBytes, CmaskLayout::Alignment) : std::vector<std::byte>{};
        auto fmask = target.fmaskAddress != 0u ? readInitialRange(target.fmaskAddress & ~std::uint64_t{65535u}, MultisampleFmaskBytes(target), 65536u) : std::vector<std::byte>{};
        const auto initial = DecodeMultisampleInitialContents(target, color, dcc, cmask, fmask, true);
        try {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.flags = flags;
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = target.format;
            info.extent = {backingExtent.width, backingExtent.height, 1};
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
                    transfer(commands, upload->Handle(), false, backingExtent);
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
        Require(sameAllocation(target, use), "multisample target layout or metadata changed; residency transition is not implemented");
        Require((use.pipeBankXor & ~0xcf00u) == 0u, "multisample target XOR tag is not implemented");
        Require((use.fmaskAddress & 65535u & ~0xcf00u) == 0u, "multisample target FMASK XOR tag is not implemented");
        Require(refusal.empty(), refusal);
        bool changed = false;
        for (const auto& range : initialRanges) {
            Require(range.generation != 0u && GuestMemory::Watched(range.address, range.bytes), "multisample target reuse requires watched guest memory; untracked writes are not implemented");
            if (auto* recorder = recorderFor(context); recorder != nullptr) Require(!recorder->PendingWriteOverlaps(range.address, range.bytes), "external GPU writes to a resident multisample target or its metadata are not implemented");
            GuestMemory::CollectWritesUncached(range.address, range.bytes);
            changed |= !GuestMemory::UnchangedSince(range.address, range.bytes, range.generation);
        }
        if (clearGeneration != 0u) Require(ImportPublishedClear(use), "guest writes to a resident multisample target or its metadata are not implemented");
        else if (!fillRanges.empty()) Require(ImportTrackedFill(use), "a tracked multisample fill has no complete exact driver store");
        else if (changed) Require(ImportPublishedClear(use), "guest writes to a resident multisample target or its metadata are not implemented");
        if (target.pipeBankXor != use.pipeBankXor) reinterpret(use.pipeBankXor);
        target = use;
    }

    bool ImportPublishedClear(const ColorTarget& use) {
        if (target.dccAddress == 0u) return false;
        if (dirty && clearGeneration == 0u) return false;
        if (clearGeneration != 0u && (clearWriteSerial != writeSerial || !GuestMemory::UnchangedSinceCollected(target.dccAddress, clearBytes, clearGeneration) || !GuestMemory::StoredOver(target.dccAddress, clearBytes, clearGeneration) || (target.cmaskAddress != 0u && !GuestMemory::UnchangedSinceCollected(target.cmaskAddress, target.cmaskBytes, clearCmaskGeneration)))) return false;
        for (const auto& range : initialRanges) {
            if (range.address == target.address || (target.fmaskAddress != 0u && range.address == (target.fmaskAddress & ~std::uint64_t{65535u}))) {
                if (!GuestMemory::UnchangedSince(range.address, range.bytes, range.generation)) return false;
            }
        }
        auto current = initialRanges;
        std::vector<std::byte> dcc, cmask;
        for (auto& range : current) {
            GuestMemory::CheckRange(reinterpret_cast<const void*>(range.address), range.bytes, 1u);
            range.generation = GuestMemory::CollectWritesUncached(range.address, range.bytes);
            if (range.address == target.dccAddress || range.address == target.cmaskAddress) {
                auto& data = range.address == target.dccAddress ? dcc : cmask;
                data.resize(range.bytes);
                std::memcpy(data.data(), reinterpret_cast<const void*>(range.address), range.bytes);
            }
        }
        if (dcc.empty()) return false;
        const auto code = std::to_integer<std::uint32_t>(dcc.front());
        if (code != 0u && code != 0x40u && code != 0x80u && code != 0xc0u) return false;
        if (clearGeneration != 0u && code != clearCode) return false;
        if (!std::ranges::all_of(dcc, [&](std::byte byte) { return byte == dcc.front(); })) return false;
        if (!cmask.empty()) {
            const auto expanded = target.samples == 2u ? std::byte{0xdd} : target.samples == 4u ? std::byte{0xee} : std::byte{0xff};
            if ((cmask.front() != std::byte{0xcc} && cmask.front() != expanded) || !std::ranges::all_of(cmask, [&](std::byte byte) { return byte == cmask.front(); })) return false;
        }
        const auto contents = DecodeMultisampleInitialContents(use, {}, dcc, cmask, {});
        Require(contents.clear.has_value(), "multisample metadata reset has no exact native clear");
        for (const auto& range : current) GuestMemory::CollectWritesUncached(range.address, range.bytes);
        Require(GuestMemory::UnchangedSinceAll(current), "guest writes raced multisample metadata reset");
        if (clearGeneration != 0u && (!GuestMemory::UnchangedSinceCollected(target.dccAddress, clearBytes, clearGeneration) || (target.cmaskAddress != 0u && !GuestMemory::UnchangedSinceCollected(target.cmaskAddress, target.cmaskBytes, clearCmaskGeneration)))) return false;
        for (const auto& range : initialRanges) {
            if (range.address == target.address || (target.fmaskAddress != 0u && range.address == (target.fmaskAddress & ~std::uint64_t{65535u}))) Require(GuestMemory::UnchangedSince(range.address, range.bytes, range.generation), "sample writes raced multisample metadata reset");
        }
        recordCommands(context, [&](VkCommandBuffer commands, Recorder* recorder) {
            constexpr VkAccessFlags access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, access, VK_ACCESS_TRANSFER_WRITE_BIT);
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
            context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, image, VK_IMAGE_LAYOUT_GENERAL, &*contents.clear, 1u, &range);
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, access);
            if (recorder != nullptr) Recorder::CountBarriers(Recorder::CommandClass::Draw, 2u);
        });
        initialRanges = std::move(current);
        dirty = true;
        ++writeSerial;
        clearGeneration = 0u;
        fillRanges.clear();
        fillPatterns.clear();
        fillHostOnly.clear();
        colorFill.Discard();
        this->cmask.SeedKeys(DccKeys::Uncompressed);
        return true;
    }

    void NoteClear(std::uint64_t bytes, std::uint32_t value, std::uint32_t mask) {
        const auto metadata = std::ranges::find_if(initialRanges, [&](const auto& range) { return range.address == target.dccAddress; });
        const auto code = value & 255u;
        if (metadata == initialRanges.end() || bytes < metadata->bytes || mask != 0u || value != code * 0x01010101u || (code != 0u && code != 0x40u && code != 0x80u && code != 0xc0u) || !GuestMemory::Accessible(reinterpret_cast<const void*>(metadata->address), metadata->bytes) || !GuestMemory::Watched(metadata->address, metadata->bytes)) {
            refusal = "native multisample color incomplete, untracked or unsupported numeric clear is not implemented";
            return;
        }
        clearBytes = metadata->bytes;
        clearCode = code;
        clearWriteSerial = writeSerial;
        clearGeneration = GuestMemory::CollectWritesUncached(metadata->address, metadata->bytes);
        clearCmaskGeneration = target.cmaskAddress != 0u ? GuestMemory::CollectWritesUncached(target.cmaskAddress, target.cmaskBytes) : 0u;
    }

    void NoteFill(std::uint64_t address, std::size_t bytes, std::uint32_t pattern, bool hostOnly) {
        Require(bytes <= std::numeric_limits<std::uint64_t>::max() - address, "multisample target fill range overflows");
        for (const auto& range : initialRanges) {
            if (address >= range.address + range.bytes || range.address >= address + bytes) continue;
            if ((range.address != target.address && range.address != target.cmaskAddress) || address > range.address || address + bytes < range.address + range.bytes || (address & 3u) != 0u || (bytes & 3u) != 0u || !GuestMemory::Watched(range.address, range.bytes)) {
                refusal = "a partial, untracked or unsupported fill of a resident multisample target or its metadata needs an unimplemented residency transition";
                continue;
            }
            if (hostOnly) {
                VkClearColorValue clear{};
                if (range.address != target.address || address != range.address || bytes != range.bytes || !MultisampledFillClearColor(target, pattern, clear)) {
                    refusal = "an absorbed multisample fill requires the complete color range and an exact native clear";
                    continue;
                }
            }
            GuestMemory::FlushGpuWrites(range.address, range.bytes);
            if (auto* recorder = recorderFor(context); recorder != nullptr) recorder->SyncThrough(range.address, range.bytes);
            const auto generation = GuestMemory::CollectWritesUncached(range.address, range.bytes);
            if (generation == 0u) {
                refusal = "a fill of a resident multisample target requires watched guest memory";
                continue;
            }
            const auto found = std::ranges::find_if(fillRanges, [&](const auto& fill) { return fill.address == range.address; });
            if (found == fillRanges.end()) fillRanges.push_back({range.address, range.bytes, generation});
            else found->generation = generation;
            fillPatterns[range.address] = pattern;
            fillHostOnly[range.address] = hostOnly;
        }
    }

    std::span<const GuestMemory::UnchangedQuery> Ranges() const { return initialRanges; }
    void MarkDirty() { dirty = true; ++writeSerial; }

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
        const auto samples = ReadSamples(true);
        CheckCurrent(target);
        const MultisampleTextureLayout layout(TextureTileMode::kR64KBX, target.elementBytes, target.samples, backingExtent.width, backingExtent.height, 1u, target.pipeBankXor);
        std::vector<std::vector<std::byte>> payloads;
        payloads.reserve(initialRanges.size());
        for (const auto& range : initialRanges) {
            auto& payload = payloads.emplace_back(range.bytes);
            if (range.address == target.address) {
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

    std::vector<std::byte> ReadSamples(bool includePadding = false) {
        Require(context.storageImageReadWithoutFormat, "device does not enable multisampled image reads without format");
        if (auto* recorder = recorderFor(context); recorder != nullptr) recorder->Sync();
        const auto extent = includePadding ? backingExtent : target.extent;
        const auto count = static_cast<std::size_t>(extent.width) * extent.height * target.samples * target.elementBytes;
        const auto bytes = (count + 3u) & ~std::size_t{3u};
        DeviceBuffer samples(context, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Buffer readback(context, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        context.detiler->BeginBatch();
        CommandBatch batch(context);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        transfer(batch.Handle(), samples.Handle(), true, extent);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        CopyBuffer(context, batch.Handle(), samples.Handle(), 0, readback.Handle(), 0, bytes);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        batch.SubmitAndWait();
        readback.Invalidate();
        return {readback.Bytes().begin(), readback.Bytes().begin() + count};
    }

    std::shared_ptr<Texture> Snapshot(const GuestTextureResource& resource, VkComponentMapping components, const std::shared_ptr<MultisampleTarget>& owner) {
        Require(resource.dimension == TextureDimension::k2DMsaa || resource.dimension == TextureDimension::k2DMsaaArray, "native multisample target snapshot requires a multisampled descriptor");
        Require(resource.pipeBankXor == target.pipeBankXor && resource.tileMode == TextureTileMode::kR64KBX && resource.baseArray == 0u && resource.depthOrLastArray == 0u && resource.mipCount == 1u && resource.baseLevel == 0u && resource.lastLevel == 0u && resource.minLod == 0u && !IsConvertedTextureFormat(resource.format) && !IsBlockCompressed(resource.format) && BytesPerElement(resource.format) == target.elementBytes, "native multisample target snapshot descriptor is incompatible with its resident allocation");
        Require(resource.dccAddress == 0u || (resource.dccAddress == target.dccAddress && resource.dccAlphaOnMsb == target.dccAlphaOnMsb && resource.dccPipeAligned == target.dccPipeAligned), "native multisample target snapshot DCC descriptor is incompatible with its resident allocation");
        CheckCurrent(target);
        return std::make_shared<Texture>(context, image, owner, resource, components, Texture::NativeSnapshotTag{});
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
    std::shared_ptr<std::atomic<bool>> active = std::make_shared<std::atomic<bool>>(true);
    ColorTarget target;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView transferView = VK_NULL_HANDLE;
    VkRenderPass resolvePass = VK_NULL_HANDLE;
    MultisampledCmask cmask;
    MultisampledColorFill colorFill;

private:
    VkExtent2D backingExtent{};
    std::vector<GuestMemory::UnchangedQuery> initialRanges;
    std::vector<GuestMemory::UnchangedQuery> fillRanges;
    std::map<std::uint64_t, std::uint32_t> fillPatterns;
    std::map<std::uint64_t, bool> fillHostOnly;
    std::string refusal;
    bool dirty = true;
    std::uint64_t writeSerial = 0u;
    std::uint64_t clearWriteSerial = 0u;
    std::uint64_t clearGeneration = 0u;
    std::uint64_t clearCmaskGeneration = 0u;
    std::size_t clearBytes = 0u;
    std::uint32_t clearCode = 0u;

    bool ApplyFastClear(const ColorTarget& use) {
        VkClearColorValue clear{};
        if (!TakeMultisampledClear(cmask, colorFill, use, clear)) return false;
        recordCommands(context, [&](VkCommandBuffer commands, Recorder* recorder) {
            constexpr VkAccessFlags access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, access, VK_ACCESS_TRANSFER_WRITE_BIT);
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
            context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1u, &range);
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, access);
            if (recorder != nullptr) Recorder::CountBarriers(Recorder::CommandClass::Draw, 2u);
        });
        MarkDirty();
        return true;
    }

    bool ImportTrackedFill(const ColorTarget& use) {
        if (clearGeneration != 0u || fillRanges.empty()) return false;
        auto current = initialRanges;
        bool colorChanged = false;
        for (auto& range : current) {
            const auto fill = std::ranges::find_if(fillRanges, [&](const auto& query) { return query.address == range.address; });
            if (fill == fillRanges.end()) {
                if (!GuestMemory::UnchangedSince(range.address, range.bytes, range.generation)) return false;
            } else {
                if (!GuestMemory::UnchangedSinceCollected(range.address, range.bytes, fill->generation)) return false;
                if (fillHostOnly.at(range.address)) {
                    if (range.address != target.address || !GuestMemory::UnchangedSince(range.address, range.bytes, fill->generation)) return false;
                    range.generation = fill->generation;
                    colorChanged = true;
                    continue;
                }
                if (!GuestMemory::StoredAllSince_nid_no_patch(range.address, range.bytes, range.generation)) return false;
                range.generation = GuestMemory::CollectWritesUncached(range.address, range.bytes);
                GuestMemory::CheckRange(reinterpret_cast<const void*>(range.address), range.bytes, sizeof(std::uint32_t));
                const auto words = std::span(reinterpret_cast<const std::uint32_t*>(range.address), range.bytes / sizeof(std::uint32_t));
                if (!std::ranges::all_of(words, [&](auto word) { return word == fillPatterns.at(range.address); })) return false;
                colorChanged |= range.address == target.address;
            }
        }
        for (const auto& range : current) GuestMemory::CollectWritesUncached(range.address, range.bytes);
        Require(GuestMemory::UnchangedSinceAll(current), "guest writes raced a tracked multisample clear");
        const auto cleared = ApplyFastClear(use);
        Require(cleared || !colorChanged, "a tracked multisample color fill has no exact native clear");
        Require(GuestMemory::UnchangedSinceAll(current), "guest writes raced a tracked multisample clear");
        initialRanges = std::move(current);
        fillRanges.clear();
        fillPatterns.clear();
        fillHostOnly.clear();
        return true;
    }

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

    void reinterpret(std::uint32_t pipeBankXor) {
        const auto alignment = std::max<std::uint64_t>(65536u, context.limits.minStorageBufferOffsetAlignment);
        const auto stride = static_cast<std::uint64_t>(context.limits.maxStorageBufferRange) / alignment * alignment;
        Require(stride != 0u, "multisample XOR transition buffer limit is too small");
        auto physical = std::make_shared<DeviceBuffer>(context, target.bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        context.detiler->BeginBatch();
        recordCommands(context, [&](VkCommandBuffer commands, Recorder* recorder) {
            if (recorder != nullptr) recorder->Keep(physical, physical->Size());
            constexpr VkAccessFlags access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, access, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            for (std::uint64_t offset = 0u; offset < target.bytes; offset += stride) {
                context.detiler->DispatchMultisampleTile(commands, target.elementBytes, target.samples, physical->Handle(), offset, std::min(stride, target.bytes - offset), transferView, backingExtent.width, backingExtent.height, static_cast<std::uint32_t>(offset / 65536u), target.pipeBankXor, true);
            }
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            for (std::uint64_t offset = 0u; offset < target.bytes; offset += stride) {
                context.detiler->DispatchMultisampleTile(commands, target.elementBytes, target.samples, physical->Handle(), offset, std::min(stride, target.bytes - offset), transferView, backingExtent.width, backingExtent.height, static_cast<std::uint32_t>(offset / 65536u), pipeBankXor, false);
            }
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT, access);
            if (recorder != nullptr) Recorder::CountBarriers(Recorder::CommandClass::Draw, 3u);
        });
    }

    void transfer(VkCommandBuffer commands, VkBuffer buffer, bool readback, VkExtent2D extent) {
        const auto total = static_cast<std::uint64_t>(extent.width) * extent.height * target.samples;
        Require(total <= std::numeric_limits<std::uint32_t>::max(), "multisample target transfer element count overflows");
        const auto alignment = std::max<std::uint64_t>({4u, target.elementBytes, context.limits.minStorageBufferOffsetAlignment});
        const auto stride = static_cast<std::uint64_t>(context.limits.maxStorageBufferRange) / alignment * alignment;
        Require(stride != 0u, "multisample target transfer buffer limit is too small");
        const auto count = total * target.elementBytes;
        for (std::uint64_t offset = 0; offset < count; offset += stride) {
            const auto bytes = std::min(stride, count - offset);
            const auto first = static_cast<std::uint32_t>(offset / target.elementBytes);
            const auto elements = static_cast<std::uint32_t>(bytes / target.elementBytes);
            if (readback) context.detiler->DispatchMultisampleReadback(commands, target.elementBytes, buffer, offset, (bytes + 3u) & ~3ull, transferView, extent.width, extent.height, 1u, target.samples, first, elements);
            else context.detiler->DispatchMultisampleImage(commands, target.elementBytes, buffer, offset, (bytes + 3u) & ~3ull, transferView, extent.width, extent.height, 1u, target.samples, first, elements);
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
    std::shared_ptr<std::atomic<bool>> active;
    bool depth;
    std::vector<GuestMemory::UnchangedQuery> live;
};

class PrimedSharedMutex : public std::shared_mutex {
public:
    PrimedSharedMutex() {
        lock();
        unlock();
    }
};

std::shared_mutex& guardedMutex() {
    static PrimedSharedMutex mutex;
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

void retireUnmapped(std::uintptr_t address, std::size_t bytes) {
    Require(address <= std::numeric_limits<std::uint64_t>::max() - bytes, "resident image release range overflows");
    const auto end = address + bytes;
    RetireUnmappedDepth_nid_no_patch(address, bytes);
    std::unique_lock lock(guardedMutex());
    for (auto& range : guarded()) {
        if (std::ranges::none_of(range.live, [&](const auto& part) { return address < part.address + part.bytes && part.address < end; })) continue;
        std::vector<GuestMemory::UnchangedQuery> remaining;
        remaining.reserve(range.live.size() + 1u);
        for (const auto& part : range.live) {
            const auto finish = part.address + part.bytes;
            const auto first = std::max<std::uint64_t>(address, part.address);
            const auto last = std::min<std::uint64_t>(end, finish);
            if (first >= last) remaining.push_back(part);
            else {
                if (part.address < first) remaining.push_back({part.address, static_cast<std::size_t>(first - part.address), 0u});
                if (last < finish) remaining.push_back({last, static_cast<std::size_t>(finish - last), 0u});
            }
        }
        range.live = std::move(remaining);
    }
    for (const auto& range : guarded()) {
        if (!range.active->load(std::memory_order_acquire)) continue;
        if (std::ranges::all_of(guarded(), [&](const auto& part) { return part.active != range.active || part.live.empty(); })) range.active->store(false, std::memory_order_release);
    }
    std::erase_if(guarded(), [](const auto& range) { return !range.active->load(std::memory_order_acquire); });
    anyGuarded().store(!guarded().empty(), std::memory_order_release);
}

void guard(const Context& context, const MultisampleTarget& target) {
    std::unique_lock lock(guardedMutex());
    for (const auto& range : target.Ranges()) guarded().push_back({context.device, range.address, range.bytes, target.target.samples, target.active, false, {{range.address, range.bytes, 0u}}});
    anyGuarded().store(true, std::memory_order_release);
    InstallResidentImageReadGuard_nid_no_patch();
}

std::mutex& targetsMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::shared_ptr<MultisampleTarget>>& targets() {
    static std::vector<std::shared_ptr<MultisampleTarget>> list;
    return list;
}

MultisampleTarget& acquire(const Context& context, const ColorTarget& use) {
    for (const auto& target : targets()) {
        if (!target->active->load(std::memory_order_acquire)) continue;
        if (target->context.device != context.device) continue;
        if (target->target.address >= use.address + use.bytes || use.address >= target->target.address + target->target.bytes) continue;
        target->CheckCurrent(use);
        return *target;
    }
    targets().push_back(std::make_shared<MultisampleTarget>(context, use));
    guard(context, *targets().back());
    auto& created = *targets().back();
    return created;
}

}

void InstallResidentImageReadGuard_nid_no_patch() {
    static std::once_flag installed;
    std::call_once(installed, [] { GuestMemory::SetReadGuard(&guardReads); });
    UpdateResidentImageUnmapObserver_nid_no_patch();
}

void UpdateResidentImageUnmapObserver_nid_no_patch() {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    GuestAllocations::GuestAllocationsSetUnmapObserver_nid_postfix(anyGuarded().load(std::memory_order_acquire) || HasResidentDepthGuards_nid_no_patch() ? &retireUnmapped : nullptr);
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

std::uint64_t MultisampledColorBytes(const ColorTarget& target) {
    const auto blockLog2 = target.tileMode == ColorTileMode::Standard4KB ? 12u : target.tileMode == ColorTileMode::Standard64KB || target.tileMode == ColorTileMode::RenderTarget ? 16u : 0u;
    if (blockLog2 == 0 || !std::has_single_bit(target.elementBytes) || !std::has_single_bit(target.samples)) return 0;
    const auto samplesLog2 = static_cast<std::uint32_t>(std::countr_zero(target.samples));
    const auto elementsLog2 = blockLog2 - static_cast<std::uint32_t>(std::countr_zero(target.elementBytes)) - samplesLog2;
    const auto widthLog2 = (elementsLog2 + ((samplesLog2 & 1u) == 0 ? 1u : 0u)) / 2u;
    const std::uint64_t blockWidth = std::uint64_t{1} << widthLog2;
    const std::uint64_t blockHeight = std::uint64_t{1} << (elementsLog2 - widthLog2);
    return ((target.extent.width + blockWidth - 1) / blockWidth) * ((target.extent.height + blockHeight - 1) / blockHeight) << blockLog2;
}

bool MultisampledFillClearColor(const ColorTarget& target, std::uint32_t fill, VkClearColorValue& clear) {
    const std::array<std::uint32_t, 4> pattern{fill, fill, fill, fill};
    VkFormat unorm = VK_FORMAT_UNDEFINED;
    switch (target.format) {
        case VK_FORMAT_R8G8B8A8_SRGB: unorm = VK_FORMAT_R8G8B8A8_UNORM; break;
        case VK_FORMAT_B8G8R8A8_SRGB: unorm = VK_FORMAT_B8G8R8A8_UNORM; break;
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: unorm = VK_FORMAT_A8B8G8R8_UNORM_PACK32; break;
        default: return ClearColorForTexel(target.format, target.elementBytes, pattern, clear);
    }
    if (!ClearColorForTexel(unorm, target.elementBytes, pattern, clear)) return false;
    return std::all_of(clear.float32, clear.float32 + 3, [](float value) { return value == 0.0f || value == 1.0f; });
}

MultisampledColorFill::MultisampledColorFill(const ColorTarget& target) : address(target.address), bytes(MultisampledColorBytes(target)) {}

void MultisampledColorFill::NoteFill(std::uint64_t fillAddress, std::size_t fillBytes, std::uint32_t pattern) {
    if (bytes == 0 ? fillAddress > address || address >= fillAddress + fillBytes : fillAddress >= address + bytes || address >= fillAddress + fillBytes) return;
    char text[192];
    if (bytes == 0) {
        std::snprintf(text, sizeof(text), "a fill of 0x%llx+0x%zx covers the color memory at 0x%llx of a multisampled target whose footprint is not modeled", static_cast<unsigned long long>(fillAddress), fillBytes, static_cast<unsigned long long>(address));
        refusal = text;
        return;
    }
    if (fillAddress > address || fillAddress + fillBytes < address + bytes) {
        std::snprintf(text, sizeof(text), "a fill of 0x%llx+0x%zx partly covers the color memory 0x%llx+0x%llx of a multisampled target, leaving its samples mixed", static_cast<unsigned long long>(fillAddress), fillBytes, static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
        refusal = text;
        return;
    }
    refusal.clear();
    filled = pattern;
}

std::optional<std::uint32_t> MultisampledColorFill::TakeFill() {
    Require(refusal.empty(), refusal);
    return std::exchange(filled, std::nullopt);
}

void MultisampledColorFill::Discard() {
    refusal.clear();
    filled.reset();
}

bool TakeMultisampledClear(MultisampledCmask& cmask, MultisampledColorFill& colorFill, const ColorTarget& use, VkClearColorValue& clear) {
    if (cmask.TakeClear()) {
        colorFill.Discard();
        clear = fastClearColor(use);
        return true;
    }
    const auto fill = colorFill.TakeFill();
    if (!fill) return false;
    Require(MultisampledFillClearColor(use, *fill, clear), "a fill over the color memory of a multisampled target has no exact clear value in its format");
    return true;
}

VkImageView MultisampleTargetView(const Context& context, const ColorTarget& target) {
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::lock_guard lock(targetsMutex());
    return acquire(context, target).view;
}

std::shared_ptr<Texture> MultisampleTargetTexture(const Context& context, const GuestTextureResource& resource, VkComponentMapping components) {
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::lock_guard lock(targetsMutex());
    const auto format = ResolveTextureFormat(resource.format);
    const MultisampleTarget* other = nullptr;
    for (const auto& target : targets()) {
        if (!target->active->load(std::memory_order_acquire) || target->context.device != context.device || target->target.surfaceAddress != resource.baseAddress) continue;
        const auto& rendered = target->target;
        if (rendered.format == format && rendered.extent.width == resource.width && rendered.extent.height == resource.height && rendered.samples == resource.samples && rendered.mip == 0) return target->Snapshot(resource, components, target);
        other = target.get();
    }
    char text[320];
    if (other != nullptr) std::snprintf(text, sizeof(text), "AGC graphics: sampling multisampled color target 0x%llx (%ux%u, %u samples, vk format %d) as a %ux%u texture of %u samples and guest format %u (vk %d) is not implemented", static_cast<unsigned long long>(resource.baseAddress), other->target.extent.width, other->target.extent.height, other->target.samples, static_cast<int>(other->target.format), resource.width, resource.height, resource.samples, resource.format, static_cast<int>(format));
    else std::snprintf(text, sizeof(text), "AGC graphics: sampling the %ux%u multisampled texture 0x%llx of %u samples, which no draw rendered to, is not implemented", resource.width, resource.height, static_cast<unsigned long long>(resource.baseAddress), resource.samples);
    throw std::runtime_error(text);
}

std::vector<std::byte> ReadMultisampleTargetSamples(const Context& context, const ColorTarget& target) {
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::lock_guard lock(targetsMutex());
    for (const auto& resident : targets()) {
        if (!resident->active->load(std::memory_order_acquire)) continue;
        if (resident->context.device == context.device && sameAllocation(resident->target, target)) {
            resident->CheckCurrent(target);
            return resident->ReadSamples();
        }
    }
    Require(false, "multisample target sample read requires an existing resident");
    return {};
}

std::shared_ptr<Texture> NativeMultisampleSnapshot_nid_no_patch(const Context& context, const GuestTextureResource& resource, VkComponentMapping components) {
    if (resource.dimension != TextureDimension::k2DMsaa && resource.dimension != TextureDimension::k2DMsaaArray) return nullptr;
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::lock_guard lock(targetsMutex());
    for (const auto& resident : targets()) {
        if (!resident->active->load(std::memory_order_acquire)) continue;
        const auto& target = resident->target;
        if (resident->context.device != context.device || target.address != resource.baseAddress) continue;
        if (resource.width != target.extent.width || resource.height != target.extent.height || resource.samples != target.samples || resource.pipeBankXor != target.pipeBankXor || resource.tileMode != TextureTileMode::kR64KBX || resource.baseArray != 0u || resource.depthOrLastArray != 0u || resource.mipCount != 1u || resource.baseLevel != 0u || resource.lastLevel != 0u || resource.minLod != 0u || IsConvertedTextureFormat(resource.format) || IsBlockCompressed(resource.format) || BytesPerElement(resource.format) != target.elementBytes) return nullptr;
        if (resource.dccAddress != 0u && (resource.dccAddress != target.dccAddress || resource.dccAlphaOnMsb != target.dccAlphaOnMsb || resource.dccPipeAligned != target.dccPipeAligned)) return nullptr;
        resident->CheckCurrent(target);
        return std::make_shared<Texture>(context, resident->image, resident, resource, components, Texture::NativeSnapshotTag{});
    }
    return nullptr;
}

void NoteNativeColorClear_nid_no_patch(std::uint64_t address, std::uint64_t bytes, std::uint32_t value, std::uint32_t mask) {
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::lock_guard lock(targetsMutex());
    for (const auto& resident : targets()) if (resident->active->load(std::memory_order_acquire) && resident->target.dccAddress != 0u && resident->target.dccAddress == address) resident->NoteClear(bytes, value, mask);
}

void MarkMultisampleTargetDirty(const Context& context, const ColorTarget& target) {
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::lock_guard lock(targetsMutex());
    for (const auto& resident : targets()) {
        if (!resident->active->load(std::memory_order_acquire)) continue;
        if (resident->context.device == context.device && sameTarget(resident->target, target)) {
            resident->MarkDirty();
            return;
        }
    }
    Require(false, "marking multisample writes requires an existing resident");
}

void SynchronizeMultisampleTargetRead(std::uint64_t address, std::size_t bytes) {
    RequireNoResidentDepthRead_nid_no_patch(address, bytes);
    if (bytes == 0u || !anyGuarded().load(std::memory_order_acquire)) return;
    Require(bytes <= std::numeric_limits<std::uint64_t>::max() - address, "multisample read range overflows");
    {
        std::shared_lock lock(guardedMutex());
        if (std::ranges::none_of(guarded(), [&](const auto& range) { return !range.depth && address < range.address + range.bytes && range.address < address + bytes; })) return;
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
    for (const auto& resident : targets()) if (resident->active->load(std::memory_order_acquire) && resident->Overlaps(address, bytes)) resident->PublishGuest();
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

bool MultisampledColorFillAbsorbs(std::uint64_t address, std::size_t bytes, std::uint32_t pattern) {
    if (bytes == 0) return false;
    std::lock_guard lock(targetsMutex());
    return std::any_of(targets().begin(), targets().end(), [&](const auto& target) {
        VkClearColorValue clear{};
        return target->active->load(std::memory_order_acquire) && target->target.address == address && MultisampledColorBytes(target->target) == bytes && MultisampledFillClearColor(target->target, pattern, clear);
    });
}

void NoteColorMetadataFill(std::uint64_t address, std::size_t bytes, std::uint32_t pattern, bool hostOnly) {
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::lock_guard lock(targetsMutex());
    for (const auto& target : targets()) {
        if (!target->active->load(std::memory_order_acquire)) continue;
        target->NoteFill(address, bytes, pattern, hostOnly);
        target->colorFill.NoteFill(address, bytes, pattern);
        const auto before = target->cmask.Keys();
        target->cmask.NoteFill(address, bytes, pattern);
        if (before == DccKeys::Clear0000 || target->cmask.Keys() != DccKeys::Clear0000) continue;
        static std::atomic<int> reports{0};
        if (reports.fetch_add(1) < 8) std::fprintf(stderr, "[gpu] multisampled color target 0x%llx fast-cleared through its CMASK 0x%llx (fill 0x%llx+0x%zx)\n", static_cast<unsigned long long>(target->target.surfaceAddress), static_cast<unsigned long long>(target->target.cmaskAddress), static_cast<unsigned long long>(address), bytes);
    }
}

void ClearMultisampleTargets(VkDevice device) {
    std::lock_guard lock(targetsMutex());
    std::erase_if(targets(), [&](const auto& target) { return target->context.device == device; });
    std::unique_lock guardLock(guardedMutex());
    std::erase_if(guarded(), [&](const auto& range) { return range.device == device && !range.depth; });
    anyGuarded().store(!guarded().empty(), std::memory_order_release);
    UpdateResidentImageUnmapObserver_nid_no_patch();
}

void RequireNoMultisampleTarget(std::uint64_t address, std::size_t bytes, const char* access) {
    RequireNoResidentDepthRead_nid_no_patch(address, bytes);
    if (bytes == 0 || !anyGuarded().load(std::memory_order_acquire)) return;
    std::shared_lock lock(guardedMutex());
    for (const auto& range : guarded()) {
        if (range.depth || address >= range.address + range.bytes || range.address >= address + bytes) continue;
        char text[256];
        std::snprintf(text, sizeof(text), "AGC graphics: %s of 0x%llx+0x%zx overlaps multisampled color target 0x%llx+0x%llx (%u samples), whose samples are only on the host: guest access to it is not implemented", access, static_cast<unsigned long long>(address), bytes, static_cast<unsigned long long>(range.address), static_cast<unsigned long long>(range.bytes), range.samples);
        throw std::runtime_error(text);
    }
}

}
