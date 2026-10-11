#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {
using AgcDriver::Graphics::Require;
bool negativeControl = false;
alignas(256) std::array<std::uint32_t, 128> input{};
alignas(256) std::array<std::uint32_t, 128> output{};
alignas(4096) std::array<std::uint32_t, 65536> fmaskStorage{};
alignas(4096) std::array<std::uint8_t, 131072> cmaskStorage{};
const std::span<std::uint32_t, 49152> fmask(fmaskStorage.data() + (((0u - reinterpret_cast<std::uintptr_t>(fmaskStorage.data())) & 0xffffu) / 4u), 49152u);
const std::span<std::uint8_t, 65536> cmask(cmaskStorage.data() + ((0u - reinterpret_cast<std::uintptr_t>(cmaskStorage.data())) & 0xffffu), 65536u);
constexpr std::array<std::uint32_t, 10> code{0x34020084u, 0xe0381000u, 0x80000201u, 0xbf8c3f70u, 0xf0001f08u, 0x00020602u, 0xbf8c3f70u, 0xe0781000u, 0x80010601u, 0xbf810000u};
std::array<std::uint32_t, 4> Buffer(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) & 0xffffu, bytes, 0x31016facu};
}
constexpr std::array<std::array<std::uint32_t, 32>, 3> fmaskAddresses{{
    {{0u, 28u, 112u, 300u, 448u, 476u, 8880u, 8940u, 12800u, 12828u, 12912u, 6444u, 6592u, 6620u, 6320u, 6380u, 10240u, 10268u, 10352u, 10540u, 10688u, 10716u, 34480u, 34540u, 38400u, 38428u, 38512u, 46380u, 46528u, 46556u, 46256u, 46316u}},
    {{67584u, 67612u, 67696u, 67884u, 68032u, 68060u, 76464u, 76524u, 80384u, 80412u, 80496u, 69932u, 70080u, 70108u, 69808u, 69868u, 73728u, 73756u, 73840u, 74028u, 74176u, 74204u, 102064u, 102124u, 105984u, 106012u, 106096u, 113964u, 114112u, 114140u, 113840u, 113900u}},
    {{132096u, 132124u, 132208u, 132396u, 132544u, 132572u, 140976u, 141036u, 144896u, 144924u, 145008u, 138540u, 138688u, 138716u, 138416u, 138476u, 142336u, 142364u, 142448u, 142636u, 142784u, 142812u, 164528u, 164588u, 168448u, 168476u, 168560u, 176428u, 176576u, 176604u, 176304u, 176364u}}
}};
constexpr std::array<std::array<std::uint32_t, 32>, 3> cmaskAddresses{{
    {{0u, 0u, 0u, 513u, 513u, 513u, 1024u, 1024u, 1025u, 1025u, 1025u, 4608u, 4608u, 4608u, 4097u, 4097u, 4098u, 4098u, 4098u, 4611u, 4611u, 4611u, 3078u, 3078u, 3079u, 3079u, 3079u, 2566u, 2566u, 2566u, 2055u, 2055u}},
    {{12288u, 12288u, 12288u, 12801u, 12801u, 12801u, 13312u, 13312u, 13313u, 13313u, 13313u, 8704u, 8704u, 8704u, 8193u, 8193u, 8194u, 8194u, 8194u, 8707u, 8707u, 8707u, 15366u, 15366u, 15367u, 15367u, 15367u, 14854u, 14854u, 14854u, 14343u, 14343u}},
    {{18432u, 18432u, 18432u, 18945u, 18945u, 18945u, 19456u, 19456u, 19457u, 19457u, 19457u, 23040u, 23040u, 23040u, 22529u, 22529u, 22530u, 22530u, 22530u, 23043u, 23043u, 23043u, 17414u, 17414u, 17415u, 17415u, 17415u, 16902u, 16902u, 16902u, 16391u, 16391u}}
}};
std::uint32_t Pixel(std::uint32_t x, std::uint32_t y, std::uint32_t layer) {
    return ((x * 0x9e3779b9u) ^ (y * 0x7f4a7c15u) ^ (layer * 0x85ebca6bu)) & 0x77777777u;
}
template<typename TAction>
void RequireRejected(TAction action, std::string_view expected) {
    try { action(); }
    catch (const std::runtime_error& error) {
        Require(std::string_view(error.what()).find(expected) != std::string_view::npos, "FMASK test received an unexpected refusal");
        return;
    }
    throw std::runtime_error("FMASK test expected a refusal");
}
void Run(AgcDriver::VulkanDevice& device, std::uint32_t format, bool compressed, std::uint8_t cmaskByte, std::uint32_t fmaskWord, const char* name, bool arrayImage = false, bool nonuniform = false, bool nullMetadata = false, bool constantOne = false) {
    std::fill(fmask.begin(), fmask.end(), fmaskWord);
    std::fill(cmask.begin(), cmask.end(), cmaskByte);
    if (nonuniform) {
        for (std::uint32_t layer = 0u; layer < (arrayImage ? 3u : 1u); ++layer) {
            for (std::uint32_t lane = 0u; lane < 32u; ++lane) {
                const auto value = Pixel(lane * 3u, lane, layer);
                const auto offset = format == 166u ? fmaskAddresses[layer][lane] : (layer * 8192u + lane * 128u + lane * 3u) * 4u;
                fmask[offset / 4u] = value;
                if (compressed && ((lane * 3u / 8u + lane / 8u + layer) & 1u) != 0u) {
                    const auto nibble = cmaskAddresses[layer][lane];
                    const auto shift = (nibble & 1u) * 4u;
                    cmask[nibble / 2u] = static_cast<std::uint8_t>((cmask[nibble / 2u] & ~(15u << shift)) | (12u << shift));
                }
            }
        }
    }
    output.fill(0xdeadbeefu);
    for (std::uint32_t lane = 0; lane < 32u; ++lane) {
        input[lane * 4u] = lane * 3u;
        input[lane * 4u + 1u] = lane;
        input[lane * 4u + 2u] = arrayImage ? lane & 1u : 0u;
        input[lane * 4u + 3u] = 0u;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(fmask.data());
    const auto meta = nullMetadata ? 0u : reinterpret_cast<std::uintptr_t>(cmask.data()) >> 8u;
    std::array<std::uint32_t, 8> image{
        static_cast<std::uint32_t>(address >> 8u),
        (static_cast<std::uint32_t>(address >> 40u) & 0xffu) | (format << 20u) | (3u << 30u),
        31u | (63u << 14u),
        4u | (constantOne ? 1u << 9u : 0u) | ((format == 166u ? 24u : 0u) << 20u) | ((arrayImage ? 13u : 9u) << 28u),
        arrayImage ? 2u | (1u << 16u) : 0u, 0u,
        compressed ? (1u << 19u) | (1u << 21u) | (static_cast<std::uint32_t>(meta & 0xffu) << 24u) : 0u,
        compressed ? static_cast<std::uint32_t>(meta >> 8u) : 0u
    };
    std::vector<std::uint32_t> userData(16u, 0u);
    const auto in = Buffer(input.data(), sizeof(input));
    const auto out = Buffer(output.data(), sizeof(output));
    std::copy(in.begin(), in.end(), userData.begin());
    std::copy(out.begin(), out.end(), userData.begin() + 4u);
    std::copy(image.begin(), image.end(), userData.begin() + 8u);
    auto instructions = code;
    if (arrayImage) instructions[4] = (instructions[4] & ~0x38u) | (5u << 3u);
    const std::span<const std::uint32_t> words(instructions);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(instructions.data()), std::as_bytes(words)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{32u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    ShaderRecompiler::RecompileRequest request{
        {ShaderRecompiler::ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(instructions.data()), words, 0u, {}},
        {32u, 0u, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(), {0u, 0u, 0u, 128u}
    };
    request.useCache = false;
    const auto shader = ShaderRecompiler::Recompile(request);
    device.Dispatch(shader, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(instructions.data()));
    device.WaitIdle();
    auto expected = std::array<std::uint32_t, 4>{format == 166u && compressed && cmaskByte == 0xccu ? 0u : fmaskWord, 0u, 0u, 0u};
    if (negativeControl && format == 20u) expected[0] ^= 1u;
    for (std::uint32_t lane = 0; lane < 32u; ++lane) {
        if (nonuniform) {
            const auto layer = arrayImage ? 1u + (lane & 1u) : 0u;
            expected[0] = compressed && ((lane * 3u / 8u + lane / 8u + layer) & 1u) != 0u ? 0u : Pixel(lane * 3u, lane, layer);
        }
        for (std::uint32_t component = 0; component < 4u; ++component) {
            Require(output[lane * 4u + component] == expected[component], std::string(name) + " observed output differs from diagnostic prediction: lane=" + std::to_string(lane) + " component=" + std::to_string(component) + " value=" + std::to_string(output[lane * 4u + component]));
        }
    }
    std::printf("case=%s format=%u compressed=%u cmask=%02x raw=%08x result=%08x,%08x,%08x,%08x lanes=32\n", name, format, compressed, cmaskByte, fmaskWord, output[0], output[1], output[2], output[3]);
}
}
int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device, 20u, false, 0xffu, 0x13579bdfu, "ordinary-r32-positive-control");
        Run(*device, 166u, true, 0xffu, 0x76543210u, "expanded-fmask");
        Run(*device, 166u, true, 0xccu, 0x76543210u, "cmask-fast-clear-same-address");
        Run(*device, 166u, false, 0xffu, 0u, "uncompressed-zero-fmask");
        Run(*device, 166u, false, 0xffu, 0x11111111u, "uncompressed-other-fmask");
        Run(*device, 166u, false, 0xffu, 0x11111100u, "uncompressed-mixed-fmask");
        Run(*device, 166u, true, 0xffu, 0x76543210u, "expanded-restored-same-address");
        Run(*device, 20u, false, 0xffu, 0x2468ace0u, "ordinary-r32-changed-same-address-control");
        Run(*device, 166u, false, 0xffu, 0u, "nonuniform-fmask", false, true);
        Run(*device, 166u, true, 0xffu, 0u, "mixed-metadata-fmask", false, true);
        Run(*device, 20u, false, 0xffu, 0u, "ordinary-r32-array-control", true, true);
        Run(*device, 166u, false, 0xffu, 0u, "nonuniform-array-fmask", true, true);
        Run(*device, 166u, true, 0xffu, 0u, "mixed-metadata-array-fmask", true, true);
        RequireRejected([&] { Run(*device, 165u, false, 0xffu, 0u, "unsupported-format"); }, "FMASK image format read semantics");
        RequireRejected([&] { Run(*device, 166u, true, 0xddu, 0u, "unsupported-compression"); }, "FMASK CMASK compression code");
        RequireRejected([&] { Run(*device, 166u, false, 0xffu, 0x88888888u, "unverified-fragment-index"); }, "FMASK contains an unverified fragment index");
        RequireRejected([&] { Run(*device, 166u, true, 0xffu, 0u, "null-metadata", false, false, true); }, "FMASK compressed image has a null CMASK address");
        RequireRejected([&] { Run(*device, 166u, false, 0xffu, 0u, "unverified-one-selector", false, false, false, true); }, "FMASK image selects an unverified channel");
        negativeControl = true;
        RequireRejected([&] { Run(*device, 20u, false, 0xffu, 0x13579bdfu, "negative-control"); }, "observed output differs");
        negativeControl = false;
        std::puts("FMASK image tests passed: 13 read cases, 5 unsupported-input refusals, negative control");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
