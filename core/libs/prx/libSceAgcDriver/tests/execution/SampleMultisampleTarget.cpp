#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTarget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#include "Optimization/ResourceProgram.hpp"
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
#include <mutex>
#include <span>
#include <string>
#include <vector>

#ifdef _WIN32
namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}
#endif

namespace {

using AgcDriver::Graphics::ColorTargetLayout;
using AgcDriver::Graphics::ColorTileMode;
using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Side = 16;
constexpr std::uint32_t Samples = 4;
constexpr std::uint32_t SampleShift = 2;
constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t Float16x4Info = 0x0730u;
constexpr std::uint32_t MultisampledAttrib = (SampleShift << 12u) | (SampleShift << 15u);
constexpr std::uint32_t TiledAttrib3 = 0x4dc6c000u;
constexpr std::uint32_t Float32Export = 9;
constexpr std::uint32_t Format16161616Float = 71;
constexpr std::uint32_t Type2DMsaa = 14;
constexpr std::uint32_t TileRenderTarget = 0x1b;
constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Words = 32;
constexpr std::uint32_t LoadResult = 12;
constexpr std::array<float, 4> Drawn{0.25f, 0.5f, 0.125f, 1.0f};
constexpr double EdgeMargin = 1.0 / 32.0;
constexpr std::uint32_t FillOnes = 0x3c003c00u;
constexpr std::array<float, 4> Ones{1.0f, 1.0f, 1.0f, 1.0f};

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 11> PixelCode{
    0x7e0802ff, 0x3e800000, 0x7e0a02ff, 0x3f000000, 0x7e0c02ff, 0x3e000000, 0x7e0e02ff, 0x3f800000, 0xf800180f, 0x07060504, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 26> LoadCode{
    0x34020087, 0xe0301000, 0x80000201, 0xe0301004, 0x80000301, 0xe0301008, 0x80000401, 0xe030100c,
    0x80000501, 0xe0301010, 0x80000601, 0xe0301014, 0x80000701, 0xbf8c3f70, 0xf0001f30, 0x00011005,
    0xbf8c3f70, 0xe0701030, 0x80001001, 0xe0701034, 0x80001101, 0xe0701038, 0x80001201, 0xe070103c,
    0x80001301, 0xbf810000,
};

constexpr std::array<std::array<float, 4>, 3> Triangle{{
    {-1.0f, -1.0f, 0.5f, 1.0f}, {0.83f, -1.0f, 0.5f, 1.0f}, {-1.0f, 0.71f, 0.5f, 1.0f}
}};

constexpr std::array<std::array<float, 4>, 3> FullTriangle{{
    {-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}
}};

alignas(256) constexpr std::array<std::uint32_t, 11> OnesPixelCode{
    0x7e0802ff, 0x3f800000, 0x7e0a02ff, 0x3f800000, 0x7e0c02ff, 0x3f800000, 0x7e0e02ff, 0x3f800000, 0xf800180f, 0x07060504, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 11> HalvesPixelCode{
    0x7e0802ff, 0x3f000000, 0x7e0a02ff, 0x3f000000, 0x7e0c02ff, 0x3f000000, 0x7e0e02ff, 0x3f000000, 0xf800180f, 0x07060504, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 7> BufferLoadCode{
    0x34020082, 0xe8001000u | (20u << 19u), 0x80000201, 0xbf8c0000, 0xe0701000, 0x80010201, 0xbf810000,
};

alignas(256) std::array<std::uint32_t, Threads * Words> Buffer{};

struct Block {
    explicit Block(std::size_t bytes, bool resident = false) : bytes(bytes) {
#ifdef _WIN32
        data = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, BlockBytes));
        if (resident) {
            section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, bytes, nullptr);
            Require(section != nullptr, "cannot create the multisampled color target's resident section");
            GuestArena::GuestArenaMapDirect_nid_postfix(data, bytes, section, 0u, PAGE_READWRITE);
        } else {
            GuestArena::GuestArenaCommit_nid_postfix(data, bytes, PAGE_READWRITE, bytes);
        }
#else
        Require(!resident, "private resident color target sections require Windows");
        data = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, bytes));
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(data, bytes);
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
        GuestArena::GuestArenaReset_nid_postfix(data, bytes);
        GuestArena::GuestArenaRelease_nid_postfix(data, bytes);
        if (section != nullptr) {
            GuestArena::GuestArenaForgetSection_nid_postfix(section);
            CloseHandle(section);
        }
#else
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(data, bytes);
        std::free(data);
#endif
    }
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(data); }
    std::size_t bytes;
    std::uint8_t* data = nullptr;
#ifdef _WIN32
    HANDLE section = nullptr;
#endif
};

AgcDriver::Graphics::ColorTarget DecodeTarget(const Block& block) {
    AgcDriver::Registers cx;
    cx[0x318] = static_cast<std::uint32_t>(block.Address() >> 8u);
    cx[0x31b] = 0;
    cx[0x31c] = Float16x4Info;
    cx[0x31d] = MultisampledAttrib;
    cx[0x321] = 0u;
    cx[0x3b0] = ((Side - 1u) << 14u) | (Side - 1u);
    cx[0x3b8] = TiledAttrib3;
    cx[0x390] = static_cast<std::uint32_t>(block.Address() >> 40u);
    const auto color = AgcDriver::Graphics::DecodeColorBuffer(cx, 0);
    Require(color.format == VK_FORMAT_R16G16B16A16_SFLOAT && color.samples == Samples && color.extent.width == Side && color.extent.height == Side, "the target did not decode as a 16x16 RGBA16F surface of 4 samples");
    return color;
}

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
    state.samples = Samples;
    return state;
}

void Draw(AgcDriver::VulkanDevice& device, const AgcDriver::Graphics::State& state, std::span<const std::array<float, 4>> vertices = Triangle, std::span<const std::uint32_t> pixelCode = PixelCode) {
    const auto target = device.Target();
    constexpr std::uint32_t waveSize = 64;
    std::vector<std::uint32_t> vertexUserData(4, 0u);
    const auto address = reinterpret_cast<std::uintptr_t>(vertices.data());
    const std::array<std::uint32_t, 4> vertexBuffer{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (16u << 16u), static_cast<std::uint32_t>(vertices.size()), 0x01016facu};
    std::copy(vertexBuffer.begin(), vertexBuffer.end(), vertexUserData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {waveSize, 0, vertexUserData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target,
        {0, 0, 0, 64}
    };
    vertex.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());
    const auto codeAddress = reinterpret_cast<std::uintptr_t>(pixelCode.data());
    const AgcDriver::Registers context{{0x1b6, 0x8000}, {0x1b3, 0}, {0x1b4, 0}, {0x203, 0}, {0x1c5, Float32Export}};
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{codeAddress, std::as_bytes(pixelCode)}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, codeAddress, pixelCode, 0, {}},
        {waveSize, 0, {}, std::nullopt, AgcDriver::Graphics::DecodePixelStageInfo(context, {}), std::nullopt, pixelMemory},
        target,
        PixelPushLayout(vertexPush, target)
    };
    fragment.useCache = false;
    const auto handle = ShaderRecompiler::PrepareShader(fragment);
    AgcDriver::DriverDetail::ShaderSnapshot snapshot{codeAddress, 0, 1, {pixelCode.begin(), pixelCode.end()}, {}};
    snapshot.prepared->entries.push_back({0, handle});
    fragment.shader.code = snapshot.code;
    fragment.context.pixel = AgcDriver::Graphics::DecodePixelStageInfo(context, AgcDriver::Graphics::ExportMappings(state));
    const ShaderRecompiler::SrtRuntime runtime{};
    const auto invocation = AgcDriver::DriverDetail::InvocationFor(snapshot, 0, fragment);
    const auto pixelResult = invocation.Materialize(*invocation.Capture(runtime));
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0},
        {ShaderStage::Fragment, pixelResult.get(), PixelPushOffset(vertexPush, target)}
    }};
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(vertices.size()), 0, 1, 0, false};
    device.Draw(state, draw, shaders);
}

std::array<std::uint32_t, 8> TargetDescriptor(const Block& block, std::uint32_t sampleShift) {
    const auto address = block.Address();
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format16161616Float << 20u) | (((Side - 1u) & 3u) << 30u),
        ((Side - 1u) >> 2u) | ((Side - 1u) << 14u),
        0xfacu | (sampleShift << 16u) | (TileRenderTarget << 20u) | (Type2DMsaa << 28u),
        0u,
        sampleShift << 4u,
        0u,
        0u,
    };
}

ShaderRecompiler::RecompileResult CompileLoad(AgcDriver::VulkanDevice& device, const std::array<std::uint32_t, 8>& texture, std::vector<std::uint32_t>& userData) {
    const auto address = reinterpret_cast<std::uintptr_t>(Buffer.data());
    const std::array<std::uint32_t, 4> buffer{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), static_cast<std::uint32_t>(Buffer.size() * 4u), 0x31016facu};
    userData.assign(16, 0u);
    std::copy(buffer.begin(), buffer.end(), userData.begin());
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(LoadCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

std::vector<std::array<float, 4>> LoadSamples(AgcDriver::VulkanDevice& device, const Block& block, std::uint32_t sampleShift = SampleShift, std::uint32_t tileMode = TileRenderTarget) {
    std::vector<std::uint32_t> userData;
    const auto samples = 1u << sampleShift;
    auto descriptor = TargetDescriptor(block, sampleShift);
    descriptor[3] = (descriptor[3] & ~(0x1fu << 20u)) | (tileMode << 20u);
    const auto program = CompileLoad(device, descriptor, userData);
    std::vector<std::array<float, 4>> loaded(Side * Side * samples);
    for (std::uint32_t first = 0; first < loaded.size(); first += Threads) {
        Buffer.fill(0xdeadbeefu);
        for (std::uint32_t tid = 0; tid < Threads; ++tid) {
            const auto index = first + tid;
            const auto pixel = index / samples;
            const std::array<std::uint32_t, 6> words{0u, 0u, 0u, pixel % Side, pixel / Side, index % samples};
            std::copy(words.begin(), words.end(), &Buffer[tid * Words]);
        }
        device.Dispatch(program, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(LoadCode.data()));
        device.WaitIdle();
        for (std::uint32_t tid = 0; tid < Threads; ++tid) {
            for (std::uint32_t component = 0; component < 4u; ++component) loaded[first + tid][component] = std::bit_cast<float>(Buffer[tid * Words + LoadResult + component]);
        }
    }
    return loaded;
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

void CheckSamples(const AgcDriver::Graphics::State& state, const std::vector<std::array<float, 4>>& loaded, const std::array<float, 4>& background) {
    const auto locations = AgcDriver::Graphics::SampleLocations(state);
    std::array<std::uint32_t, Samples + 1u> pixelsByCoverage{};
    std::array<std::uint32_t, Samples> coveredBySample{};
    std::uint32_t checked = 0;
    for (std::uint32_t y = 0; y < Side; ++y) {
        for (std::uint32_t x = 0; x < Side; ++x) {
            std::uint32_t covered = 0;
            for (std::uint32_t sample = 0; sample < Samples; ++sample) {
                const auto& texel = loaded[(y * Side + x) * Samples + sample];
                const bool drawn = texel == Drawn;
                const bool empty = texel == background;
                char where[160];
                std::snprintf(where, sizeof(where), "sample %u of pixel (%u, %u) loaded (%g, %g, %g, %g)", sample, x, y, texel[0], texel[1], texel[2], texel[3]);
                Require(drawn || empty, std::string(where) + ", neither the drawn color nor the cleared target");
                covered += drawn ? 1u : 0u;
                coveredBySample[sample] += drawn ? 1u : 0u;
                const auto px = x + static_cast<double>(locations[sample].x);
                const auto py = y + static_cast<double>(locations[sample].y);
                const auto distance = std::min({EdgeDistance(px, py, 0), EdgeDistance(px, py, 1), EdgeDistance(px, py, 2)});
                if (std::fabs(distance) < EdgeMargin) continue;
                ++checked;
                Require(drawn == (distance > 0), std::string(where) + (distance > 0 ? ", which the triangle covers" : ", which the triangle does not cover"));
            }
            ++pixelsByCoverage[covered];
        }
    }
    Require(checked >= Side * Side * Samples * 9u / 10u, "fewer than nine in ten samples are clear of the triangle's edges");
    Require(pixelsByCoverage[0] != 0 && pixelsByCoverage[Samples] != 0, "the triangle covers every pixel or none");
    Require(pixelsByCoverage[1] + pixelsByCoverage[2] + pixelsByCoverage[3] != 0, "no pixel is partly covered, so the samples of a pixel were not told apart");
    Require(std::adjacent_find(coveredBySample.begin(), coveredBySample.end(), std::not_equal_to<>()) != coveredBySample.end(), "every sample index has the same coverage, so the sample index was not told apart");
}

void CheckUniform(const std::vector<std::array<float, 4>>& loaded, const std::array<float, 4>& expected, const std::string& what) {
    for (std::size_t index = 0; index < loaded.size(); ++index) {
        const auto& texel = loaded[index];
        char where[160];
        std::snprintf(where, sizeof(where), "sample %zu of pixel (%zu, %zu) loaded (%g, %g, %g, %g)", index % Samples, index / Samples % Side, index / Samples / Side, texel[0], texel[1], texel[2], texel[3]);
        Require(texel == expected, std::string(where) + " " + what);
    }
}

ShaderRecompiler::RecompileResult CompileBufferLoad(AgcDriver::VulkanDevice& device, std::uint64_t source, std::uint64_t destination, std::uint32_t sourceBytes = Threads * 4u) {
    const std::array<std::uint32_t, 8> userData{
        static_cast<std::uint32_t>(source), static_cast<std::uint32_t>((source >> 32u) & 0xffffu), sourceBytes, 0x31016facu,
        static_cast<std::uint32_t>(destination), static_cast<std::uint32_t>((destination >> 32u) & 0xffffu), Threads * 4u, 0x31016facu
    };
    const std::span<const std::uint32_t> code(BufferLoadCode);
    const auto address = reinterpret_cast<std::uintptr_t>(code.data());
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{address, std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, address, code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory}, device.Target(), {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

bool BufferPublication() {
    auto device = OpenVulkanTestDevice();
    if (!device) return false;
    Block block(4u * BlockBytes);
    Block output(2u * 1024u * 1024u);
    struct ReleaseDevice {
        decltype(device)& value;
        ~ReleaseDevice() { value.reset(); }
    } releaseDevice{device};
    const std::span words(reinterpret_cast<std::uint32_t*>(output.data), Threads * 2u);
    const auto reset = [](std::span<std::uint32_t> destination) { std::fill(destination.begin(), destination.end(), 0xdeadbeefu); };
    const auto checkBoundary = [](std::span<std::uint32_t> destination) { Require(std::all_of(destination.begin() + Threads, destination.end(), [](auto word) { return word == 0xdeadbeefu; }), "native buffer publication overwrote its output boundary"); };
    const auto programAddress = reinterpret_cast<std::uintptr_t>(BufferLoadCode.data());
    const auto warm = CompileBufferLoad(*device, block.Address(), output.Address());
    std::shared_ptr<const AgcDriver::Recipe> recipe;
    reset(words);
    device->Dispatch(warm, 1, 1, 1, {}, programAddress, nullptr, &recipe);
    device->WaitIdle();
    checkBoundary(words);
    Require(recipe != nullptr, "the native buffer publication control did not capture a recipe");
    Require(std::all_of(words.begin(), words.begin() + Threads, [](auto word) { return word == 0; }), "the native buffer publication control did not load zeros");
    constexpr std::uint32_t stagedBytes = 2u * BlockBytes;
    const auto stagedOutput = block.Address() + BlockBytes;
    const std::span stagedWords(reinterpret_cast<std::uint32_t*>(block.data + BlockBytes), Threads * 2u);
    const auto stagedWarm = CompileBufferLoad(*device, block.Address(), stagedOutput, stagedBytes);
    std::shared_ptr<const AgcDriver::Recipe> stagedRecipe;
    reset(stagedWords);
    device->Dispatch(stagedWarm, 1, 1, 1, {}, programAddress, nullptr, &stagedRecipe);
    device->WaitIdle();
    checkBoundary(stagedWords);
    Require(stagedRecipe != nullptr, "the staged native buffer control did not capture a recipe");
    Require(std::all_of(stagedWords.begin(), stagedWords.begin() + Threads, [](auto word) { return word == 0; }), "the staged native buffer control did not load zeros");
    const auto stagedDeferred = CompileBufferLoad(*device, block.Address() + 4u, stagedOutput, stagedBytes);
    const auto stagedPrepared = device->PrepareDispatch(stagedDeferred, {}, Threads, programAddress);
    Require(stagedPrepared != nullptr, "the staged native buffer control did not prepare its deferred resources");
    std::cout << "native buffer staging: source=0x" << std::hex << block.Address() << " bytes=0x" << stagedBytes << std::dec << '\n';
    const auto deferred = CompileBufferLoad(*device, block.Address() + 128u, output.Address());
    const auto prepared = device->PrepareDispatch(deferred, {}, Threads, programAddress);
    Require(prepared != nullptr, "the native buffer publication control did not prepare its deferred resources");
    const auto color = DecodeTarget(block);
    Require(AgcDriver::Graphics::MultisampledColorBytes(color) == BlockBytes, "the staged native buffer output overlaps the target");
    const auto state = TargetState(color);
    std::uint32_t missing = 0;
    const auto check = [&](const char* name, std::uint64_t source, std::uint32_t expected, std::span<std::uint32_t> destination, const auto& dispatch) {
        const bool ones = expected == FillOnes;
        const std::array<float, 4> value = ones ? Ones : std::array<float, 4>{0.5f, 0.5f, 0.5f, 0.5f};
        Draw(*device, state, FullTriangle, ones ? std::span<const std::uint32_t>(OnesPixelCode) : std::span<const std::uint32_t>(HalvesPixelCode));
        CheckUniform(LoadSamples(*device, block), value, "before the native buffer read");
        reset(destination);
        dispatch();
        device->WaitIdle();
        checkBoundary(destination);
        std::array<std::uint32_t, Threads> actual{};
        std::copy_n(destination.begin(), Threads, actual.begin());
        std::array<std::uint32_t, Threads> published{};
        AgcDriver::GuestMemory::Read(source, std::as_writable_bytes(std::span(published)));
        Require(std::any_of(published.begin(), published.end(), [&](auto word) { return word == expected; }) && std::all_of(published.begin(), published.end(), [&](auto word) { return word == 0 || word == expected; }), "the explicit native publication control did not match the rendered color and clear padding");
        const bool matched = actual == published;
        missing += !matched;
        CheckUniform(LoadSamples(*device, block), value, "after the native buffer read");
        std::cout << "native buffer publication " << name << ": matched=" << matched << " samples=1024 boundary=passed\n";
    };
    check("warmed", block.Address(), FillOnes, words, [&] { device->Dispatch(warm, 1, 1, 1, {}, programAddress); });
    check("deferred", block.Address() + 128u, 0x38003800u, words, [&] { device->Dispatch(deferred, 1, 1, 1, {}, programAddress, prepared); });
    const auto fresh = CompileBufferLoad(*device, block.Address() + 256u, output.Address());
    check("fresh", block.Address() + 256u, FillOnes, words, [&] { device->Dispatch(fresh, 1, 1, 1, {}, programAddress); });
    check("recipe", block.Address(), 0x38003800u, words, [&] {
        const auto hit = device->PrepareRecipe(recipe, false);
        Require(hit != nullptr, "the native buffer publication recipe was not reusable");
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        AgcDriver::VulkanDevice::IndirectOutcome outcome{};
        device->DispatchRecipe(warm, 1, 1, 1, 0, programAddress, hit, outcome);
    });
    check("staged-warmed", block.Address(), FillOnes, stagedWords, [&] { device->Dispatch(stagedWarm, 1, 1, 1, {}, programAddress); });
    check("staged-deferred", block.Address() + 4u, 0x38003800u, stagedWords, [&] { device->Dispatch(stagedDeferred, 1, 1, 1, {}, programAddress, stagedPrepared); });
    const auto stagedFresh = CompileBufferLoad(*device, block.Address() + 8u, stagedOutput, stagedBytes);
    check("staged-fresh", block.Address() + 8u, FillOnes, stagedWords, [&] { device->Dispatch(stagedFresh, 1, 1, 1, {}, programAddress); });
    check("staged-recipe", block.Address(), 0x38003800u, stagedWords, [&] {
        const auto hit = device->PrepareRecipe(stagedRecipe, false);
        Require(hit != nullptr, "the staged native buffer recipe was not reusable");
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        AgcDriver::VulkanDevice::IndirectOutcome outcome{};
        device->DispatchRecipe(stagedWarm, 1, 1, 1, 0, programAddress, hit, outcome);
    });
    std::array<std::uint32_t, Threads> neighborValues{};
    for (std::uint32_t lane = 0; lane < Threads; ++lane) neighborValues[lane] = 0x12340000u + lane;
    std::copy(neighborValues.begin(), neighborValues.end(), stagedWords.begin());
    const auto neighbor = CompileBufferLoad(*device, block.Address() + AgcDriver::Graphics::MultisampledColorBytes(color), output.Address());
    reset(words);
    device->Dispatch(neighbor, 1, 1, 1, {}, programAddress);
    device->WaitIdle();
    checkBoundary(words);
    Require(std::equal(words.begin(), words.begin() + Threads, neighborValues.begin()), "the neighboring buffer did not retain its distinct input pattern");
    CheckUniform(LoadSamples(*device, block), {0.5f, 0.5f, 0.5f, 0.5f}, "after the distinct neighboring buffer input");
    const auto writer = CompileBufferLoad(*device, output.Address(), block.Address() + 256u);
    std::string refusal;
    try {
        device->Dispatch(writer, 1, 1, 1, {}, programAddress);
        device->WaitIdle();
    } catch (const std::exception& error) {
        refusal = error.what();
    }
    const bool refused = refusal.find("whose samples are only on the host") != std::string::npos;
    missing += !refused;
    std::cout << "native buffer publication controls: neighbor=passed recipe=1 deferred=1 write-refused=" << refused << '\n';
    Require(missing == 0, "native buffer publication missed " + std::to_string(missing) + " logical descriptor checks");
    return true;
}

void StoreWords(const Block& block, std::uint64_t bytes, std::uint32_t word) {
    for (std::uint64_t offset = 0; offset < bytes; offset += sizeof(word)) std::memcpy(block.data + offset, &word, sizeof(word));
}

void RequireRefused(AgcDriver::VulkanDevice& device, const std::array<std::uint32_t, 8>& texture, const std::string& expected, const std::string& what) {
    std::string refusal;
    try {
        std::vector<std::uint32_t> userData;
        const auto program = CompileLoad(device, texture, userData);
        Buffer.fill(0u);
        device.Dispatch(program, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(LoadCode.data()));
        device.WaitIdle();
    } catch (const std::exception& error) {
        refusal = error.what();
    }
    Require(refusal.find(expected) != std::string::npos, what + " was not refused: " + refusal);
}

#ifdef _WIN32
bool MovedBufferPublication() {
    auto device = OpenVulkanTestDevice();
    if (!device) return false;
    Block source(4u * BlockBytes);
    Block original(BlockBytes);
    Block target(4u * BlockBytes);
    struct ReleaseDevice {
        decltype(device)& value;
        ~ReleaseDevice() { value.reset(); }
    } releaseDevice{device};
    StoreWords(original, original.bytes, FillOnes);
    alignas(256) static constexpr std::array<std::uint32_t, 9> code{
        0x7e020280u, 0xe0301000u, 0x80000201u, 0xbf8c3f70u, 0x7e000280u, 0x7e0602f2u, 0xf800180fu, 0x03010002u, 0xbf810000u,
    };
    const std::array<std::uint32_t, 4> userData{
        static_cast<std::uint32_t>(original.Address()), static_cast<std::uint32_t>((original.Address() >> 32u) & 0xffffu), Threads * 4u, 0x31016facu
    };
    const auto address = reinterpret_cast<std::uintptr_t>(code.data());
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{address, std::as_bytes(std::span(code))}}};
    const auto destination = DecodeTarget(target);
    const AgcDriver::Registers registers{{0x1b6, 0x8000}, {0x1b3, 0}, {0x1b4, 0}, {0x203, 0}, {0x1c5, Float32Export}};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Fragment, address, code, 0, {}},
        {64, 0, userData, std::nullopt, AgcDriver::Graphics::DecodePixelStageInfo(registers, AgcDriver::Graphics::ExportMappings(TargetState(destination))), std::nullopt, memory},
        device->Target(), {0, 0, 0, 128}
    };
    request.useCache = false;
    auto program = ShaderRecompiler::Recompile(request);
    const auto binding = std::find_if(program.bindings.begin(), program.bindings.end(), [](const auto& entry) { return entry.role == ShaderRecompiler::DescriptorRole::GuestBuffers; });
    Require(binding != program.bindings.end() && binding->count == 1 && binding->guestDescriptor.size() == 4 && binding->bufferWritten.size() == 1 && !binding->bufferWritten[0], "the moved graphics shader does not have one read-only guest buffer");
    const std::array<AgcDriver::Graphics::CompiledShader, 1> stages{{{ShaderStage::Fragment, &program, 0}}};
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device);
    Require(context.recorder != nullptr, "the moved graphics fixture has no recorder");
    std::unique_ptr<AgcDriver::Graphics::ShaderResources> resources;
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        context.recorder->Activate();
        resources = std::make_unique<AgcDriver::Graphics::ShaderResources>(context, stages, destination, 1, 0, 0);
        Require(resources->Reusable(), "the moved graphics fixture did not build reusable resources");
    }
    CheckUniform(LoadSamples(*device, original, SampleShift, 9u), Ones, "from the original standard-tiled buffer");
    std::string refusal;
    try {
        AgcDriver::Graphics::SynchronizeMultisampleTargetRead(original.Address(), Threads * 4u);
    } catch (const std::exception& error) {
        refusal = error.what();
    }
    Require(refusal.find("uniform standard multisample target") != std::string::npos, "the original graphics range is not an unsupported publication control");
    Draw(*device, TargetState(DecodeTarget(source)), FullTriangle, OnesPixelCode);
    CheckUniform(LoadSamples(*device, source), Ones, "before the moved graphics snapshot");
    const std::span guest(reinterpret_cast<const std::uint32_t*>(source.data), Threads);
    Require(std::all_of(guest.begin(), guest.end(), [](auto word) { return word == 0; }), "the moved graphics fixture published its current range before snapshot preparation");
    binding->guestDescriptor[0] = static_cast<std::uint32_t>(source.Address());
    binding->guestDescriptor[1] = (binding->guestDescriptor[1] & 0xffff0000u) | static_cast<std::uint32_t>((source.Address() >> 32u) & 0xffffu);
    std::array<std::uint32_t, Threads> actual{};
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        context.recorder->Activate();
        Require(resources->Revalidate(stages), "the moved graphics resources were not reusable");
        const auto moved = resources->MovedReadOnlyBuffers(stages, *context.recorder);
        Require(moved.has_value() && std::any_of(moved->begin(), moved->end(), [&](const auto& entry) { return entry.address == source.Address() && entry.size == Threads * 4u; }), "the current graphics range was not recorded as a moved buffer");
        const auto prepared = resources->PrepareDrawBindings(*context.recorder, *moved);
        Require(prepared != nullptr, "the moved graphics snapshot was not prepared");
        const auto snapshot = std::find_if(prepared->snapshots.begin(), prepared->snapshots.end(), [&](const auto& entry) { return entry.address == source.Address(); });
        Require(snapshot != prepared->snapshots.end() && snapshot->buffer->Bytes().size() == sizeof(actual), "the moved graphics snapshot has the wrong range");
        std::memcpy(actual.data(), snapshot->buffer->Bytes().data(), sizeof(actual));
    }
    std::array<std::uint32_t, Threads> published{};
    AgcDriver::GuestMemory::Read(source.Address(), std::as_writable_bytes(std::span(published)));
    Require(std::all_of(published.begin(), published.end(), [](auto word) { return word == FillOnes; }), "the moved graphics publication control did not contain rendered half-float ones");
    const bool matched = actual == published;
    CheckUniform(LoadSamples(*device, source), Ones, "after the moved graphics snapshot");
    CheckUniform(LoadSamples(*device, original, SampleShift, 9u), Ones, "after moving away from the standard-tiled buffer");
    std::cout << "native buffer publication moved: matched=" << matched << " samples=1024 boundary=passed\n";
    std::puts("native buffer moved controls: original-refused=1 current-range=1 revalidated=1");
    Require(matched, "native buffer publication missed 1 logical descriptor checks");
    return true;
}

void ResidentDrawNeighborImport(AgcDriver::VulkanDevice& device) {
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    Block block(4u * BlockBytes, true);
    struct Cleanup {
        AgcDriver::VulkanDevice& device;
        VkDevice handle;
        ~Cleanup() {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            device.SubmitRecorded(true);
            device.WaitIdle();
            AgcDriver::Graphics::ClearMultisampleTargets(handle);
            AgcDriver::Graphics::ClearHostImports(handle);
        }
    } cleanup{device, context.device};
    const auto color = DecodeTarget(block);
    Require(AgcDriver::Graphics::MultisampledColorBytes(color) == BlockBytes, "the resident drawn target does not occupy exactly one block");
    std::uintptr_t start = 0u, end = 0u;
    Require(GuestArena::GuestArenaResidentSpan_nid_postfix(block.Address(), block.bytes, &start, &end) && start == block.Address() && end == start + block.bytes, "the drawn target and its neighbor are not one private resident run");
    const auto state = TargetState(color);
    Draw(device, state);
    const auto before = LoadSamples(device, block);
    CheckSamples(state, before, {});
    bool neighborCompared = false;
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        Require(AgcDriver::GuestMemory::Watched(block.Address(), BlockBytes), "the drawn resident target is not watched before the neighboring import");
        const auto* imported = AgcDriver::Graphics::HostImportFor(context, block.Address() + BlockBytes, BlockBytes);
        Require(imported != nullptr && imported->alias == nullptr, "the neighboring import did not remain private");
        neighborCompared = imported->unwatched;
        Require(AgcDriver::GuestMemory::Watched(block.Address(), BlockBytes), "the neighboring import removed the drawn resident target's watch");
    }
    const auto after = LoadSamples(device, block);
    CheckSamples(state, after, {});
    Require(after == before, "the neighboring import changed the resident target's drawn samples");
    std::cout << "resident GPU-written target: private neighboring import compared=" << neighborCompared << ", " << after.size() << " samples preserved\n";
}
#endif

}

int main() {
    try {
        if (!BufferPublication()) return VulkanTestSkipped;
#ifdef _WIN32
        if (!MovedBufferPublication()) return VulkanTestSkipped;
#endif
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Block block(4u * BlockBytes);
        Block other(BlockBytes);
        const auto color = DecodeTarget(block);
        const auto state = TargetState(color);
        Draw(*device, state);
        CheckSamples(state, LoadSamples(*device, block), {});
        const auto colorBytes = AgcDriver::Graphics::MultisampledColorBytes(color);
        Require(colorBytes != 0 && colorBytes <= block.bytes, "the 4x target's color memory does not fit its block");
        AgcDriver::Graphics::NoteColorMetadataFill(block.Address(), static_cast<std::size_t>(colorBytes), FillOnes);
        AgcDriver::GuestMemory::StoreOwnBytes(block.Address(), static_cast<std::size_t>(colorBytes), [&] { std::fill_n(reinterpret_cast<std::uint32_t*>(block.data), colorBytes / 4u, FillOnes); });
        CheckUniform(LoadSamples(*device, block), Ones, "after the color fill");
        CheckUniform(LoadSamples(*device, block, 3u), Ones, "after the color fill with an 8-sample descriptor");
        const auto guestSamples = LoadSamples(*device, other);
        Require(std::all_of(guestSamples.begin(), guestSamples.end(), [](const auto& texel) { return texel == std::array<float, 4>{}; }), "a guest-only multisampled texture did not load its zero samples");
        Block stored(BlockBytes);
        StoreWords(stored, colorBytes, FillOnes);
        CheckUniform(LoadSamples(*device, stored), Ones, "from color memory of half-float ones no draw rendered to");
        const auto storedState = TargetState(DecodeTarget(stored));
        Draw(*device, storedState);
        CheckSamples(storedState, LoadSamples(*device, stored), Ones);
        for (const auto tileMode : {5u, 9u}) {
            Block uniform(BlockBytes);
            StoreWords(uniform, uniform.bytes, FillOnes);
            CheckUniform(LoadSamples(*device, uniform, SampleShift, tileMode), Ones, "from uniformly filled standard-tiled multisample memory");
            CheckUniform(LoadSamples(*device, uniform, SampleShift, tileMode), Ones, "from the reused standard-tiled multisample target");
            Block mixed(BlockBytes);
            StoreWords(mixed, mixed.bytes, FillOnes);
            auto standard = color;
            standard.tileMode = tileMode == 5u ? AgcDriver::Graphics::ColorTileMode::Standard4KB : AgcDriver::Graphics::ColorTileMode::Standard64KB;
            const auto bytes = AgcDriver::Graphics::MultisampledColorBytes(standard);
            Require(bytes >= sizeof(std::uint32_t) && bytes <= mixed.bytes, "the standard-tiled multisample fixture footprint is invalid");
            const std::uint32_t odd = 0x3c000000u;
            std::memcpy(mixed.data + bytes - sizeof(odd), &odd, sizeof(odd));
            auto descriptor = TargetDescriptor(mixed, SampleShift);
            descriptor[3] = (descriptor[3] & ~(0x1fu << 20u)) | (tileMode << 20u);
            RequireRefused(*device, descriptor, "does not hold one repeated 32-bit value", "nonuniform standard-tiled multisample initial contents");
        }
#ifdef _WIN32
        ResidentDrawNeighborImport(*device);
#endif
        std::puts("every sample of a 4x color target loaded through its multisampled texture descriptor matches the triangle drawn into it, then the fill of its color memory");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
