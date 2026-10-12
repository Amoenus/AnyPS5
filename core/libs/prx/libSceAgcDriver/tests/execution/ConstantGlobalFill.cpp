#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/BufferFill.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
constexpr std::size_t BlockBytes = 1024u * 1024u;
alignas(256) constexpr auto Code = AgcDriver::DriverDetail::ConstantFillKernel;

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint32_t*>(std::aligned_alloc(65536u, BlockBytes));
#endif
        Require(block != nullptr, "constant global fill allocation failed");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }
    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        VirtualFree(block, 0u, MEM_RELEASE);
#else
        std::free(block);
#endif
    }
    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;
    std::uint32_t* Data() { return block; }
private:
    std::uint32_t* block = nullptr;
};

void Run(AgcDriver::VulkanDevice& device, GuestBlock& guest, std::uint32_t waveSize, std::uint32_t groups, std::int32_t adjustment, std::uint32_t value) {
    const auto blocks = static_cast<std::uint32_t>(groups * 64u + adjustment);
    const auto bytes = blocks * 16u;
    const auto written = std::min(groups * 1024u, bytes);
    auto* data = guest.Data();
    const auto original = [&](std::size_t index) { return ~value ^ static_cast<std::uint32_t>(index * 0x9e3779b1u); };
    for (std::size_t index = 0u; index < BlockBytes / 4u; ++index) data[index] = original(index);
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data + 4u));
    const std::vector<std::uint32_t> userData{0u, 0u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), bytes - 16u, value};
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{64u, 1u, 1u}, 0u, {true, false, false}, false, 1u};
    ShaderRecompiler::RecompileRequest request{
        {ShaderRecompiler::ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0u, {}},
        {waveSize, 0u, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0u, 0u, 0u, 128u}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, groups, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
    for (std::size_t index = 0u; index < BlockBytes / 4u; ++index) {
        const auto expected = index >= 4u && (index - 4u) * 4u < written ? value : original(index);
        Require(data[index] == expected, "constant global fill lost a DWORD, clamp or guard at " + std::to_string(index));
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestBlock guest;
        std::uint32_t checked = 0u;
        for (const auto wave : {32u, 64u}) {
            for (const auto groups : {1u, 2u, 80u, 540u}) {
                for (const auto adjustment : {-1, 0, 1}) {
                    for (const auto value : {0u, 0x40404040u, 0xccccccccu}) { Run(*device, guest, wave, groups, adjustment, value); ++checked; }
                }
            }
        }
        std::printf("constant global fill: %u wave/group/clamp/value cases, every DWORD and guard checked\n", checked);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
