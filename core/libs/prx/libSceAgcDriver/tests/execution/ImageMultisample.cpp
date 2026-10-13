#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTextureLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleDcc.hpp"
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

std::uint32_t Pattern(std::uint32_t x, std::uint32_t y, std::uint32_t layer, std::uint32_t fragment) {
    return 100000u + layer * 100000u + y * 512u + x * 16u + fragment;
}

std::uint16_t HalfPattern(std::uint32_t x, std::uint32_t y, std::uint32_t layer, std::uint32_t fragment, std::uint32_t component) {
    return static_cast<std::uint16_t>(0x3c00u | ((x * 8u + y * 17u + layer * 173u + fragment * 37u + component * 211u) & 0x3ffu));
}

void Run(AgcDriver::VulkanDevice& device, std::uint32_t format, std::uint32_t fragments, std::uint32_t variant, bool compressed) {
    const bool floating = format == 71u;
    const auto elementBytes = floating ? 8u : 4u;
    const bool arrayed = variant != 0u;
    const bool firstLayer = variant == 2u;
    const bool alphaOnMsb = !firstLayer;
    const auto layers = arrayed ? 3u : 1u;
    const auto baseArray = arrayed ? 1u : 0u;
    const auto viewLayer = arrayed && !firstLayer ? 1u : 0u;
    const MultisampleTextureLayout layout(TextureTileMode::kR64KBX, elementBytes, fragments, Width, Height, layers);
    GuestBlock texels(static_cast<std::size_t>(layout.Bytes()));
    const auto metadataBytes = AgcDriver::Graphics::MultisampleDccBytes_nid_no_patch(elementBytes, fragments, Width, Height, layers);
    GuestBlock metadata((metadataBytes + MultisampleTextureLayout::Alignment - 1u) & ~(MultisampleTextureLayout::Alignment - 1u));
    std::memset(texels.Data(), 0, static_cast<std::size_t>(layout.Bytes()));
    for (std::uint32_t layer = 0u; layer < layers; ++layer) {
        for (std::uint32_t y = 0u; y < Height; ++y) {
            for (std::uint32_t x = 0u; x < Width; ++x) {
                for (std::uint32_t fragment = 0u; fragment < fragments; ++fragment) {
                    if (floating) {
                        std::array<std::uint16_t, 4> value{};
                        for (std::uint32_t component = 0u; component < 4u; ++component) value[component] = HalfPattern(x, y, layer, fragment, component);
                        std::memcpy(texels.Data() + layout.Offset(x, y, layer, fragment), value.data(), sizeof(value));
                    } else {
                        const auto value = Pattern(x, y, layer, fragment);
                        std::memcpy(texels.Data() + layout.Offset(x, y, layer, fragment), &value, sizeof(value));
                    }
                }
            }
        }
    }
    const auto imageAddress = reinterpret_cast<std::uintptr_t>(texels.Data());
    const auto outputAddress = reinterpret_cast<std::uintptr_t>(output.data());
    const auto fragmentBits = static_cast<std::uint32_t>(std::countr_zero(fragments));
    std::array<std::uint32_t, 24> users{};
    const std::array<std::uint32_t, 4> buffer{static_cast<std::uint32_t>(outputAddress), static_cast<std::uint32_t>(outputAddress >> 32u), static_cast<std::uint32_t>(sizeof(output)), 0x31016facu};
    std::array<std::uint32_t, 8> image{
        static_cast<std::uint32_t>(imageAddress >> 8u),
        static_cast<std::uint32_t>((imageAddress >> 40u) & 0xffu) | (format << 20u) | (((Width - 1u) & 3u) << 30u),
        ((Width - 1u) >> 2u) | ((Height - 1u) << 14u),
        ((arrayed ? 15u : 14u) << 28u) | (27u << 20u) | (fragmentBits << 16u) | 0xfacu,
        (layers - 1u) | (baseArray << 16u), fragmentBits << 4u, 0u, 0u,
    };
    if (compressed) {
        const auto metadataAddress = reinterpret_cast<std::uintptr_t>(metadata.Data()) >> 8u;
        image[6] = (1u << 19u) | (1u << 21u) | (static_cast<std::uint32_t>(alphaOnMsb) << 22u) | (static_cast<std::uint32_t>(metadataAddress & 0xffu) << 24u);
        image[7] = static_cast<std::uint32_t>(metadataAddress >> 8u);
    }
    const auto decoded = AgcDriver::Graphics::DecodeTextureResource(image);
    Require(decoded.baseArray == baseArray && decoded.samples == fragments && decoded.depthOrLastArray + 1u == layers, "MS fixture descriptor does not encode its intended surface");
    Require(!compressed || (decoded.dccAddress == reinterpret_cast<std::uintptr_t>(metadata.Data()) && decoded.dccPipeAligned && decoded.dccAlphaOnMsb == alphaOnMsb), "MS fixture descriptor does not encode its intended DCC metadata");
    std::copy(buffer.begin(), buffer.end(), users.begin());
    std::copy(image.begin(), image.end(), users.begin() + 4u);
    const auto code = arrayed && !firstLayer ? std::span<const std::uint32_t>(LoadArray) : std::span<const std::uint32_t>(Load2D);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{32u, 1u, 1u}, 0u, {true, false, false}, false, 1u};
    const std::array<std::uint8_t, 8> metadataCodes{0x00u, 0x40u, 0x80u, 0xc0u, 0xffu, 0xffu, 0x00u, 0xc0u};
    for (std::uint32_t state = 0u; state < (compressed ? metadataCodes.size() : 1u); ++state) {
        const auto metadataCode = metadataCodes[state];
        std::memset(metadata.Data(), metadataCode, metadataBytes);
        const bool changedTexels = state >= 5u;
        if (state == 5u) {
            for (std::uint32_t layer = 0u; layer < layers; ++layer) {
                for (std::uint32_t y = 0u; y < Height; ++y) {
                    for (std::uint32_t x = 0u; x < Width; ++x) {
                        for (std::uint32_t fragment = 0u; fragment < fragments; ++fragment) {
                            auto* texel = texels.Data() + layout.Offset(x, y, layer, fragment);
                            if (floating) {
                                std::array<std::uint16_t, 4> value{};
                                std::memcpy(value.data(), texel, sizeof(value));
                                for (auto& component : value) component ^= 0x100u;
                                std::memcpy(texel, value.data(), sizeof(value));
                            } else {
                                std::uint32_t value;
                                std::memcpy(&value, texel, sizeof(value));
                                value ^= 0x100u;
                                std::memcpy(texel, &value, sizeof(value));
                            }
                        }
                    }
                }
            }
        }
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
                    Require(binding.imageShape == (arrayed && !firstLayer ? ShaderRecompiler::DescriptorImageShape::Image2DMultisampleArray : ShaderRecompiler::DescriptorImageShape::Image2DMultisample), "recompiled MS image_load has the wrong physical shape");
                    ++images;
                }
                Require(images == 1u, "recompiled MS image_load has an unexpected image binding count");
                device.Dispatch(shader, CheckedWidth / 32u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(code.data()));
                device.WaitIdle();
                for (std::uint32_t x = 0u; x < CheckedWidth; ++x) {
                    std::array<std::uint32_t, 4> expected{Pattern(x, y, baseArray + viewLayer, fragment) ^ (changedTexels ? 0x100u : 0u), 0u, 0u, 1u};
                    if (floating) {
                        for (std::uint32_t component = 0u; component < 4u; ++component) expected[component] = 0x3f800000u | (static_cast<std::uint32_t>((HalfPattern(x, y, baseArray + viewLayer, fragment, component) ^ (changedTexels ? 0x100u : 0u)) & 0x3ffu) << 13u);
                    }
                    if (compressed && metadataCode != 0xffu) {
                        if (floating) {
                            for (std::uint32_t component = 0u; component < 4u; ++component) expected[component] = ((metadataCode >> (component == (alphaOnMsb ? 3u : 0u) ? 6u : 7u)) & 1u) != 0u ? 0x3f800000u : 0u;
                        } else expected[0] = (metadataCode & 0x40u) != 0u ? 0xffffffffu : 0u;
                    }
                    for (std::uint32_t component = 0u; component < 4u; ++component) {
                        Require(output[x * 4u + component] == expected[component], "recompiled MS image_load differs: fragments=" + std::to_string(fragments) + " variant=" + std::to_string(variant) + " compressed=" + std::to_string(compressed) + " state=" + std::to_string(state) + " x=" + std::to_string(x) + " y=" + std::to_string(y) + " fragment=" + std::to_string(fragment) + " component=" + std::to_string(component) + " actual=" + std::to_string(output[x * 4u + component]) + " expected=" + std::to_string(expected[component]));
                    }
                }
            }
        }
    }
    std::printf("recompiled MS image_load format=%u fragments=%u variant=%u compressed=%u: every fragment and cross-block coordinate exact\n", format, fragments, variant, compressed);
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::uint32_t checked = 0u, unavailable = 0u;
        for (const auto format : {20u, 71u}) {
            for (const auto fragments : {2u, 4u, 8u}) {
                for (std::uint32_t variant = 0u; variant < 3u; ++variant) {
                    try { Run(*device, format, fragments, variant, false); Run(*device, format, fragments, variant, true); checked += 2u; }
                    catch (const std::runtime_error& error) {
                        if (std::string(error.what()).find("multisampled texture extent or fragment count is unsupported by the device") == std::string::npos || std::getenv("ANYPS5_REQUIRE_MSAA_ALL") != nullptr) throw;
                        std::printf("unavailable MS image_load format=%u fragments=%u variant=%u: %s\n", format, fragments, variant, error.what());
                        ++unavailable;
                    }
                }
            }
        }
        std::printf("recompiled MS image_load checked=%u unavailable=%u\n", checked, unavailable);
        return checked == 0u ? VulkanTestSkipped : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
