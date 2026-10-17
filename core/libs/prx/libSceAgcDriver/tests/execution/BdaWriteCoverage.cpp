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

constexpr std::size_t BlockBytes = 65536;
constexpr std::size_t PageBytes = 4096;

class GuestBlock {
public:
    GuestBlock() {
        Require(Memory::WriteWatched(), "BDA write coverage requires guest write watching");
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, BlockBytes));
        Require(block != nullptr, "cannot allocate the BDA write coverage block");
        GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
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
};

void Run(AgcDriver::VulkanDevice& device, std::uint32_t waveSize) {
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
    const auto generation = Memory::CollectWritesUncached(address, BlockBytes);
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
    std::printf("BDA wave%u: exact 64 KiB readback passed; first page overlap=%d complete=%d next page overlap=%d\n", waveSize, overlap, complete, untouched);
    Require(overlap && !untouched, "BDA stores did not invalidate only their written page");
    Require(!complete, "sparse BDA stores falsely prove a complete 4096-byte overwrite");
}

}

int main(int argc, char** argv) {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device, argc == 2 && std::string_view(argv[1]) == "--wave64" ? 64 : 32);
        std::puts("BDA sparse-store coverage passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
