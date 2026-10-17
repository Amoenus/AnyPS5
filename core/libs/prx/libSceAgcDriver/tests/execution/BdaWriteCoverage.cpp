#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "../NarrowConstantStoreFixture.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <span>
#include <string>
#include <string_view>

namespace {

using AgcDriver::Graphics::Require;
using namespace NarrowConstantStoreFixture;
namespace Memory = AgcDriver::GuestMemory;

constexpr std::size_t BlockBytes = 1048576;
constexpr std::size_t PageBytes = 4096;

class GuestBlock {
public:
    GuestBlock() {
        Require(Memory::WriteWatched(), "BDA write coverage requires guest write watching");
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, BlockBytes));
        Require(block != nullptr, "cannot allocate the BDA write coverage block");
        section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, BlockBytes, nullptr);
        Require(section != nullptr, "cannot create the BDA write coverage backing");
        GuestArena::GuestArenaMapDirect_nid_postfix(block, BlockBytes, section, 0, PAGE_READWRITE);
#else
        void* raw = mmap(nullptr, 2 * BlockBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Require(raw != MAP_FAILED, "cannot map the BDA write coverage block");
        const auto begin = reinterpret_cast<std::uintptr_t>(raw);
        const auto aligned = (begin + BlockBytes - 1) & ~(static_cast<std::uintptr_t>(BlockBytes) - 1);
        if (aligned != begin) munmap(raw, aligned - begin);
        if (aligned != begin + BlockBytes) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + BlockBytes - aligned);
        block = reinterpret_cast<std::uint8_t*>(aligned);
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
#endif
        Require(Memory::Watched(Address(), BlockBytes), "the BDA write coverage block is not watched");
        std::memset(block, Fill, BlockBytes);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
        GuestArena::GuestArenaForgetSection_nid_postfix(section);
        CloseHandle(section);
#else
        munmap(block, BlockBytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, BlockBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(block); }
    const std::uint8_t* Data() const { return block; }

private:
    std::uint8_t* block = nullptr;
#ifdef _WIN32
    HANDLE section = nullptr;
#endif
};

int Run(AgcDriver::VulkanDevice& device, std::uint32_t waveSize) {
    using namespace ShaderRecompiler;
    const GuestBlock guest;
    const auto address = guest.Address();
    const std::array<std::uint32_t, 2> userData{
        static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u),
    };
    const std::span<const std::uint32_t> code(Code);
    const std::array<MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderComputeStageInfo compute{{Threads, 1, 1}, 0, {false, false, false}, false, 1, {}};
    RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {waveSize, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(), {0, 0, 0, 128}, std::nullopt, false,
    };
    const auto result = Recompile(request);
    Require(std::ranges::any_of(result.bindings, [](const auto& binding) { return binding.role == DescriptorRole::BdaPagetable; }), "the coverage shader did not use BDA");
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
    const auto generation = Memory::CollectWritesUncached(address, BlockBytes);
    std::printf("BDA warm-up: watched=%d generation=%llu overlap=%d\n", Memory::Watched(address, BlockBytes), static_cast<unsigned long long>(generation), Memory::StoredOver(address, BlockBytes, generation));
    if (generation == 0 || !Memory::Watched(address, BlockBytes)) {
        std::puts("skipped, the BDA import does not retain guest write watching");
        return VulkanTestSkipped;
    }
    Require(generation != 0 && !Memory::StoredOver(address, BlockBytes, generation), "the BDA write coverage baseline is not clean");
    Require(!Memory::StoredAllSince_nid_no_patch(address, PageBytes, generation), "the untouched page already has complete store coverage");
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
    for (std::size_t offset = 0; offset < BlockBytes; ++offset) {
        const auto expected = offset < Threads * LaneBytes ? ExpectedLane[offset % LaneBytes] : Fill;
        Require(guest.Data()[offset] == expected, "BDA write coverage: byte " + std::to_string(offset) + " differs from the narrow-store fixture");
    }
    const auto overlap = Memory::StoredOver(address, PageBytes, generation);
    const auto complete = Memory::StoredAllSince_nid_no_patch(address, PageBytes, generation);
    const auto untouched = Memory::StoredOver(address + PageBytes, PageBytes, generation);
    std::printf("BDA wave%u: exact %zu-byte readback passed; first page overlap=%d complete=%d next page overlap=%d\n", waveSize, BlockBytes, overlap, complete, untouched);
    Require(overlap && !untouched, "BDA stores did not invalidate only their written page");
    Require(!complete, "sparse BDA stores falsely prove a complete 4096-byte overwrite");
    const auto uncertainGeneration = Memory::CollectWritesUncached(address, BlockBytes);
    Memory::MarkPossiblyWritten_nid_no_patch(address, 65536);
    Require(Memory::StoredOver(address, 65536, uncertainGeneration), "a possible whole-block store did not invalidate its range");
    Require(!Memory::StoredAllSince_nid_no_patch(address, 65536, uncertainGeneration), "a possible whole-block store proved complete coverage");
    const auto exactGeneration = Memory::CollectWritesUncached(address, BlockBytes);
    const std::array<std::byte, 4> word{};
    Memory::StoreOwnBytes(address + 8, word);
    Require(Memory::StoredAllSince_nid_no_patch(address + 8, word.size(), exactGeneration), "an exact dword store lost its coverage proof");
    Require(!Memory::StoredAllSince_nid_no_patch(address, PageBytes, exactGeneration), "an exact dword store proved a whole-page overwrite");
    const std::array<std::byte, PageBytes> page{};
    Memory::StoreOwnBytes(address, page);
    Require(Memory::StoredAllSince_nid_no_patch(address, PageBytes, exactGeneration), "an exact page store lost its coverage proof");
    std::puts("BDA sparse-store coverage passed");
    return 0;
}

}

int main(int argc, char** argv) {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        return Run(*device, argc == 2 && std::string_view(argv[1]) == "--wave64" ? 64 : 32);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
