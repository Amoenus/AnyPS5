#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTextureLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace AgcDriver::Graphics {

void Texture::initializeMultisample(TextureDetiler& detiler, const GuestTextureResource& descriptor, VkComponentMapping components, std::span<const std::byte> snapshot) {
    Require(context.storageImageMultisample && context.storageImageWriteWithoutFormat, "device does not enable multisampled storage image writes without format");
    Require(descriptor.dccAddress == 0u, "compressed multisampled texture snapshot is not implemented");
    Require(descriptor.tileMode == TextureTileMode::kR64KBX || (descriptor.tileMode == TextureTileMode::kZ64KBX && (descriptor.format == 7u || descriptor.format == 22u)), "multisampled texture snapshot requires R tiling or numeric Z depth");
    Require(!IsConvertedTextureFormat(descriptor.format) && !IsBlockCompressed(descriptor.format), "multisampled texture format requires an unsupported conversion");
    const auto geometry = DescribeSurface(descriptor);
    const auto elementBytes = BytesPerElement(descriptor.format);
    const MultisampleTextureLayout address(descriptor.tileMode, elementBytes, descriptor.fragments, descriptor.width, descriptor.height, geometry.layers, descriptor.pipeBankXor);
    Require(snapshot.size() == address.Bytes(), "multisampled texture snapshot size mismatch");
    const auto totalElements = address.LinearBytes() / elementBytes;
    Require(totalElements <= std::numeric_limits<std::uint32_t>::max() && address.LinearBytes() <= std::numeric_limits<std::size_t>::max() - 3u, "multisampled texture upload size overflows");
    const auto vkFormat = SampledTextureFormat(context, descriptor.format);
    const auto elementFormat = TextureDetiler::ImageElementFormat(elementBytes);
    Require(elementFormat != VK_FORMAT_UNDEFINED && context.imageFormatProperties != nullptr && context.formatProperties != nullptr, "multisampled texture requires image format queries");
    VkFormatProperties sampledProperties{};
    context.formatProperties(context.physical, vkFormat, &sampledProperties);
    Require((sampledProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0u, "multisampled texture format cannot be sampled");
    VkImageFormatProperties imageProperties{};
    const auto usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    Check(context.imageFormatProperties(context.physical, elementFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, &imageProperties), "vkGetPhysicalDeviceImageFormatProperties multisample texture");
    const auto samples = static_cast<VkSampleCountFlagBits>(descriptor.fragments);
    Require((imageProperties.sampleCounts & samples) != 0u && descriptor.width <= imageProperties.maxExtent.width && descriptor.height <= imageProperties.maxExtent.height && geometry.layers <= imageProperties.maxArrayLayers, "multisampled texture extent or fragment count is unsupported by the device");
    const auto bufferBytes = (address.LinearBytes() + 3u) & ~3ull;
    const auto alignment = std::max<std::uint64_t>({4u, elementBytes, context.limits.minStorageBufferOffsetAlignment});
    const auto stride = static_cast<std::uint64_t>(context.limits.maxStorageBufferRange) / alignment * alignment;
    Require(stride != 0u, "multisampled texture upload buffer limit is too small");

    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = elementFormat;
    info.extent = {descriptor.width, descriptor.height, 1u};
    info.mipLevels = 1u;
    info.arrayLayers = geometry.layers;
    info.samples = samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &image), "vkCreateImage multisample texture");
    owned = std::make_shared<OwnedImage>(context, image, VK_NULL_HANDLE);
    VkMemoryRequirements requirements{};
    context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    allocationBytes = requirements.size;
    Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &owned->memory), "vkAllocateMemory multisample texture");
    Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, owned->memory, 0u), "vkBindImageMemory multisample texture");
    VkImageViewUsageCreateInfo viewUsage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    viewUsage.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageViewCreateInfo transferInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &viewUsage};
    transferInfo.image = image;
    transferInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    transferInfo.format = elementFormat;
    transferInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, geometry.layers};
    VkImageView transferView = VK_NULL_HANDLE;
    owned->transferViews.reserve(1u);
    Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &transferInfo, nullptr, &transferView), "vkCreateImageView multisample upload");
    owned->transferViews.push_back(transferView);

    auto linear = std::make_shared<Buffer>(context, static_cast<std::size_t>(bufferBytes), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    address.Detile(snapshot, linear->Bytes());
    if (bufferBytes != address.LinearBytes()) std::memset(linear->Bytes().data() + address.LinearBytes(), 0, bufferBytes - address.LinearBytes());
    detiler.BeginBatch();
    auto* recorder = Recorder::Active();
    std::unique_ptr<CommandBatch> batch;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    if (recorder != nullptr) {
        commands = recorder->Commands();
        recorder->Keep(linear, static_cast<std::size_t>(bufferBytes));
        recorder->Keep(owned);
    } else {
        batch = std::make_unique<CommandBatch>(context);
        commands = batch->Handle();
    }
    VkImageMemoryBarrier transition{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    transition.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    transition.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    transition.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    transition.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    transition.image = image;
    transition.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, geometry.layers};
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &transition);
    const VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 1u, &host, 0u, nullptr, 0u, nullptr);
    for (std::uint64_t offset = 0u; offset < address.LinearBytes(); offset += stride) {
        const auto dataBytes = std::min(stride, address.LinearBytes() - offset);
        detiler.DispatchMultisampleImage(commands, elementBytes, linear->Handle(), offset, (dataBytes + 3u) & ~3ull, transferView, descriptor.width, descriptor.height, geometry.layers, descriptor.fragments, static_cast<std::uint32_t>(offset / elementBytes), static_cast<std::uint32_t>(dataBytes / elementBytes));
    }
    transition.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    transition.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    transition.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    transition.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &transition);
    if (batch != nullptr) batch->SubmitAndWait();

    viewUsage.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageViewCreateInfo sampledInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &viewUsage};
    sampledInfo.image = image;
    sampledInfo.viewType = descriptor.dimension == TextureDimension::k2DMsaaArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    sampledInfo.format = vkFormat;
    sampledInfo.components = components;
    sampledInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, descriptor.baseArray, geometry.layers - descriptor.baseArray};
    Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &sampledInfo, nullptr, &view), "vkCreateImageView multisample texture");
    viewFormat = vkFormat;
    viewRange = {sampledInfo.viewType, 1u, sampledInfo.subresourceRange.layerCount};
    createFirstLayerView(descriptor, sampledInfo);
}

}
