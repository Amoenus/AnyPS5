#include "GraphicsTests.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleHtile.hpp"
#include <array>
#include <cstdio>
#include <cstdint>
#include <map>
#include <memory>
#include <string_view>

namespace {

using namespace AgcDriver::Graphics;

constexpr std::uint32_t FormatR8Uint = 5;
constexpr std::uint32_t FormatR16Unorm = 7;
constexpr std::uint32_t FormatR16Uint = 11;
constexpr std::uint32_t FormatR32Uint = 20;
constexpr std::uint32_t FormatR32Float = 22;
constexpr std::uint32_t FormatR11G11B10Float = 36;
constexpr std::uint32_t FormatR8G8B8A8Unorm = 56;
constexpr std::uint32_t TileDepth64KB = 0x18;
constexpr std::uint32_t TileRenderTarget64KB = 0x1b;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Type2DArray = 13;

std::uintptr_t nextHandle = 0x1000;

template<typename THandle>
THandle newHandle() {
    return reinterpret_cast<THandle>(nextHandle += 0x10);
}

VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage* image) {
    *image = newHandle<VkImage>();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL imageMemoryRequirements(VkDevice, VkImage, VkMemoryRequirements* requirements) {
    *requirements = {65536, 65536, 1};
}

VKAPI_ATTR VkResult VKAPI_CALL allocateMemory(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory* memory) {
    *memory = newHandle<VkDeviceMemory>();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL bindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL createImageView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView* view) {
    *view = newHandle<VkImageView>();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL allocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer* commands) {
    *commands = newHandle<VkCommandBuffer>();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL createFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* fence) {
    *fence = newHandle<VkFence>();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL beginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL endCommandBuffer(VkCommandBuffer) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL queueSubmit(VkQueue, std::uint32_t, const VkSubmitInfo*, VkFence) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL waitForFences(VkDevice, std::uint32_t, const VkFence*, VkBool32, std::uint64_t) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL getFenceStatus(VkDevice, VkFence) {
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL freeCommandBuffers(VkDevice, VkCommandPool, std::uint32_t, const VkCommandBuffer*) {}
VKAPI_ATTR void VKAPI_CALL destroyFence(VkDevice, VkFence, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL destroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice, VkImage, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL freeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL cmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, std::uint32_t, const VkMemoryBarrier*, std::uint32_t, const VkBufferMemoryBarrier*, std::uint32_t, const VkImageMemoryBarrier*) {}
VKAPI_ATTR void VKAPI_CALL cmdClearDepthStencilImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearDepthStencilValue*, std::uint32_t, const VkImageSubresourceRange*) {}

VKAPI_ATTR void VKAPI_CALL formatProperties(VkPhysicalDevice, VkFormat, VkFormatProperties* properties) {
    *properties = {};
    properties->optimalTilingFeatures = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
}

PFN_vkVoidFunction VKAPI_CALL deviceProc(VkDevice, const char* name) {
    static const std::map<std::string_view, PFN_vkVoidFunction> table{
        {"vkCreateImage", reinterpret_cast<PFN_vkVoidFunction>(createImage)},
        {"vkGetImageMemoryRequirements", reinterpret_cast<PFN_vkVoidFunction>(imageMemoryRequirements)},
        {"vkAllocateMemory", reinterpret_cast<PFN_vkVoidFunction>(allocateMemory)},
        {"vkBindImageMemory", reinterpret_cast<PFN_vkVoidFunction>(bindImageMemory)},
        {"vkCreateImageView", reinterpret_cast<PFN_vkVoidFunction>(createImageView)},
        {"vkAllocateCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(allocateCommandBuffers)},
        {"vkCreateFence", reinterpret_cast<PFN_vkVoidFunction>(createFence)},
        {"vkBeginCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(beginCommandBuffer)},
        {"vkEndCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(endCommandBuffer)},
        {"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(queueSubmit)},
        {"vkWaitForFences", reinterpret_cast<PFN_vkVoidFunction>(waitForFences)},
        {"vkGetFenceStatus", reinterpret_cast<PFN_vkVoidFunction>(getFenceStatus)},
        {"vkFreeCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(freeCommandBuffers)},
        {"vkDestroyFence", reinterpret_cast<PFN_vkVoidFunction>(destroyFence)},
        {"vkDestroyImageView", reinterpret_cast<PFN_vkVoidFunction>(destroyImageView)},
        {"vkDestroyImage", reinterpret_cast<PFN_vkVoidFunction>(destroyImage)},
        {"vkFreeMemory", reinterpret_cast<PFN_vkVoidFunction>(freeMemory)},
        {"vkCmdPipelineBarrier", reinterpret_cast<PFN_vkVoidFunction>(cmdPipelineBarrier)},
        {"vkCmdClearDepthStencilImage", reinterpret_cast<PFN_vkVoidFunction>(cmdClearDepthStencilImage)}
    };
    const auto it = table.find(name);
    return it == table.end() ? nullptr : it->second;
}

struct View {
    std::uint64_t address;
    std::uint32_t format;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t tileMode = TileDepth64KB;
    std::uint32_t type = Type2D;
    std::uint32_t lastArray = 0;
};

std::array<std::uint32_t, 8> tsharp(const View& view) {
    std::array<std::uint32_t, 8> words{};
    const auto base = view.address >> 8u;
    const auto widthMinus1 = view.width - 1u;
    const auto heightMinus1 = view.height - 1u;
    words[0] = static_cast<std::uint32_t>(base);
    words[1] = static_cast<std::uint32_t>((base >> 32u) & 0xffu) | ((view.format & 0x1ffu) << 20u) | ((widthMinus1 & 0x3u) << 30u);
    words[2] = ((widthMinus1 >> 2u) & 0xfffu) | ((heightMinus1 & 0x3fffu) << 14u);
    words[3] = 4u | (5u << 3u) | (6u << 6u) | (7u << 9u) | ((view.tileMode & 0x1fu) << 20u) | ((view.type & 0xfu) << 28u);
    words[4] = view.lastArray & 0x1fffu;
    return words;
}

std::shared_ptr<Texture> lookup(const Context& context, const View& view) {
    const auto words = tsharp(view);
    return DepthSurfaceTexture(context, words, DecodeTextureResource(words), VkComponentMapping{});
}

}

void RunDepthSurfaceReuseTests() {
    static int deviceTag = 0;
    Context context{};
    context.device = reinterpret_cast<VkDevice>(&deviceTag);
    context.deviceProc = deviceProc;
    context.formatProperties = formatProperties;
    context.memory.memoryTypeCount = 1;
    context.memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    context.limits.maxFramebufferWidth = 16384;
    context.limits.maxFramebufferHeight = 16384;
    constexpr std::uint64_t depth32 = 0x40000000;
    constexpr std::uint64_t depth16 = 0x50000000;
    constexpr std::uint64_t depthStencil = 0x60000000;
    constexpr std::uint64_t stencil = 0x60800000;
    DepthSurfaceView(context, {depth32, 0, {384, 384}, VK_FORMAT_D32_SFLOAT, 1.0f, 0});
    DepthSurfaceView(context, {depth16, 0, {256, 256}, VK_FORMAT_D16_UNORM, 1.0f, 0});
    DepthSurfaceView(context, {depthStencil, stencil, {320, 180}, VK_FORMAT_D32_SFLOAT_S8_UINT, 1.0f, 0});

    Require(lookup(context, {depth32, FormatR32Float, 384, 384}) != nullptr, "an R32F view of a D32 surface's own extent must sample its depth plane");
    Require(lookup(context, {depth16, FormatR16Unorm, 256, 256}) != nullptr, "an R16 view of a D16 surface's own extent must sample its depth plane");
    Require(lookup(context, {depth32, FormatR32Uint, 384, 384}) != nullptr, "a 32-bit raw depth bits view of a D32 surface must sample its depth plane");
    Require(lookup(context, {depth16, FormatR16Uint, 256, 256}) != nullptr, "a 16-bit raw depth bits view of a D16 surface must sample its depth plane");

    Require(lookup(context, {depth32, FormatR32Float, 192, 192}) == nullptr, "a view of another extent over a depth surface must be read as reused memory");
    Require(lookup(context, {depth32, FormatR11G11B10Float, 384, 384}) == nullptr, "an R11G11B10 view of a D32 surface's own extent must be read as reused memory");
    Require(lookup(context, {depth32, FormatR32Float, 512, 512, TileDepth64KB, Type2DArray, 5}) == nullptr, "a 2D array view of another extent over a depth surface must be read as reused memory");
    Require(lookup(context, {depth32, FormatR32Uint, 384, 384, TileRenderTarget64KB}) == nullptr, "an R32 uint view without a depth layout must be read as reused memory");
    Require(lookup(context, {depth16, FormatR32Float, 256, 256}) == nullptr, "an R32F view of a D16 surface's own extent must be read as reused memory");

    Require(lookup(context, {stencil, FormatR8Uint, 320, 180}) != nullptr, "an 8-bit view of a stencil plane's own extent must sample the stencil plane");
    Require(lookup(context, {stencil, FormatR8G8B8A8Unorm, 320, 180, TileRenderTarget64KB}) == nullptr, "an RGBA8 view of a stencil plane's own extent must be read as reused memory");
    Require(lookup(context, {stencil, FormatR32Float, 320, 180}) == nullptr, "an R32F view of a stencil plane's own extent must be read as reused memory");
    const auto depth32Bytes = DepthSliceBytes({384u, 384u}, 4u);
    Require(DepthSurfaceGuestOverlap_nid_no_patch(context, depth32, 1u), "resident depth base was not detected");
    Require(DepthSurfaceGuestOverlap_nid_no_patch(context, depth32 + depth32Bytes - 1u, 1u), "resident depth final byte was not detected");
    Require(DepthSurfaceGuestOverlap_nid_no_patch(context, depth32 - 1u, 2u), "resident depth overlap from before its base was not detected");
    Require(!DepthSurfaceGuestOverlap_nid_no_patch(context, depth32 - 1u, 1u), "adjacent bytes before depth overlap");
    Require(!DepthSurfaceGuestOverlap_nid_no_patch(context, depth32 + depth32Bytes, 1u), "adjacent bytes after depth overlap");
    auto another = context;
    another.device = reinterpret_cast<VkDevice>(static_cast<std::uintptr_t>(1u));
    Require(!DepthSurfaceGuestOverlap_nid_no_patch(another, depth32, depth32Bytes), "resident depth from another device overlaps");
    constexpr std::uint64_t overlapStencil = 0x70000000u;
    DepthSurfaceView(context, {0x60000000u, overlapStencil, {256u, 256u}, VK_FORMAT_D16_UNORM_S8_UINT, 1.0f, 0});
    Require(DepthSurfaceGuestOverlap_nid_no_patch(context, overlapStencil + DepthSliceBytes({256u, 256u}, 1u) - 1u, 1u), "resident stencil final byte was not detected");
    Require(!DepthSurfaceGuestOverlap_nid_no_patch(context, overlapStencil + DepthSliceBytes({256u, 256u}, 1u), 1u), "adjacent bytes after stencil overlap");
    constexpr std::uint64_t htile = 0x78000000u;
    DepthSurfaceView(context, {0x79000000u, 0u, {16u, 16u}, VK_FORMAT_D32_SFLOAT, 1.0f, 0, htile});
    Require(DepthSurfaceGuestOverlap_nid_no_patch(context, htile, 1u), "resident HTILE base was not detected");
    Require(DepthSurfaceGuestOverlap_nid_no_patch(context, htile + 32767u, 1u), "resident HTILE padding was not detected");
    Require(!DepthSurfaceGuestOverlap_nid_no_patch(context, htile + 32768u, 1u), "adjacent bytes after HTILE overlap");
    for (const auto range : {std::array<std::uint64_t, 2>{0u, 0u}, std::array<std::uint64_t, 2>{~0ull, 1u}}) {
        bool rejected = false;
        try { DepthSurfaceGuestOverlap_nid_no_patch(context, range[0], range[1]); } catch (const std::runtime_error&) { rejected = true; }
        Require(rejected, "invalid resident depth overlap range did not throw");
    }

    ClearDepthSurfaces(context.device);
    std::uint32_t checked = 0u;
    constexpr std::array<std::array<VkExtent2D, 3>, 3> blocks{{
        {{{128u, 256u}, {128u, 128u}, {64u, 128u}}},
        {{{128u, 128u}, {128u, 64u}, {64u, 64u}}},
        {{{64u, 128u}, {64u, 64u}, {32u, 64u}}}
    }};
    for (const auto extent : {VkExtent2D{5u, 3u}, VkExtent2D{129u, 67u}, VkExtent2D{3840u, 2160u}}) {
        for (std::uint32_t sampleIndex = 0u; sampleIndex < 3u; ++sampleIndex) {
            const auto samples = 2u << sampleIndex;
            for (std::uint32_t formatIndex = 1u; formatIndex <= 2u; ++formatIndex) {
                const auto bytes = 1u << formatIndex;
                const auto expectedBytes = [&](std::uint32_t index) {
                    const auto block = blocks[index][sampleIndex];
                    return static_cast<std::uint64_t>((extent.width + block.width - 1u) / block.width) * ((extent.height + block.height - 1u) / block.height) * 65536u;
                };
                const auto depthBytes = expectedBytes(formatIndex);
                const auto stencilBytes = expectedBytes(0u);
                const auto metadataBytes = MultisampleHtileBytes_nid_no_patch(extent.width, extent.height, 1u);
                Require(DepthPlaneBytes_nid_no_patch(extent, bytes, samples) == depthBytes && DepthPlaneBytes_nid_no_patch(extent, 1u, samples) == stencilBytes, "multisample depth/stencil footprint differs from the 64 KiB block geometry");
                constexpr std::uint64_t depthBase = 0x100000000ull;
                constexpr std::uint64_t stencilBase = 0x200000000ull;
                constexpr std::uint64_t metadataBase = 0x300000000ull;
                const DepthTarget target{depthBase, stencilBase, extent, formatIndex == 1u ? VK_FORMAT_D16_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT, 0.0f, 0, metadataBase, true, samples};
                for (const auto range : {std::array<std::uint64_t, 2>{depthBase, depthBytes}, std::array<std::uint64_t, 2>{stencilBase, stencilBytes}, std::array<std::uint64_t, 2>{metadataBase, metadataBytes}}) {
                    Require(DepthTargetGuestOverlap_nid_no_patch(target, range[0], 1u), "multisample plane base was not detected");
                    Require(DepthTargetGuestOverlap_nid_no_patch(target, range[0] + range[1] - 1u, 1u), "multisample plane final byte was not detected");
                    Require(DepthTargetGuestOverlap_nid_no_patch(target, range[0] - 1u, 2u), "multisample plane overlap from before its base was not detected");
                    Require(!DepthTargetGuestOverlap_nid_no_patch(target, range[0] - 1u, 1u), "adjacent bytes before a multisample plane overlap");
                    Require(!DepthTargetGuestOverlap_nid_no_patch(target, range[0] + range[1], 1u), "adjacent bytes after a multisample plane overlap");
                }
                ClearDepthSurfaces(context.device);
                ++checked;
            }
        }
    }
    Require(DepthPlaneBytes_nid_no_patch({3840u, 2160u}, 4u, 8u) == 0xff00000u, "captured eight-sample D32 physical footprint changed");
    for (const auto samples : {0u, 3u, 16u}) {
        bool rejected = false;
        try { DepthPlaneBytes_nid_no_patch({16u, 16u}, 4u, samples); } catch (const std::runtime_error&) { rejected = true; }
        Require(rejected, "unsupported depth plane sample count did not throw");
    }
    std::printf("Multisample depth/stencil footprint configurations checked=%u\n", checked);
}
