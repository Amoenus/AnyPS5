#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTextureLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTarget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleDcc.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleInitialContents.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
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
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

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
            constexpr auto queueFlags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT;
            while (family < count && (families[family].queueFlags & queueFlags) != queueFlags) ++family;
            Require(family < count, "no graphics and compute queue");
            VkPhysicalDeviceFeatures available{};
            function<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(context.physical, &available);
            Require(available.shaderStorageImageMultisample && available.shaderStorageImageWriteWithoutFormat, "device lacks multisampled image upload features");
            VkPhysicalDeviceFeatures enabled{};
            enabled.shaderStorageImageMultisample = VK_TRUE;
            enabled.shaderStorageImageWriteWithoutFormat = VK_TRUE;
            enabled.shaderStorageImageReadWithoutFormat = available.shaderStorageImageReadWithoutFormat;
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
            context.storageImageReadWithoutFormat = enabled.shaderStorageImageReadWithoutFormat == VK_TRUE;
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
            ClearMultisampleTargets(context.device);
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

class TargetMemory {
public:
    explicit TargetMemory(std::size_t bytes) : bytes(bytes) {
#ifdef _WIN32
        pointer = GuestArena::GuestArenaAllocate_nid_postfix(bytes, 65536u);
        GuestArena::GuestArenaCommit_nid_postfix(pointer, bytes, PAGE_READWRITE, bytes);
#else
        void* raw = mmap(nullptr, bytes + 65536u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Require(raw != MAP_FAILED, "cannot allocate multisample target fixture");
        const auto begin = reinterpret_cast<std::uintptr_t>(raw);
        const auto aligned = (begin + 65535u) & ~std::uintptr_t{65535u};
        if (aligned != begin) munmap(raw, aligned - begin);
        if (aligned + bytes != begin + bytes + 65536u) munmap(reinterpret_cast<void*>(aligned + bytes), begin + 65536u - aligned);
        pointer = reinterpret_cast<void*>(aligned);
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(pointer, bytes);
#endif
        Require(pointer != nullptr, "multisample target fixture allocation failed");
        std::memset(pointer, 0xa5, bytes);
    }
    ~TargetMemory() {
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(pointer, bytes);
        GuestArena::GuestArenaRelease_nid_postfix(pointer, bytes);
#else
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(pointer, bytes);
        munmap(pointer, bytes);
#endif
    }
    std::byte* Data() const { return static_cast<std::byte*>(pointer); }
    std::uint64_t Address() const { return reinterpret_cast<std::uint64_t>(pointer); }
private:
    void* pointer = nullptr;
    std::size_t bytes;
};

template<typename Action>
void TargetRefuses(Action action, std::string_view expected) {
    std::string message;
    try { action(); }
    catch (const std::exception& error) { message = error.what(); }
    Require(message.find(expected) != std::string::npos, "multisample target refusal did not match " + std::string(expected) + ": " + message);
}

bool TargetSupported(const Context& context, VkFormat format, std::uint32_t fragments) {
    VkImageFormatProperties properties{};
    const auto status = context.imageFormatProperties(context.physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT, &properties);
    return status == VK_SUCCESS && (properties.sampleCounts & fragments) != 0u && (context.limits.framebufferColorSampleCounts & fragments) != 0u;
}

void QueueTargetByteWrite(const Context& context, Recorder& recorder, std::uint64_t address, std::byte value) {
    auto buffer = std::make_shared<Buffer>(context, 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto commands = recorder.Commands();
    const auto byte = std::to_integer<std::uint32_t>(value);
    context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, buffer->Handle(), 0u, 4u, byte * 0x01010101u);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    recorder.OnComplete([buffer, address] {
        buffer->Invalidate();
        AgcDriver::GuestMemory::StoreOwnBytes(address, buffer->Bytes().first(1u));
    });
    recorder.Keep(buffer);
    recorder.NotePendingWrite(address, 1u);
}

void ClearTargetAttachment(const Context& context, VkImageView view, const ColorTarget& target) {
    VkAttachmentDescription attachment{};
    attachment.format = target.format;
    attachment.samples = static_cast<VkSampleCountFlagBits>(target.samples);
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    const VkAttachmentReference reference{0u, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1u;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1u;
    info.pAttachments = &attachment;
    info.subpassCount = 1u;
    info.pSubpasses = &subpass;
    VkRenderPass pass = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateRenderPass>("vkCreateRenderPass")(context.device, &info, nullptr, &pass), "vkCreateRenderPass target fixture");
    try {
        const std::array views{view};
        Framebuffer framebuffer(context, pass, views, target.extent);
        CommandBatch batch(context);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        VkClearValue clear{};
        clear.color.uint32[0] = 0x11u;
        clear.color.uint32[1] = 0x22u;
        clear.color.uint32[2] = 0x33u;
        clear.color.uint32[3] = 0x44u;
        if (target.format == VK_FORMAT_R8G8B8A8_UNORM || target.format == VK_FORMAT_R8G8B8A8_SRGB) {
            clear.color.float32[0] = clear.color.float32[2] = 0.0f;
            clear.color.float32[1] = clear.color.float32[3] = 1.0f;
        }
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = pass;
        begin.framebuffer = framebuffer.Handle();
        begin.renderArea = {{0, 0}, target.extent};
        begin.clearValueCount = 1u;
        begin.pClearValues = &clear;
        context.Function<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(batch.Handle(), &begin, VK_SUBPASS_CONTENTS_INLINE);
        context.Function<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(batch.Handle());
        batch.SubmitAndWait();
        MarkMultisampleTargetDirty(context, target);
    } catch (...) {
        context.Function<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(context.device, pass, nullptr);
        throw;
    }
    context.Function<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(context.device, pass, nullptr);
}

void CheckPublishedSampledRead(const Context& context, Recorder& recorder, const ColorTarget& target, std::span<const std::byte> samples);

VkImageView CheckTargetPublication(const Context& context, Recorder& recorder, const ColorTarget& target, std::span<const std::byte> samples) {
    const MultisampleTextureLayout layout(TextureTileMode::kR64KBX, target.elementBytes, target.samples, target.extent.width, target.extent.height, 1u, target.pipeBankXor);
    const auto* color = reinterpret_cast<const std::byte*>(target.address);
    std::vector<std::byte> physical(color, color + target.bytes);
    for (std::uint32_t y = 0; y < target.extent.height; ++y) {
        for (std::uint32_t x = 0; x < target.extent.width; ++x) {
            for (std::uint32_t sample = 0; sample < target.samples; ++sample) {
                const auto linear = ((static_cast<std::size_t>(y) * target.extent.width + x) * target.samples + sample) * target.elementBytes;
                std::memcpy(physical.data() + layout.Offset(x, y, 0u, sample), samples.data() + linear, target.elementBytes);
            }
        }
    }
    std::array<std::byte, 1> unrelated{std::byte{0x5a}}, observed{};
    MarkMultisampleTargetDirty(context, target);
    recorder.Commands();
    recorder.OnComplete([&] { AgcDriver::GuestMemory::Read(reinterpret_cast<std::uint64_t>(unrelated.data()), observed); });
    std::array<std::byte, 1> probe{};
    const auto offset = layout.Offset(target.extent.width - 1u, 0u, 0u, target.samples - 1u);
    AgcDriver::GuestMemory::Read(target.address + offset, probe);
    Require(observed == unrelated, "multisample publication blocked an unrelated completion read");
    Require(probe.front() == physical[offset], "partial CPU read did not publish the newest physical fragment");
    Require(std::equal(physical.begin(), physical.end(), color), "multisample guest publication lost a sample, XOR addressing or tile padding");
    if (target.dccAddress != 0u) {
        const auto* dcc = reinterpret_cast<const std::byte*>(target.dccAddress);
        const auto bytes = MultisampleDccBytes_nid_no_patch(target.elementBytes, target.samples, target.extent.width, target.extent.height, 1u);
        Require(std::all_of(dcc, dcc + bytes, [](auto byte) { return byte == std::byte{0xff}; }), "multisample publication left compressed DCC");
    }
    if (target.cmaskAddress != 0u) {
        const auto* cmask = reinterpret_cast<const std::byte*>(target.cmaskAddress);
        const auto code = target.samples == 2u ? std::byte{0xdd} : target.samples == 4u ? std::byte{0xee} : std::byte{0xff};
        Require(std::all_of(cmask, cmask + target.cmaskBytes, [&](auto byte) { return byte == code; }), "multisample publication left CMASK compressed or mapped to fragment zero");
    }
    if (target.fmaskAddress != 0u) {
        const auto* fmask = reinterpret_cast<const std::byte*>(target.fmaskAddress & ~std::uint64_t{65535u});
        const auto bytes = MultisampleFmaskBytes(target);
        const auto pattern = target.samples == 2u ? 0x02020202u : target.samples == 4u ? 0xe4e4e4e4u : 0x76543210u;
        for (std::size_t byte = 0; byte < bytes; ++byte) Require(fmask[byte] == static_cast<std::byte>((pattern >> (byte % 4u * 8u)) & 0xffu), "multisample publication did not expand the full FMASK identity mapping");
    }
    Require(ReadMultisampleTargetSamples(context, target) == std::vector<std::byte>(samples.begin(), samples.end()), "publication changed resident GPU samples");
    ClearMultisampleTargets(context.device);
    const auto view = MultisampleTargetView(context, target);
    Require(ReadMultisampleTargetSamples(context, target) == std::vector<std::byte>(samples.begin(), samples.end()), "reimport of published multisample contents lost samples or metadata");
    AgcDriver::GuestMemory::FlushGpuWrites(target.address, target.bytes);
    return view;
}

void CheckTargetImport(Context context, std::uint32_t bytes, std::uint32_t fragments, VkFormat format, bool metadata) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const MultisampleTextureLayout layout(TextureTileMode::kR64KBX, bytes, fragments, 65u, 5u, 1u, 0x8c00u);
    TargetMemory memory(static_cast<std::size_t>(layout.Bytes()) + 3u * 65536u);
    auto detiler = std::make_unique<TextureDetiler>(context);
    context.detiler = detiler.get();
    ColorTarget target{};
    target.address = target.surfaceAddress = memory.Address();
    target.extent = target.surfaceExtent = {65u, 5u};
    target.format = format;
    target.bytes = layout.Bytes();
    target.tileMode = ColorTileMode::RenderTarget;
    target.elementBytes = bytes;
    target.samples = fragments;
    target.pipeBankXor = 0x8c00u;
    std::vector<std::byte> expected(layout.LinearBytes());
    for (std::uint32_t y = 0; y < 5u; ++y) {
        for (std::uint32_t x = 0; x < 65u; ++x) {
            for (std::uint32_t fragment = 0; fragment < fragments; ++fragment) {
                for (std::uint32_t byte = 0; byte < bytes; ++byte) {
                    const auto value = static_cast<std::byte>((x * 13u + y * 31u + fragment * 53u + byte * 7u) & 0xffu);
                    memory.Data()[layout.Offset(x, y, 0u, fragment) + byte] = value;
                    expected[((y * 65u + x) * fragments + fragment) * bytes + byte] = value;
                }
            }
        }
    }
    Recorder recorder(context);
    recorder.Activate();
    struct Cleanup {
        Recorder& recorder;
        VkDevice device;
        ~Cleanup() { recorder.Sync(); ClearMultisampleTargets(device); }
    } cleanup{recorder, context.device};
    if (metadata) {
        target.dccAddress = target.address + target.bytes;
        target.cmaskAddress = target.dccAddress + 65536u;
        target.fmaskAddress = target.cmaskAddress + 65536u;
        target.fmaskSwizzle = 0x18u;
        target.dccPipeAligned = true;
        target.dccAlphaOnMsb = true;
        target.dccControl = 0x100038u;
        target.cmaskBytes = CmaskBytes(65u, 5u);
        target.cmaskFastClear = true;
        target.clearWords = {0xffffffffu, 0xffffffffu};
        const auto dccBytes = MultisampleDccBytes_nid_no_patch(bytes, fragments, 65u, 5u, 1u);
        std::memset(reinterpret_cast<void*>(target.dccAddress), 0u, dccBytes);
        std::memset(reinterpret_cast<void*>(target.cmaskAddress), 0xccu, target.cmaskBytes);
        const std::array<std::uint32_t, 4> codes{0u, 0x40u, 0x80u, 0xc0u};
        for (const auto code : codes) {
            ClearMultisampleTargets(context.device);
            std::memset(reinterpret_cast<void*>(target.dccAddress), code, dccBytes);
            const auto view = MultisampleTargetView(context, target);
            Require(view != VK_NULL_HANDLE, "DCC target view is absent");
            const auto actual = ReadMultisampleTargetSamples(context, target);
            Require(actual.size() == expected.size(), "DCC target sample size changed");
            for (std::size_t offset = 0; offset < actual.size(); offset += 8u) {
                for (std::uint32_t channel = 0; channel < 4u; ++channel) {
                    std::uint16_t value = 0;
                    std::memcpy(&value, actual.data() + offset + channel * 2u, 2u);
                    const bool set = channel == 3u ? code == 0x40u || code == 0xc0u : code == 0x80u || code == 0xc0u;
                    Require(value == (set ? 0x3c00u : 0u), "DCC target GPU import used clear registers, raw color or an incorrect channel");
                }
            }
            if (AgcDriver::GuestMemory::Watched(target.address, target.bytes)) {
                CheckPublishedSampledRead(context, recorder, target, actual);
                CheckTargetPublication(context, recorder, target, actual);
            }
        }
        const bool watched = AgcDriver::GuestMemory::Watched(target.address, target.bytes);
        Require(watched || std::getenv("ANYPS5_REQUIRE_TARGET_WATCHED") == nullptr, "watched metadata reuse was required but unavailable");
        if (watched) {
            TargetRefuses([&] { *reinterpret_cast<std::byte*>(target.dccAddress) = std::byte{0}; MultisampleTargetView(context, target); }, "guest writes");
            for (const auto address : {target.dccAddress, target.cmaskAddress, target.fmaskAddress + 65535u}) {
                ClearMultisampleTargets(context.device);
                std::memset(reinterpret_cast<void*>(target.dccAddress), 0xc0u, dccBytes);
                MultisampleTargetView(context, target);
                ReadMultisampleTargetSamples(context, target);
                const auto original = *reinterpret_cast<std::byte*>(address);
                *reinterpret_cast<volatile std::byte*>(address) = original;
                TargetRefuses([&] { MultisampleTargetView(context, target); }, "guest writes");
                ClearMultisampleTargets(context.device);
                MultisampleTargetView(context, target);
                ReadMultisampleTargetSamples(context, target);
                NoteColorMetadataFill(address, 1u, 0xc0c0c0c0u);
                TargetRefuses([&] { MultisampleTargetView(context, target); }, "unimplemented residency transition");
            }
        }
        ClearMultisampleTargets(context.device);
        target.dccAddress = 0u;
        layout.Tile(expected, std::span(memory.Data(), static_cast<std::size_t>(target.bytes)));
        std::memset(reinterpret_cast<void*>(target.cmaskAddress), 0xccu, target.cmaskBytes);
        MultisampleTargetView(context, target);
        auto actual = ReadMultisampleTargetSamples(context, target);
        for (std::size_t offset = 0; offset < expected.size(); offset += bytes * fragments) {
            for (std::uint32_t fragment = 0; fragment < fragments; ++fragment) {
                Require(std::equal(expected.begin() + offset, expected.begin() + offset + bytes, actual.begin() + offset + fragment * bytes), "CMASK CC GPU import did not replicate physical fragment zero");
            }
        }
        if (watched) CheckTargetPublication(context, recorder, target, actual);
        ClearMultisampleTargets(context.device);
        std::memset(reinterpret_cast<void*>(target.cmaskAddress), 0u, target.cmaskBytes);
        target.clearWords = {0x3c000000u, 0x38003400u};
        MultisampleTargetView(context, target);
        actual = ReadMultisampleTargetSamples(context, target);
        const std::array<std::uint16_t, 4> channels{0u, 0x3c00u, 0x3400u, 0x3800u};
        for (std::size_t offset = 0; offset < actual.size(); offset += bytes) {
            Require(std::memcmp(actual.data() + offset, channels.data(), bytes) == 0, "CMASK register clear GPU import lost exact channels");
        }
        if (watched) {
            CheckPublishedSampledRead(context, recorder, target, actual);
            CheckTargetPublication(context, recorder, target, actual);
        }
        ClearMultisampleTargets(context.device);
        target.fmaskSwizzle = 0u;
        TargetRefuses([&] { MultisampleTargetView(context, target); }, "FMASK footprint requires");
        return;
    }
    const auto producedPhysical = layout.Offset(64u, 0u, 0u, fragments - 1u);
    const auto producedLinear = static_cast<std::size_t>(64u * fragments + fragments - 1u) * bytes;
    QueueTargetByteWrite(context, recorder, target.address + producedPhysical, std::byte{0x5a});
    expected[producedLinear] = std::byte{0x5a};
    Require(recorder.PendingWriteOverlaps(target.address + producedPhysical, 1u), "initial target fixture did not queue a producer");
    auto view = MultisampleTargetView(context, target);
    Require(!recorder.PendingWriteOverlaps(target.address + producedPhysical, 1u), "initial target import did not wait for its producer");
    Require(ReadMultisampleTargetSamples(context, target) == expected, "raw target import lost physical fragments, components or tile addressing");
    if (AgcDriver::GuestMemory::Watched(target.address, target.bytes)) view = CheckTargetPublication(context, recorder, target, expected);
    ClearTargetAttachment(context, view, target);
    const auto rendered = ReadMultisampleTargetSamples(context, target);
    const std::array<std::uint32_t, 4> clear{0x11u, 0x22u, 0x33u, 0x44u};
    for (std::size_t offset = 0; offset < rendered.size(); offset += bytes) {
        for (std::uint32_t byte = 0; byte < bytes; ++byte) {
            if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB) {
                Require(rendered[offset + byte] == static_cast<std::byte>(byte % 2u == 0u ? 0u : 255u), "normalized attachment clear and sample readback disagree");
                continue;
            }
            const auto channel = bytes < 4u ? 0u : byte / 4u;
            const auto shift = bytes < 4u ? byte * 8u : byte % 4u * 8u;
            Require(rendered[offset + byte] == static_cast<std::byte>((clear[channel] >> shift) & 0xffu), "the target's attachment view and sample-preserving readback disagree");
        }
    }
    if (AgcDriver::GuestMemory::Watched(target.address, target.bytes)) {
        Require(MultisampleTargetView(context, target) == view && ReadMultisampleTargetSamples(context, target) == rendered, "resident reuse changed the image or reapplied its initial guest contents");
        if (format == VK_FORMAT_R32_UINT) CheckPublishedSampledRead(context, recorder, target, rendered);
        view = CheckTargetPublication(context, recorder, target, rendered);
        auto alias = target;
        alias.pipeBankXor ^= 0x400u;
        TargetRefuses([&] { MultisampleTargetView(context, alias); }, "layout or metadata changed");
        memory.Data()[layout.Offset(64u, 0u, 0u, fragments - 1u)] ^= std::byte{1};
        TargetRefuses([&] { MultisampleTargetView(context, target); }, "guest writes");
        std::array<std::byte, 1> conflict{};
        TargetRefuses([&] { AgcDriver::GuestMemory::Read(target.address, conflict); }, "guest writes");
        ClearMultisampleTargets(context.device);
        MultisampleTargetView(context, target);
        ReadMultisampleTargetSamples(context, target);
        recorder.Commands();
        recorder.NotePendingWrite(target.address, 4u);
        TargetRefuses([&] { MultisampleTargetView(context, target); }, "external GPU writes");
        TargetRefuses([&] { AgcDriver::GuestMemory::Read(target.address, conflict); }, "external GPU writes");
        recorder.Sync();
        NoteColorMetadataFill(target.address, target.bytes, 0xa5a5a5a5u);
        TargetRefuses([&] { MultisampleTargetView(context, target); }, "unimplemented residency transition");
    } else {
        Require(std::getenv("ANYPS5_REQUIRE_TARGET_WATCHED") == nullptr, "watched target reuse was required but unavailable");
        TargetRefuses([&] { MultisampleTargetView(context, target); }, "watched guest memory");
    }
    std::array<std::byte, 1> probe{};
    TargetRefuses([&] { AgcDriver::GuestMemory::Read(target.address + layout.Offset(64u, 0u, 0u, fragments - 1u), probe); }, AgcDriver::GuestMemory::Watched(target.address, target.bytes) ? "unimplemented residency transition" : "watched guest memory");
}

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

void CheckPublishedSampledRead(const Context& context, Recorder& recorder, const ColorTarget& target, std::span<const std::byte> samples) {
    const bool floating = target.format == VK_FORMAT_R16G16B16A16_SFLOAT;
    Require(floating || target.format == VK_FORMAT_R32_UINT, "published sampled fixture format is unsupported");
    const auto format = floating ? 71u : 20u;
    const auto base = (target.address | target.pipeBankXor) >> 8u;
    const auto metadata = target.dccAddress >> 8u;
    const auto logFragments = static_cast<std::uint32_t>(std::countr_zero(target.samples));
    const auto width = target.extent.width, height = target.extent.height;
    const auto flags = target.dccAddress == 0u ? 0u : (1u << 21u) | (static_cast<std::uint32_t>(target.dccAlphaOnMsb) << 22u) | (static_cast<std::uint32_t>(target.dccPipeAligned) << 19u);
    const std::array<std::uint32_t, 8> words{static_cast<std::uint32_t>(base), static_cast<std::uint32_t>((base >> 32u) & 255u) | (format << 20u) | (((width - 1u) & 3u) << 30u), ((width - 1u) >> 2u) | ((height - 1u) << 14u), (14u << 28u) | (27u << 20u) | (logFragments << 16u) | 0xfacu, 0u, logFragments << 4u, flags | (static_cast<std::uint32_t>(metadata & 255u) << 24u), static_cast<std::uint32_t>(metadata >> 8u)};
    const auto texture = CachedSampledTexture(context, words);
    recorder.Sync();
    const auto count = width * height * target.samples;
    std::vector<std::array<std::uint32_t, 4>> expected(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto& value = expected[index];
        if (floating) {
            for (std::uint32_t component = 0; component < 4u; ++component) {
                std::uint16_t half = 0u;
                std::memcpy(&half, samples.data() + index * 8u + component * 2u, 2u);
                Require(half == 0u || (half >= 0x0400u && half < 0x7c00u), "published sampled fixture needs nonnegative normal half values");
                value[component] = half == 0u ? 0u : ((static_cast<std::uint32_t>(half & 0x7c00u) + 0x1c000u) << 13u) | (static_cast<std::uint32_t>(half & 0x3ffu) << 13u);
            }
        } else {
            std::memcpy(&value[0], samples.data() + index * 4u, 4u);
            value[3] = 1u;
        }
    }
    Buffer output(context, (count + 2u) * 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(output.Bytes().data(), 0xa5, output.Bytes().size());
    const Fetch fetch(context, *texture, output, FetchShader(width, height, target.samples, false, floating));
    CommandBatch batch(context);
    fetch.Run(batch.Handle(), count);
    RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    batch.SubmitAndWait();
    output.Invalidate();
    Require(std::memcmp(output.Bytes().data() + 16u, expected.data(), expected.size() * 16u) == 0, "sampled reader returned stale resident samples or compressed metadata");
    Require(std::all_of(output.Bytes().begin(), output.Bytes().begin() + 16u, [](auto byte) { return byte == std::byte{0xa5}; }) && std::all_of(output.Bytes().end() - 16u, output.Bytes().end(), [](auto byte) { return byte == std::byte{0xa5}; }), "published sample fetch changed output guards");
    ClearCachedTextures(context.device);
}

void CheckTexture(Context context, std::uint32_t format, std::uint32_t fragments, bool arrayed, bool chunks, std::uint32_t baseArray = 0u, bool firstLayer = false, bool recorded = false, TextureTileMode tileMode = TextureTileMode::kR64KBX) {
    const auto width = chunks ? 65u : 5u;
    const auto height = chunks ? 63u : 3u;
    const auto layers = arrayed ? 2u : 1u;
    const auto bytes = BytesPerElement(format);
    const auto count = width * height * layers * fragments;
    const bool floating = format == 71u || format == 7u || format == 22u;
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
    descriptor.tileMode = tileMode;
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
        if (format == 7u || format == 22u) {
            const auto value = format == 7u ? static_cast<float>(element & 1u) : static_cast<float>((element * 13u) & 127u) / 128.0f;
            const auto bits = format == 7u ? (element & 1u) * 65535u : std::bit_cast<std::uint32_t>(value);
            std::memcpy(linear.data() + element * bytes, &bits, bytes);
            values[0] = std::bit_cast<std::uint32_t>(value);
        } else if (floating) {
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


bool CheckReadback(Context context, std::uint32_t bytes, std::uint32_t fragments, bool chunks) {
    constexpr auto width = 13u, height = 7u, layers = 3u;
    const auto count = width * height * layers * fragments;
    const auto format = TextureDetiler::ImageElementFormat(bytes);
    VkImageFormatProperties supported{};
    constexpr auto usage = VK_IMAGE_USAGE_STORAGE_BIT;
    const auto samples = static_cast<VkSampleCountFlagBits>(fragments);
    if (context.imageFormatProperties(context.physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0u, &supported) != VK_SUCCESS || (supported.sampleCounts & samples) == 0u) return false;
    context.limits.maxComputeWorkGroupCount[0] = 1u;
    TextureDetiler detiler(context);
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1u};
    info.mipLevels = 1u;
    info.arrayLayers = layers;
    info.samples = samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkImage image = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &image), "vkCreateImage readback test");
    OwnedImage owned(context, image, VK_NULL_HANDLE);
    VkMemoryRequirements requirements{};
    context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &owned.memory), "vkAllocateMemory readback test");
    Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, owned.memory, 0u), "vkBindImageMemory readback test");
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, layers};
    VkImageView view = VK_NULL_HANDLE;
    owned.transferViews.reserve(1u);
    Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView readback test");
    owned.transferViews.push_back(view);
    const auto align = std::max<std::uint64_t>(16u, context.limits.minStorageBufferOffsetAlignment);
    const auto first = chunks ? 3u : 0u;
    const auto selected = chunks ? count - 7u : count;
    const auto chunkElements = chunks ? 73u : selected;
    const auto chunkCount = (selected + chunkElements - 1u) / chunkElements;
    const auto stride = (static_cast<std::uint64_t>(chunkElements) * bytes + 16u + align - 1u) / align * align;
    Buffer input(context, align + count * bytes + 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer output(context, align + chunkCount * stride, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (bytes == 4u && fragments == 2u && !chunks) {
        CommandBatch validation(context);
        const auto reject = [&](auto&& call, const char* reason) {
            try { call(); }
            catch (const std::runtime_error& error) {
                Require(std::string(error.what()).find(reason) != std::string::npos, std::string("MS readback refusal differs: ") + error.what());
                return;
            }
            throw std::runtime_error("invalid MS readback was accepted");
        };
        auto disabled = context;
        disabled.storageImageReadWithoutFormat = false;
        TextureDetiler noReads(disabled);
        reject([&] { noReads.DispatchMultisampleReadback(validation.Handle(), bytes, output.Handle(), align, count * bytes, view, width, height, layers, fragments, 0u, count); }, "does not enable");
        disabled = context;
        disabled.storageImageMultisample = false;
        TextureDetiler noMultisample(disabled);
        reject([&] { noMultisample.DispatchMultisampleReadback(validation.Handle(), bytes, output.Handle(), align, count * bytes, view, width, height, layers, fragments, 0u, count); }, "does not enable");
        reject([&] { detiler.DispatchMultisampleReadback(VK_NULL_HANDLE, bytes, output.Handle(), align, count * bytes, view, width, height, layers, fragments, 0u, count); }, "requires commands");
        reject([&] { detiler.DispatchMultisampleReadback(validation.Handle(), 3u, output.Handle(), align, count * bytes, view, width, height, layers, fragments, 0u, count); }, "element size");
        reject([&] { detiler.DispatchMultisampleReadback(validation.Handle(), bytes, output.Handle(), align, count * bytes, view, 0u, height, layers, fragments, 0u, count); }, "extent");
        reject([&] { detiler.DispatchMultisampleReadback(validation.Handle(), bytes, output.Handle(), align, count * bytes, view, width, height, layers, 3u, 0u, count); }, "fragment count");
        reject([&] { detiler.DispatchMultisampleReadback(validation.Handle(), bytes, output.Handle(), align, count * bytes, view, width, height, layers, fragments, 1u, count); }, "element range");
        reject([&] { detiler.DispatchMultisampleReadback(validation.Handle(), bytes, output.Handle(), align + 1u, count * bytes, view, width, height, layers, fragments, 0u, count); }, "buffer range");
        reject([&] { detiler.DispatchMultisampleReadback(validation.Handle(), bytes, output.Handle(), align, count * bytes - 4u, view, width, height, layers, fragments, 0u, count); }, "buffer range");
        std::puts("MS readback missing features, commands, invalid elements/extent/fragments/ranges refused");
    }
    for (std::uint32_t round = 0u; round < 2u; ++round) {
        std::memset(input.Bytes().data(), 0xa5, input.Bytes().size());
        for (std::uint32_t index = 0u; index < count * bytes; ++index) input.Bytes()[align + index] = static_cast<std::byte>((index * 137u + (index >> 8u) * 29u + round * 71u) & 255u);
        const auto saved = std::vector(input.Bytes().begin(), input.Bytes().end());
        std::vector<std::byte> expected(output.Bytes().size(), std::byte{0xa5});
        std::memset(output.Bytes().data(), 0xa5, output.Bytes().size());
        detiler.BeginBatch();
        CommandBatch batch(context);
        VkImageMemoryBarrier transition{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        transition.srcAccessMask = round == 0u ? 0u : VK_ACCESS_SHADER_READ_BIT;
        transition.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        transition.oldLayout = round == 0u ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
        transition.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        transition.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        transition.image = image;
        transition.subresourceRange = viewInfo.subresourceRange;
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(batch.Handle(), round == 0u ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &transition);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        detiler.DispatchMultisampleImage(batch.Handle(), bytes, input.Handle(), align, (static_cast<std::uint64_t>(count) * bytes + 3u) & ~3ull, view, width, height, layers, fragments, 0u, count);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        for (std::uint32_t chunk = 0u; chunk < chunkCount; ++chunk) {
            const auto begin = chunk * chunkElements;
            const auto elements = std::min(chunkElements, selected - begin);
            const auto offset = align + chunk * stride;
            const auto range = (static_cast<std::uint64_t>(elements) * bytes + 3u) & ~3ull;
            detiler.DispatchMultisampleReadback(batch.Handle(), bytes, output.Handle(), offset, range, view, width, height, layers, fragments, first + begin, elements);
            std::memcpy(expected.data() + offset, input.Bytes().data() + align + (first + begin) * bytes, elements * bytes);
        }
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        batch.SubmitAndWait();
        output.Invalidate();
        Require(std::ranges::equal(output.Bytes(), expected), "MS readback lost element, sample, layer, chunk or guard bytes");
        Require(std::ranges::equal(input.Bytes(), saved), "MS readback altered its upload source");
    }
    std::printf("MS readback bytes=%u fragments=%u chunks=%u two uploads exact samples/layers/tails/guards\n", bytes, fragments, chunks);
    return true;
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


bool Supports(const Context& context, std::uint32_t format, std::uint32_t fragments) {
    VkImageFormatProperties properties{};
    const auto result = context.imageFormatProperties(context.physical, TextureDetiler::ImageElementFormat(BytesPerElement(format)), VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, &properties);
    if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) return false;
    Check(result, "MS test image format query");
    return (properties.sampleCounts & fragments) != 0u;
}

VKAPI_ATTR VkResult VKAPI_CALL SingleSampleProperties(VkPhysicalDevice, VkFormat, VkImageType, VkImageTiling, VkImageUsageFlags, VkImageCreateFlags, VkImageFormatProperties* properties) {
    *properties = {};
    properties->maxExtent = {16384u, 16384u, 1u};
    properties->maxArrayLayers = 8192u;
    properties->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
    return VK_SUCCESS;
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
    reject(context, candidate, false, "numeric Z depth");
    reject(context, descriptor, true, "multisampled comparison");
    auto disabled = context;
    disabled.storageImageMultisample = false;
    reject(disabled, descriptor, false, "does not enable multisampled");
    disabled = context;
    disabled.storageImageWriteWithoutFormat = false;
    reject(disabled, descriptor, false, "does not enable multisampled");
    disabled = context;
    disabled.imageFormatProperties = SingleSampleProperties;
    Require(!Supports(disabled, 20u, 2u), "MS test selected an unadvertised fragment count");
    reject(disabled, descriptor, false, "fragment count is unsupported by the device");
    bool rejected = false;
    try { const StorageTexture storage(context, detiler, descriptor, 0u); }
    catch (const std::runtime_error& error) { rejected = std::string(error.what()).find("multisampled storage texture writes") != std::string::npos; }
    Require(rejected, "multisampled storage texture writes were not refused");
    std::puts("compressed/unsupported depth/storage MS paths and disabled upload features refused");
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
        std::unique_ptr<Device> device;
        try { device = std::make_unique<Device>(); }
        catch (const std::exception& error) {
            if (std::getenv("ANYPS5_REQUIRE_VULKAN") != nullptr) throw;
            std::printf("skipped, no usable Vulkan MS upload device: %s\n", error.what());
            return 77;
        }
        const auto context = device->GetContext();
        const std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        std::uint32_t checked = 0u, unavailable = 0u;
        if (argc > 1 && std::string(argv[1]) == "--targets") {
            const std::array formats{VK_FORMAT_R8_UINT, VK_FORMAT_R16_UINT, VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32A32_UINT};
            for (std::uint32_t index = 0; index < formats.size(); ++index) {
                for (const auto fragments : {2u, 4u, 8u}) {
                    if (!TargetSupported(context, formats[index], fragments)) { ++unavailable; continue; }
                    CheckTargetImport(context, 1u << index, fragments, formats[index], false);
                    ++checked;
                }
            }
            for (const auto fragments : {2u, 4u, 8u}) {
                if (!TargetSupported(context, VK_FORMAT_R16G16B16A16_SFLOAT, fragments)) { ++unavailable; continue; }
                CheckTargetImport(context, 8u, fragments, VK_FORMAT_R16G16B16A16_SFLOAT, true);
                ++checked;
            }
            for (const auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB}) {
                for (const auto fragments : {2u, 4u, 8u}) {
                    if (!TargetSupported(context, format, fragments)) { ++unavailable; continue; }
                    CheckTargetImport(context, 4u, fragments, format, false);
                    ++checked;
                }
            }
            Require(checked != 0u, "device advertises no supported multisample target imports");
            Require(unavailable == 0u || std::getenv("ANYPS5_REQUIRE_MSAA_ALL") == nullptr, "all multisample target cases were required but some are unsupported");
            std::printf("MS target import checked=%u unavailable=%u\n", checked, unavailable);
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "--readback") {
            if (!context.storageImageReadWithoutFormat) {
                Require(std::getenv("ANYPS5_REQUIRE_VULKAN") == nullptr, "device lacks required formatless MS storage image reads");
                return 77;
            }
            for (const auto bytes : {1u, 2u, 4u, 8u, 16u}) {
                for (const auto fragments : {2u, 4u, 8u}) {
                    for (const bool chunks : {false, true}) {
                        if (CheckReadback(context, bytes, fragments, chunks)) ++checked;
                        else ++unavailable;
                    }
                }
            }
            Require(checked != 0u, "device advertises no supported MS readback cases");
            if (std::getenv("ANYPS5_REQUIRE_MSAA_ALL") != nullptr) Require(unavailable == 0u, "device did not exercise every required MS readback case");
            std::printf("MS readback checked=%u unavailable=%u\n", checked, unavailable);
            return 0;
        }
        for (const auto format : {5u, 11u, 20u, 62u, 75u, 71u}) {
            for (const auto fragments : {2u, 4u, 8u}) {
                if (!Supports(context, format, fragments)) {
                    std::printf("format=%u fragments=%u: device does not advertise sampled/storage support\n", format, fragments);
                    unavailable += 2u;
                    continue;
                }
                for (const bool arrayed : {false, true}) { CheckTexture(context, format, fragments, arrayed, false); ++checked; }
            }
        }
        if (Supports(context, 71u, 8u)) { CheckTexture(context, 71u, 8u, true, true); ++checked; }
        else ++unavailable;
        for (const auto fragments : {2u, 4u, 8u}) {
            if (Supports(context, 20u, fragments)) { CheckCache(context, fragments); ++checked; }
            else ++unavailable;
            if (Supports(context, 71u, fragments)) {
                CheckTexture(context, 71u, fragments, true, false, 1u);
                CheckTexture(context, 71u, fragments, true, false, 1u, true);
                CheckTexture(context, 71u, fragments, true, false, 0u, false, true);
                checked += 3u;
            } else unavailable += 3u;
        }
        for (const auto format : {7u, 22u}) {
            for (const auto fragments : {2u, 4u, 8u}) {
                if (!Supports(context, format, fragments)) { unavailable += 2u; continue; }
                for (const bool arrayed : {false, true}) {
                    CheckTexture(context, format, fragments, arrayed, false, arrayed ? 1u : 0u, false, arrayed, TextureTileMode::kZ64KBX);
                    ++checked;
                }
            }
        }
        Require(checked != 0u, "device advertises no supported MS test cases");
        if (std::getenv("ANYPS5_REQUIRE_MSAA_ALL") != nullptr) Require(unavailable == 0u, "device did not exercise every required MS case");
        CheckRefusals(context);
        std::printf("MS cases checked=%u unavailable=%u\n", checked, unavailable);
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
