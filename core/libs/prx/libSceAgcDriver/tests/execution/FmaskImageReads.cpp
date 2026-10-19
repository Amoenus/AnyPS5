#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureSwizzleEquations.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>
#include <utility>

namespace {

using namespace AgcDriver::Graphics;

constexpr std::uint32_t Width = 128;
constexpr std::uint32_t Height = 64;
constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Sentinel = 0xdeadbeef;
alignas(4096) std::array<std::byte, 4 * 65536> TexelStorage{};
const auto Texels = std::span(TexelStorage).subspan((0u - reinterpret_cast<std::uintptr_t>(TexelStorage.data())) & 0xffffu, 3 * 65536);
alignas(256) std::array<std::uint32_t, Threads * 4> Input{};
alignas(256) std::array<std::uint32_t, Threads * 4 + 4> Output{};
alignas(256) constexpr std::array<std::uint32_t, 10> Code{0x34020084u, 0xe0381000u, 0x80000201u, 0xbf8c3f70u, 0xf0001f08u, 0x00020602u, 0xbf8c3f70u, 0xe0781000u, 0x80010601u, 0xbf810000u};

std::array<std::uint32_t, 4> Buffer(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) & 0xffffu, bytes, 0x31016facu};
}

std::array<std::uint32_t, 8> Image(bool layered, std::uint32_t pipeXor, std::uint32_t swizzle = 4u) {
    const auto address = reinterpret_cast<std::uintptr_t>(Texels.data()) + pipeXor;
    return {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u) | (166u << 20u) | (3u << 30u), 31u | ((Height - 1u) << 14u), swizzle | (24u << 20u) | ((layered ? 13u : 9u) << 28u), layered ? 2u | (1u << 16u) : 0u, 0u, 0u, 0u};
}

std::uint32_t Mapping(std::uint32_t x, std::uint32_t y, std::uint32_t layer, std::uint32_t round) {
    if (round == 0) return 0u;
    if (round == 1) return 0x11111111u;
    if (round == 2) return 0x11111100u;
    std::uint32_t word = 0;
    for (std::uint32_t sample = 0; sample < 8; ++sample) word |= ((x + 3u * y + 5u * layer + sample + round) & 7u) << (sample * 4u);
    return word;
}

std::uint64_t EquationOffset(const TextureSwizzleEquation& equation, std::uint32_t x, std::uint32_t y, std::uint32_t layer) {
    std::uint64_t offset = 0;
    for (std::uint32_t bit = 0; bit < 16; ++bit) {
        const auto mask = equation.bits[bit];
        const auto selected = (x & (mask & 0xfffu)) ^ ((y << 12u) & (mask & 0xfff000u)) ^ ((layer << 24u) & (mask & 0xff000000u));
        offset |= static_cast<std::uint64_t>(std::popcount(selected) & 1u) << bit;
    }
    return offset;
}

void Fill(bool layered, std::uint32_t pipeXor, std::uint32_t round) {
    auto words = Image(layered, pipeXor);
    words[1] = (words[1] & ~(0x1ffu << 20u)) | (20u << 20u);
    const auto resource = DecodeTextureResource(words);
    const auto geometry = DescribeSurface(resource);
    Require(geometry.guestBytes <= Texels.size(), "FMASK fixture exceeds its allocation");
    const auto& mip = geometry.mips.at(0);
    const auto block = ThinBlockLayout(resource.tileMode, 4u);
    const auto* equation = FindTextureSwizzleEquation(24u, 4u);
    Require(equation != nullptr, "FMASK fixture has no Z swizzle equation");
    std::ranges::fill(Texels, std::byte{0});
    for (std::uint32_t layer = 0; layer < geometry.layers; ++layer) {
        for (std::uint32_t y = 0; y < Height; ++y) {
            for (std::uint32_t x = 0; x < Width; ++x) {
                auto offset = geometry.GuestLayerOffset(layer) + mip.tiledOffset;
                if (mip.tail) offset += EquationOffset(*equation, x + mip.tailX, y + mip.tailY, layer) ^ pipeXor;
                else offset += (static_cast<std::uint64_t>(y / block[2]) * mip.blocksPerRow + x / block[1]) * block[0] + (EquationOffset(*equation, x, y, layer) ^ pipeXor);
                const auto value = Mapping(x, y, layer, round);
                Require(offset + sizeof(value) <= Texels.size(), "FMASK fixture addresses outside its allocation");
                std::memcpy(Texels.data() + offset, &value, sizeof(value));
            }
        }
    }
}

ShaderRecompiler::RecompileResult Compile(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, std::span<const std::uint32_t> image, bool registration) {
    std::vector<std::uint32_t> userData(16u);
    const auto input = Buffer(Input.data(), sizeof(Input));
    const auto output = Buffer(Output.data(), Threads * 16u);
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4u);
    std::copy(image.begin(), image.end(), userData.begin() + 8u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    auto target = device.Target();
    target.registrationModules = registration;
    ShaderRecompiler::RecompileRequest request{{ShaderRecompiler::ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0u, {}}, {32u, 0u, userData, compute, std::nullopt, std::nullopt, memory}, target, {0u, 0u, 0u, 128u}};
    return ShaderRecompiler::Recompile(request);
}

void Run(AgcDriver::VulkanDevice& device, bool layered, bool registration, std::uint32_t pipeXor) {
    auto code = Code;
    if (layered) code[4] = 0xf0001f28u;
    ShaderRecompiler::CompiledShaderArtifact first;
    for (std::uint32_t round = 0; round < 5; ++round) {
        Fill(layered, pipeXor, round);
        for (std::uint32_t lane = 0; lane < Threads; ++lane) {
            Input[lane * 4u] = (lane * 37u + 11u) % Width;
            Input[lane * 4u + 1u] = (lane * 23u + 5u) % Height;
            Input[lane * 4u + 2u] = layered ? lane & 1u : 0u;
            Input[lane * 4u + 3u] = 0u;
        }
        Output.fill(Sentinel);
        const auto swizzle = round == 4u ? 4u | (1u << 3u) | (4u << 6u) : 4u;
        const auto shader = Compile(device, code, Image(layered, pipeXor, swizzle), registration);
        if (first.variantId == 0u) first = shader;
        else Require(shader.cacheHit && shader.variantId == first.variantId, "FMASK data or channel selectors changed the compiled shader artifact");
        device.Dispatch(shader, 1u, 1u, 1u);
        device.WaitIdle();
        for (std::uint32_t lane = 0; lane < Threads; ++lane) {
            const auto value = Mapping(Input[lane * 4u], Input[lane * 4u + 1u], layered ? Input[lane * 4u + 2u] + 1u : 0u, round);
            const std::array expected{value, round == 4u ? 1u : 0u, round == 4u ? value : 0u, 0u};
            for (std::uint32_t component = 0; component < 4u; ++component) Require(Output[lane * 4u + component] == expected[component], "FMASK load did not read its mapping: round " + std::to_string(round) + ", lane " + std::to_string(lane) + ", component " + std::to_string(component));
        }
        Require(std::all_of(Output.begin() + Threads * 4u, Output.end(), [](auto value) { return value == Sentinel; }), "FMASK load wrote outside its output");
        if (round == 3u) {
            auto compressed = Image(layered, pipeXor);
            compressed[6] = 1u << 21u;
            Output.fill(Sentinel);
            bool rejected = false;
            try {
                const auto invalid = Compile(device, code, compressed, registration);
                device.Dispatch(invalid, 1u, 1u, 1u);
                device.WaitIdle();
            } catch (const std::runtime_error& error) {
                Require(std::string(error.what()).find("FMASK CMASK compression is not implemented") != std::string::npos, "compressed FMASK failed for another reason: " + std::string(error.what()));
                rejected = true;
            }
            Require(rejected, "a cached FMASK shader accepted compression without decoding CMASK");
            Require(std::all_of(Output.begin(), Output.end(), [](auto value) { return value == Sentinel; }), "the rejected compressed FMASK load wrote output");
        }
    }
}


alignas(4096) std::array<std::byte, 3u * 4096u> Cmask{};

std::size_t CmaskNibble(std::uint32_t tileX, std::uint32_t tileY, std::uint32_t layer, std::uint32_t pipeXor) {
    constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 13> equation{{
        {8u, 0u}, {0u, 16u}, {64u, 0u}, {0u, 64u}, {128u, 0u}, {0u, 128u}, {256u, 0u},
        {0u, 256u}, {512u, 0u}, {8u, 8u}, {16u, 16u}, {64u, 32u}, {32u, 64u},
    }};
    std::size_t offset = 0u;
    for (std::size_t bit = 0; bit < equation.size(); ++bit) offset |= static_cast<std::size_t>(std::popcount((tileX * 8u & equation[bit].first) ^ (tileY * 8u & equation[bit].second)) & 1u) << bit;
    const auto layerXor = ((layer & 8u) << 6u) | ((layer & 4u) << 8u) | ((layer & 2u) << 10u) | ((layer & 1u) << 12u);
    return layer * 8192u + (offset ^ layerXor ^ ((pipeXor >> 8u & 15u) << 9u));
}

void RunCompressed(AgcDriver::VulkanDevice& device, bool layered, bool registration, std::uint32_t pipeXor) {
    auto code = Code;
    if (layered) code[4] = 0xf0001f28u;
    Fill(layered, pipeXor, 3u);
    const auto originalTexels = std::vector<std::byte>(Texels.begin(), Texels.end());
    const auto metadata = reinterpret_cast<std::uintptr_t>(Cmask.data());
    ShaderRecompiler::CompiledShaderArtifact first;
    for (std::uint32_t round = 0; round < 4u; ++round) {
        Cmask.fill(round == 1u ? std::byte{0xcc} : std::byte{0xff});
        if (round == 3u) {
            for (std::uint32_t layer = 0; layer < (layered ? 3u : 1u); ++layer) {
                for (std::uint32_t tileY = 0; tileY < Height / 8u; ++tileY) {
                    for (std::uint32_t tileX = 0; tileX < Width / 8u; ++tileX) {
                        if (((tileX + tileY + layer) & 1u) == 0u) continue;
                        const auto nibble = CmaskNibble(tileX, tileY, layer, pipeXor);
                        Require(nibble / 2u < Cmask.size(), "CMASK fixture exceeds its allocation");
                        const auto shift = (nibble & 1u) * 4u;
                        Cmask[nibble / 2u] = static_cast<std::byte>((std::to_integer<std::uint32_t>(Cmask[nibble / 2u]) & ~(15u << shift)) | (12u << shift));
                    }
                }
            }
        }
        for (std::uint32_t lane = 0; lane < Threads; ++lane) {
            Input[lane * 4u] = (lane * 37u + 11u) % Width;
            Input[lane * 4u + 1u] = (lane * 23u + 5u) % Height;
            Input[lane * 4u + 2u] = layered ? lane & 1u : 0u;
            Input[lane * 4u + 3u] = 0u;
        }
        auto image = Image(layered, pipeXor, 4u | (1u << 3u) | (4u << 6u));
        image[6] = (1u << 19u) | (1u << 21u) | (static_cast<std::uint32_t>(metadata >> 8u) << 24u);
        image[7] = static_cast<std::uint32_t>(metadata >> 16u);
        Output.fill(Sentinel);
        const auto shader = Compile(device, code, image, registration);
        if (first.variantId == 0u) first = shader;
        else Require(shader.cacheHit && shader.variantId == first.variantId, "CMASK bytes changed the compiled shader artifact");
        device.Dispatch(shader, 1u, 1u, 1u);
        device.WaitIdle();
        for (std::uint32_t lane = 0; lane < Threads; ++lane) {
            const auto x = Input[lane * 4u];
            const auto y = Input[lane * 4u + 1u];
            const auto layer = layered ? Input[lane * 4u + 2u] + 1u : 0u;
            const bool clear = round == 1u || (round == 3u && ((x / 8u + y / 8u + layer) & 1u) != 0u);
            const auto value = clear ? 0u : Mapping(x, y, layer, 3u);
            const std::array expected{value, 1u, value, 0u};
            for (std::uint32_t component = 0; component < 4u; ++component) Require(Output[lane * 4u + component] == expected[component], "compressed FMASK load differs: round " + std::to_string(round) + ", lane " + std::to_string(lane) + ", component " + std::to_string(component));
        }
        Require(std::all_of(Output.begin() + Threads * 4u, Output.end(), [](auto value) { return value == Sentinel; }), "compressed FMASK wrote outside its output");
        Require(std::equal(Texels.begin(), Texels.end(), originalTexels.begin()), "CMASK transition changed stored FMASK bytes");
    }
    Cmask.fill(std::byte{0xdd});
    Output.fill(Sentinel);
    bool rejected = false;
    try {
        auto image = Image(layered, pipeXor);
        image[6] = (1u << 19u) | (1u << 21u) | (static_cast<std::uint32_t>(metadata >> 8u) << 24u);
        image[7] = static_cast<std::uint32_t>(metadata >> 16u);
        const auto invalid = Compile(device, code, image, registration);
        device.Dispatch(invalid, 1u, 1u, 1u);
        device.WaitIdle();
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()).find("FMASK CMASK compression code is not implemented") != std::string::npos, "unsupported CMASK failed for another reason: " + std::string(error.what()));
        rejected = true;
    }
    Require(rejected, "unsupported CMASK was accepted");
    Require(std::all_of(Output.begin(), Output.end(), [](auto value) { return value == Sentinel; }), "unsupported CMASK wrote output");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        for (const bool registration : {false, true}) {
            for (const bool layered : {false, true}) {
                for (const auto pipeXor : {0u, 0xf00u}) {
                    Run(*device, layered, registration, pipeXor);
                    RunCompressed(*device, layered, registration, pipeXor);
                }
            }
        }
        std::cout << "FMASK image reads passed: 72 dispatches, 9216 component checks, 16 compression refusals; CMASK FF-CC-FF and mixed tiles/layers at unchanged addresses\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
