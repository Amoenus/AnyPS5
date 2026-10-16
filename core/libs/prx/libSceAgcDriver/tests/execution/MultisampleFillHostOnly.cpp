#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/BufferFill.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTarget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleDcc.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleInitialContents.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTextureLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace AgcDriver::DriverDetail {

struct DriverTestAccess {
    static std::shared_ptr<VulkanDevice> DeviceOf(Driver& driver) { return driver.device.Load(); }
};

}

namespace {

using AgcDriver::Graphics::ColorTarget;
using AgcDriver::Graphics::Require;

constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t Side = 64;
constexpr std::byte Marker{0xab};

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::byte*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, BlockBytes));
        GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
        block = static_cast<std::byte*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "multisampled fill: cannot allocate a guest block");
        std::memset(block, 0, BlockBytes);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::byte* Data() { return block; }

    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)); }

    bool Holds(std::byte value, std::size_t offset, std::size_t bytes) const {
        return std::all_of(block + offset, block + offset + bytes, [&](std::byte held) { return held == value; });
    }

private:
    std::byte* block = nullptr;
};

GuestBlock& NewBlock() {
    static auto* blocks = new std::deque<GuestBlock>();
    return blocks->emplace_back();
}

std::uint32_t Low(std::uint64_t value) { return static_cast<std::uint32_t>(value); }

std::uint32_t High(std::uint64_t value) { return static_cast<std::uint32_t>(value >> 32u); }

std::vector<std::uint32_t> Pm4Packet(std::uint32_t opcode, std::initializer_list<std::uint32_t> body) {
    std::vector<std::uint32_t> words{0xc0000000u | (static_cast<std::uint32_t>(body.size() - 1) << 16u) | (opcode << 8u)};
    words.insert(words.end(), body);
    return words;
}

void Submit(std::initializer_list<std::vector<std::uint32_t>> packets) {
    std::vector<std::uint32_t> words;
    for (const auto& packet : packets) words.insert(words.end(), packet.begin(), packet.end());
    Packet dcb{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&dcb) == 0, "multisampled fill: DCB submission failed");
    AgcDriverWaitIdle_nid_postfix();
}

constexpr std::array<std::uint32_t, 9> FillKernel{0xd7460004u, 0x04010c08u, 0x7e000204u, 0x7e020205u, 0x7e040206u, 0x7e060207u, 0xe01c2000u, 0x80000004u, 0xbf810000u};

void DispatchFill(const GuestBlock& code, std::uint64_t base, std::uint32_t bytes, std::uint32_t value) {
    const auto kernel = code.Address();
    Submit({
        Pm4Packet(0x76, {0x20c, Low(kernel >> 8u)}),
        Pm4Packet(0x76, {0x20d, High(kernel >> 8u) & 0xffu}),
        Pm4Packet(0x76, {0x207, 64}),
        Pm4Packet(0x76, {0x208, 1}),
        Pm4Packet(0x76, {0x209, 1}),
        Pm4Packet(0x76, {0x213, (8u << 1u) | 0x80u}),
        Pm4Packet(0x76, {0x240, Low(base)}),
        Pm4Packet(0x76, {0x241, (High(base) & 0xffffu) | (16u << 16u)}),
        Pm4Packet(0x76, {0x242, bytes / 16u}),
        Pm4Packet(0x76, {0x243, 0x4bfacu}),
        Pm4Packet(0x76, {0x244, value}),
        Pm4Packet(0x76, {0x245, value}),
        Pm4Packet(0x76, {0x246, value}),
        Pm4Packet(0x76, {0x247, value}),
        Pm4Packet(0x15, {bytes / 1024u, 1, 1, 0x41}),
    });
}

void Settle(const GuestBlock& code, const GuestBlock& sentinel) {
    static std::uint32_t round = 0;
    ++round;
    DispatchFill(code, sentinel.Address(), 1024, round);
    std::array<std::uint32_t, 4> stored{};
    AgcDriver::GuestMemory::Read(sentinel.Address(), std::as_writable_bytes(std::span(stored)), 4);
    Require(stored == std::array<std::uint32_t, 4>{round, round, round, round}, "the sentinel fill did not land");
}

ColorTarget MultisampledTarget(const GuestBlock& color) {
    ColorTarget target{};
    target.address = color.Address();
    target.surfaceAddress = target.address;
    target.extent = {Side, Side};
    target.surfaceExtent = target.extent;
    target.format = VK_FORMAT_R8G8B8A8_UNORM;
    target.tileMode = AgcDriver::Graphics::ColorTileMode::RenderTarget;
    target.elementBytes = 4;
    target.bytes = BlockBytes;
    target.samples = 4;
    return target;
}

#ifdef _WIN32
enum class NeighborTransition { None, Alias, HostImport, TargetHostImport, CpuStore };

void DispatchConstantFill(const GuestBlock& code, std::uint64_t address, std::uint32_t bytes, std::uint32_t value, std::uint32_t groups, std::uint32_t flags = 0x41u) {
    Submit({
        Pm4Packet(0x76, {0x20c, Low(code.Address() >> 8u)}),
        Pm4Packet(0x76, {0x20d, High(code.Address() >> 8u) & 0xffu}),
        Pm4Packet(0x76, {0x207, 64u, 1u, 1u}),
        Pm4Packet(0x76, {0x213, (6u << 1u) | 0x80u}),
        Pm4Packet(0x76, {0x240, 0u, 0u, Low(address), High(address), bytes - 16u, value}),
        Pm4Packet(0x15, {groups, 1u, 1u, flags}),
    });
}

void NativeMetadataClearViaDriver(AgcDriver::VulkanDevice& device, VkFormat format, std::uint32_t fragments) {
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    const std::uint32_t elementBytes = format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u : 4u;
    const AgcDriver::Graphics::MultisampleTextureLayout layout(AgcDriver::Graphics::TextureTileMode::kR64KBX, elementBytes, fragments, Side, Side, 1u, 0u);
    const auto colorBytes = static_cast<std::size_t>(layout.Bytes());
    const auto residentBytes = colorBytes + 3u * BlockBytes;
    auto* memory = static_cast<std::byte*>(GuestArena::GuestArenaAllocate_nid_postfix(residentBytes, BlockBytes));
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, residentBytes, nullptr);
    Require(memory != nullptr && section != nullptr, "native metadata submit: cannot allocate a private resident run");
    struct Cleanup {
        AgcDriver::VulkanDevice& device;
        VkDevice handle;
        std::byte* memory;
        HANDLE section;
        std::size_t bytes;
        ~Cleanup() {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            device.SubmitRecorded(true);
            device.WaitIdle();
            AgcDriver::Graphics::ClearMultisampleTargets(handle);
            AgcDriver::Graphics::ClearHostImports(handle);
            GuestAllocations::Mutation().Remove(memory);
            GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
            GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
            GuestArena::GuestArenaForgetSection_nid_postfix(section);
            CloseHandle(section);
        }
    } cleanup{device, context.device, memory, section, residentBytes};
    GuestArena::GuestArenaMapDirect_nid_postfix(memory, residentBytes, section, 0u, PAGE_READWRITE);
    GuestAllocations::Mutation().Add(memory, residentBytes, true, true, true);
    std::cout << "native metadata submit: resident base=0x" << std::hex << reinterpret_cast<std::uintptr_t>(memory) << " bytes=0x" << residentBytes << " color-bytes=0x" << colorBytes << std::dec << " color-watched=" << AgcDriver::GuestMemory::Watched(reinterpret_cast<std::uintptr_t>(memory), colorBytes) << '\n' << std::flush;
    auto& code = NewBlock();
    auto& settleCode = NewBlock();
    auto& sentinel = NewBlock();
    auto& control = NewBlock();
    std::memset(memory, 0x40, colorBytes);
    std::memset(memory + colorBytes, 0xff, BlockBytes);
    std::memset(memory + colorBytes + BlockBytes, fragments == 2u ? 0xdd : fragments == 4u ? 0xee : 0xff, BlockBytes);
    std::fill_n(reinterpret_cast<std::uint32_t*>(memory + colorBytes + 2u * BlockBytes), BlockBytes / 4u, AgcDriver::Graphics::MultisampleExpandedFmask(fragments));
    std::memcpy(code.Data(), AgcDriver::DriverDetail::ConstantFillKernel.data(), sizeof(AgcDriver::DriverDetail::ConstantFillKernel));
    std::memcpy(settleCode.Data(), FillKernel.data(), sizeof(FillKernel));
    for (const auto count : {16u, 1024u, 1040u, static_cast<std::uint32_t>(BlockBytes)}) {
        constexpr std::uint32_t pattern = 0x12345678u;
        std::memset(control.Data(), static_cast<int>(Marker), BlockBytes);
        DispatchConstantFill(code, control.Address(), count, pattern, (count + 1023u) / 1024u + 1u);
        Settle(settleCode, sentinel);
        std::vector<std::uint32_t> stored(BlockBytes / 4u);
        AgcDriver::GuestMemory::Read(control.Address(), std::as_writable_bytes(std::span(stored)), 4u);
        for (std::size_t index = 0u; index < stored.size(); ++index) Require(stored[index] == (index < count / 4u ? pattern : 0xababababu), "native metadata submit: constant fill changed its requested bytes or the untouched suffix");
    }
    std::memset(control.Data(), static_cast<int>(Marker), BlockBytes);
    DispatchConstantFill(code, control.Address(), 16u, 0x12345678u, 17u, 0x61u);
    Settle(settleCode, sentinel);
    std::vector<std::uint32_t> threadMode(BlockBytes / 4u);
    AgcDriver::GuestMemory::Read(control.Address(), std::as_writable_bytes(std::span(threadMode)), 4u);
    for (std::size_t index = 0u; index < threadMode.size(); ++index) Require(threadMode[index] == (index < 4u ? 0x12345678u : 0xababababu), "native metadata submit: the GPU thread-count form changed its requested bytes or untouched suffix");
    std::cout << "native metadata submit: constant-fill plain-buffer bounds verified\n" << std::flush;
    std::cout << "native metadata submit: after plain-buffer control color-watched=" << AgcDriver::GuestMemory::Watched(reinterpret_cast<std::uintptr_t>(memory), colorBytes) << " imported=" << AgcDriver::Graphics::HostImportCovers(context, reinterpret_cast<std::uintptr_t>(memory), colorBytes) << '\n' << std::flush;
    ColorTarget target{};
    target.address = target.surfaceAddress = reinterpret_cast<std::uintptr_t>(memory);
    target.extent = target.surfaceExtent = {Side, Side};
    target.format = format;
    target.tileMode = AgcDriver::Graphics::ColorTileMode::RenderTarget;
    target.elementBytes = elementBytes;
    target.bytes = colorBytes;
    target.samples = fragments;
    target.dccAddress = target.address + colorBytes;
    target.cmaskAddress = target.dccAddress + BlockBytes;
    target.fmaskAddress = target.cmaskAddress + BlockBytes;
    target.dccPipeAligned = target.dccAlphaOnMsb = true;
    target.dccControl = 0x100038u;
    target.cmaskBytes = AgcDriver::Graphics::CmaskBytes(Side, Side);
    target.fmaskSwizzle = 0x18u;
    const auto bytes = AgcDriver::Graphics::MultisampleDccBytes_nid_no_patch(elementBytes, fragments, Side, Side, 1u);
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        Require(AgcDriver::Graphics::MultisampleTargetView(context, target) != VK_NULL_HANDLE, "native metadata submit: no initial native view");
        std::cout << "native metadata submit: after initial native view color-watched=" << AgcDriver::GuestMemory::Watched(target.address, colorBytes) << '\n' << std::flush;
        const auto samples = AgcDriver::Graphics::ReadMultisampleTargetSamples(context, target);
        Require(!samples.empty() && std::all_of(samples.begin(), samples.end(), [](auto value) { return value == std::byte{0x40}; }), "native metadata submit: initial native samples differ");
    }
    Require(AgcDriver::GuestMemory::Watched(target.address, colorBytes), "native metadata submit: color untracked before driver submission");
    std::cout << "native metadata submit: initial samples and watch verified\n" << std::flush;
    for (const auto value : {0xc0u, 0u, 0x40u, 0x80u}) {
        DispatchConstantFill(code, target.dccAddress, static_cast<std::uint32_t>(bytes), value * 0x01010101u, static_cast<std::uint32_t>((bytes + 1023u) / 1024u));
        Require(AgcDriver::GuestMemory::Watched(target.address, colorBytes), "native metadata submit: driver clear removed the color watch");
        Settle(settleCode, sentinel);
        Require(AgcDriver::GuestMemory::Watched(target.address, colorBytes), "native metadata submit: settling removed the color watch");
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        const auto samples = AgcDriver::Graphics::ReadMultisampleTargetSamples(context, target);
        Require(samples.size() == Side * Side * fragments * elementBytes, "native metadata submit: native sample count changed");
        const auto channelBytes = elementBytes / 4u;
        for (std::size_t index = 0u; index < samples.size() / channelBytes; ++index) {
            const auto mask = index % 4u == 3u ? 0x40u : 0x80u;
            std::uint16_t actual = 0u;
            std::memcpy(&actual, samples.data() + index * channelBytes, channelBytes);
            const std::uint16_t expected = (value & mask) == 0u ? 0u : channelBytes == 2u ? 0x3c00u : 0xffu;
            Require(actual == expected, "native metadata submit: known clear did not reach every native sample");
        }
    }
    *reinterpret_cast<volatile std::byte*>(memory + colorBytes) = std::byte{0x80};
    std::string refusal;
    try {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        static_cast<void>(AgcDriver::Graphics::ReadMultisampleTargetSamples(context, target));
    } catch (const std::runtime_error& error) {
        refusal = error.what();
    }
    Require(refusal.find("guest writes to a resident multisample target") != std::string::npos, "native metadata submit: same-value CPU metadata store was accepted");
    std::cout << "native metadata submit: four known DCC clears reached every sample, format=" << format << " fragments=" << fragments << "; same-value CPU store refused\n";
}

void ResidentColorNeighborTransition(AgcDriver::VulkanDevice& device, NeighborTransition transition) {
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    constexpr std::size_t bytes = 2 * BlockBytes;
    auto* memory = static_cast<std::byte*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, BlockBytes));
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, bytes, nullptr);
    Require(memory != nullptr && section != nullptr, "MSAA neighbor: cannot allocate a resident run");
    struct Cleanup {
        AgcDriver::VulkanDevice& device;
        VkDevice handle;
        std::byte* memory;
        HANDLE section;
        void* alias = nullptr;
        ~Cleanup() {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            device.SubmitRecorded(true);
            device.WaitIdle();
            AgcDriver::Graphics::ClearMultisampleTargets(handle);
            AgcDriver::Graphics::ClearHostImports(handle);
            if (alias != nullptr) GuestArena::GuestArenaUnmapAlias_nid_postfix(alias);
            GuestAllocations::Mutation().Remove(memory);
            GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
            GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
            GuestArena::GuestArenaForgetSection_nid_postfix(section);
            CloseHandle(section);
        }
    } cleanup{device, context.device, memory, section};
    std::unique_lock lock(AgcDriver::GuestMemory::GpuMutex());
    GuestArena::GuestArenaMapDirect_nid_postfix(memory, bytes, section, 0u, PAGE_READWRITE);
    std::memset(memory, 0, bytes);
    GuestAllocations::Mutation().Add(memory, bytes, true, true, true);
    const auto base = reinterpret_cast<std::uintptr_t>(memory);
    std::uintptr_t start = 0u, end = 0u;
    Require(GuestArena::GuestArenaResidentSpan_nid_postfix(base, bytes, &start, &end) && start == base && end == base + bytes, "MSAA neighbor: the fixture is not one private resident run");
    ColorTarget target{};
    target.address = base;
    target.surfaceAddress = base;
    target.extent = {Side, Side};
    target.surfaceExtent = target.extent;
    target.format = VK_FORMAT_R8G8B8A8_UNORM;
    target.tileMode = AgcDriver::Graphics::ColorTileMode::RenderTarget;
    target.elementBytes = 4u;
    target.bytes = BlockBytes;
    target.samples = 4u;
    Require(AgcDriver::Graphics::MultisampleTargetView(context, target) != VK_NULL_HANDLE, "MSAA neighbor: initial target has no native view");
    device.SubmitRecorded(true);
    device.WaitIdle();
    Require(AgcDriver::Graphics::MultisampleTargetView(context, target) != VK_NULL_HANDLE, "MSAA neighbor: baseline target reuse failed");
    const char* label = "none";
    if (transition == NeighborTransition::Alias) {
        label = "alias";
        cleanup.alias = GuestArena::GuestArenaMapAlias_nid_postfix(base, bytes);
        Require(cleanup.alias != nullptr, "MSAA neighbor: the explicit whole-run alias was not created");
    } else if (transition == NeighborTransition::HostImport) {
        label = "host-import";
        const auto* imported = AgcDriver::Graphics::HostImportFor(context, base + BlockBytes, BlockBytes);
        Require(imported != nullptr && imported->alias == nullptr, "MSAA neighbor: the private import was not created");
        Require(AgcDriver::GuestMemory::Watched(base, BlockBytes), "MSAA neighbor: the private import removed the native color watch");
        std::cout << "MSAA neighbor host-import private=1 native-watched=1 neighbor-compared=" << imported->unwatched << '\n';
        lock.unlock();
        auto& code = NewBlock();
        std::memcpy(code.Data(), FillKernel.data(), sizeof(FillKernel));
        auto& sentinel = NewBlock();
        constexpr std::uint32_t pattern = 0x12345678u;
        DispatchFill(code, base + BlockBytes, BlockBytes, pattern);
        Settle(code, sentinel);
        std::vector<std::uint32_t> stored(BlockBytes / sizeof(pattern));
        AgcDriver::GuestMemory::Read(base + BlockBytes, std::as_writable_bytes(std::span(stored)), 4);
        Require(std::all_of(stored.begin(), stored.end(), [](std::uint32_t value) { return value == pattern; }), "MSAA neighbor: the imported buffer fill did not reach all guest memory");
        std::cout << "MSAA neighbor imported-fill=" << pattern << " verified-bytes=" << BlockBytes << '\n';
        lock.lock();
    } else if (transition == NeighborTransition::TargetHostImport) {
        label = "target-host-import";
        const auto* imported = AgcDriver::Graphics::HostImportFor(context, base, BlockBytes);
        Require(imported != nullptr && imported->unwatched && !AgcDriver::GuestMemory::Watched(base, BlockBytes), "a new import over a native owner retained a byte-comparison watch proof");
        bool promotionRefused = false;
        try {
            AgcDriver::Graphics::RequireGuestWriteWatch(context, base, BlockBytes);
        } catch (const std::runtime_error& error) {
            promotionRefused = std::string_view(error.what()).find("over a resident image") != std::string_view::npos;
        }
        Require(promotionRefused && !AgcDriver::GuestMemory::Watched(base, BlockBytes), "an existing native owner was silently re-promoted");
    } else if (transition == NeighborTransition::CpuStore) {
        label = "same-value-cpu-store";
        *reinterpret_cast<volatile std::uint8_t*>(memory) = 0u;
    }
    bool refused = false;
    std::string refusal;
    try {
        static_cast<void>(AgcDriver::Graphics::MultisampleTargetView(context, target));
    } catch (const std::runtime_error& error) {
        refusal = error.what();
        refused = std::string_view(refusal).find("guest writes to a resident multisample target or its metadata are not implemented") != std::string_view::npos || std::string_view(refusal).find("multisample target reuse requires watched guest memory") != std::string_view::npos;
    }
    std::cout << "MSAA neighbor transition=" << label << " watched=" << AgcDriver::GuestMemory::Watched(base, BlockBytes) << " refused=" << refused << " message=" << refusal << '\n';
    Require(refused == (transition == NeighborTransition::Alias || transition == NeighborTransition::CpuStore || transition == NeighborTransition::TargetHostImport), "MSAA neighbor: the observed reuse result did not match the hypothesis control");
    Require(refusal.empty() || refused, "MSAA neighbor: an unrelated exception interrupted the control");
}

void DirectImportColorTracking(AgcDriver::VulkanDevice& device) {
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    struct Cleanup {
        AgcDriver::VulkanDevice& device;
        VkDevice handle;
        ~Cleanup() {
            device.SubmitRecorded(true);
            device.WaitIdle();
            AgcDriver::Graphics::ClearMultisampleTargets(handle);
            AgcDriver::Graphics::ClearHostImports(handle);
        }
    } cleanup{device, context.device};
    auto& color = NewBlock();
    const auto target = MultisampledTarget(color);
    Require(AgcDriver::GuestMemory::Watched(target.address, target.bytes), "the color allocation is not watched before import");
    const auto oldGeneration = AgcDriver::GuestMemory::CollectWritesUncached(target.address, target.bytes);
    const auto* imported = AgcDriver::Graphics::HostImportFor(context, target.address, target.bytes);
    Require(imported != nullptr && imported->alias == nullptr, "the color fixture did not select a direct private host import");
    if (!imported->unwatched) {
        Require(AgcDriver::GuestMemory::Watched(target.address, target.bytes), "a settled private host import removed the color allocation's watch");
        Require(AgcDriver::Graphics::MultisampleTargetView(context, target) != VK_NULL_HANDLE, "initial settled private color contents have no native view");
        Require(AgcDriver::Graphics::MultisampleTargetView(context, target) != VK_NULL_HANDLE, "settled private color contents cannot be reused");
        std::cout << "direct private color import: watched=1 native reuse preserved\n";
        return;
    }
    Require(!AgcDriver::GuestMemory::Watched(target.address, target.bytes), "a direct host import left the color allocation watched");
    const auto* exclusion = AgcDriver::GuestMemory::ExclusionReason_nid_no_patch(target.address, target.bytes);
    Require(exclusion != nullptr && std::string_view(exclusion) == "range excluded by host import", "a direct host import lost or contaminated its exclusion cause");
    auto otherDevice = context;
    otherDevice.device = VK_NULL_HANDLE;
    bool ownerRefused = false;
    try {
        AgcDriver::Graphics::RequireGuestWriteWatch(otherDevice, target.address, target.bytes);
    } catch (const std::runtime_error& error) {
        ownerRefused = std::string_view(error.what()).find("import-owning device") != std::string_view::npos;
    }
    Require(ownerRefused && !AgcDriver::GuestMemory::Watched(target.address, target.bytes), "promotion used a foreign import device");
    bool settleFailed = false;
    try {
        AgcDriver::GuestMemory::RestoreHostImportWatch(target.address, target.bytes, [] { throw std::runtime_error("injected settle failure"); });
    } catch (const std::runtime_error& error) {
        settleFailed = std::string_view(error.what()) == "injected settle failure";
    }
    Require(settleFailed && !AgcDriver::GuestMemory::Watched(target.address, target.bytes), "a failed settle restored host-import tracking");
    auto unavailable = context;
    unavailable.settleQueue = VK_NULL_HANDLE;
    bool queueRefused = false;
    try {
        AgcDriver::Graphics::RequireGuestWriteWatch(unavailable, target.address, target.bytes);
    } catch (const std::runtime_error&) {
        queueRefused = true;
    }
    Require(queueRefused && !AgcDriver::GuestMemory::Watched(target.address, target.bytes), "promotion without a settle queue restored tracking");
    AgcDriver::Graphics::RequireGuestWriteWatch(context, target.address, target.bytes);
    Require(!imported->unwatched && AgcDriver::GuestMemory::Watched(target.address, target.bytes), "the demanded private import did not retain its watch");
    Require(!AgcDriver::GuestMemory::UnchangedSince(target.address, target.bytes, oldGeneration), "watch promotion preserved a pre-promotion proof");
    Require(AgcDriver::Graphics::MultisampleTargetView(context, target) != VK_NULL_HANDLE, "initial imported color contents have no native view");
    Require(AgcDriver::Graphics::MultisampleTargetView(context, target) != VK_NULL_HANDLE, "promoted private color contents cannot be reused");
    *reinterpret_cast<volatile std::uint8_t*>(color.Data()) = 0u;
    bool refused = false;
    try {
        static_cast<void>(AgcDriver::Graphics::MultisampleTargetView(context, target));
    } catch (const std::runtime_error& error) {
        refused = std::string_view(error.what()).find("guest writes to a resident multisample target") != std::string_view::npos;
    }
    Require(refused, "promoted private color accepted a same-value CPU write");
    auto& excluded = NewBlock();
    const auto* excludedImport = AgcDriver::Graphics::HostImportFor(context, excluded.Address(), BlockBytes);
    Require(excludedImport != nullptr, "the explicit-unwatch control has no host import");
    AgcDriver::GuestMemory::Unwatch(excluded.Address(), BlockBytes);
    bool settled = false;
    Require(!AgcDriver::GuestMemory::RestoreHostImportWatch(excluded.Address(), BlockBytes, [&] { settled = true; }) && !settled, "host-import promotion cleared an explicit-unwatch exclusion");
    Require(!AgcDriver::GuestMemory::Watched(excluded.Address(), BlockBytes), "an explicitly unwatched range became watched");
    std::cout << "direct private color import: demanded watch preserved; old proof invalidated; same-value CPU write, foreign device, failed settle and missing queue refused; explicit exclusion retained\n";
}
#endif

bool FillsTakenAsMultisampledClearsAreNotStored(AgcDriver::VulkanDevice& device) {
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    if ((context.limits.framebufferColorSampleCounts & VK_SAMPLE_COUNT_4_BIT) == 0) {
        std::cout << "skipped, the device has no 4x color targets\n";
        return false;
    }
    struct ClearTargets {
        AgcDriver::VulkanDevice& device;
        VkDevice handle;
        ~ClearTargets() {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            device.SubmitRecorded(true);
            device.WaitIdle();
            AgcDriver::Graphics::ClearMultisampleTargets(handle);
            AgcDriver::Graphics::ClearCachedTextures(handle);
        }
    } clearTargets{device, context.device};
    auto& code = NewBlock();
    std::memcpy(code.Data(), FillKernel.data(), sizeof(FillKernel));

    auto& sentinel = NewBlock();
    auto& control = NewBlock();
    std::memset(control.Data(), static_cast<int>(Marker), BlockBytes);
    DispatchFill(code, control.Address(), BlockBytes, 0);
    Settle(code, sentinel);
    if (control.Holds(Marker, 0, BlockBytes)) {
        std::cout << "skipped, the device does not store compute fills into guest memory\n";
        return false;
    }
    Require(control.Holds(std::byte{}, 0, BlockBytes), "a compute fill of 0 over plain guest memory did not store 0");

    auto& color = NewBlock();
    const auto target = MultisampledTarget(color);
    Require(AgcDriver::Graphics::MultisampledColorBytes(target) == BlockBytes, "the 4x 64x64 RGBA8 target does not take one 64 KiB block");
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        Require(AgcDriver::Graphics::MultisampleTargetView(context, target) != VK_NULL_HANDLE, "the multisampled target has no view");
        device.SubmitRecorded(true);
        device.WaitIdle();
    }
    Require(AgcDriver::Graphics::MultisampledColorFillAbsorbs(color.Address(), BlockBytes, 0), "a fill of 0 over the whole color memory is not taken as the target's clear");
    Require(!AgcDriver::Graphics::MultisampledColorFillAbsorbs(color.Address(), BlockBytes / 2, 0), "a fill over half of the color memory is taken as the target's clear");
    Require(!AgcDriver::Graphics::MultisampledColorFillAbsorbs(color.Address() + 4096, BlockBytes - 4096, 0), "a fill starting inside the color memory is taken as the target's clear");
    Require(!AgcDriver::Graphics::MultisampledColorFillAbsorbs(color.Address(), 2 * BlockBytes, 0), "a fill reaching past the color memory is taken as the target's clear");

    std::memset(color.Data(), static_cast<int>(Marker), BlockBytes);
    DispatchFill(code, color.Address(), BlockBytes, 0xffffffffu);
    Settle(code, sentinel);
    Require(color.Holds(Marker, 0, BlockBytes), "a compute fill taken as the multisampled target's clear was stored into its color memory");
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        const auto samples = AgcDriver::Graphics::ReadMultisampleTargetSamples(context, target);
        Require(!samples.empty() && std::all_of(samples.begin(), samples.end(), [](std::byte sample) { return sample == std::byte{0xff}; }), "an absorbed fill did not clear every native sample");
    }
    DispatchFill(code, color.Address(), BlockBytes, 0);
    Settle(code, sentinel);
    color.Data()[0] = std::byte{0x42};
    bool cpuWriteRefused = false;
    try {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        static_cast<void>(AgcDriver::Graphics::ReadMultisampleTargetSamples(context, target));
    } catch (const std::runtime_error& error) {
        cpuWriteRefused = std::string_view(error.what()).find("tracked multisample fill") != std::string_view::npos;
    }
    Require(cpuWriteRefused, "a CPU write after an absorbed fill was accepted as unchanged native contents");
    DispatchFill(code, color.Address(), BlockBytes / 2, 0);
    Settle(code, sentinel);
    Require(color.Holds(std::byte{}, 0, BlockBytes / 2) && color.Holds(Marker, BlockBytes / 2, BlockBytes / 2), "a compute fill over half of the color memory was not stored");

    std::memset(control.Data(), static_cast<int>(Marker), BlockBytes);
    DispatchFill(code, control.Address(), BlockBytes, 0);
    Settle(code, sentinel);
    Require(control.Holds(std::byte{}, 0, BlockBytes), "a compute fill of plain guest memory was not stored once a multisampled target exists");
    auto& srgbColor = NewBlock();
    auto srgb = MultisampledTarget(srgbColor);
    srgb.format = VK_FORMAT_R8G8B8A8_SRGB;
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        Require(AgcDriver::Graphics::MultisampleTargetView(context, srgb) != VK_NULL_HANDLE, "the sRGB multisampled target has no view");
        device.SubmitRecorded(true);
        device.WaitIdle();
    }
    Require(AgcDriver::Graphics::MultisampledColorFillAbsorbs(srgbColor.Address(), BlockBytes, 0), "a fill of 0 over an sRGB target's color memory is not taken as its clear");
    Require(!AgcDriver::Graphics::MultisampledColorFillAbsorbs(srgbColor.Address(), BlockBytes, 0x80808080u), "a fill with no exact sRGB clear value is taken as the target's clear");
    std::memset(srgbColor.Data(), static_cast<int>(Marker), BlockBytes);
    DispatchFill(code, srgbColor.Address(), BlockBytes, 0x80808080u);
    Settle(code, sentinel);
    Require(srgbColor.Holds(std::byte{0x80}, 0, BlockBytes), "a compute fill with no exact clear value was not stored into the color memory");

    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(color.Data());
        mutation.Add(color.Data(), BlockBytes, true, true, true);
    }
    Require(!AgcDriver::Graphics::MultisampledColorFillAbsorbs(color.Address(), BlockBytes, 0), "a fill over a retired target's color memory is taken as its clear");
    std::memset(color.Data(), static_cast<int>(Marker), BlockBytes);
    DispatchFill(code, color.Address(), BlockBytes, 0);
    Settle(code, sentinel);
    Require(color.Holds(std::byte{}, 0, BlockBytes), "a compute fill over a retired target's color memory was not stored");
    return true;
}

}

int main(int argc, char** argv) {
    try {
        const bool nativeClearsOnly = argc == 2 && std::string_view(argv[1]) == "--native-clears-only";
        Require(argc == 1 || nativeClearsOnly, "invalid multisampled fill fixture arguments");
        auto available = OpenVulkanTestDevice();
        if (!available) return VulkanTestSkipped;
#ifdef _WIN32
        available.reset();
        auto& warmupCode = NewBlock();
        auto& warmupTarget = NewBlock();
        std::memcpy(warmupCode.Data(), FillKernel.data(), sizeof(FillKernel));
        DispatchFill(warmupCode, warmupTarget.Address(), 1024u, 0u);
        const auto device = AgcDriver::DriverDetail::DriverTestAccess::DeviceOf(AgcDriver::DriverDetail::Driver::Get());
        Require(device != nullptr, "the submitted fixture has no driver device");
        for (const auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT}) {
            for (const auto fragments : {2u, 4u, 8u}) NativeMetadataClearViaDriver(*device, format, fragments);
        }
        if (nativeClearsOnly) {
            LibcRunShutdown_nid_postfix();
            std::cout << "submitted native color clears passed\n";
            return 0;
        }
        ResidentColorNeighborTransition(*device, NeighborTransition::None);
        ResidentColorNeighborTransition(*device, NeighborTransition::Alias);
        ResidentColorNeighborTransition(*device, NeighborTransition::HostImport);
        ResidentColorNeighborTransition(*device, NeighborTransition::TargetHostImport);
        ResidentColorNeighborTransition(*device, NeighborTransition::CpuStore);
        DirectImportColorTracking(*device);
#else
        Require(!nativeClearsOnly, "submitted native color clears require the Windows fixture");
        const auto device = std::move(available);
#endif
        const bool tested = FillsTakenAsMultisampledClearsAreNotStored(*device);
        LibcRunShutdown_nid_postfix();
        if (!tested) return VulkanTestSkipped;
        std::cout << "multisampled fill tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try {
            LibcRunShutdown_nid_postfix();
        } catch (...) {
        }
        return 1;
    }
}
