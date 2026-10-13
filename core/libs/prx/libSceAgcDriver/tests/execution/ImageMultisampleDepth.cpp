#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTextureLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleHtile.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::MultisampleTextureLayout;
using AgcDriver::Graphics::TextureTileMode;

constexpr std::uint32_t Width = 129u;
constexpr std::uint32_t Height = 67u;
constexpr std::uint32_t CheckedWidth = 128u;
alignas(256) std::array<std::uint32_t, CheckedWidth * 4u> output{};

alignas(256) constexpr std::array<std::uint32_t, 16> Load2D{
    0x7e080218u, 0x343c0885u, 0x4a3c3d00u, 0x34063c84u, 0x7e3e020eu, 0x7e40020cu,
    0x7e140280u, 0x7e160280u, 0x7e180280u, 0x7e1a0280u, 0xf0001f30u, 0x00010a1eu,
    0xbf8c3f70u, 0xe0781000u, 0x80000a03u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 17> LoadArray{
    0x7e080218u, 0x343c0885u, 0x4a3c3d00u, 0x34063c84u, 0x7e3e020eu, 0x7e40020du,
    0x7e42020cu, 0x7e140280u, 0x7e160280u, 0x7e180280u, 0x7e1a0280u, 0xf0001f38u,
    0x00010a1eu, 0xbf8c3f70u, 0xe0781000u, 0x80000a03u, 0xbf810000u,
};

class GuestBlock {
public:
    explicit GuestBlock(std::size_t bytes) {
#ifdef _WIN32
        data = static_cast<std::byte*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        data = static_cast<std::byte*>(std::aligned_alloc(MultisampleTextureLayout::Alignment, ((bytes + MultisampleTextureLayout::Alignment - 1u) / MultisampleTextureLayout::Alignment) * MultisampleTextureLayout::Alignment));
#endif
        Require(data != nullptr, "cannot allocate the multisampled guest surface");
        GuestAllocations::Mutation().Add(data, bytes, true, true, true);
    }
    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(data);
#ifdef _WIN32
        VirtualFree(data, 0u, MEM_RELEASE);
#else
        std::free(data);
#endif
    }
    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;
    std::byte* Data() const { return data; }
private:
    std::byte* data = nullptr;
};

std::uint32_t StoredBits(std::uint32_t format, std::uint32_t x, std::uint32_t y, std::uint32_t layer, std::uint32_t fragment, bool changed) {
    const auto tag = x * 13u + y * 113u + layer * 997u + fragment * 101u + 1u;
    if (format == 7u) return (tag & 0xffffu) ^ (changed ? 0x1234u : 0u);
    return std::bit_cast<std::uint32_t>(static_cast<float>((tag + (changed ? 128u : 0u)) & 1023u) / 1024.0f);
}

std::uint32_t TileState(std::uint32_t x, std::uint32_t y, std::uint32_t layer) {
    return (x / 8u + 2u * (y / 8u) + layer) % 3u;
}

void Run(AgcDriver::VulkanDevice& device, std::uint32_t format, std::uint32_t fragments, std::uint32_t variant, bool compressed) {
    using namespace AgcDriver::Graphics;
    const bool arrayed = variant != 0u;
    const bool firstLayer = variant == 2u;
    const auto layers = arrayed ? 3u : 1u;
    const auto baseArray = variant == 1u ? 1u : 0u;
    const auto viewLayer = variant == 1u ? 1u : 0u;
    const auto elementBytes = format == 7u ? 2u : 4u;
    const auto pipeBankXor = variant == 2u ? 0xcf00u : variant == 1u ? 0x500u : 0u;
    const MultisampleTextureLayout layout(TextureTileMode::kZ64KBX, elementBytes, fragments, Width, Height, layers, pipeBankXor);
    GuestBlock texels(layout.Bytes());
    const auto metadataBytes = MultisampleHtileBytes_nid_no_patch(Width, Height, layers);
    GuestBlock metadata(metadataBytes);
    const auto fillSurface = [&](bool changed) {
        for (std::uint32_t layer = 0u; layer < layers; ++layer) {
            for (std::uint32_t y = 0u; y < Height; ++y) {
                for (std::uint32_t x = 0u; x < Width; ++x) {
                    for (std::uint32_t fragment = 0u; fragment < fragments; ++fragment) {
                        const auto value = StoredBits(format, x, y, layer, fragment, changed);
                        std::memcpy(texels.Data() + layout.Offset(x, y, layer, fragment), &value, elementBytes);
                    }
                }
            }
        }
    };
    fillSurface(false);
    const auto imageAddress = reinterpret_cast<std::uintptr_t>(texels.Data());
    const auto outputAddress = reinterpret_cast<std::uintptr_t>(output.data());
    const auto fragmentBits = static_cast<std::uint32_t>(std::countr_zero(fragments));
    std::array<std::uint32_t, 24> users{};
    const std::array<std::uint32_t, 4> buffer{static_cast<std::uint32_t>(outputAddress), static_cast<std::uint32_t>(outputAddress >> 32u), static_cast<std::uint32_t>(sizeof(output)), 0x31016facu};
    std::array<std::uint32_t, 8> image{
        static_cast<std::uint32_t>((imageAddress | pipeBankXor) >> 8u),
        static_cast<std::uint32_t>((imageAddress >> 40u) & 0xffu) | (format << 20u) | (((Width - 1u) & 3u) << 30u),
        ((Width - 1u) >> 2u) | ((Height - 1u) << 14u),
        ((arrayed ? 15u : 14u) << 28u) | (24u << 20u) | (fragmentBits << 16u) | 0xfacu,
        (layers - 1u) | (baseArray << 16u), fragmentBits << 4u, 0u, 0u,
    };
    if (compressed) {
        const auto metadataAddress = reinterpret_cast<std::uintptr_t>(metadata.Data()) >> 8u;
        image[6] = (1u << 10u) | (1u << 19u) | (1u << 21u) | (static_cast<std::uint32_t>(metadataAddress & 0xffu) << 24u);
        image[7] = static_cast<std::uint32_t>(metadataAddress >> 8u);
    }
    const auto decoded = DecodeTextureResource(image);
    Require(decoded.baseAddress == imageAddress && decoded.pipeBankXor == pipeBankXor && decoded.baseArray == baseArray && decoded.samples == fragments && decoded.depthOrLastArray + 1u == layers, "depth fixture descriptor does not encode its intended surface");
    Require(!compressed || (decoded.dccAddress == reinterpret_cast<std::uintptr_t>(metadata.Data()) && decoded.dccPipeAligned && decoded.iterate256), "depth fixture descriptor does not encode its intended HTILE metadata");
    std::copy(buffer.begin(), buffer.end(), users.begin());
    std::copy(image.begin(), image.end(), users.begin() + 4u);
    const auto code = arrayed && !firstLayer ? std::span<const std::uint32_t>(LoadArray) : std::span<const std::uint32_t>(Load2D);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{32u, 1u, 1u}, 0u, {true, false, false}, false, 1u};
    const std::array<std::uint32_t, 7> metadataCodes{0xffffffffu, 0x000000f0u, 0xfffc00f0u, 0xffffffffu, 0xffffffffu, 0x000000f0u, 0xfffc00f0u};
    for (std::uint32_t state = 0u; state < (compressed ? metadataCodes.size() : 1u); ++state) {
        auto words = std::span(reinterpret_cast<std::uint32_t*>(metadata.Data()), metadataBytes / 4u);
        std::fill(words.begin(), words.end(), metadataCodes[state]);
        if (state == 3u) {
            constexpr std::array<std::uint32_t, 3> tileCodes{0xffffffffu, 0x000000f0u, 0xfffc00f0u};
            for (std::uint32_t layer = 0u; layer < layers; ++layer) {
                for (std::uint32_t y = 0u; y < Height; y += 8u) {
                    for (std::uint32_t x = 0u; x < Width; x += 8u) {
                        const auto offset = MultisampleHtileOffset_nid_no_patch(fragments, Width, Height, layers, pipeBankXor, x, y, layer);
                        words[offset / 4u] = tileCodes[TileState(x, y, layer)];
                    }
                }
            }
        }
        if (state == 4u) fillSurface(true);
        for (const auto y : {0u, 64u}) {
            for (std::uint32_t fragment = 0u; fragment < fragments; ++fragment) {
                output.fill(0xdeadbeefu);
                users[12] = fragment;
                users[13] = viewLayer;
                users[14] = y;
                ShaderRecompiler::RecompileRequest request{
                    {ShaderRecompiler::ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0u, {}},
                    {32u, 0u, users, compute, std::nullopt, std::nullopt, memory}, device.ComputeTarget(32u), {0u, 0u, 0u, 128u},
                };
                request.useCache = false;
                const auto shader = ShaderRecompiler::Recompile(request);
                std::uint32_t images = 0u;
                for (const auto& binding : shader.bindings) {
                    if (!binding.imageShape.has_value()) continue;
                    Require(binding.imageShape == (arrayed && !firstLayer ? ShaderRecompiler::DescriptorImageShape::Image2DMultisampleArray : ShaderRecompiler::DescriptorImageShape::Image2DMultisample), "recompiled depth image_load has the wrong physical shape");
                    ++images;
                }
                Require(images == 1u, "recompiled depth image_load has an unexpected image binding count");
                device.Dispatch(shader, CheckedWidth / 32u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(code.data()));
                device.WaitIdle();
                for (std::uint32_t x = 0u; x < CheckedWidth; ++x) {
                    const auto bits = StoredBits(format, x, y, baseArray + viewLayer, fragment, state >= 4u);
                    auto expected = format == 7u ? std::bit_cast<std::uint32_t>(static_cast<float>(bits) / 65535.0f) : bits;
                    if (compressed) {
                        const auto mode = state == 3u ? TileState(x, y, baseArray + viewLayer) : metadataCodes[state] == 0xffffffffu ? 0u : metadataCodes[state] == 0x000000f0u ? 1u : 2u;
                        if (mode != 0u) expected = mode == 1u ? 0u : 0x3f800000u;
                    }
                    const auto actual = output[x * 4u];
                    const auto difference = actual > expected ? actual - expected : expected - actual;
                    Require(difference <= (format == 7u && expected != 0u && expected != 0x3f800000u ? 1u : 0u), "recompiled depth image_load differs: format=" + std::to_string(format) + " fragments=" + std::to_string(fragments) + " variant=" + std::to_string(variant) + " compressed=" + std::to_string(compressed) + " state=" + std::to_string(state) + " x=" + std::to_string(x) + " y=" + std::to_string(y) + " fragment=" + std::to_string(fragment) + " actual=" + std::to_string(actual) + " expected=" + std::to_string(expected));
                    Require(output[x * 4u + 1u] == 0u && output[x * 4u + 2u] == 0u && output[x * 4u + 3u] == 0x3f800000u, "recompiled depth image_load has incorrect missing components");
                }
            }
        }
    }
    std::printf("recompiled depth image_load format=%u fragments=%u variant=%u compressed=%u: fragment/array/XOR/metadata transitions checked\n", format, fragments, variant, compressed);
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::uint32_t checked = 0u, unavailable = 0u;
        for (const auto format : {7u, 22u}) {
            for (const auto fragments : {2u, 4u, 8u}) {
                for (std::uint32_t variant = 0u; variant < 3u; ++variant) {
                    try { Run(*device, format, fragments, variant, false); Run(*device, format, fragments, variant, true); checked += 2u; }
                    catch (const std::runtime_error& error) {
                        if (std::string(error.what()).find("multisampled texture extent or fragment count is unsupported by the device") == std::string::npos || std::getenv("ANYPS5_REQUIRE_MSAA_ALL") != nullptr) throw;
                        std::printf("unavailable depth image_load format=%u fragments=%u variant=%u: %s\n", format, fragments, variant, error.what());
                        ++unavailable;
                    }
                }
            }
        }
        std::printf("recompiled depth image_load checked=%u unavailable=%u\n", checked, unavailable);
        return checked == 0u ? VulkanTestSkipped : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
