#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::ColorTargetLayout;
using AgcDriver::Graphics::ColorTileMode;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Side = 16u;
constexpr std::size_t BlockBytes = 65536u;
constexpr std::uint32_t Threads = 32u;
constexpr std::uint32_t Float32Export = 9u;
constexpr double EdgeMargin = 1.0 / 32.0;
constexpr std::uint32_t FillOnes = 0x3c003c00u;
constexpr std::uint32_t MovedWord = 0x3f000000u;
alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000u, 0x80000005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u,
};
constexpr std::array<std::array<float, 4>, 3> Triangle{{
    {-1.0f, -1.0f, 0.5f, 1.0f}, {0.83f, -1.0f, 0.5f, 1.0f}, {-1.0f, 0.31f, 0.5f, 1.0f}
}};

struct Block {
    explicit Block(std::size_t bytes) : bytes(bytes) {
#ifdef _WIN32
        data = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        data = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, bytes));
#endif
        Require(data != nullptr, "cannot allocate the multisampled color target");
        std::memset(data, 0, bytes);
        GuestAllocations::Mutation mutation;
        mutation.Add(data, bytes, true, true, true);
    }
    ~Block() {
        {
            GuestAllocations::Mutation mutation;
            mutation.Remove(data);
        }
#ifdef _WIN32
        VirtualFree(data, 0, MEM_RELEASE);
#else
        std::free(data);
#endif
    }
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(data); }
    std::size_t bytes;
    std::uint8_t* data = nullptr;
};

AgcDriver::Graphics::State TargetState(const AgcDriver::Graphics::ColorTarget& color) {
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xf;
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 64u, 64u, std::nullopt, std::nullopt};
    state.colors = {color};
    state.color = color;
    state.hasColorTarget = true;
    state.renderExtent = {Side, Side};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Side), static_cast<float>(Side), -static_cast<float>(Side), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Side, Side}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blends = {blend};
    state.blend = blend;
    state.blendConstants = {};
    state.samples = color.samples;
    return state;
}

double EdgeDistance(double x, double y, std::size_t from) {
    const auto pixels = [](const std::array<float, 4>& vertex) {
        return std::array<double, 2>{(static_cast<double>(vertex[0]) + 1.0) * 0.5 * Side, Side - (static_cast<double>(vertex[1]) + 1.0) * 0.5 * Side};
    };
    const auto a = pixels(Triangle[from]);
    const auto b = pixels(Triangle[(from + 1u) % Triangle.size()]);
    const auto c = pixels(Triangle[(from + 2u) % Triangle.size()]);
    const auto length = std::hypot(b[0] - a[0], b[1] - a[1]);
    const auto side = [&](double px, double py) { return ((b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0])) / length; };
    return side(c[0], c[1]) < 0 ? -side(x, y) : side(x, y);
}

ShaderRecompiler::RecompileResult CompileMovedVertex(AgcDriver::VulkanDevice& device, std::uint64_t address) {
    const std::array<std::uint32_t, 4> userData{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (16u << 16u), static_cast<std::uint32_t>(Triangle.size()), 0x01016facu};
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {64, 0, userData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, memory},
        device.Target(), {0, 0, 0, 64}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

void CheckRecipePixels(AgcDriver::VulkanDevice& device, const Block& destination, const AgcDriver::Graphics::ColorTarget& target, std::uint32_t word) {
    device.WaitIdle();
    std::vector<std::byte> tiled(destination.bytes);
    AgcDriver::GuestMemory::Read(destination.Address(), tiled);
    const AgcDriver::Graphics::ColorTargetLayout layout(Side, Side, target.tileMode, target.elementBytes);
    Require(layout.Bytes() == target.bytes && target.bytes <= tiled.size(), "the recipe output footprint is invalid");
    Require(std::all_of(tiled.begin() + target.bytes, tiled.end(), [](auto value) { return value == std::byte{0xa5}; }), "the recipe output changed its neighboring bytes");
    std::vector<std::array<std::uint32_t, 4>> pixels(Side * Side);
    layout.Detile(std::span(tiled).first(target.bytes), std::as_writable_bytes(std::span(pixels)));
    const std::array<std::uint32_t, 4> drawn{word, 0u, 0u, std::bit_cast<std::uint32_t>(1.0f)};
    std::uint32_t checked = 0u;
    std::uint32_t covered = 0u;
    for (std::uint32_t y = 0; y < Side; ++y) {
        for (std::uint32_t x = 0; x < Side; ++x) {
            const auto& pixel = pixels[y * Side + x];
            Require(pixel == drawn || pixel == std::array<std::uint32_t, 4>{}, "the recipe draw rendered stale or unexpected buffer bits at pixel " + std::to_string(y * Side + x) + ": expected red " + std::to_string(word) + ", got " + std::to_string(pixel[0]) + "," + std::to_string(pixel[1]) + "," + std::to_string(pixel[2]) + "," + std::to_string(pixel[3]));
            covered += pixel == drawn ? 1u : 0u;
            const auto distance = std::min({EdgeDistance(x + 0.5, y + 0.5, 0u), EdgeDistance(x + 0.5, y + 0.5, 1u), EdgeDistance(x + 0.5, y + 0.5, 2u)});
            if (std::fabs(distance) < EdgeMargin) continue;
            ++checked;
            Require((pixel == drawn) == (distance > 0.0), "the recipe result disagrees with triangle coverage");
        }
    }
    Require(checked >= Side * Side * 9u / 10u && covered != 0u && covered != pixels.size(), "the recipe fixture did not check covered and clear pixels");
    std::printf("draw recipe: word=0x%08x covered=%u checked=%u; pixels and neighboring bytes passed\n", word, covered, checked);
}

void CheckMovedBuffer(std::unique_ptr<AgcDriver::VulkanDevice>& device) {
    using namespace AgcDriver::Graphics;
    Block source(4u * BlockBytes);
    Block movedSource(4u * BlockBytes);
    Block destination(2u * BlockBytes);
    Block vertexData(BlockBytes);
    struct ReleaseDevice {
        std::unique_ptr<AgcDriver::VulkanDevice>& value;
        ~ReleaseDevice() { value.reset(); }
    } releaseDevice{device};
    for (std::size_t offset = 0; offset < source.bytes; offset += sizeof(FillOnes)) std::memcpy(source.data + offset, &FillOnes, sizeof(FillOnes));
    const std::array<std::uint32_t, 5> movedWords{0x3e800000u, 0x3e000000u, MovedWord, 0x3e800000u, 0x3f400000u};
    std::memcpy(movedSource.data, movedWords.data(), sizeof(movedWords));
    std::memcpy(vertexData.data, Triangle.data(), sizeof(Triangle));
    ColorTarget output{};
    output.address = destination.Address();
    output.surfaceAddress = output.address;
    output.extent = {Side, Side};
    output.surfaceExtent = output.extent;
    output.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    output.componentMapping = 0xe4u;
    output.elementBytes = 16u;
    output.tileMode = ColorTileMode::RenderTarget;
    output.bytes = ColorTargetLayout(Side, Side, output.tileMode, output.elementBytes).Bytes();
    Require(output.bytes < destination.bytes, "the recipe target has no neighboring sentinel range");
    std::fill(destination.data + output.bytes, destination.data + destination.bytes, std::uint8_t{0xa5});
    const auto state = TargetState(output);
    auto vertex = CompileMovedVertex(*device, vertexData.Address());
    alignas(256) static constexpr std::array<std::uint32_t, 9> code{
        0x7e020280u, 0xe0301000u, 0x80000201u, 0xbf8c3f70u, 0x7e000280u, 0x7e0602f2u, 0xf800180fu, 0x03010002u, 0xbf810000u,
    };
    const auto address = source.Address() + 4u;
    const std::array<std::uint32_t, 4> userData{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), Threads * 4u, 0x31016facu};
    const auto codeAddress = reinterpret_cast<std::uintptr_t>(code.data());
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{codeAddress, std::as_bytes(std::span(code))}}};
    const AgcDriver::Registers registers{{0x1b6, 0x8000}, {0x1b3, 0}, {0x1b4, 0}, {0x203, 0}, {0x1c5, Float32Export}};
    const auto vertexPush = static_cast<std::uint32_t>(vertex.pushConstants.size());
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Fragment, codeAddress, code, 0, {}},
        {64, 0, userData, std::nullopt, DecodePixelStageInfo(registers, ExportMappings(state)), std::nullopt, memory},
        device->Target(), PixelPushLayout(vertexPush, device->Target())
    };
    request.useCache = false;
    auto fragment = ShaderRecompiler::Recompile(request);
    const auto binding = std::find_if(fragment.bindings.begin(), fragment.bindings.end(), [](const auto& entry) { return entry.role == ShaderRecompiler::DescriptorRole::GuestBuffers; });
    Require(binding != fragment.bindings.end() && binding->count == 1u && binding->guestDescriptor.size() == 4u && binding->bufferWritten.size() == 1u && !binding->bufferWritten[0], "the recipe fragment shader does not have one read-only guest buffer");
    const std::array<CompiledShader, 2> shaders{{{ShaderStage::Vertex, &vertex, 0u}, {ShaderStage::Fragment, &fragment, PixelPushOffset(vertexPush, device->Target())}}};
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(Triangle.size()), 0, 1, 0, false};
    std::shared_ptr<const AgcDriver::DrawRecipe> recipe;
    device->Draw(state, draw, shaders, {}, &recipe);
    Require(recipe != nullptr, "the initial buffer draw did not capture a recipe");
    CheckRecipePixels(*device, destination, output, FillOnes);
    Require(device->DrawFromRecipe(state, draw, shaders, {}, recipe) == AgcDriver::RecipeOutcome::Recorded, "the unchanged buffer recipe did not replay");
    CheckRecipePixels(*device, destination, output, FillOnes);
    const auto movedAddress = movedSource.Address() + 8u;
    binding->guestDescriptor[0] = static_cast<std::uint32_t>(movedAddress);
    binding->guestDescriptor[1] = (binding->guestDescriptor[1] & 0xffff0000u) | static_cast<std::uint32_t>((movedAddress >> 32u) & 0xffffu);
    const auto outcome = device->DrawFromRecipe(state, draw, shaders, {}, recipe);
    std::printf("ordinary moved draw recipe: outcome=%s\n", outcome == AgcDriver::RecipeOutcome::Recorded ? "recorded" : "rebuild");
    if (outcome == AgcDriver::RecipeOutcome::Rebuild) device->Draw(state, draw, shaders, {}, &recipe);
    else Require(outcome == AgcDriver::RecipeOutcome::Recorded, "the moved buffer recipe returned an unknown outcome");
    CheckRecipePixels(*device, destination, output, MovedWord);
}

}

int main() {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        CheckMovedBuffer(device);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
