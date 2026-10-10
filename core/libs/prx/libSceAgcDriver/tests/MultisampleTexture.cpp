#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTextureLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "Optimization/DescriptorBindingBuilder.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include "SpirvBackend/SpirvModule.hpp"
#include <SDL_loadso.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;

class Device {
public:
    Device() {
#ifdef _WIN32
        library = SDL_LoadObject("vulkan-1.dll");
#else
        library = SDL_LoadObject("libvulkan.so.1");
#endif
        Require(library != nullptr, "cannot load Vulkan");
        proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_LoadFunction(library, "vkGetInstanceProcAddr"));
        Require(proc != nullptr, "Vulkan instance procedure is missing");
        try {
            VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            app.apiVersion = VK_API_VERSION_1_1;
            VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            info.pApplicationInfo = &app;
            Check(function<PFN_vkCreateInstance>("vkCreateInstance")(&info, nullptr, &instance), "vkCreateInstance");
            std::uint32_t count = 0u;
            const auto enumerate = function<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
            Check(enumerate(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
            std::vector<VkPhysicalDevice> devices(count);
            Check(enumerate(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
            int selected = -1;
            for (const auto physical : devices) {
                VkPhysicalDeviceProperties properties{};
                function<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(physical, &properties);
                if (const auto* wanted = std::getenv("ANYPS5_GPU"); wanted != nullptr && std::string(properties.deviceName).find(wanted) == std::string::npos) continue;
                const int rank = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
                if (properties.apiVersion < VK_API_VERSION_1_1 || rank <= selected) continue;
                context.physical = physical;
                context.limits = properties.limits;
                selected = rank;
            }
            Require(context.physical != VK_NULL_HANDLE, "no Vulkan 1.1 device");
            const auto queues = function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
            queues(context.physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            queues(context.physical, &count, families.data());
            std::uint32_t family = 0u;
            while (family < count && (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0u) ++family;
            Require(family < count, "no compute queue");
            VkPhysicalDeviceFeatures available{};
            function<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(context.physical, &available);
            Require(available.shaderStorageImageMultisample && available.shaderStorageImageWriteWithoutFormat, "device lacks multisampled image upload features");
            VkPhysicalDeviceFeatures enabled{};
            enabled.shaderStorageImageMultisample = VK_TRUE;
            enabled.shaderStorageImageWriteWithoutFormat = VK_TRUE;
            const float priority = 1.0f;
            VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue.queueFamilyIndex = family;
            queue.queueCount = 1u;
            queue.pQueuePriorities = &priority;
            VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            device.queueCreateInfoCount = 1u;
            device.pQueueCreateInfos = &queue;
            device.pEnabledFeatures = &enabled;
            Check(function<PFN_vkCreateDevice>("vkCreateDevice")(context.physical, &device, nullptr, &context.device), "vkCreateDevice");
            context.deviceProc = function<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
            context.formatProperties = function<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
            context.imageFormatProperties = function<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties");
            function<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(context.physical, &context.memory);
            context.storageImageMultisample = true;
            context.storageImageWriteWithoutFormat = true;
            context.Function<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(context.device, family, 0u, &context.queue);
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.queueFamilyIndex = family;
            Check(context.Function<PFN_vkCreateCommandPool>("vkCreateCommandPool")(context.device, &pool, nullptr, &context.pool), "vkCreateCommandPool");
        } catch (...) { release(); throw; }
    }
    ~Device() { release(); }
    Context GetContext() const { return context; }
private:
    template<typename TFunction> TFunction function(const char* name) const {
        const auto result = reinterpret_cast<TFunction>(proc(instance, name));
        Require(result != nullptr, std::string("missing Vulkan function ") + name);
        return result;
    }
    void release() noexcept {
        if (context.device != VK_NULL_HANDLE) {
            context.Function<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(context.device);
            ClearCachedTextures(context.device);
            context.bufferPool.reset();
            if (context.pool) context.Function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(context.device, context.pool, nullptr);
            function<PFN_vkDestroyDevice>("vkDestroyDevice")(context.device, nullptr);
        }
        if (instance) function<PFN_vkDestroyInstance>("vkDestroyInstance")(instance, nullptr);
        if (library) SDL_UnloadObject(library);
    }
    void* library = nullptr;
    PFN_vkGetInstanceProcAddr proc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    Context context{};
};

std::vector<std::uint32_t> FetchShader(std::uint32_t width, std::uint32_t height, std::uint32_t fragments, bool arrayed, bool floating) {
    ShaderRecompiler::SpirvModule module;
    module.EmitCapability(spv::CapabilityShader);
    if (arrayed) module.EmitCapability(spv::CapabilityImageMSArray);
    module.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
    const auto u32 = module.Type(spv::OpTypeInt, 32u, 0u);
    const auto i32 = module.Type(spv::OpTypeInt, 32u, 1u);
    const auto scalar = floating ? module.Type(spv::OpTypeFloat, 32u) : u32;
    const auto v4 = module.Type(spv::OpTypeVector, scalar, 4u);
    const auto v3 = module.Type(spv::OpTypeVector, u32, 3u);
    const auto coordinateType = module.Type(spv::OpTypeVector, i32, arrayed ? 3u : 2u);
    const auto voidType = module.Type(spv::OpTypeVoid);
    const auto constant = [&](std::uint32_t value) { return module.Constant(spv::OpConstant, u32, value); };
    const auto array = module.Type(spv::OpTypeRuntimeArray, v4);
    module.AddAnnotation(spv::OpDecorate, array, spv::DecorationArrayStride, 16u);
    const auto block = module.Type(spv::OpTypeStruct, array);
    module.AddAnnotation(spv::OpDecorate, block, spv::DecorationBlock);
    module.AddAnnotation(spv::OpMemberDecorate, block, 0u, spv::DecorationOffset, 0u);
    const auto buffer = module.DefineGlobalVariable(module.Type(spv::OpTypePointer, spv::StorageClassStorageBuffer, block), spv::StorageClassStorageBuffer);
    module.AddAnnotation(spv::OpDecorate, buffer, spv::DecorationDescriptorSet, 0u);
    module.AddAnnotation(spv::OpDecorate, buffer, spv::DecorationBinding, 1u);
    const auto imageType = module.Type(spv::OpTypeImage, scalar, spv::Dim2D, 0u, arrayed ? 1u : 0u, 1u, 1u, spv::ImageFormatUnknown);
    const auto image = module.DefineGlobalVariable(module.Type(spv::OpTypePointer, spv::StorageClassUniformConstant, imageType), spv::StorageClassUniformConstant);
    module.AddAnnotation(spv::OpDecorate, image, spv::DecorationDescriptorSet, 0u);
    module.AddAnnotation(spv::OpDecorate, image, spv::DecorationBinding, 0u);
    const auto group = module.DefineGlobalVariable(module.Type(spv::OpTypePointer, spv::StorageClassInput, v3), spv::StorageClassInput);
    module.AddAnnotation(spv::OpDecorate, group, spv::DecorationBuiltIn, spv::BuiltInWorkgroupId);
    const auto main = module.AllocateId();
    module.AddFunction(spv::OpFunction, voidType, main, spv::FunctionControlMaskNone, module.Type(spv::OpTypeFunction, voidType));
    module.AddFunction(spv::OpLabel, module.AllocateId());
    const auto loaded = module.AllocateId();
    module.AddFunction(spv::OpLoad, v3, loaded, group);
    const auto index = module.AllocateId();
    module.AddFunction(spv::OpCompositeExtract, u32, index, loaded, 0u);
    const auto binary = [&](spv::Op op, std::uint32_t a, std::uint32_t b) {
        const auto result = module.AllocateId();
        module.AddFunction(op, u32, result, a, b);
        return result;
    };
    const auto pixel = binary(spv::OpUDiv, index, constant(fragments));
    const auto x = binary(spv::OpUMod, pixel, constant(width));
    const auto y = binary(spv::OpUMod, binary(spv::OpUDiv, pixel, constant(width)), constant(height));
    const auto z = binary(spv::OpUDiv, pixel, constant(width * height));
    const auto signedValue = [&](std::uint32_t value) {
        const auto result = module.AllocateId();
        module.AddFunction(spv::OpBitcast, i32, result, value);
        return result;
    };
    const auto coordinate = module.AllocateId();
    if (arrayed) module.AddFunction(spv::OpCompositeConstruct, coordinateType, coordinate, signedValue(x), signedValue(y), signedValue(z));
    else module.AddFunction(spv::OpCompositeConstruct, coordinateType, coordinate, signedValue(x), signedValue(y));
    const auto object = module.AllocateId();
    module.AddFunction(spv::OpLoad, imageType, object, image);
    const auto value = module.AllocateId();
    module.AddFunction(spv::OpImageFetch, v4, value, object, coordinate, spv::ImageOperandsSampleMask, binary(spv::OpUMod, index, constant(fragments)));
    const auto pointer = module.AllocateId();
    module.AddFunction(spv::OpAccessChain, module.Type(spv::OpTypePointer, spv::StorageClassStorageBuffer, v4), pointer, buffer, constant(0u), binary(spv::OpIAdd, index, constant(1u)));
    module.AddFunction(spv::OpStore, pointer, value);
    module.AddFunction(spv::OpReturn);
    module.AddFunction(spv::OpFunctionEnd);
    module.EmitEntryPoint(spv::ExecutionModelGLCompute, main, "main", {group});
    module.AddExecutionMode(main, spv::ExecutionModeLocalSize, 1u, 1u, 1u);
    return module.Finalize();
}

class Fetch {
public:
    Fetch(const Context& context, const Texture& texture, Buffer& output, std::span<const std::uint32_t> code, bool firstLayer = false) : context(context) {
        try {
            const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{{0u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {1u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
            VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            info.bindingCount = 2u;
            info.pBindings = bindings.data();
            Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &info, nullptr, &setLayout), "fetch descriptor layout");
            const std::array<VkDescriptorPoolSize, 2> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1u}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u}}};
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = 1u;
            poolInfo.poolSizeCount = 2u;
            poolInfo.pPoolSizes = sizes.data();
            Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &pool), "fetch descriptor pool");
            const VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool, 1u, &setLayout};
            Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &allocate, &set), "fetch descriptor set");
            const VkDescriptorImageInfo image{VK_NULL_HANDLE, firstLayer ? texture.FirstLayerView() : texture.View(), texture.Layout()};
            const VkDescriptorBufferInfo buffer{output.Handle(), 0u, output.Bytes().size()};
            const std::array<VkWriteDescriptorSet, 2> writes{{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0u, 0u, 1u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &image, nullptr, nullptr}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1u, 0u, 1u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &buffer, nullptr}}};
            context.Function<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(context.device, 2u, writes.data(), 0u, nullptr);
            VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layoutInfo.setLayoutCount = 1u;
            layoutInfo.pSetLayouts = &setLayout;
            Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &layoutInfo, nullptr, &layout), "fetch pipeline layout");
            VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            shaderInfo.codeSize = code.size_bytes();
            shaderInfo.pCode = code.data();
            Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &shaderInfo, nullptr, &module), "fetch shader module");
            VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipelineInfo.layout = layout;
            pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0u, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
            Check(context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, VK_NULL_HANDLE, 1u, &pipelineInfo, nullptr, &pipeline), "fetch pipeline");
        } catch (...) { release(); throw; }
    }
    ~Fetch() { release(); }
    void Run(VkCommandBuffer commands, std::uint32_t groups) const {
        context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        context.Function<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0u, 1u, &set, 0u, nullptr);
        context.Function<PFN_vkCmdDispatch>("vkCmdDispatch")(commands, groups, 1u, 1u);
    }
private:
    void release() noexcept {
        if (pipeline) context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, pipeline, nullptr);
        if (module) context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
        if (layout) context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, layout, nullptr);
        if (pool) context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(context.device, pool, nullptr);
        if (setLayout) context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, setLayout, nullptr);
    }
    Context context;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

void CheckTexture(Context context, std::uint32_t format, std::uint32_t fragments, bool arrayed, bool chunks, std::uint32_t baseArray = 0u, bool firstLayer = false, bool recorded = false) {
    const auto width = chunks ? 65u : 5u;
    const auto height = chunks ? 63u : 3u;
    const auto layers = arrayed ? 2u : 1u;
    const auto bytes = BytesPerElement(format);
    const auto count = width * height * layers * fragments;
    const bool floating = format == 71u;
    if (chunks) {
        context.limits.maxStorageBufferRange = 4096u;
        context.limits.maxComputeWorkGroupCount[0] = 2u;
    }
    TextureDetiler detiler(context);
    context.detiler = &detiler;
    GuestTextureResource descriptor{};
    descriptor.width = width;
    descriptor.height = height;
    descriptor.depthOrLastArray = layers - 1u;
    descriptor.mipCount = 1u;
    descriptor.allocatedMipCount = 1u;
    descriptor.tileMode = TextureTileMode::kR64KBX;
    descriptor.dimension = arrayed ? TextureDimension::k2DMsaaArray : TextureDimension::k2DMsaa;
    descriptor.format = format;
    descriptor.fragments = fragments;
    descriptor.baseArray = baseArray;
    descriptor.pipeBankXor = 0x8100u;
    const MultisampleTextureLayout address(descriptor.tileMode, bytes, fragments, width, height, layers, descriptor.pipeBankXor);
    std::vector<std::byte> linear(address.LinearBytes()), guest(address.Bytes() + 32u, std::byte{0xa5});
    std::vector<std::array<std::uint32_t, 4>> expected(count);
    for (std::uint32_t element = 0u; element < count; ++element) {
        auto& values = expected[element];
        values = {0u, 0u, 0u, floating ? std::bit_cast<std::uint32_t>(1.0f) : 1u};
        if (floating) {
            for (std::uint32_t component = 0u; component < 4u; ++component) {
                const float value = static_cast<float>((element / fragments) % 7u) * 16.0f + static_cast<float>(element % fragments) + static_cast<float>(component) * 0.25f;
                const auto bits = std::bit_cast<std::uint32_t>(value);
                const auto half = static_cast<std::uint16_t>(bits == 0u ? 0u : ((bits >> 13u) & 0x3ffu) | ((((bits >> 23u) & 255u) - 112u) << 10u));
                std::memcpy(linear.data() + element * bytes + component * 2u, &half, 2u);
                values[component] = bits;
            }
        } else {
            for (std::uint32_t component = 0u; component < std::max(1u, bytes / 4u); ++component) {
                const auto value = (element * 13u + component * 7u + 1u) & (bytes == 1u ? 255u : bytes == 2u ? 65535u : 0xffffffffu);
                std::memcpy(linear.data() + element * bytes + component * 4u, &value, std::min(4u, bytes));
                values[component] = value;
            }
        }
    }
    address.Tile(linear, std::span(guest).subspan(16u, address.Bytes()));
    const auto saved = guest;
    std::unique_ptr<Recorder> recorder;
    if (recorded) { recorder = std::make_unique<Recorder>(context); recorder->Activate(); }
    const Texture texture(context, detiler, descriptor, {}, std::span(guest).subspan(16u, address.Bytes()));
    if (recorder != nullptr) { recorder->Sync(); recorder.reset(); }
    Require(texture.SampledViewRange(false).levels == 1u && texture.SampledViewRange(false).layers == layers - baseArray, "MS texture view range differs");
    Require(!arrayed || texture.FirstLayerView() != VK_NULL_HANDLE, "MS array texture has no first-layer view");
    const auto fetched = width * height * fragments * (firstLayer ? 1u : layers - baseArray);
    const auto first = width * height * fragments * baseArray;
    Buffer output(context, (fetched + 2u) * 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(output.Bytes().data(), 0xa5, output.Bytes().size());
    const Fetch fetch(context, texture, output, FetchShader(width, height, fragments, arrayed && !firstLayer, floating), firstLayer);
    CommandBatch batch(context);
    fetch.Run(batch.Handle(), fetched);
    const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(batch.Handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0u, 1u, &barrier, 0u, nullptr, 0u, nullptr);
    batch.SubmitAndWait();
    output.Invalidate();
    Require(std::memcmp(output.Bytes().data() + 16u, expected.data() + first, fetched * 16u) == 0, "runtime MS texture fetch lost fragment, layer or element values");
    Require(guest == saved, "runtime MS texture upload changed source or guards");
    Require(std::all_of(output.Bytes().begin(), output.Bytes().begin() + 16u, [](auto byte) { return byte == std::byte{0xa5}; }) && std::all_of(output.Bytes().begin() + (fetched + 1u) * 16u, output.Bytes().begin() + output.Bytes().size(), [](auto byte) { return byte == std::byte{0xa5}; }), "runtime MS texture fetch changed output guards");
    std::printf("format=%u fragments=%u arrayed=%u chunks=%u base=%u firstLayer=%u recorded=%u elements=%u exact values and guards\n", format, fragments, arrayed, chunks, baseArray, firstLayer, recorded, fetched);
}


void CheckCache(Context context, std::uint32_t fragments) {
    TextureDetiler detiler(context);
    context.detiler = &detiler;
    constexpr auto width = 5u, height = 3u, layers = 2u;
    const auto count = width * height * layers * fragments;
    const MultisampleTextureLayout address(TextureTileMode::kR64KBX, 4u, fragments, width, height, layers, 0x8100u);
    std::vector<std::byte> allocation(address.Bytes() + 65536u);
    const auto base = (reinterpret_cast<std::uintptr_t>(allocation.data()) + 65535u) & ~std::uintptr_t{65535u};
    auto guest = std::span(reinterpret_cast<std::byte*>(base), address.Bytes());
    std::vector<std::array<std::uint32_t, 4>> expected(count);
    std::vector<std::uint32_t> linear(count);
    const auto base40 = (base | 0x8100u) >> 8u;
    const auto logFragments = static_cast<std::uint32_t>(std::countr_zero(fragments));
    const std::array<std::uint32_t, 8> words{static_cast<std::uint32_t>(base40), static_cast<std::uint32_t>((base40 >> 32u) & 255u) | (20u << 20u) | ((width - 1u) & 3u) << 30u, ((width - 1u) >> 2u) | ((height - 1u) << 14u), (15u << 28u) | (27u << 20u) | (logFragments << 16u) | 0xfacu, layers - 1u, logFragments << 4u, 0u, 0u};
    std::shared_ptr<Texture> previous;
    for (std::uint32_t generation = 0u; generation < 2u; ++generation) {
        for (std::uint32_t index = 0u; index < count; ++index) {
            linear[index] = index * 13u + generation * 107u + 1u;
            expected[index] = {linear[index], 0u, 0u, 1u};
        }
        address.Tile(std::as_bytes(std::span(linear)), guest);
        AgcDriver::GuestMemory::MarkWritten(base, guest.size());
        const auto texture = CachedSampledTexture(context, words);
        Require(texture != previous && CachedSampledTexture(context, words) == texture, "MS sampled cache does not replace changed bytes or reuse unchanged bytes");
        Buffer output(context, (count + 2u) * 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memset(output.Bytes().data(), 0xa5, output.Bytes().size());
        const Fetch fetch(context, *texture, output, FetchShader(width, height, fragments, true, false));
        CommandBatch batch(context);
        fetch.Run(batch.Handle(), count);
        const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(batch.Handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0u, 1u, &barrier, 0u, nullptr, 0u, nullptr);
        batch.SubmitAndWait();
        output.Invalidate();
        Require(std::memcmp(output.Bytes().data() + 16u, expected.data(), expected.size() * 16u) == 0, "MS sampled cache returned stale fragments or layers");
        previous = texture;
    }
    previous.reset();
    ClearCachedTextures(context.device);
    std::printf("fragments=%u sampled cache hit and refresh fetch exact values\n", fragments);
}

void CheckRefusals(Context context) {
    TextureDetiler detiler(context);
    GuestTextureResource descriptor{};
    descriptor.width = 5u;
    descriptor.height = 3u;
    descriptor.mipCount = descriptor.allocatedMipCount = 1u;
    descriptor.tileMode = TextureTileMode::kR64KBX;
    descriptor.dimension = TextureDimension::k2DMsaa;
    descriptor.format = 20u;
    descriptor.fragments = 8u;
    const MultisampleTextureLayout address(descriptor.tileMode, 4u, 8u, 5u, 3u, 1u);
    const std::vector<std::byte> snapshot(address.Bytes());
    const auto reject = [&](const Context& candidateContext, const GuestTextureResource& candidate, bool depth, const char* reason) {
        try {
            const Texture texture(candidateContext, detiler, candidate, {}, snapshot, depth);
        } catch (const std::runtime_error& error) {
            Require(std::string(error.what()).find(reason) != std::string::npos, std::string("MS refusal expected ") + reason + ": " + error.what());
            return;
        }
        throw std::runtime_error("unsupported MS texture was accepted");
    };
    auto candidate = descriptor;
    candidate.dccAddress = 0x10000u;
    reject(context, candidate, false, "compressed multisampled");
    candidate = descriptor;
    candidate.tileMode = TextureTileMode::kZ64KBX;
    reject(context, candidate, false, "multisampled depth");
    reject(context, descriptor, true, "multisampled comparison");
    auto disabled = context;
    disabled.storageImageMultisample = false;
    reject(disabled, descriptor, false, "does not enable multisampled");
    disabled = context;
    disabled.storageImageWriteWithoutFormat = false;
    reject(disabled, descriptor, false, "does not enable multisampled");
    bool rejected = false;
    try { const StorageTexture storage(context, detiler, descriptor, 0u); }
    catch (const std::runtime_error& error) { rejected = std::string(error.what()).find("multisampled storage texture writes") != std::string::npos; }
    Require(rejected, "multisampled storage texture writes were not refused");
    std::puts("compressed/depth/storage MS paths and disabled upload features refused");
}

void CheckShapes() {
    using namespace ShaderRecompiler;
    for (const auto dimension : {RdnaImageDimension::Dim2DMsaa, RdnaImageDimension::Dim2DMsaaArray}) {
        for (const auto resourceClass : {ImageResourceClass::Sampled, ImageResourceClass::Storage}) {
            ImageResource image;
            image.resourceClass = resourceClass;
            image.numericClass = IrTextureNumericClass::Float;
            image.dimension = dimension;
            image.read = true;
            image.written = resourceClass == ImageResourceClass::Storage;
            ShaderInfo info;
            info.images.push_back(image);
            ResourceMaterializer::PrepareImageModes(info);
            DescriptorValue descriptor{{1u, 71u << 20u, 0u, (dimension == RdnaImageDimension::Dim2DMsaa ? 14u : 15u) << 28u | (27u << 20u) | (3u << 16u) | 0xfacu, 0u, 3u << 4u, 0u, 0u}, 8u};
            ResourceSnapshot snapshot;
            snapshot.images.push_back(descriptor);
            const auto mode = ResourceMaterializer::RuntimeImageMode(image, descriptor, info.runtimeImageModes.at(0));
            IrBindingLayout layout;
            layout.runtimeImageCount = 1u;
            layout.descriptors.push_back({DescriptorBindingForImage(info.runtimeImageModes.at(0).at(mode)), {0u}});
            const std::array<std::uint8_t, 8> exports{};
            const auto bindings = DescriptorBindingBuilder{}.Prepare(layout, info, IrShaderStage::Pixel, snapshot, exports);
            Require(bindings.bindings.size() == 1u, "MS shape builder returned an unexpected binding count");
            Require(bindings.bindings.front().descriptor.imageShape == (dimension == RdnaImageDimension::Dim2DMsaa ? DescriptorImageShape::Image2DMsaa : DescriptorImageShape::Image2DMsaaArray), "MS physical descriptor shape differs");
        }
    }
    std::puts("MS sampled/storage 2D/array physical shapes preserved");
}

}

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string(argv[1]) == "--shapes") { CheckShapes(); return 0; }
        Device device;
        const std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        for (const auto format : {5u, 11u, 20u, 62u, 75u, 71u}) {
            for (const auto fragments : {2u, 4u, 8u}) {
                for (const bool arrayed : {false, true}) CheckTexture(device.GetContext(), format, fragments, arrayed, false);
            }
        }
        CheckTexture(device.GetContext(), 71u, 8u, true, true);
        for (const auto fragments : {2u, 4u, 8u}) {
            CheckCache(device.GetContext(), fragments);
            CheckTexture(device.GetContext(), 71u, fragments, true, false, 1u);
            CheckTexture(device.GetContext(), 71u, fragments, true, false, 1u, true);
            CheckTexture(device.GetContext(), 71u, fragments, true, false, 0u, false, true);
        }
        CheckRefusals(device.GetContext());
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
