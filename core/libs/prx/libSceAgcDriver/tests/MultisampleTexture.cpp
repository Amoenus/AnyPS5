#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTextureLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTarget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleHtile.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleDcc.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleInitialContents.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/BufferFill.hpp"
#include "Optimization/DescriptorBindingBuilder.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include "SpirvBackend/SpirvModule.hpp"
#include <SDL_loadso.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <utility>
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
    void PrintDevice() const {
        VkPhysicalDeviceProperties properties{};
        function<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(context.physical, &properties);
        std::printf("tile GPU vendor=0x%04x device=0x%04x name=%s\n", properties.vendorID, properties.deviceID, properties.deviceName);
    }
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
            ClearDepthSurfaces(context.device);
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

void ClearTargetAttachment(const Context& context, VkImageView view, const ColorTarget& target, const ColorTarget* transition = nullptr) {
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
        if (transition != nullptr) {
            Require(MultisampleTargetView(context, *transition) == view, "XOR transition invalidated a retained framebuffer attachment");
            if (auto* recorder = Recorder::Active(); recorder != nullptr && recorder->Device() == context.device) recorder->Sync();
        }
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
        MarkMultisampleTargetDirty(context, transition != nullptr ? *transition : target);
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
    if (target.dccAddress != 0u || target.cmaskAddress != 0u) {
        const auto snapshot = [](std::uint64_t address, std::size_t bytes) { return std::span(reinterpret_cast<const std::byte*>(address), address == 0u ? 0u : bytes); };
        const auto contents = DecodeMultisampleInitialContents(target, physical, snapshot(target.dccAddress, target.dccAddress == 0u ? 0u : MultisampleDccBytes_nid_no_patch(target.elementBytes, target.samples, target.extent.width, target.extent.height, 1u)), snapshot(target.cmaskAddress, target.cmaskBytes), snapshot(target.fmaskAddress & ~std::uint64_t{65535u}, target.fmaskAddress == 0u ? 0u : MultisampleFmaskBytes(target)), true);
        if (contents.clear) {
            for (std::size_t offset = 0u; offset < physical.size(); offset += target.elementBytes) std::memcpy(physical.data() + offset, samples.data(), target.elementBytes);
        } else {
            const MultisampleTextureLayout padded(TextureTileMode::kR64KBX, target.elementBytes, target.samples, layout.Pitch(), layout.PaddedHeight(), 1u, target.pipeBankXor);
            padded.Tile(contents.samples, physical);
        }
    }
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

void CheckTrackedFillProof(Context context) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const MultisampleTextureLayout layout(TextureTileMode::kR64KBX, 4u, 4u, 65u, 5u, 1u);
    TargetMemory memory(static_cast<std::size_t>(layout.Bytes()));
    TextureDetiler detiler(context);
    context.detiler = &detiler;
    ColorTarget target{};
    target.address = target.surfaceAddress = memory.Address();
    target.extent = target.surfaceExtent = {65u, 5u};
    target.format = VK_FORMAT_R32_UINT;
    target.bytes = layout.Bytes();
    target.tileMode = ColorTileMode::RenderTarget;
    target.elementBytes = 4u;
    target.samples = 4u;
    Recorder recorder(context);
    recorder.Activate();
    struct Cleanup {
        Recorder& recorder;
        VkDevice device;
        ~Cleanup() { recorder.Sync(); ClearMultisampleTargets(device); }
    } cleanup{recorder, context.device};
    for (const auto invalid : {0u, 1u, 2u, 3u}) {
        ClearMultisampleTargets(context.device);
        std::memset(memory.Data(), 0, target.bytes);
        MultisampleTargetView(context, target);
        ReadMultisampleTargetSamples(context, target);
        NoteColorMetadataFill(target.address, target.bytes, 0u);
        if (invalid == 1u) AgcDriver::GuestMemory::StoreOwnBytes(target.address, target.bytes - 1u, [&] { std::memset(memory.Data(), 0, target.bytes - 1u); });
        if (invalid == 2u) AgcDriver::GuestMemory::StoreOwnBytes(target.address, target.bytes, [&] { std::memset(memory.Data(), 0x5a, target.bytes); });
        if (invalid == 3u) std::memset(memory.Data(), 0, target.bytes);
        TargetRefuses([&] { MultisampleTargetView(context, target); }, "complete exact driver store");
        std::array<std::byte, 1> destination{std::byte{0x7f}};
        TargetRefuses([&] { AgcDriver::GuestMemory::Read(target.address, destination); }, "complete exact driver store");
        Require(destination[0] == std::byte{0x7f}, "an unproved tracked fill changed the refused read's destination");
    }
    for (const auto beforeNotification : {false, true}) {
        ClearMultisampleTargets(context.device);
        std::memset(memory.Data(), 0xa5, target.bytes);
        MultisampleTargetView(context, target);
        ReadMultisampleTargetSamples(context, target);
        const auto store = [&] { AgcDriver::GuestMemory::StoreOwnBytes(target.address, target.bytes, [&] { std::memset(memory.Data(), 0, target.bytes); }); };
        if (beforeNotification) store();
        NoteColorMetadataFill(target.address, target.bytes, 0u);
        if (!beforeNotification) store();
        MultisampleTargetView(context, target);
        const auto samples = ReadMultisampleTargetSamples(context, target);
        Require(std::all_of(samples.begin(), samples.end(), [](auto byte) { return byte == std::byte{0}; }), "a completed exact tracked fill did not preserve zero in every sample");
    }
    std::puts("tracked full fills require complete driver stores and exact bytes; bare, partial, wrong-pattern and CPU-only writes refused");
}

#ifdef _WIN32
void CheckUnwatchedTargetDiagnostic(Context context) {
    const MultisampleTextureLayout layout(TextureTileMode::kR64KBX, 4u, 4u, 64u, 64u, 1u, 0u);
    auto* memory = VirtualAlloc(nullptr, static_cast<std::size_t>(layout.Bytes()), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    Require(memory != nullptr, "cannot allocate an unwatched color fixture");
    auto detiler = std::make_unique<TextureDetiler>(context);
    context.detiler = detiler.get();
    Recorder recorder(context);
    recorder.Activate();
    struct Cleanup {
        Recorder& recorder;
        VkDevice device;
        void* memory;
        ~Cleanup() { recorder.Sync(); ClearMultisampleTargets(device); VirtualFree(memory, 0u, MEM_RELEASE); }
    } cleanup{recorder, context.device, memory};
    ColorTarget target{};
    target.address = target.surfaceAddress = reinterpret_cast<std::uintptr_t>(memory);
    target.extent = target.surfaceExtent = {64u, 64u};
    target.format = VK_FORMAT_R8G8B8A8_UNORM;
    target.bytes = layout.Bytes();
    target.tileMode = ColorTileMode::RenderTarget;
    target.elementBytes = 4u;
    target.samples = 4u;
    Require(!GuestArena::GuestArenaContains_nid_postfix(memory, static_cast<std::size_t>(layout.Bytes())) && !AgcDriver::GuestMemory::Watched(target.address, target.bytes), "the diagnostic fixture unexpectedly belongs to the watched arena");
    Require(MultisampleTargetView(context, target) != VK_NULL_HANDLE, "unwatched initial import has no native view");
    TargetRefuses([&] { MultisampleTargetView(context, target); }, "color: outside guest arena");
}
#endif

void CheckTargetImport(Context context, std::uint32_t bytes, std::uint32_t fragments, VkFormat format, bool metadata, bool chunks = false) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    if (chunks) context.limits.maxStorageBufferRange = std::min(context.limits.maxStorageBufferRange, 256u);
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
        alias.pipeBankXor ^= 0x1000u;
        TargetRefuses([&] { MultisampleTargetView(context, alias); }, "XOR tag");
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
        AgcDriver::GuestMemory::StoreOwnBytes(target.address, target.bytes, [&] { std::memset(memory.Data(), 0, target.bytes); });
        NoteColorMetadataFill(target.address, target.bytes, 0u);
        const auto filledView = MultisampleTargetView(context, target);
        const auto filled = ReadMultisampleTargetSamples(context, target);
        Require(filledView != VK_NULL_HANDLE && filled.size() == rendered.size() && std::all_of(filled.begin(), filled.end(), [](auto byte) { return byte == std::byte{0}; }), "tracked complete color fill lost exact zero samples");
        NoteColorMetadataFill(target.address + 1u, target.bytes - 1u, 0xa5a5a5a5u);
        TargetRefuses([&] { MultisampleTargetView(context, target); }, "unimplemented residency transition");
    } else {
        Require(std::getenv("ANYPS5_REQUIRE_TARGET_WATCHED") == nullptr, "watched target reuse was required but unavailable");
        TargetRefuses([&] { MultisampleTargetView(context, target); }, "watched guest memory");
    }
    std::array<std::byte, 1> probe{};
    TargetRefuses([&] { AgcDriver::GuestMemory::Read(target.address + layout.Offset(64u, 0u, 0u, fragments - 1u), probe); }, AgcDriver::GuestMemory::Watched(target.address, target.bytes) ? "unimplemented residency transition" : "watched guest memory");
}

void CheckTargetRetirement(Context context, bool removeAllocation, bool partialMetadata) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    constexpr std::uint32_t fragments = 4u;
    constexpr std::uint32_t width = 65u, height = 5u;
    const MultisampleTextureLayout layout(TextureTileMode::kR64KBX, 4u, fragments, width, height, 1u, 0u);
    const auto total = static_cast<std::size_t>(layout.Bytes()) + 3u * 65536u;
    TargetMemory memory(total);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(memory.Data(), total, true, true, true);
    }
    struct Registration {
        void* address;
        ~Registration() { GuestAllocations::Mutation mutation; mutation.Remove(address); }
    } registration{memory.Data()};
    auto detiler = std::make_unique<TextureDetiler>(context);
    context.detiler = detiler.get();
    Recorder recorder(context);
    recorder.Activate();
    struct Cleanup {
        Recorder& recorder;
        VkDevice device;
        ~Cleanup() { recorder.Sync(); ClearCachedTextures(device); ClearMultisampleTargets(device); }
    } cleanup{recorder, context.device};
    ColorTarget target{};
    target.address = target.surfaceAddress = memory.Address();
    target.bytes = layout.Bytes();
    target.extent = target.surfaceExtent = {width, height};
    target.format = VK_FORMAT_R8G8B8A8_UNORM;
    target.tileMode = ColorTileMode::RenderTarget;
    target.elementBytes = 4u;
    target.samples = fragments;
    target.dccAddress = target.address + target.bytes;
    target.cmaskAddress = target.dccAddress + 65536u;
    target.fmaskAddress = target.cmaskAddress + 65536u;
    target.fmaskSwizzle = 0x18u;
    target.dccPipeAligned = target.dccAlphaOnMsb = true;
    target.dccControl = 0x100038u;
    target.cmaskBytes = CmaskBytes(width, height);
    const auto initialize = [&](std::byte value) {
        std::vector<std::byte> physical(total, std::byte{0xff});
        const std::vector<std::byte> logical(layout.LinearBytes(), value);
        layout.Tile(logical, std::span(physical).first(static_cast<std::size_t>(target.bytes)));
        std::fill(physical.begin() + (target.cmaskAddress - target.address), physical.begin() + (target.cmaskAddress - target.address) + target.cmaskBytes, std::byte{0xee});
        const auto bytes = MultisampleFmaskBytes(target);
        std::fill(physical.begin() + (target.fmaskAddress - target.address), physical.begin() + (target.fmaskAddress - target.address) + bytes, std::byte{0xe4});
        AgcDriver::GuestMemory::StoreOwnBytes(target.address, physical);
    };
    initialize(std::byte{0x17});
    const auto oldView = MultisampleTargetView(context, target);
    recorder.Sync();
    {
        GuestAllocations::Mutation mutation;
        mutation.Protect(memory.Data(), total, true, true, true, [] {});
    }
    TargetRefuses([&] { RequireNoMultisampleTarget(target.address, 1u, "retirement probe"); }, "retirement probe");
    bool failed = false;
    try {
        GuestAllocations::Mutation mutation;
        mutation.Unmap(memory.Data(), total, [](const void*, std::size_t, const void*, bool) { throw std::runtime_error("retirement unmap failed"); });
    } catch (const std::runtime_error& error) { failed = std::string_view(error.what()) == "retirement unmap failed"; }
    Require(failed, "a failed retirement unmap did not throw");
    TargetRefuses([&] { RequireNoMultisampleTarget(target.address, 1u, "retirement probe"); }, "retirement probe");
    const auto partial = partialMetadata ? target.cmaskAddress : target.address;
    {
        GuestAllocations::Mutation mutation;
        mutation.Unmap(reinterpret_cast<void*>(partial), 65536u, [](const void*, std::size_t, const void*, bool) {});
    }
    TargetRefuses([&] { RequireNoMultisampleTarget(target.address, 1u, "retirement probe"); }, "retirement probe");
    TargetRefuses([&] { RequireNoMultisampleTarget(target.cmaskAddress, 1u, "retirement probe"); }, "retirement probe");
    {
        GuestAllocations::Mutation mutation;
        if (removeAllocation) mutation.Remove(memory.Data());
        else mutation.Unmap(memory.Data(), total, [](const void*, std::size_t, const void*, bool) {});
        mutation.Add(memory.Data(), total, true, true, true);
    }
    RequireNoMultisampleTarget(target.address, total, "retired color and metadata read");
    initialize(std::byte{0x92});
    const auto newView = MultisampleTargetView(context, target);
    Require(newView != oldView, "a remapped allocation reused a retired native attachment");
    const auto samples = ReadMultisampleTargetSamples(context, target);
    const std::vector<std::byte> expected(samples.size(), std::byte{0x92});
    Require(samples == expected, "a remapped native attachment retained its old contents");
    std::puts("native color-plus-metadata retirement: protect/failed/partial retained; full removal/unmap recreated exact samples");
}

void CheckMetadataReset(Context context, VkFormat format, std::uint32_t fragments) {
    const std::uint32_t bytes = format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u : 4u;
    constexpr VkExtent2D extent{65u, 5u};
    const MultisampleTextureLayout layout(TextureTileMode::kR64KBX, bytes, fragments, extent.width, extent.height, 1u, 0x8c00u);
    TargetMemory memory(static_cast<std::size_t>(layout.Bytes()) + 3u * 65536u);
    auto detiler = std::make_unique<TextureDetiler>(context);
    context.detiler = detiler.get();
    ColorTarget target{};
    target.address = target.surfaceAddress = memory.Address();
    target.extent = target.surfaceExtent = extent;
    target.bytes = layout.Bytes();
    target.format = format;
    target.tileMode = ColorTileMode::RenderTarget;
    target.elementBytes = bytes;
    target.samples = fragments;
    target.pipeBankXor = 0x8c00u;
    target.dccAddress = target.address + target.bytes;
    target.cmaskAddress = target.dccAddress + 65536u;
    target.fmaskAddress = target.cmaskAddress + 65536u;
    target.fmaskSwizzle = 0x18u;
    target.dccPipeAligned = target.dccAlphaOnMsb = true;
    target.dccControl = 0x100038u;
    target.cmaskBytes = CmaskBytes(extent.width, extent.height);
    const auto dccBytes = MultisampleDccBytes_nid_no_patch(bytes, fragments, extent.width, extent.height, 1u);
    const auto fmaskBytes = MultisampleFmaskBytes(target);
    const auto expanded = fragments == 2u ? std::byte{0xdd} : fragments == 4u ? std::byte{0xee} : std::byte{0xff};
    std::vector<std::byte> original(layout.LinearBytes());
    for (std::size_t index = 0u; index < original.size() / bytes; ++index) {
        for (std::uint32_t channel = 0u; channel < 4u; ++channel) {
            if (bytes == 8u) {
                const auto value = static_cast<std::uint16_t>(0x3400u + (index % fragments) * 0x80u + channel * 0x10u);
                std::memcpy(original.data() + index * bytes + channel * 2u, &value, 2u);
            } else original[index * bytes + channel] = static_cast<std::byte>((index * 13u + channel * 37u) & 255u);
        }
    }
    Recorder recorder(context);
    recorder.Activate();
    struct Cleanup {
        Recorder& recorder;
        VkDevice device;
        ~Cleanup() { recorder.Sync(); ClearCachedTextures(device); ClearMultisampleTargets(device); }
    } cleanup{recorder, context.device};
    const auto store = [](std::uint64_t address, std::size_t count, std::byte value) {
        const std::vector<std::byte> data(count, value);
        AgcDriver::GuestMemory::StoreOwnBytes(address, data);
    };
    const auto publish = [&] {
        std::array<std::byte, 1> probe{};
        AgcDriver::GuestMemory::Read(target.address + layout.Offset(64u, 0u, 0u, fragments - 1u), probe);
    };
    const auto fresh = [&](bool published = true) {
        recorder.Sync();
        ClearCachedTextures(context.device);
        ClearMultisampleTargets(context.device);
        layout.Tile(original, std::span(memory.Data(), static_cast<std::size_t>(target.bytes)));
        store(target.dccAddress, dccBytes, std::byte{0xff});
        store(target.cmaskAddress, target.cmaskBytes, expanded);
        const auto mapping = MultisampleExpandedFmask(fragments);
        for (std::size_t at = 0u; at < fmaskBytes; ++at) memory.Data()[target.fmaskAddress - target.address + at] = static_cast<std::byte>((mapping >> (at % 4u * 8u)) & 255u);
        const auto view = MultisampleTargetView(context, target);
        Require(ReadMultisampleTargetSamples(context, target) == original, "metadata reset initial raw samples differ");
        if (published) publish();
        return view;
    };
    auto view = fresh();
    for (const std::uint32_t code : {0u, 0x40u, 0x80u, 0xc0u}) {
        auto physical = std::vector<std::byte>(memory.Data(), memory.Data() + target.bytes);
        store(target.dccAddress, dccBytes, static_cast<std::byte>(code));
        store(target.cmaskAddress, target.cmaskBytes, std::byte{0xcc});
        QueueTargetByteWrite(context, recorder, target.dccAddress + dccBytes - 1u, static_cast<std::byte>(code));
        TargetRefuses([&] { MultisampleTargetView(context, target); }, "external GPU writes");
        recorder.Sync();
        Require(MultisampleTargetView(context, target) == view, "metadata reset replaced the retained attachment view");
        std::vector<std::byte> expected(original.size());
        for (std::size_t index = 0u; index < expected.size() / bytes; ++index) {
            for (std::uint32_t channel = 0u; channel < 4u; ++channel) {
                const bool one = channel == 3u ? code == 0x40u || code == 0xc0u : code == 0x80u || code == 0xc0u;
                if (bytes == 8u) {
                    const std::uint16_t value = one ? 0x3c00u : 0u;
                    std::memcpy(expected.data() + index * bytes + channel * 2u, &value, 2u);
                } else expected[index * bytes + channel] = one ? std::byte{0xff} : std::byte{0};
            }
        }
        Require(ReadMultisampleTargetSamples(context, target) == expected, "metadata reset native GPU samples lost clear channels or fragments");
        if (bytes == 8u) CheckPublishedSampledRead(context, recorder, target, expected);
        publish();
        for (std::size_t offset = 0u; offset < physical.size(); offset += bytes) std::memcpy(physical.data() + offset, expected.data(), bytes);
        Require(std::equal(physical.begin(), physical.end(), memory.Data()), "metadata reset publication kept stale sample bytes");
        Require(std::all_of(reinterpret_cast<const std::byte*>(target.dccAddress), reinterpret_cast<const std::byte*>(target.dccAddress) + dccBytes, [](auto byte) { return byte == std::byte{0xff}; }), "metadata reset publication left DCC compressed");
        Require(std::all_of(reinterpret_cast<const std::byte*>(target.cmaskAddress), reinterpret_cast<const std::byte*>(target.cmaskAddress) + target.cmaskBytes, [&](auto byte) { return byte == expanded; }), "metadata reset publication left CMASK collapsed");
    }
    view = fresh(false);
    const auto savedPhysical = std::vector<std::byte>(memory.Data(), memory.Data() + target.bytes);
    const auto generation = AgcDriver::GuestMemory::CollectWritesUncached(target.address, target.bytes);
    std::vector<std::byte> finalClear(original.size());
    for (const std::uint32_t code : {0u, 0x40u, 0x80u, 0xc0u}) {
        const std::array<std::uint32_t, 6> users{0u, 0u, static_cast<std::uint32_t>(target.dccAddress), static_cast<std::uint32_t>(target.dccAddress >> 32u), static_cast<std::uint32_t>(dccBytes - 16u), code * 0x01010101u};
        const ShaderRecompiler::ShaderComputeStageInfo compute{{64u, 1u, 1u}, 0u, {true, false, false}, false, 1u};
        const std::array<std::uint32_t, 5> packet{0xc0031502u, static_cast<std::uint32_t>((dccBytes + 1023u) / 1024u), 1u, 1u, 0x2047u};
        const auto fill = AgcDriver::DriverDetail::MatchConstantFill(AgcDriver::DriverDetail::ConstantFillKernel, users, compute, packet);
        Require(fill.has_value() && fill->bytes == dccBytes, "registered constant fill does not cover the full metadata range");
        NoteNativeColorClear_nid_no_patch(fill->address, fill->bytes, fill->value, 0u);
        store(target.dccAddress, dccBytes, static_cast<std::byte>(code));
        store(target.cmaskAddress, target.cmaskBytes, std::byte{0xcc});
        Require(MultisampleTargetView(context, target) == view, "registered clear replaced the retained attachment view");
        for (std::size_t index = 0u; index < finalClear.size() / bytes; ++index) {
            for (std::uint32_t channel = 0u; channel < 4u; ++channel) {
                const bool one = channel == 3u ? code == 0x40u || code == 0xc0u : code == 0x80u || code == 0xc0u;
                if (bytes == 8u) {
                    const std::uint16_t value = one ? 0x3c00u : 0u;
                    std::memcpy(finalClear.data() + index * bytes + channel * 2u, &value, 2u);
                } else finalClear[index * bytes + channel] = one ? std::byte{0xff} : std::byte{0};
            }
        }
        Require(ReadMultisampleTargetSamples(context, target) == finalClear, "unpublished native clear lost channel or sample values");
        if (bytes == 8u) CheckPublishedSampledRead(context, recorder, target, finalClear);
        AgcDriver::GuestMemory::CollectWritesUncached(target.address, target.bytes);
        Require(AgcDriver::GuestMemory::UnchangedSince(target.address, target.bytes, generation) && std::equal(savedPhysical.begin(), savedPhysical.end(), memory.Data()), "native clear or snapshot published guest sample bytes");
    }
    publish();
    auto finalPhysical = savedPhysical;
    for (std::size_t offset = 0u; offset < finalPhysical.size(); offset += bytes) std::memcpy(finalPhysical.data() + offset, finalClear.data(), bytes);
    Require(std::equal(finalPhysical.begin(), finalPhysical.end(), memory.Data()), "final native clear CPU publication lost samples or padding");
    for (std::uint32_t field = 0u; field < 20u; ++field) {
        view = fresh(field == 15u);
        if (field == 5u) store(target.dccAddress, dccBytes, std::byte{0});
        const auto count = field == 0u ? dccBytes - 1u : dccBytes;
        const auto value = field == 2u ? 0x20u : field == 3u ? 0x20202020u : field == 19u ? 0x01010100u : 0u;
        NoteNativeColorClear_nid_no_patch(target.dccAddress, count, value, field == 1u ? 0xfffffff0u : 0u);
        if (field != 4u && field != 5u && field != 15u && field != 18u) store(target.dccAddress, dccBytes, std::byte{0});
        if (field != 15u && field != 18u) store(target.cmaskAddress, target.cmaskBytes, std::byte{0xcc});
        std::string_view reason = field <= 3u || field == 19u ? "unsupported numeric clear" : "guest writes";
        switch (field) {
            case 6u: memory.Data()[target.dccAddress - target.address] = std::byte{0}; break;
            case 7u: store(target.dccAddress, dccBytes, std::byte{0x40}); break;
            case 8u: MarkMultisampleTargetDirty(context, target); break;
            case 9u: QueueTargetByteWrite(context, recorder, target.dccAddress + dccBytes - 1u, std::byte{0}); reason = "external GPU writes"; break;
            case 10u: QueueTargetByteWrite(context, recorder, target.address, std::byte{0}); reason = "external GPU writes"; break;
            case 11u: store(target.address + target.bytes - 1u, 1u, std::byte{0}); break;
            case 12u: store(target.fmaskAddress + fmaskBytes - 1u, 1u, std::byte{0}); break;
            case 13u: store(target.dccAddress + dccBytes - 1u, 1u, std::byte{0x40}); break;
            case 14u: store(target.cmaskAddress + target.cmaskBytes - 1u, 1u, std::byte{0xdd}); break;
            case 16u: memory.Data()[target.cmaskAddress - target.address] = std::byte{0xcc}; break;
            case 17u: store(target.cmaskAddress, target.cmaskBytes, std::byte{0xaa}); break;
        }
        TargetRefuses([&] { MultisampleTargetView(context, target); }, reason);
    }
    for (std::uint32_t field = 0u; field < 10u; ++field) {
        view = fresh();
        store(target.dccAddress, dccBytes, std::byte{0});
        store(target.cmaskAddress, target.cmaskBytes, std::byte{0xcc});
        std::string_view reason = "guest writes";
        switch (field) {
            case 0u: store(target.dccAddress + dccBytes - 1u, 1u, std::byte{0x40}); break;
            case 1u: store(target.dccAddress, dccBytes, std::byte{0x20}); break;
            case 2u: store(target.dccAddress, dccBytes, std::byte{0xff}); break;
            case 3u: store(target.cmaskAddress + target.cmaskBytes - 1u, 1u, std::byte{0xdd}); break;
            case 4u: store(target.cmaskAddress, target.cmaskBytes, std::byte{0xaa}); break;
            case 5u: store(target.address + target.bytes - 1u, 1u, std::byte{0}); break;
            case 6u: store(target.fmaskAddress + fmaskBytes - 1u, 1u, std::byte{0}); break;
            case 7u: ClearTargetAttachment(context, view, target); break;
            case 8u: NoteColorMetadataFill(target.dccAddress + dccBytes - 1u, 1u, 0u); reason = "unimplemented residency transition"; break;
            case 9u: AgcDriver::GuestMemory::Unwatch(target.dccAddress, dccBytes); reason = "watched guest memory"; break;
        }
        TargetRefuses([&] { MultisampleTargetView(context, target); }, reason);
    }
    std::printf("Metadata reset format=%u samples=%u: four published and four unpublished clear cycles, 30 conflict refusals\n", static_cast<std::uint32_t>(format), fragments);
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

std::vector<std::uint32_t> DepthVertexShader() {
    ShaderRecompiler::SpirvModule module;
    module.EmitCapability(spv::CapabilityShader);
    module.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
    const auto f32 = module.Type(spv::OpTypeFloat, 32u);
    const auto i32 = module.Type(spv::OpTypeInt, 32u, 1u);
    const auto boolean = module.Type(spv::OpTypeBool);
    const auto v4 = module.Type(spv::OpTypeVector, f32, 4u);
    const auto voidType = module.Type(spv::OpTypeVoid);
    const auto index = module.DefineGlobalVariable(module.Type(spv::OpTypePointer, spv::StorageClassInput, i32), spv::StorageClassInput);
    const auto position = module.DefineGlobalVariable(module.Type(spv::OpTypePointer, spv::StorageClassOutput, v4), spv::StorageClassOutput);
    module.AddAnnotation(spv::OpDecorate, index, spv::DecorationBuiltIn, spv::BuiltInVertexIndex);
    module.AddAnnotation(spv::OpDecorate, position, spv::DecorationBuiltIn, spv::BuiltInPosition);
    const auto main = module.AllocateId();
    module.AddFunction(spv::OpFunction, voidType, main, spv::FunctionControlMaskNone, module.Type(spv::OpTypeFunction, voidType));
    module.AddFunction(spv::OpLabel, module.AllocateId());
    const auto loaded = module.AllocateId();
    module.AddFunction(spv::OpLoad, i32, loaded, index);
    const auto coordinate = [&](std::uint32_t vertex) {
        const auto equal = module.AllocateId(), value = module.AllocateId();
        module.AddFunction(spv::OpIEqual, boolean, equal, loaded, module.Constant(spv::OpConstant, i32, vertex));
        module.AddFunction(spv::OpSelect, f32, value, equal, module.Constant(spv::OpConstant, f32, std::bit_cast<std::uint32_t>(3.0f)), module.Constant(spv::OpConstant, f32, std::bit_cast<std::uint32_t>(-1.0f)));
        return value;
    };
    const auto value = module.AllocateId();
    module.AddFunction(spv::OpCompositeConstruct, v4, value, coordinate(1u), coordinate(2u), module.Constant(spv::OpConstant, f32, 0u), module.Constant(spv::OpConstant, f32, std::bit_cast<std::uint32_t>(1.0f)));
    module.AddFunction(spv::OpStore, position, value);
    module.AddFunction(spv::OpReturn);
    module.AddFunction(spv::OpFunctionEnd);
    module.EmitEntryPoint(spv::ExecutionModelVertex, main, "main", {index, position});
    return module.Finalize();
}

std::vector<std::uint32_t> DepthFragmentShader() {
    ShaderRecompiler::SpirvModule module;
    module.EmitCapability(spv::CapabilityShader);
    module.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
    const auto f32 = module.Type(spv::OpTypeFloat, 32u);
    const auto u32 = module.Type(spv::OpTypeInt, 32u, 0u);
    const auto voidType = module.Type(spv::OpTypeVoid);
    const auto block = module.Type(spv::OpTypeStruct, f32);
    module.AddAnnotation(spv::OpDecorate, block, spv::DecorationBlock);
    module.AddAnnotation(spv::OpMemberDecorate, block, 0u, spv::DecorationOffset, 0u);
    const auto push = module.DefineGlobalVariable(module.Type(spv::OpTypePointer, spv::StorageClassPushConstant, block), spv::StorageClassPushConstant);
    const auto depth = module.DefineGlobalVariable(module.Type(spv::OpTypePointer, spv::StorageClassOutput, f32), spv::StorageClassOutput);
    module.AddAnnotation(spv::OpDecorate, depth, spv::DecorationBuiltIn, spv::BuiltInFragDepth);
    const auto main = module.AllocateId();
    module.AddFunction(spv::OpFunction, voidType, main, spv::FunctionControlMaskNone, module.Type(spv::OpTypeFunction, voidType));
    module.AddFunction(spv::OpLabel, module.AllocateId());
    const auto pointer = module.AllocateId(), value = module.AllocateId();
    module.AddFunction(spv::OpAccessChain, module.Type(spv::OpTypePointer, spv::StorageClassPushConstant, f32), pointer, push, module.Constant(spv::OpConstant, u32, 0u));
    module.AddFunction(spv::OpLoad, f32, value, pointer);
    module.AddFunction(spv::OpStore, depth, value);
    module.AddFunction(spv::OpReturn);
    module.AddFunction(spv::OpFunctionEnd);
    module.EmitEntryPoint(spv::ExecutionModelFragment, main, "main", {depth});
    module.AddExecutionMode(main, spv::ExecutionModeOriginUpperLeft);
    module.AddExecutionMode(main, spv::ExecutionModeDepthReplacing);
    return module.Finalize();
}

class DepthRaster {
public:
    DepthRaster(const Context& context, const DepthTarget& target, VkImageView view) : context(context), extent(target.extent), samples(target.samples) {
        try {
            const VkAttachmentDescription attachment{0u, target.format, static_cast<VkSampleCountFlagBits>(samples), VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL};
            const VkAttachmentReference reference{0u, VK_IMAGE_LAYOUT_GENERAL};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.pDepthStencilAttachment = &reference;
            VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            passInfo.attachmentCount = 1u;
            passInfo.pAttachments = &attachment;
            passInfo.subpassCount = 1u;
            passInfo.pSubpasses = &subpass;
            Check(context.Function<PFN_vkCreateRenderPass>("vkCreateRenderPass")(context.device, &passInfo, nullptr, &pass), "depth sample render pass");
            VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebufferInfo.renderPass = pass;
            framebufferInfo.attachmentCount = 1u;
            framebufferInfo.pAttachments = &view;
            framebufferInfo.width = extent.width;
            framebufferInfo.height = extent.height;
            framebufferInfo.layers = 1u;
            Check(context.Function<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(context.device, &framebufferInfo, nullptr, &framebuffer), "depth sample framebuffer");
            const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0u, 4u};
            VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layoutInfo.pushConstantRangeCount = 1u;
            layoutInfo.pPushConstantRanges = &push;
            Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &layoutInfo, nullptr, &layout), "depth sample pipeline layout");
            std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
            const std::array codes{DepthVertexShader(), DepthFragmentShader()};
            for (std::uint32_t i = 0u; i < 2u; ++i) {
                VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                info.codeSize = codes[i].size() * 4u;
                info.pCode = codes[i].data();
                Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &info, nullptr, &modules[i]), "depth sample shader");
                stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0u, i == 0u ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT, modules[i], "main", nullptr};
            }
            VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
            const VkRect2D scissor{{0, 0}, extent};
            VkPipelineViewportStateCreateInfo viewportInfo{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewportInfo.viewportCount = viewportInfo.scissorCount = 1u;
            viewportInfo.pViewports = &viewport;
            viewportInfo.pScissors = &scissor;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode = VK_CULL_MODE_NONE;
            raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            raster.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            multisample.rasterizationSamples = static_cast<VkSampleCountFlagBits>(samples);
            VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            depth.depthTestEnable = depth.depthWriteEnable = VK_TRUE;
            depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            info.stageCount = 2u;
            info.pStages = stages.data();
            info.pVertexInputState = &input;
            info.pInputAssemblyState = &assembly;
            info.pViewportState = &viewportInfo;
            info.pRasterizationState = &raster;
            info.pMultisampleState = &multisample;
            info.pDepthStencilState = &depth;
            info.pColorBlendState = &blend;
            info.layout = layout;
            info.renderPass = pass;
            pipelines.resize(samples, VK_NULL_HANDLE);
            for (std::uint32_t sample = 0u; sample < samples; ++sample) {
                const auto mask = 1u << sample;
                multisample.pSampleMask = &mask;
                Check(context.Function<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(context.device, VK_NULL_HANDLE, 1u, &info, nullptr, &pipelines[sample]), "depth sample pipeline");
            }
        } catch (...) { release(); throw; }
    }
    ~DepthRaster() { release(); }
    void Run(VkCommandBuffer commands, float offset) const {
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = pass;
        begin.framebuffer = framebuffer;
        begin.renderArea = {{0, 0}, extent};
        context.Function<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
        for (std::uint32_t sample = 0u; sample < samples; ++sample) {
            const auto value = offset + static_cast<float>(sample + 1u) / 16.0f;
            context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[sample]);
            context.Function<PFN_vkCmdPushConstants>("vkCmdPushConstants")(commands, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0u, 4u, &value);
            context.Function<PFN_vkCmdDraw>("vkCmdDraw")(commands, 3u, 1u, 0u, 0u);
        }
        context.Function<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(commands);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
private:
    void release() noexcept {
        for (const auto pipeline : pipelines) if (pipeline) context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, pipeline, nullptr);
        for (const auto module : modules) if (module) context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
        if (layout) context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, layout, nullptr);
        if (framebuffer) context.Function<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer")(context.device, framebuffer, nullptr);
        if (pass) context.Function<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(context.device, pass, nullptr);
    }
    Context context;
    VkExtent2D extent;
    std::uint32_t samples;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::array<VkShaderModule, 2> modules{};
    std::vector<VkPipeline> pipelines;
};

std::array<std::uint32_t, 8> DepthSampleWords(const DepthTarget& target) {
    const auto base = (target.address | target.pipeBankXor) >> 8u;
    const auto metadata = target.htileAddress >> 8u;
    const auto format = target.format == VK_FORMAT_D16_UNORM ? 7u : 22u;
    const auto count = static_cast<std::uint32_t>(std::countr_zero(target.samples));
    const auto width = target.extent.width - 1u, height = target.extent.height - 1u;
    return {static_cast<std::uint32_t>(base), static_cast<std::uint32_t>((base >> 32u) & 255u) | (format << 20u) | ((width & 3u) << 30u), (width >> 2u) | (height << 14u), (14u << 28u) | (24u << 20u) | (count << 16u) | 0x204u, 0u, count << 4u, (1u << 10u) | (1u << 19u) | (1u << 21u) | (static_cast<std::uint32_t>(metadata & 255u) << 24u), static_cast<std::uint32_t>(metadata >> 8u)};
}

bool DepthSupported(const Context& context, VkFormat format, std::uint32_t fragments) {
    VkImageFormatProperties properties{};
    const auto result = context.imageFormatProperties(context.physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0u, &properties);
    return result == VK_SUCCESS && (properties.sampleCounts & fragments) != 0u && (context.limits.framebufferDepthSampleCounts & fragments) != 0u;
}

void CheckDepthRetirement(Context context, VkFormat format, std::uint32_t fragments, std::uint32_t releaseMode) {
    const VkExtent2D extent{5u, 3u};
    const auto depthBytes = static_cast<std::size_t>(DepthPlaneBytes_nid_no_patch(extent, format == VK_FORMAT_D16_UNORM ? 2u : 4u, fragments));
    Require(depthBytes == 65536u, "depth retirement fixture has unexpected plane geometry");
    TargetMemory memory(3u * 65536u);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(memory.Data(), 3u * 65536u, true, true, true);
    }
    struct Registration {
        void* address;
        ~Registration() { GuestAllocations::Mutation mutation; mutation.Remove(address); }
    } registration{memory.Data()};
    const auto metadataBytes = MultisampleHtileBytes_nid_no_patch(extent.width, extent.height, 1u);
    DepthTarget target{memory.Address(), 0u, extent, format, 0.0f, 0u, memory.Address() + 65536u, false, fragments};
    target.depthSwizzle = 24u;
    target.iterate256 = target.htilePipeAligned = true;
    Recorder recorder(context);
    recorder.Activate();
    struct Cleanup {
        Recorder& recorder;
        Context context;
        ~Cleanup() { recorder.Sync(); ClearCachedTextures(context.device); ClearDepthSurfaces(context.device); }
    } cleanup{recorder, context};
    std::memset(memory.Data() + 65536u, 0, metadataBytes);
    const auto oldView = DepthSurfaceView(context, target);
    auto oldTexture = CachedSampledTexture(context, DepthSampleWords(target));
    Require(oldView != VK_NULL_HANDLE && oldTexture != nullptr, "initial native depth retirement target was not created");
    recorder.Sync();
    ClearMultisampleTargets(context.device);
    const auto retained = [&] {
        for (const auto address : {target.address, target.htileAddress}) TargetRefuses([&] { RequireNoResidentDepthRead_nid_no_patch(address, 1u); }, "sample-preserving guest publication");
    };
    {
        GuestAllocations::Mutation mutation;
        mutation.Protect(memory.Data(), 3u * 65536u, true, true, true, [] {});
    }
    retained();
    bool failed = false;
    bool attempted = false;
    try {
        GuestAllocations::Mutation mutation;
        mutation.Unmap(memory.Data(), 3u * 65536u, [&](const void*, std::size_t, const void*, bool) { attempted = true; throw std::runtime_error("depth host unmap failed"); });
    } catch (const std::runtime_error&) { failed = true; }
    Require(failed && attempted, "failed depth host unmap did not throw from the callback");
    retained();
    if (releaseMode == 2u) {
        {
            GuestAllocations::Mutation mutation;
            mutation.Unmap(memory.Data(), depthBytes / 2u, [](const void*, std::size_t, const void*, bool) {});
        }
        retained();
        {
            GuestAllocations::Mutation mutation;
            mutation.Unmap(memory.Data() + depthBytes / 2u, depthBytes / 2u, [](const void*, std::size_t, const void*, bool) {});
        }
        retained();
        {
            GuestAllocations::Mutation mutation;
            mutation.Unmap(memory.Data() + depthBytes, 3u * 65536u - depthBytes, [](const void*, std::size_t, const void*, bool) {});
        }
    } else if (releaseMode == 3u) {
        for (const auto [offset, bytes] : std::array<std::pair<std::size_t, std::size_t>, 3>{{
                 {depthBytes / 4u, depthBytes / 2u}, {0u, depthBytes / 4u}, {3u * depthBytes / 4u, depthBytes / 4u}}}) {
            {
                GuestAllocations::Mutation mutation;
                mutation.Unmap(memory.Data() + offset, bytes, [](const void*, std::size_t, const void*, bool) {});
            }
            retained();
        }
        {
            GuestAllocations::Mutation mutation;
            mutation.Unmap(memory.Data() + depthBytes, 3u * 65536u - depthBytes, [](const void*, std::size_t, const void*, bool) {});
        }
    } else {
        GuestAllocations::Mutation mutation;
        if (releaseMode == 0u) mutation.Remove(memory.Data());
        else mutation.Unmap(memory.Data(), 3u * 65536u, [](const void*, std::size_t, const void*, bool) {});
    }
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(memory.Data(), 3u * 65536u, true, true, true);
    }
    std::array<std::byte, 1> probe{};
    for (const auto address : {target.address, target.address + depthBytes - 1u, target.htileAddress, target.htileAddress + metadataBytes - 1u}) {
        RequireNoResidentDepthRead_nid_no_patch(address, 1u);
        Require(!DepthSurfaceGuestOverlap_nid_no_patch(context, address, 1u), "a retired native depth target still owns its allocation");
        AgcDriver::GuestMemory::Read(address, probe);
    }
    const std::uint32_t one = 0xfffc00f0u;
    for (std::size_t i = 0u; i < metadataBytes / 4u; ++i) std::memcpy(memory.Data() + 65536u + i * 4u, &one, 4u);
    target.clearDepth = 1.0f;
    const auto newView = DepthSurfaceView(context, target);
    auto texture = CachedSampledTexture(context, DepthSampleWords(target));
    Require(newView != VK_NULL_HANDLE && newView != oldView && texture != nullptr && texture != oldTexture, "remapped native depth reused its retired image or sampled view");
    recorder.Sync();
    const auto count = extent.width * extent.height * fragments;
    Buffer output(context, (count + 2u) * 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(output.Bytes().data(), 0xa5, output.Bytes().size());
    const Fetch fetch(context, *texture, output, FetchShader(extent.width, extent.height, fragments, false, true));
    {
        CommandBatch batch(context);
        fetch.Run(batch.Handle(), count);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        batch.SubmitAndWait();
    }
    output.Invalidate();
    for (std::uint32_t i = 0u; i < count; ++i) {
        std::array<float, 4> value{};
        std::memcpy(value.data(), output.Bytes().data() + (i + 1u) * 16u, 16u);
        Require(value == std::array<float, 4>{1.0f, 0.0f, 0.0f, 1.0f}, "remapped native depth did not preserve its new clear samples");
    }
    Require(std::all_of(output.Bytes().begin(), output.Bytes().begin() + 16u, [](auto byte) { return byte == std::byte{0xa5}; }) && std::all_of(output.Bytes().end() - 16u, output.Bytes().end(), [](auto byte) { return byte == std::byte{0xa5}; }), "depth retirement sampled fetch overwrote output guards");
    retained();
    std::printf("Native depth retirement format=%u samples=%u release=%u: retained live/partial/failed/protected, recreated exact clear-one samples\n", static_cast<std::uint32_t>(format), fragments, releaseMode);
}

void CheckDepthTarget(Context context, VkFormat format, std::uint32_t fragments, bool one, bool clearing = false, bool depthOnly = false, bool partialCompletion = false) {
    const VkExtent2D extent{5u, 3u};
    const auto bytes = static_cast<std::size_t>(DepthPlaneBytes_nid_no_patch(extent, format == VK_FORMAT_D16_UNORM ? 2u : 4u, fragments));
    TargetMemory memory(bytes), metadata(65536u);
    const auto metadataBytes = MultisampleHtileBytes_nid_no_patch(extent.width, extent.height, 1u);
    const std::uint32_t pattern = one ? (depthOnly ? 0xfffffff0u : 0xfffc00f0u) : 0u;
    for (std::size_t i = 0u; i < metadataBytes / 4u; ++i) std::memcpy(metadata.Data() + i * 4u, &pattern, 4u);
    DepthTarget target{memory.Address(), 0u, extent, format, one ? 1.0f : 0.0f, 0u, metadata.Address(), false, fragments};
    target.depthSwizzle = 24u;
    target.pipeBankXor = one ? 0x8c00u : 0u;
    target.iterate256 = target.htilePipeAligned = true;
    target.htileDepthOnly = depthOnly;
    Recorder recorder(context);
    recorder.Activate();
    struct Cleanup {
        Recorder& recorder;
        Context context;
        ~Cleanup() { recorder.Sync(); ClearCachedTextures(context.device); ClearDepthSurfaces(context.device); }
    } cleanup{recorder, context};
    if (!one) {
        metadata.Data()[metadataBytes - 1u] = std::byte{0x7f};
        QueueTargetByteWrite(context, recorder, target.htileAddress + metadataBytes - 1u, std::byte{0});
        Require(recorder.PendingWriteOverlaps(target.htileAddress, metadataBytes), "initial depth producer was not registered");
    }
    const auto view = DepthSurfaceView(context, target);
    Require(DepthSurfaceView(context, target) == view, "native depth attachment did not reuse its image");
    const auto words = DepthSampleWords(target);
    auto texture = CachedSampledTexture(context, words);
    Require(texture != nullptr && CachedSampledTexture(context, words) == texture, "native depth sampled view was not reused");
    Require(DepthSurfaceHolds(context, DecodeTextureResource(words), texture.get()), "native depth sampled view is not held by its attachment");
    recorder.Sync();
    const auto count = extent.width * extent.height * fragments;
    Buffer output(context, (count + 2u) * 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const Fetch fetch(context, *texture, output, FetchShader(extent.width, extent.height, fragments, false, true));
    const auto check = [&](bool rendered, float offset) {
        output.Invalidate();
        for (std::uint32_t index = 0u; index < count; ++index) {
            std::array<float, 4> result{};
            std::memcpy(result.data(), output.Bytes().data() + (index + 1u) * 16u, 16u);
            const auto expected = rendered ? offset + static_cast<float>(index % fragments + 1u) / 16.0f : target.clearDepth;
            const auto tolerance = format == VK_FORMAT_D16_UNORM && rendered ? 1.0f / 65535.0f : 0.0f;
            Require(std::isfinite(result[0]) && std::abs(result[0] - expected) <= tolerance && result[1] == 0.0f && result[2] == 0.0f && result[3] == 1.0f, "native depth sampled fetch lost a raster sample or its component mapping");
        }
        Require(std::all_of(output.Bytes().begin(), output.Bytes().begin() + 16u, [](auto byte) { return byte == std::byte{0xa5}; }) && std::all_of(output.Bytes().end() - 16u, output.Bytes().end(), [](auto byte) { return byte == std::byte{0xa5}; }), "native depth sampled fetch changed output guards");
    };
    std::memset(output.Bytes().data(), 0xa5, output.Bytes().size());
    {
        CommandBatch batch(context);
        fetch.Run(batch.Handle(), count);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        batch.SubmitAndWait();
        check(false, 0.0f);
    }
    {
        const DepthRaster raster(context, target, view);
        for (const auto offset : {0.0f, 0.25f}) {
            Require(CachedSampledTexture(context, words) == texture, "native sampled view changed between raster writes");
            std::memset(output.Bytes().data(), 0xa5, output.Bytes().size());
            CommandBatch batch(context);
            raster.Run(batch.Handle(), offset);
            fetch.Run(batch.Handle(), count);
            RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
            batch.SubmitAndWait();
            check(true, offset);
        }
        if (clearing) {
            for (std::uint32_t iteration = 0u; iteration < 3u; ++iteration) {
                {
                    CommandBatch batch(context);
                    raster.Run(batch.Handle(), static_cast<float>(iteration) / 16.0f);
                    batch.SubmitAndWait();
                }
                NoteHtileDepthClear(target.htileAddress, metadataBytes, metadataBytes);
                const auto storedBytes = partialCompletion ? 4u : metadataBytes;
                AgcDriver::GuestMemory::StoreOwnBytes(target.htileAddress, storedBytes, [&] {
                    for (std::size_t i = 0u; i < storedBytes / 4u; ++i) std::memcpy(metadata.Data() + i * 4u, &pattern, 4u);
                });
                Require(DepthSurfaceHolds(context, DecodeTextureResource(words), texture.get()), "completed native depth clear lost its retained sampled view");
                recorder.Sync();
                Require(DepthSurfaceView(context, target) == view && CachedSampledTexture(context, words) == texture, "completed native depth clear replaced an attachment or cached view");
                std::memset(output.Bytes().data(), 0xa5, output.Bytes().size());
                CommandBatch batch(context);
                fetch.Run(batch.Handle(), count);
                RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
                batch.SubmitAndWait();
                check(false, 0.0f);
                if (partialCompletion) std::printf("Partial HTILE driver store=%zu of%zu cleared every rasterized depth sample, cycle=%u\n", static_cast<std::size_t>(storedBytes), metadataBytes, iteration);
            }
        }
    }
    Require(std::all_of(memory.Data(), memory.Data() + bytes, [](auto value) { return value == std::byte{0xa5}; }), "native depth sampling silently published guest memory");
    std::array<std::byte, 1> probe{};
    for (const auto address : {target.address, target.address + bytes - 1u, target.htileAddress, target.htileAddress + metadataBytes - 1u}) {
        Require(DepthSurfaceGuestOverlap_nid_no_patch(context, address, 1u), "actual resident depth range omitted its final byte");
        TargetRefuses([&] { AgcDriver::GuestMemory::Read(address, probe); }, "sample-preserving guest publication");
        TargetRefuses([&] { AgcDriver::GuestMemory::ReadCommitted(address, probe); }, "sample-preserving guest publication");
        Require(probe[0] == std::byte{0}, "refused native depth read modified its destination");
    }
    TargetMemory unrelated(65536u);
    AgcDriver::GuestMemory::Read(unrelated.Address(), probe);
    Require(probe[0] == std::byte{0xa5}, "resident depth guard blocked an unrelated read");
    auto alias = target;
    alias.htileAddress += 65536u;
    TargetRefuses([&] { DepthSurfaceView(context, alias); }, "layout or metadata changed");
    auto resource = DecodeTextureResource(words);
    for (std::uint32_t field = 0u; field < 18u; ++field) {
        auto changed = resource;
        switch (field) {
            case 0u: changed.samples = fragments == 2u ? 4u : 2u; break;
            case 1u: ++changed.width; break;
            case 2u: ++changed.height; break;
            case 3u: changed.format = 20u; break;
            case 4u: changed.dimension = TextureDimension::k2DMsaaArray; break;
            case 5u: changed.depthOrLastArray = 1u; break;
            case 6u: changed.baseArray = 1u; break;
            case 7u: changed.minLod = 1u; break;
            case 8u: changed.pipeBankXor ^= 0x100u; break;
            case 9u: changed.dccAddress += 256u; break;
            case 10u: changed.dccPipeAligned = false; break;
            case 11u: changed.iterate256 = false; break;
            case 12u: changed.allocatedMipCount = 2u; break;
            case 13u: changed.mipCount = 2u; break;
            case 14u: changed.baseLevel = 1u; break;
            case 15u: changed.lastLevel = 1u; break;
            case 16u: changed.dccAlphaOnMsb = true; break;
            case 17u: changed.tileMode = TextureTileMode::kR64KBX; break;
        }
        TargetRefuses([&] { DepthSurfaceTexture(context, words, changed, VkComponentMapping{}); }, "native multisample depth sampled");
    }
    memory.Data()[bytes - 1u] = std::byte{0x5a};
    TargetRefuses([&] { CachedSampledTexture(context, words); }, "guest writes");
    TargetRefuses([&] { DepthSurfaceHolds(context, resource, texture.get()); }, "guest writes");
    texture.reset();
    std::printf("Native depth format=%u samples=%u clear=%u depthOnly=%u clearCycles=%u: imported, rasterized, sampled and refused conflicts\n", static_cast<std::uint32_t>(format), fragments, one, depthOnly, clearing ? 3u : 0u);
}

void CheckDepthRefusals(Context context, std::uint32_t fragments) {
    constexpr VkExtent2D extent{129u, 67u};
    const auto bytes = static_cast<std::size_t>(DepthPlaneBytes_nid_no_patch(extent, 4u, fragments));
    const auto metadataBytes = MultisampleHtileBytes_nid_no_patch(extent.width, extent.height, 1u);
    TargetMemory memory(bytes), metadata(65536u);
    std::memset(metadata.Data(), 0, metadataBytes);
    DepthTarget target{memory.Address(), 0u, extent, VK_FORMAT_D32_SFLOAT, 0.0f, 0u, metadata.Address(), false, fragments};
    target.depthSwizzle = 24u;
    target.iterate256 = target.htilePipeAligned = true;
    Recorder recorder(context);
    recorder.Activate();
    struct Cleanup {
        Recorder& recorder;
        Context context;
        ~Cleanup() { recorder.Sync(); ClearCachedTextures(context.device); ClearDepthSurfaces(context.device); }
    } cleanup{recorder, context};
    std::uint32_t checked = 0u;
    const auto refuseSampling = [&](const DepthTarget& unsupported, std::string_view expected) {
        if ((unsupported.address & 65535u) != 0u) {
            DepthSurfaceView(context, unsupported);
            recorder.Sync();
            const auto words = DepthSampleWords(unsupported);
            Require(DepthSurfaceTexture(context, words, DecodeTextureResource(words), VkComponentMapping{}) == nullptr, "a descriptor with a different aligned base acquired an unsupported depth attachment");
            ClearDepthSurfaces(context.device);
            return;
        }
        TargetRefuses([&] {
            DepthSurfaceView(context, unsupported);
            recorder.Sync();
            const auto words = DepthSampleWords(unsupported);
            DepthSurfaceTexture(context, words, DecodeTextureResource(words), VkComponentMapping{});
        }, expected);
        recorder.Sync();
        ClearCachedTextures(context.device);
        ClearDepthSurfaces(context.device);
    };
    for (std::uint32_t field = 0u; field < 11u; ++field) {
        auto unsupported = target;
        std::string_view expected;
        switch (field) {
            case 0u: unsupported.format = VK_FORMAT_D32_SFLOAT_S8_UINT; expected = "stencil"; break;
            case 1u: unsupported.depthSwizzle = 0u; expected = "non-Z_X"; break;
            case 2u: unsupported.firstSlice = 1u; expected = "arrays"; break;
            case 3u: unsupported.lastSlice = 1u; expected = "arrays"; break;
            case 4u: unsupported.pipeBankXor = 0x10u; expected = "XOR"; break;
            case 5u: unsupported.address += 256u; expected = "aligned"; break;
            case 6u: unsupported.htileAddress = 0u; expected = "HTILE"; break;
            case 7u: unsupported.iterate256 = false; expected = "HTILE"; break;
            case 8u: unsupported.htilePipeAligned = false; expected = "HTILE"; break;
            case 9u: unsupported.htileVrsEncoding = 1u; expected = "VRS"; break;
            case 10u: unsupported.clearDepth = 0.5f; expected = "clear value"; break;
        }
        refuseSampling(unsupported, expected);
        ++checked;
    }
    {
        TargetMemory unwatched(bytes);
        AgcDriver::GuestMemory::Unwatch(unwatched.Address(), bytes);
        auto unsupported = target;
        unsupported.address = unwatched.Address();
        refuseSampling(unsupported, "watched accessible guest memory");
        ++checked;
    }
    for (const std::uint32_t word : {0x1000u, 0xfffc00f0u, 0xfu}) {
        std::memcpy(metadata.Data() + metadataBytes - 4u, &word, 4u);
        refuseSampling(target, "clear value");
        std::memset(metadata.Data(), 0, metadataBytes);
        ++checked;
    }
    const auto reset = [&] {
        recorder.Sync();
        ClearCachedTextures(context.device);
        ClearDepthSurfaces(context.device);
        std::memset(memory.Data(), 0xa5, bytes);
        std::memset(metadata.Data(), 0, metadataBytes);
    };
    const auto words = DepthSampleWords(target);
    for (const auto address : {target.address + bytes - 1u, target.htileAddress + metadataBytes - 1u}) {
        DepthSurfaceView(context, target);
        recorder.Sync();
        QueueTargetByteWrite(context, recorder, address, std::byte{0});
        TargetRefuses([&] { CachedSampledTexture(context, words); }, "external GPU writes");
        ++checked;
        reset();
    }
    DepthSurfaceView(context, target);
    recorder.Sync();
    metadata.Data()[metadataBytes - 1u] = std::byte{1};
    TargetRefuses([&] { CachedSampledTexture(context, words); }, "guest writes");
    ++checked;
    reset();
    DepthSurfaceView(context, target);
    recorder.Sync();
    NoteHtileDepthClear(target.htileAddress, 0u);
    TargetRefuses([&] { CachedSampledTexture(context, words); }, "fast-clear transition");
    ++checked;
    reset();
    DepthSurfaceView(context, target);
    recorder.Sync();
    NoteDepthMetadataFill(target.htileAddress + metadataBytes - 1u, 1u, 0u);
    TargetRefuses([&] { CachedSampledTexture(context, words); }, "metadata fill transition");
    ++checked;
    reset();
    for (std::uint32_t field = 0u; field < 10u; ++field) {
        DepthSurfaceView(context, target);
        auto texture = CachedSampledTexture(context, words);
        recorder.Sync();
        const auto store = [&](std::uint32_t word) {
            AgcDriver::GuestMemory::StoreOwnBytes(target.htileAddress, metadataBytes, [&] {
                for (std::size_t i = 0u; i < metadataBytes / 4u; ++i) std::memcpy(metadata.Data() + i * 4u, &word, 4u);
            });
        };
        if (field == 1u) store(0u);
        NoteHtileDepthClear(target.htileAddress, metadataBytes, field == 2u ? metadataBytes - 1u : metadataBytes);
        if (field != 0u && field != 1u) store(field == 4u ? 0x1000u : 0u);
        std::string_view expected = "complete tracked HTILE store";
        switch (field) {
            case 2u: expected = "incomplete"; break;
            case 3u: {
                const std::uint32_t one = 0xfffc00f0u;
                AgcDriver::GuestMemory::StoreOwnBytes(target.htileAddress + metadataBytes - 4u, std::as_bytes(std::span(&one, 1u)));
                expected = "clear value";
                break;
            }
            case 4u: expected = "clear value"; break;
            case 5u: metadata.Data()[metadataBytes - 1u] = std::byte{0}; expected = "CPU writes"; break;
            case 6u: memory.Data()[bytes - 1u] = std::byte{0}; expected = "guest writes"; break;
            case 7u: QueueTargetByteWrite(context, recorder, target.htileAddress + metadataBytes - 1u, std::byte{0}); expected = "external GPU writes"; break;
            case 8u: QueueTargetByteWrite(context, recorder, target.address + bytes - 1u, std::byte{0}); expected = "external GPU writes"; break;
            case 9u: {
                auto unsupported = target;
                unsupported.clearDepth = 0.5f;
                expected = "clear value";
                TargetRefuses([&] { DepthSurfaceView(context, unsupported); }, expected);
                break;
            }
        }
        TargetRefuses([&] { CachedSampledTexture(context, words); }, expected);
        TargetRefuses([&] { DepthSurfaceHolds(context, DecodeTextureResource(words), texture.get()); }, expected);
        texture.reset();
        ++checked;
        reset();
    }
    DepthSurfaceView(context, target);
    recorder.Sync();
    AgcDriver::GuestMemory::Unwatch(target.address, bytes);
    TargetRefuses([&] { CachedSampledTexture(context, words); }, "watched accessible guest memory");
    ++checked;
    std::printf("Native depth refusal cases=%u\n", checked);
}

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

void CheckNativeSnapshot(Context context, VkFormat format, std::uint32_t fragments, bool arrayed, bool recorded) {
    const auto guest = FindGuestTextureFormat(format, format == VK_FORMAT_R8_UINT ? 1u : format == VK_FORMAT_R16_UINT ? 2u : format == VK_FORMAT_R32G32_UINT || format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u : format == VK_FORMAT_R32G32B32A32_UINT ? 16u : 4u);
    Require(guest.has_value(), "native snapshot fixture format is unavailable");
    const auto bytes = BytesPerElement(*guest);
    const bool half = format == VK_FORMAT_R16G16B16A16_SFLOAT;
    const bool normalized = format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB;
    constexpr std::uint32_t width = 65u, height = 5u;
    const MultisampleTextureLayout layout(TextureTileMode::kR64KBX, bytes, fragments, width, height, 1u, 0x8c00u);
    TargetMemory memory(static_cast<std::size_t>(layout.Bytes()));
    auto detiler = std::make_unique<TextureDetiler>(context);
    context.detiler = detiler.get();
    ColorTarget target{};
    target.address = target.surfaceAddress = memory.Address();
    target.bytes = layout.Bytes();
    target.extent = target.surfaceExtent = {width, height};
    target.format = format;
    target.tileMode = ColorTileMode::RenderTarget;
    target.elementBytes = bytes;
    target.samples = fragments;
    target.pipeBankXor = 0x8c00u;
    const auto count = width * height * fragments;
    std::vector<std::byte> original(count * bytes);
    for (std::uint32_t element = 0u; element < count; ++element) {
        for (std::uint32_t component = 0u; component < (bytes < 4u ? 1u : 4u); ++component) {
            if (half) {
                const auto value = static_cast<std::uint16_t>(0x3c00u + ((element * 7u + component * 19u) & 0x3ffu));
                std::memcpy(original.data() + element * bytes + component * 2u, &value, 2u);
            } else if (normalized) original[element * bytes + component] = (element + component) % 2u != 0u ? std::byte{255} : std::byte{0};
            else if (component < bytes / 4u || bytes < 4u) {
                const auto value = element * 37u + component * 71u + 19u;
                std::memcpy(original.data() + element * bytes + component * (bytes < 4u ? bytes : 4u), &value, std::min(bytes, 4u));
            }
        }
    }
    std::memset(memory.Data(), 0xa5, target.bytes);
    layout.Tile(original, std::span(memory.Data(), static_cast<std::size_t>(target.bytes)));
    Recorder recorder(context);
    if (recorded) recorder.Activate();
    struct Cleanup { Recorder& recorder; VkDevice device; ~Cleanup() { recorder.Sync(); ClearCachedTextures(device); ClearMultisampleTargets(device); } } cleanup{recorder, context.device};
    const auto view = MultisampleTargetView(context, target);
    Require(AgcDriver::GuestMemory::Watched(target.address, target.bytes), "native snapshot requires watched fixture memory");
    const auto generation = AgcDriver::GuestMemory::CollectWritesUncached(target.address, target.bytes);
    const auto base = (target.address | target.pipeBankXor) >> 8u;
    const auto log = static_cast<std::uint32_t>(std::countr_zero(fragments));
    const std::array<std::uint32_t, 8> words{static_cast<std::uint32_t>(base), static_cast<std::uint32_t>((base >> 32u) & 255u) | (*guest << 20u) | (((width - 1u) & 3u) << 30u), ((width - 1u) >> 2u) | ((height - 1u) << 14u), ((arrayed ? 15u : 14u) << 28u) | (27u << 20u) | (log << 16u) | 0xfacu, 0u, log << 4u, 0u, 0u};
    const auto keptBefore = recorder.OpenKeptBytes();
    const auto first = CachedSampledTexture(context, words);
    if (recorded) Require(recorder.OpenKeptBytes() >= keptBefore + first->AllocationBytes(), "native snapshot image allocation is missing from the retained memory budget");
    ClearTargetAttachment(context, view, target);
    const auto changed = ReadMultisampleTargetSamples(context, target);
    const auto second = CachedSampledTexture(context, words);
    if (recorded) Require(recorder.OpenKeptBytes() >= second->AllocationBytes(), "later native snapshot image is missing from the retained memory budget");
    ClearMultisampleTargets(context.device);
    recorder.Sync();
    AgcDriver::GuestMemory::CollectWritesUncached(target.address, target.bytes);
    Require(AgcDriver::GuestMemory::UnchangedSince(target.address, target.bytes, generation), "native GPU snapshot published guest sample bytes");
    Require(first.get() != second.get() && first->View() != second->View(), "native snapshots reused a live view");
    const auto check = [&](const Texture& texture, std::span<const std::byte> samples) {
        std::vector<std::array<std::uint32_t, 4>> expected(count);
        for (std::uint32_t element = 0u; element < count; ++element) {
            expected[element][3] = half || normalized ? 0x3f800000u : 1u;
            for (std::uint32_t component = 0u; component < (half || normalized ? 4u : std::max(bytes / 4u, 1u)); ++component) {
                if (half) {
                    std::uint16_t value = 0u;
                    std::memcpy(&value, samples.data() + element * bytes + component * 2u, 2u);
                    Require(value == 0u || (value >= 0x400u && value < 0x7c00u), "native half fixture requires nonnegative normal values");
                    expected[element][component] = value == 0u ? 0u : ((static_cast<std::uint32_t>(value & 0x7c00u) + 0x1c000u) << 13u) | ((value & 0x3ffu) << 13u);
                } else if (normalized) expected[element][component] = samples[element * bytes + component] == std::byte{0} ? 0u : 0x3f800000u;
                else std::memcpy(&expected[element][component], samples.data() + element * bytes + component * (bytes < 4u ? bytes : 4u), std::min(bytes, 4u));
            }
        }
        Buffer output(context, (count + 2u) * 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memset(output.Bytes().data(), 0xa5, output.Bytes().size());
        Fetch fetch(context, texture, output, FetchShader(width, height, fragments, arrayed, half || normalized));
        CommandBatch batch(context);
        fetch.Run(batch.Handle(), count);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        batch.SubmitAndWait();
        output.Invalidate();
        Require(std::memcmp(output.Bytes().data() + 16u, expected.data(), expected.size() * 16u) == 0, "native GPU snapshot lost samples, components or immutable source values");
        Require(std::all_of(output.Bytes().begin(), output.Bytes().begin() + 16u, [](auto byte) { return byte == std::byte{0xa5}; }) && std::all_of(output.Bytes().end() - 16u, output.Bytes().end(), [](auto byte) { return byte == std::byte{0xa5}; }), "native snapshot fetch changed output guards");
    };
    check(*first, original);
    check(*second, changed);
}

void CheckXorViews(Context context, VkFormat format, std::uint32_t fragments, std::uint32_t metadata, bool recorded) {
    const auto bytes = format == VK_FORMAT_R8_UINT ? 1u : format == VK_FORMAT_R16_UINT ? 2u : format == VK_FORMAT_R32_UINT ? 4u : format == VK_FORMAT_R32G32B32A32_UINT ? 16u : 8u;
    context.limits.maxStorageBufferRange = std::min(context.limits.maxStorageBufferRange, 131072u);
    context.limits.maxComputeWorkGroupCount[0] = 1u;
    const MultisampleTextureLayout block(TextureTileMode::kR64KBX, bytes, fragments, 1u, 1u, 1u);
    const VkExtent2D extent{block.BlockWidth() + 3u, block.BlockHeight() + 5u};
    const MultisampleTextureLayout layout(TextureTileMode::kR64KBX, bytes, fragments, extent.width, extent.height, 1u, 0x8000u);
    const MultisampleTextureLayout padded(TextureTileMode::kR64KBX, bytes, fragments, layout.Pitch(), layout.PaddedHeight(), 1u, 0x8000u);
    TargetMemory memory(static_cast<std::size_t>(layout.Bytes()) + 8u * 65536u);
    TextureDetiler detiler(context);
    context.detiler = &detiler;
    ColorTarget target{};
    target.address = target.surfaceAddress = memory.Address() + 65536u;
    target.extent = target.surfaceExtent = extent;
    target.bytes = layout.Bytes();
    target.format = format;
    target.samples = fragments;
    target.elementBytes = bytes;
    target.tileMode = ColorTileMode::RenderTarget;
    target.pipeBankXor = 0x8000u;
    auto* color = reinterpret_cast<std::byte*>(target.address);
    for (std::size_t index = 0u; index < target.bytes; ++index) color[index] = static_cast<std::byte>(((index * 37u) ^ (index >> 8u) ^ ((index >> 16u) * 11u)) & 255u);
    if (metadata != 0u) {
        target.cmaskAddress = target.address + target.bytes + 65536u;
        target.cmaskBytes = CmaskBytes(extent.width, extent.height);
        target.fmaskAddress = target.cmaskAddress + 65536u + 0x8000u;
        target.fmaskSwizzle = 0x18u;
        std::memset(reinterpret_cast<void*>(target.cmaskAddress), 0xcc, target.cmaskBytes);
        if (metadata == 1u) {
            target.dccAddress = target.address + target.bytes;
            target.dccPipeAligned = target.dccAlphaOnMsb = true;
            target.dccControl = 0x100038u;
            std::memset(reinterpret_cast<void*>(target.dccAddress), 0x40, MultisampleDccBytes_nid_no_patch(bytes, fragments, extent.width, extent.height, 1u));
        }
    }
    std::vector<std::byte> physical(color, color + target.bytes);
    if (metadata == 1u) {
        const std::array<std::uint16_t, 4> clear{0u, 0u, 0u, 0x3c00u};
        for (std::size_t offset = 0u; offset < physical.size(); offset += bytes) std::memcpy(physical.data() + offset, clear.data(), bytes);
    } else if (metadata == 2u) {
        std::vector<std::byte> expanded(padded.LinearBytes());
        padded.Detile(physical, expanded);
        for (std::size_t offset = 0u; offset < expanded.size(); offset += bytes * fragments) {
            for (std::uint32_t fragment = 1u; fragment < fragments; ++fragment) std::memcpy(expanded.data() + offset + fragment * bytes, expanded.data() + offset, bytes);
        }
        padded.Tile(expanded, physical);
    }
    std::vector<std::byte> original(layout.LinearBytes());
    layout.Detile(physical, original);
    Recorder recorder(context);
    if (recorded) recorder.Activate();
    struct Cleanup { Recorder& recorder; VkDevice device; ~Cleanup() { recorder.Sync(); ClearCachedTextures(device); ClearMultisampleTargets(device); } } cleanup{recorder, context.device};
    const auto view = MultisampleTargetView(context, target);
    Require(AgcDriver::GuestMemory::Watched(target.address, target.bytes), "XOR views require watched fixture memory");
    const auto generation = AgcDriver::GuestMemory::CollectWritesUncached(target.address, target.bytes);
    const auto untouched = std::vector<std::byte>(color, color + target.bytes);
    GuestTextureResource resource{};
    resource.baseAddress = target.address;
    resource.width = extent.width;
    resource.height = extent.height;
    resource.samples = fragments;
    resource.dimension = TextureDimension::k2DMsaa;
    resource.tileMode = TextureTileMode::kR64KBX;
    resource.pipeBankXor = target.pipeBankXor;
    resource.format = 20u;
    resource.mipCount = resource.allocatedMipCount = 1u;
    std::shared_ptr<Texture> snapshot;
    if (format == VK_FORMAT_R32_UINT) {
        snapshot = NativeMultisampleSnapshot_nid_no_patch(context, resource, {});
        Require(snapshot != nullptr, "XOR fixture did not acquire its prior native snapshot");
    }
    auto alias = target;
    alias.pipeBankXor = 0x4000u;
    if (metadata != 0u) alias.fmaskAddress = (target.fmaskAddress & ~std::uint64_t{65535u}) | 0x4000u;
    const auto kept = recorder.OpenKeptBytes();
    Require(MultisampleTargetView(context, alias) == view, "XOR transition replaced the retained attachment view");
    if (recorded) Require(recorder.OpenKeptBytes() >= kept + target.bytes, "XOR scratch buffer is missing from the recorder memory budget");
    Require(MultisampleTargetView(context, target) == view && MultisampleTargetView(context, alias) == view, "queued A-B-A-B transition replaced an attachment handle");
    const MultisampleTextureLayout other(TextureTileMode::kR64KBX, bytes, fragments, extent.width, extent.height, 1u, alias.pipeBankXor);
    std::vector<std::byte> expected(other.LinearBytes());
    other.Detile(physical, expected);
    Require(ReadMultisampleTargetSamples(context, alias) == expected, "XOR view lost current samples or exposed stale tile padding");
    AgcDriver::GuestMemory::CollectWritesUncached(target.address, target.bytes);
    Require(AgcDriver::GuestMemory::UnchangedSince(target.address, target.bytes, generation) && std::equal(untouched.begin(), untouched.end(), color), "XOR conversion published or overwrote guest sample bytes");
    Require(MultisampleTargetView(context, target) == view && ReadMultisampleTargetSamples(context, target) == original, "unmodified B-A transition lost original samples");
    ClearTargetAttachment(context, view, target, &alias);
    const auto rendered = ReadMultisampleTargetSamples(context, alias);
    other.Tile(rendered, physical);
    Require(MultisampleTargetView(context, target) == view, "modified B-A transition replaced an attachment view");
    layout.Detile(physical, expected);
    Require(ReadMultisampleTargetSamples(context, target) == expected, "B-A transition discarded writes or modified hidden physical samples");
    if (snapshot) {
        const auto count = extent.width * extent.height * fragments;
        Buffer output(context, (count + 2u) * 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memset(output.Bytes().data(), 0xa5, output.Bytes().size());
        Fetch fetch(context, *snapshot, output, FetchShader(extent.width, extent.height, fragments, false, false));
        CommandBatch batch(context);
        fetch.Run(batch.Handle(), count);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        batch.SubmitAndWait();
        output.Invalidate();
        for (std::uint32_t element = 0u; element < count; ++element) {
            const std::array<std::uint32_t, 4> value{[&] { std::uint32_t word; std::memcpy(&word, original.data() + element * 4u, 4u); return word; }(), 0u, 0u, 1u};
            Require(std::memcmp(output.Bytes().data() + (element + 1u) * 16u, value.data(), 16u) == 0, "XOR transition modified a prior native snapshot");
        }
        Require(std::all_of(output.Bytes().begin(), output.Bytes().begin() + 16u, [](auto byte) { return byte == std::byte{0xa5}; }) && std::all_of(output.Bytes().end() - 16u, output.Bytes().end(), [](auto byte) { return byte == std::byte{0xa5}; }), "XOR snapshot fetch changed buffer guards");
    }
    std::array<std::byte, 1> probe{};
    AgcDriver::GuestMemory::Read(target.address + target.bytes - 1u, probe);
    Require(std::equal(physical.begin(), physical.end(), color), "XOR publication lost modified physical bytes or tile padding");
    Require(std::all_of(memory.Data(), memory.Data() + 65536u, [](auto byte) { return byte == std::byte{0xa5}; }) && std::all_of(color + target.bytes + 6u * 65536u, color + target.bytes + 7u * 65536u, [](auto byte) { return byte == std::byte{0xa5}; }), "XOR publication overwrote guest allocation guards");
    ClearMultisampleTargets(context.device);
    const auto reimportedView = MultisampleTargetView(context, alias);
    other.Detile(physical, expected);
    Require(ReadMultisampleTargetSamples(context, alias) == expected, "reimport of XOR-converted allocation lost sample values");
    auto changedClear = alias;
    changedClear.clearWords[0] ^= 1u;
    changedClear.cmaskFastClear = !changedClear.cmaskFastClear;
    Require(MultisampleTargetView(context, changedClear) == reimportedView && ReadMultisampleTargetSamples(context, changedClear) == expected, "clear-register changes replaced the resident allocation or changed samples without a tracked clear");
    Require(MultisampleTargetView(context, alias) == reimportedView && ReadMultisampleTargetSamples(context, alias) == expected, "restoring clear registers changed resident samples");
    for (std::uint32_t field = 0u; field < 8u; ++field) {
        auto invalid = alias;
        switch (field) {
            case 0u: invalid.address += 256u; break;
            case 1u: invalid.extent.width += 1u; break;
            case 2u: invalid.samples = fragments == 8u ? 4u : 8u; break;
            case 3u: invalid.fmaskAddress = invalid.fmaskAddress == 0u ? target.address + target.bytes : invalid.fmaskAddress + 65536u; break;
            case 4u: invalid.dccControl ^= 256u; break;
            case 5u: invalid.arraySlice = 1u; break;
            case 6u: invalid.elementBytes *= 2u; break;
            case 7u: invalid.bytes += 65536u; break;
        }
        TargetRefuses([&] { MultisampleTargetView(context, invalid); }, "layout or metadata changed");
    }
    auto invalid = alias;
    invalid.pipeBankXor |= 0x1000u;
    TargetRefuses([&] { MultisampleTargetView(context, invalid); }, "XOR tag");
    if (metadata != 0u) {
        invalid = alias;
        invalid.fmaskAddress |= 0x1000u;
        TargetRefuses([&] { MultisampleTargetView(context, invalid); }, "FMASK XOR tag");
    }
    Require(ReadMultisampleTargetSamples(context, alias) == expected, "refused transition modified resident contents");
    recorder.Activate();
    recorder.Commands();
    recorder.NotePendingWrite(target.address, 4u);
    TargetRefuses([&] { MultisampleTargetView(context, target); }, "external GPU writes");
    recorder.Sync();
    color[target.bytes - 1u] ^= std::byte{1};
    TargetRefuses([&] { MultisampleTargetView(context, target); }, "guest writes");
    std::printf("XOR views bytes=%u fragments=%u metadata=%u recorded=%u preserved image, queued conversions, physical padding, publication and refusals\n", bytes, fragments, metadata, recorded ? 1u : 0u);
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
    descriptor.samples = fragments;
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


bool CheckTileTransfer(Context context, std::uint32_t bytes, std::uint32_t fragments, bool chunks) {
    const MultisampleTextureLayout block(TextureTileMode::kR64KBX, bytes, fragments, 1u, 1u, 1u);
    const auto width = block.BlockWidth() * 3u, height = block.BlockHeight() * 2u;
    constexpr std::uint64_t surfaceBytes = 6u * 65536u;
    const auto count = static_cast<std::uint32_t>(surfaceBytes / bytes);
    const auto format = TextureDetiler::ImageElementFormat(bytes);
    VkImageFormatProperties supported{};
    if (context.imageFormatProperties(context.physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT, 0u, &supported) != VK_SUCCESS || (supported.sampleCounts & fragments) == 0u) return false;
    context.limits.maxComputeWorkGroupCount[0] = 1u;
    context.limits.maxStorageBufferRange = 2u * 65536u;
    TextureDetiler detiler(context);
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1u};
    info.mipLevels = info.arrayLayers = 1u;
    info.samples = static_cast<VkSampleCountFlagBits>(fragments);
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkImage image = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &image), "vkCreateImage tile test");
    OwnedImage owned(context, image, VK_NULL_HANDLE);
    VkMemoryRequirements requirements{};
    context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &owned.memory), "vkAllocateMemory tile test");
    Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, owned.memory, 0u), "vkBindImageMemory tile test");
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
    VkImageView view = VK_NULL_HANDLE;
    owned.transferViews.reserve(1u);
    Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView tile test");
    owned.transferViews.push_back(view);
    const auto align = std::max<std::uint64_t>(65536u, context.limits.minStorageBufferOffsetAlignment);
    Buffer input(context, align + surfaceBytes + align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer raw(context, input.Bytes().size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer output(context, input.Bytes().size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const auto tileChunk = chunks ? 65536u : 131072u;
    if (bytes == 4u && fragments == 2u && !chunks) {
        CommandBatch validation(context);
        const auto transfer = [&](const Context& candidate, std::uint32_t element, std::uint32_t samples, VkCommandBuffer commands, std::uint64_t start, std::uint64_t length, std::uint32_t imageWidth, std::uint32_t imageHeight, std::uint32_t first, std::uint32_t xorBytes) {
            TextureDetiler checked(candidate);
            checked.DispatchMultisampleTile(commands, element, samples, raw.Handle(), start, length, view, imageWidth, imageHeight, first, xorBytes, false);
        };
        for (std::uint32_t disabledFlag = 0u; disabledFlag < 3u; ++disabledFlag) {
            auto disabled = context;
            if (disabledFlag == 0u) disabled.storageImageMultisample = false;
            if (disabledFlag == 1u) disabled.storageImageReadWithoutFormat = false;
            if (disabledFlag == 2u) disabled.storageImageWriteWithoutFormat = false;
            TargetRefuses([&] { transfer(disabled, bytes, fragments, validation.Handle(), align, 65536u, width, height, 0u, 0u); }, "does not enable");
        }
        TargetRefuses([&] { transfer(context, bytes, fragments, VK_NULL_HANDLE, align, 65536u, width, height, 0u, 0u); }, "requires commands");
        TargetRefuses([&] { transfer(context, 3u, fragments, validation.Handle(), align, 65536u, width, height, 0u, 0u); }, "element size");
        TargetRefuses([&] { transfer(context, bytes, 3u, validation.Handle(), align, 65536u, width, height, 0u, 0u); }, "fragment count");
        TargetRefuses([&] { transfer(context, bytes, fragments, validation.Handle(), align, 65536u, width - 1u, height, 0u, 0u); }, "padded image");
        TargetRefuses([&] { transfer(context, bytes, fragments, validation.Handle(), align, 65532u, width, height, 0u, 0u); }, "buffer range");
        TargetRefuses([&] { transfer(context, bytes, fragments, validation.Handle(), align + 1u, 65536u, width, height, 0u, 0u); }, "buffer range");
        TargetRefuses([&] { transfer(context, bytes, fragments, validation.Handle(), align, 65536u, width, height, 6u, 0u); }, "buffer range");
        TargetRefuses([&] { transfer(context, bytes, fragments, validation.Handle(), align, 65536u, width, height, 0u, 0x1000u); }, "outside its 16-pipe");
    }
    for (std::uint32_t round = 0u; round < 2u; ++round) {
        std::memset(input.Bytes().data(), 0xa5, input.Bytes().size());
        std::memset(raw.Bytes().data(), 0xa5, raw.Bytes().size());
        std::memset(output.Bytes().data(), 0xa5, output.Bytes().size());
        for (std::uint32_t index = 0u; index < surfaceBytes; ++index) input.Bytes()[align + index] = static_cast<std::byte>((index * 137u + (index >> 8u) * 29u + round * 71u) & 255u);
        const auto savedInput = std::vector(input.Bytes().begin(), input.Bytes().end());
        std::vector<std::byte> expectedRaw(raw.Bytes().size(), std::byte{0xa5}), expectedOutput(output.Bytes().size(), std::byte{0xa5});
        const auto oldXor = round == 0u ? 0x8000u : 0xcf00u;
        const auto newXor = round == 0u ? 0x4000u : 0x8c00u;
        const MultisampleTextureLayout oldLayout(TextureTileMode::kR64KBX, bytes, fragments, width, height, 1u, oldXor), newLayout(TextureTileMode::kR64KBX, bytes, fragments, width, height, 1u, newXor);
        oldLayout.Tile(input.Bytes().subspan(align, surfaceBytes), std::span(expectedRaw).subspan(align, surfaceBytes));
        newLayout.Detile(std::span(expectedRaw).subspan(align, surfaceBytes), std::span(expectedOutput).subspan(align, surfaceBytes));
        detiler.BeginBatch();
        CommandBatch batch(context);
        VkImageMemoryBarrier transition{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        transition.srcAccessMask = round == 0u ? 0u : VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        transition.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        transition.oldLayout = round == 0u ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
        transition.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        transition.srcQueueFamilyIndex = transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        transition.image = image;
        transition.subresourceRange = viewInfo.subresourceRange;
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(batch.Handle(), round == 0u ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &transition);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        for (std::uint32_t offset = 0u; offset < surfaceBytes; offset += 131072u) detiler.DispatchMultisampleImage(batch.Handle(), bytes, input.Handle(), align + offset, 131072u, view, width, height, 1u, fragments, offset / bytes, 131072u / bytes);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        for (std::uint32_t offset = 0u; offset < surfaceBytes; offset += tileChunk) detiler.DispatchMultisampleTile(batch.Handle(), bytes, fragments, raw.Handle(), align + offset, tileChunk, view, width, height, offset / 65536u, oldXor, true);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        for (std::uint32_t offset = 0u; offset < surfaceBytes; offset += tileChunk) detiler.DispatchMultisampleTile(batch.Handle(), bytes, fragments, raw.Handle(), align + offset, tileChunk, view, width, height, offset / 65536u, newXor, false);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        for (std::uint32_t offset = 0u; offset < surfaceBytes; offset += 131072u) detiler.DispatchMultisampleReadback(batch.Handle(), bytes, output.Handle(), align + offset, 131072u, view, width, height, 1u, fragments, offset / bytes, 131072u / bytes);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        batch.SubmitAndWait();
        raw.Invalidate();
        output.Invalidate();
        Require(std::ranges::equal(raw.Bytes(), expectedRaw), "MS tile retile changed physical bytes or guards");
        Require(std::ranges::equal(output.Bytes(), expectedOutput), "MS tile reinterpretation changed pixels, fragments, components or guards");
        Require(std::ranges::equal(input.Bytes(), savedInput), "MS tile transfer changed its source");
    }
    std::printf("MS tile bytes=%u fragments=%u chunks=%u two XOR conversions exact physical bytes/samples/guards\n", bytes, fragments, chunks);
    return true;
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
    descriptor.samples = 8u;
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
    try { const auto storage = StorageTexture::Create(context, detiler, descriptor, 0u); }
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
            Require(bindings.bindings.front().descriptor.imageShape == (dimension == RdnaImageDimension::Dim2DMsaa ? DescriptorImageShape::Image2DMultisample : DescriptorImageShape::Image2DMultisampleArray), "MS physical descriptor shape differs");
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
        if (argc > 1 && std::string(argv[1]) == "--xor-views") {
            device->PrintDevice();
            for (const auto format : {VK_FORMAT_R8_UINT, VK_FORMAT_R16_UINT, VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32A32_UINT}) {
                for (const auto fragments : {2u, 4u, 8u}) {
                    if (!TargetSupported(context, format, fragments)) { unavailable += 2u; continue; }
                    for (const bool recorded : {false, true}) { CheckXorViews(context, format, fragments, 0u, recorded); ++checked; }
                }
            }
            for (const auto fragments : {2u, 4u, 8u}) {
                if (!TargetSupported(context, VK_FORMAT_R16G16B16A16_SFLOAT, fragments)) { unavailable += 4u; continue; }
                for (const auto metadata : {1u, 2u}) {
                    for (const bool recorded : {false, true}) { CheckXorViews(context, VK_FORMAT_R16G16B16A16_SFLOAT, fragments, metadata, recorded); ++checked; }
                }
            }
            Require(checked != 0u && (unavailable == 0u || std::getenv("ANYPS5_REQUIRE_MSAA_ALL") == nullptr), "required XOR view configurations are unsupported");
            std::printf("XOR views checked=%u unavailable=%u\n", checked, unavailable);
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "--native-snapshots") {
            for (const auto format : {VK_FORMAT_R8_UINT, VK_FORMAT_R16_UINT, VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB}) {
                for (const auto fragments : {2u, 4u, 8u}) {
                    if (!TargetSupported(context, format, fragments)) { unavailable += 4u; continue; }
                    for (const bool arrayed : {false, true}) {
                        for (const bool recorded : {false, true}) { CheckNativeSnapshot(context, format, fragments, arrayed, recorded); ++checked; }
                    }
                }
            }
            Require(checked != 0u && (unavailable == 0u || std::getenv("ANYPS5_REQUIRE_MSAA_ALL") == nullptr), "required native GPU snapshot configurations are unsupported");
            std::printf("Native GPU snapshots checked=%u unavailable=%u\n", checked, unavailable);
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "--depth-retirement") {
            device->PrintDevice();
            for (const auto format : {VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT}) {
                for (const auto fragments : {2u, 4u, 8u}) {
                    Require(DepthSupported(context, format, fragments), "native depth retirement fixture requires every depth configuration");
                    for (std::uint32_t mode = 0u; mode < 4u; ++mode) CheckDepthRetirement(context, format, fragments, mode);
                }
            }
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "--retirement") {
            device->PrintDevice();
            Require(context.storageImageReadWithoutFormat && TargetSupported(context, VK_FORMAT_R8G8B8A8_UNORM, 4u), "native retirement fixture requires four-sample formatless transfers");
            for (const bool remove : {false, true}) {
                for (const bool metadata : {false, true}) CheckTargetRetirement(context, remove, metadata);
            }
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "--metadata-reset") {
            for (const auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT}) {
                for (const auto fragments : {2u, 4u, 8u}) {
                    if (!TargetSupported(context, format, fragments)) { unavailable += 4u; continue; }
                    CheckMetadataReset(context, format, fragments);
                    checked += 4u;
                }
            }
            Require(checked != 0u && (unavailable == 0u || std::getenv("ANYPS5_REQUIRE_MSAA_ALL") == nullptr), "required metadata reset configurations are unsupported");
            std::printf("Metadata reset checked=%u unavailable=%u\n", checked, unavailable);
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "--depth-refusals") {
            for (const auto fragments : {2u, 4u, 8u}) {
                if (DepthSupported(context, VK_FORMAT_D32_SFLOAT, fragments)) { CheckDepthRefusals(context, fragments); return 0; }
            }
            Require(false, "no supported native depth refusal configuration");
        }
        if (argc > 1 && std::string(argv[1]) == "--depth-clear-partial") {
            Require(DepthSupported(context, VK_FORMAT_D32_SFLOAT, 4u), "partial clear fixture requires sampled four-fragment D32");
            TargetRefuses([&] { CheckDepthTarget(context, VK_FORMAT_D32_SFLOAT, 4u, false, true, false, true); }, "complete tracked HTILE store");
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "--depth-clears") {
            for (const auto format : {VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT}) {
                for (const auto fragments : {2u, 4u, 8u}) {
                    if (!DepthSupported(context, format, fragments)) { unavailable += 4u; continue; }
                    for (const bool depthOnly : {false, true}) {
                        for (const bool one : {false, true}) { CheckDepthTarget(context, format, fragments, one, true, depthOnly); ++checked; }
                    }
                }
            }
            Require(checked != 0u && (unavailable == 0u || std::getenv("ANYPS5_REQUIRE_MSAA_ALL") == nullptr), "required native depth clear configurations are unsupported");
            std::printf("Native depth clear checked=%u unavailable=%u\n", checked, unavailable);
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "--depth-targets") {
            for (const auto format : {VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT}) {
                for (const auto fragments : {2u, 4u, 8u}) {
                    if (!DepthSupported(context, format, fragments)) { unavailable += 2u; continue; }
                    for (const bool one : {false, true}) { CheckDepthTarget(context, format, fragments, one); ++checked; }
                }
            }
            Require(checked != 0u && (unavailable == 0u || std::getenv("ANYPS5_REQUIRE_MSAA_ALL") == nullptr), "required native depth configurations are unsupported");
            std::printf("Native depth checked=%u unavailable=%u\n", checked, unavailable);
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "--targets") {
#ifdef _WIN32
            CheckUnwatchedTargetDiagnostic(context);
#endif
            CheckTrackedFillProof(context);
            const std::array formats{VK_FORMAT_R8_UINT, VK_FORMAT_R16_UINT, VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32A32_UINT};
            for (std::uint32_t index = 0; index < formats.size(); ++index) {
                for (const auto fragments : {2u, 4u, 8u}) {
                    if (!TargetSupported(context, formats[index], fragments)) { unavailable += 2u; continue; }
                    for (const bool chunks : {false, true}) { CheckTargetImport(context, 1u << index, fragments, formats[index], false, chunks); ++checked; }
                }
            }
            for (const auto fragments : {2u, 4u, 8u}) {
                if (!TargetSupported(context, VK_FORMAT_R16G16B16A16_SFLOAT, fragments)) { unavailable += 2u; continue; }
                for (const bool chunks : {false, true}) { CheckTargetImport(context, 8u, fragments, VK_FORMAT_R16G16B16A16_SFLOAT, true, chunks); ++checked; }
            }
            for (const auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB}) {
                for (const auto fragments : {2u, 4u, 8u}) {
                    if (!TargetSupported(context, format, fragments)) { unavailable += 2u; continue; }
                    for (const bool chunks : {false, true}) { CheckTargetImport(context, 4u, fragments, format, false, chunks); ++checked; }
                }
            }
            Require(checked != 0u, "device advertises no supported multisample target imports");
            Require(unavailable == 0u || std::getenv("ANYPS5_REQUIRE_MSAA_ALL") == nullptr, "all multisample target cases were required but some are unsupported");
            std::printf("MS target import checked=%u unavailable=%u\n", checked, unavailable);
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "--tile-transfer") {
            device->PrintDevice();
            for (const auto bytes : {1u, 2u, 4u, 8u, 16u}) {
                for (const auto fragments : {2u, 4u, 8u}) {
                    for (const bool chunks : {false, true}) {
                        if (CheckTileTransfer(context, bytes, fragments, chunks)) ++checked;
                        else ++unavailable;
                    }
                }
            }
            Require(checked != 0u, "device advertises no supported MS tile transfer cases");
            Require(unavailable == 0u || std::getenv("ANYPS5_REQUIRE_MSAA_ALL") == nullptr, "all MS tile transfer cases were required but some are unsupported");
            std::printf("MS tile transfer checked=%u unavailable=%u\n", checked, unavailable);
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
