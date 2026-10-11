#include "prx/libSceAgcDriver/Graphics/include/MultisampleTarget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

namespace AgcDriver::Graphics {
namespace {

bool sameTarget(const ColorTarget& a, const ColorTarget& b) {
    return a.surfaceAddress == b.surfaceAddress && a.format == b.format && a.extent.width == b.extent.width && a.extent.height == b.extent.height && a.samples == b.samples;
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

template<typename Record>
void recordCommands(const Context& context, Record&& record) {
    auto* recorder = Recorder::Active();
    std::unique_ptr<CommandBatch> batch;
    if (recorder == nullptr) batch = std::make_unique<CommandBatch>(context);
    record(recorder != nullptr ? recorder->Commands() : batch->Handle(), recorder);
    if (batch) batch->SubmitAndWait();
}

class MultisampleTarget {
public:
    MultisampleTarget(const Context& context, const ColorTarget& target) : context(context), target(target), cmask(target.cmaskAddress, target.cmaskBytes, target.samples, target.cmaskFastClear, target.dccAddress != 0u) {
        Require(false, "initial guest contents of a multisampled color target are not imported");
        this->context.bufferPool.reset();
        const auto samples = static_cast<VkSampleCountFlagBits>(target.samples);
        Require((context.limits.framebufferColorSampleCounts & samples) != 0, "the device cannot render color with the target's sample count");
        VkFormatProperties properties{};
        context.formatProperties(context.physical, target.format, &properties);
        Require((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0, "multisampled render-target format cannot be a color attachment");
        constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImageFormatProperties supported{};
        Check(context.imageFormatProperties(context.physical, target.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &supported), "vkGetPhysicalDeviceImageFormatProperties multisampled target");
        Require((supported.sampleCounts & samples) != 0 && target.extent.width <= supported.maxExtent.width && target.extent.height <= supported.maxExtent.height, "multisampled render target exceeds device image limits");
        try {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
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
            viewInfo.image = image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = target.format;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView multisampled target");
            recordCommands(context, [&](VkCommandBuffer commands, Recorder* recorder) {
                VkImageMemoryBarrier toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                toGeneral.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toGeneral.image = image;
                toGeneral.subresourceRange = viewInfo.subresourceRange;
                context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toGeneral);
                const VkClearColorValue zero{};
                context.Resolved(&DeviceFunctions::cmdClearColorImage, "vkCmdClearColorImage")(commands, image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &toGeneral.subresourceRange);
                RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
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

    void ApplyFastClear(const ColorTarget& use) {
        if (!cmask.TakeClear()) return;
        const auto clear = fastClearColor(use);
        recordCommands(context, [&](VkCommandBuffer commands, Recorder* recorder) {
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            context.Resolved(&DeviceFunctions::cmdClearColorImage, "vkCmdClearColorImage")(commands, image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            if (recorder != nullptr) Recorder::CountBarriers(Recorder::CommandClass::Draw, 2);
        });
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
    VkRenderPass resolvePass = VK_NULL_HANDLE;
    MultisampledCmask cmask;

private:
    void release() noexcept {
        if (resolvePass) context.Function<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(context.device, resolvePass, nullptr);
        if (view) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, view, nullptr);
        if (image) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
        if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
    }
};

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
        if (target->context.device != context.device || !sameTarget(target->target, use)) continue;
        target->cmask.CheckAddress(use.cmaskAddress);
        target->cmask.CheckMode(use.samples, use.cmaskFastClear, use.dccAddress != 0u);
        target->ApplyFastClear(use);
        return *target;
    }
    targets().push_back(std::make_unique<MultisampleTarget>(context, use));
    auto& created = *targets().back();
    if (use.cmaskAddress != 0u) {
        std::vector<std::byte> metadata(use.cmaskBytes);
        GuestMemory::Read(use.cmaskAddress, metadata);
        created.cmask.Seed(metadata);
    }
    created.ApplyFastClear(use);
    return created;
}

}

MultisampledCmask::MultisampledCmask(std::uint64_t cmaskAddress, std::uint64_t cmaskBytes, std::uint32_t samples, bool colorFastClear, bool dcc) : address(cmaskAddress), bytes(cmaskBytes), samples(samples), colorClearAllowed(colorFastClear && !dcc) {
    Require((address == 0u) == (bytes == 0u) && bytes <= std::numeric_limits<std::size_t>::max() && address <= std::numeric_limits<std::uint64_t>::max() - bytes, "invalid multisampled CMASK range");
    Require(address == 0u || samples == 2u || samples == 4u || samples == 8u, "unsupported multisampled CMASK sample count");
}

void MultisampledCmask::setPattern(std::uint32_t pattern) {
    state = MultisampleCmaskState::Unsupported;
    clearPending = false;
    refusal.clear();
    const auto nibble = pattern & 15u;
    if (pattern == nibble * 0x11111111u) {
        const auto expanded = samples == 2u ? 0xdu : samples == 4u ? 0xeu : 0xfu;
        if (nibble == 0u && colorClearAllowed) state = MultisampleCmaskState::ColorClear;
        else if (nibble == 0xcu) state = MultisampleCmaskState::FmaskClear;
        else if (nibble == expanded) state = MultisampleCmaskState::Expanded;
    }
    clearPending = state == MultisampleCmaskState::ColorClear;
    if (state != MultisampleCmaskState::Unsupported) return;
    char text[192];
    std::snprintf(text, sizeof(text), "the CMASK 0x%llx pattern 0x%08x for %u samples or its color-clear mode is not modeled", static_cast<unsigned long long>(address), pattern, samples);
    refusal = text;
}

void MultisampledCmask::NoteFill(std::uint64_t fillAddress, std::size_t fillBytes, std::uint32_t pattern) {
    Require(fillAddress <= std::numeric_limits<std::uint64_t>::max() - fillBytes, "multisampled CMASK fill range overflows");
    if (fillBytes == 0u) return;
    if (address == 0 || fillAddress >= address + bytes || address >= fillAddress + fillBytes) return;
    if (fillAddress > address || fillAddress + fillBytes < address + bytes) {
        state = MultisampleCmaskState::Unsupported;
        clearPending = false;
        char text[192];
        std::snprintf(text, sizeof(text), "a fill of 0x%llx+0x%zx partly covers the CMASK 0x%llx+0x%llx of a multisampled color target, leaving its tiles mixed", static_cast<unsigned long long>(fillAddress), fillBytes, static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
        refusal = text;
        return;
    }
    setPattern(pattern);
}

void MultisampledCmask::Seed(std::span<const std::byte> metadata) {
    Require(address != 0u && metadata.size() == bytes, "multisampled CMASK snapshot size mismatch");
    state = MultisampleCmaskState::Unsupported;
    clearPending = false;
    const auto first = metadata.front();
    if (!std::ranges::all_of(metadata, [&](std::byte value) { return value == first; })) {
        refusal = "mixed multisampled CMASK snapshot is not modeled";
        return;
    }
    setPattern(std::to_integer<std::uint32_t>(first) * 0x01010101u);
}

void MultisampledCmask::CheckAddress(std::uint64_t cmaskAddress) const {
    if (cmaskAddress == address) return;
    char text[192];
    std::snprintf(text, sizeof(text), "a multisampled color target used CMASK 0x%llx after its first use with CMASK 0x%llx, which is not modeled", static_cast<unsigned long long>(cmaskAddress), static_cast<unsigned long long>(address));
    Require(false, std::string(text));
}

bool MultisampledCmask::TakeClear() {
    Require(refusal.empty(), refusal);
    if (!clearPending) return false;
    clearPending = false;
    return true;
}

void MultisampledCmask::CheckMode(std::uint32_t samples, bool colorFastClear, bool dcc) const {
    Require(samples == this->samples && colorClearAllowed == (colorFastClear && !dcc), "multisampled CMASK sample count or color-clear mode changed after first use");
}

VkImageView MultisampleTargetView(const Context& context, const ColorTarget& target) {
    std::lock_guard lock(targetsMutex());
    return acquire(context, target).view;
}

void ResolveMultisampleTarget(const Context& context, const ColorTarget& source, StorageTexture& destination, VkImageView destinationView) {
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
    for (const auto& target : targets()) {
        const auto before = target->cmask.ClearPending();
        target->cmask.NoteFill(address, bytes, pattern);
        if (before || !target->cmask.ClearPending()) continue;
        static std::atomic<int> reports{0};
        if (reports.fetch_add(1) < 8) std::fprintf(stderr, "[gpu] multisampled color target 0x%llx fast-cleared through its CMASK 0x%llx (fill 0x%llx+0x%zx)\n", static_cast<unsigned long long>(target->target.surfaceAddress), static_cast<unsigned long long>(target->target.cmaskAddress), static_cast<unsigned long long>(address), bytes);
    }
}

void ClearMultisampleTargets(VkDevice device) {
    std::lock_guard lock(targetsMutex());
    std::erase_if(targets(), [&](const auto& target) { return target->context.device == device; });
}

}
