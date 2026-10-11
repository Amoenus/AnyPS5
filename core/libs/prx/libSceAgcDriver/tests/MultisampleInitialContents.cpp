#include "prx/libSceAgcDriver/Graphics/include/MultisampleInitialContents.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleDcc.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTextureLayout.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <string_view>

using namespace AgcDriver::Graphics;

template<typename Action>
void Refuses(Action action, std::string_view text) {
    std::string message;
    try { action(); }
    catch (const std::exception& error) { message = error.what(); }
    Require(message.find(text) != std::string::npos, "initial-content refusal did not match: " + message);
}

ColorTarget Target(std::uint32_t bytes, std::uint32_t fragments, VkFormat format) {
    ColorTarget target{};
    target.extent = target.surfaceExtent = {65u, 5u};
    target.tileMode = ColorTileMode::RenderTarget;
    target.elementBytes = bytes;
    target.samples = fragments;
    target.format = format;
    target.address = target.surfaceAddress = 0x10000u;
    target.pipeBankXor = 0x8c00u;
    target.bytes = MultisampleTextureLayout(TextureTileMode::kR64KBX, bytes, fragments, target.extent.width, target.extent.height, 1u, target.pipeBankXor).Bytes();
    return target;
}

void RawSamples() {
    const std::array formats{VK_FORMAT_R8_UINT, VK_FORMAT_R16_UINT, VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32A32_UINT};
    for (std::uint32_t index = 0; index < formats.size(); ++index) {
        for (const auto fragments : {2u, 4u, 8u}) {
            const auto bytes = 1u << index;
            auto target = Target(bytes, fragments, formats[index]);
            const MultisampleTextureLayout layout(TextureTileMode::kR64KBX, bytes, fragments, target.extent.width, target.extent.height, 1u, target.pipeBankXor);
            std::vector<std::byte> tiled(layout.Bytes(), std::byte{0xa5}), expected(layout.LinearBytes());
            for (std::uint32_t y = 0; y < target.extent.height; ++y) {
                for (std::uint32_t x = 0; x < target.extent.width; ++x) {
                    for (std::uint32_t sample = 0; sample < fragments; ++sample) {
                        for (std::uint32_t byte = 0; byte < bytes; ++byte) {
                            const auto value = static_cast<std::byte>((x * 13u + y * 31u + sample * 53u + byte * 7u) & 0xffu);
                            tiled[layout.Offset(x, y, 0u, sample) + byte] = value;
                            expected[((y * target.extent.width + x) * fragments + sample) * bytes + byte] = value;
                        }
                    }
                }
            }
            auto decoded = DecodeMultisampleInitialContents(target, tiled, {}, {});
            Require(!decoded.clear && decoded.samples == expected, "initial raw sample bytes, components or order differ");
            target.cmaskAddress = 0x20000u;
            target.cmaskBytes = CmaskBytes(target.extent.width, target.extent.height);
            target.fmaskAddress = 0x30000u;
            std::vector<std::byte> cmask(target.cmaskBytes, std::byte{0xcc});
            decoded = DecodeMultisampleInitialContents(target, tiled, {}, cmask);
            for (std::size_t offset = 0; offset < expected.size(); offset += bytes * fragments) {
                for (std::uint32_t sample = 1; sample < fragments; ++sample) std::memcpy(expected.data() + offset + sample * bytes, expected.data() + offset, bytes);
            }
            Require(!decoded.clear && decoded.samples == expected, "FMASK-clear0 did not preserve the first physical fragment for every logical sample");
            cmask.front() = std::byte{0xff};
            Refuses([&] { DecodeMultisampleInitialContents(target, tiled, {}, cmask); }, "mixed initial");
            std::fill(cmask.begin(), cmask.end(), fragments == 2u ? std::byte{0xdd} : fragments == 4u ? std::byte{0xee} : std::byte{0xff});
            Refuses([&] { DecodeMultisampleInitialContents(target, tiled, {}, cmask); }, "FMASK mapping");
            target.fmaskAddress = 0;
            Refuses([&] { DecodeMultisampleInitialContents(target, tiled, {}, cmask); }, "FMASK mapping");
            target.cmaskAddress = 0;
            Refuses([&] { DecodeMultisampleInitialContents(target, std::span(tiled).first(tiled.size() - 1u), {}, {}); }, "color snapshot size");
        }
    }
}

void DccClears() {
    for (const auto fragments : {2u, 4u, 8u}) {
        auto target = Target(8u, fragments, VK_FORMAT_R16G16B16A16_SFLOAT);
        target.dccAddress = 0x20000u;
        target.dccPipeAligned = true;
        target.dccControl = 0x100038u;
        target.fmaskAddress = 0x40000u;
        target.cmaskAddress = 0x30000u;
        target.cmaskBytes = CmaskBytes(target.extent.width, target.extent.height);
        target.cmaskFastClear = true;
        target.clearWords = {0xffffffffu, 0xffffffffu};
        std::vector<std::byte> dcc(MultisampleDccBytes_nid_no_patch(8u, fragments, target.extent.width, target.extent.height, 1u));
        std::vector<std::byte> cmask(target.cmaskBytes, std::byte{0xcc});
        for (const bool alphaOnMsb : {false, true}) {
            target.dccAlphaOnMsb = alphaOnMsb;
            for (const auto code : {0u, 0x40u, 0x80u, 0xc0u}) {
                std::fill(dcc.begin(), dcc.end(), static_cast<std::byte>(code));
                const auto contents = DecodeMultisampleInitialContents(target, {}, dcc, cmask);
                Require(contents.clear.has_value() && contents.samples.empty(), "uniform DCC clear was not represented without a raw color snapshot");
                const bool color = code == 0x80u || code == 0xc0u;
                const bool alpha = code == 0x40u || code == 0xc0u;
                for (std::uint32_t channel = 0; channel < 4u; ++channel) Require(contents.clear->float32[channel] == ((channel == (alphaOnMsb ? 3u : 0u) ? alpha : color) ? 1.0f : 0.0f), "initial DCC clear used raw memory or the clear registers instead of its numeric code");
            }
        }
        dcc.back() = std::byte{0};
        Refuses([&] { DecodeMultisampleInitialContents(target, {}, dcc, cmask); }, "mixed multisampled DCC");
        std::fill(dcc.begin(), dcc.end(), std::byte{0});
        Refuses([&] { DecodeMultisampleInitialContents(target, {}, std::span(dcc).first(dcc.size() - 1u), cmask); }, "snapshot size mismatch");
        std::fill(cmask.begin(), cmask.end(), std::byte{0});
        Refuses([&] { DecodeMultisampleInitialContents(target, {}, dcc, cmask); }, "register clear with DCC");
        std::fill(cmask.begin(), cmask.end(), std::byte{0xcc});
        target.arrayLayers = 2;
        Refuses([&] { DecodeMultisampleInitialContents(target, {}, dcc, cmask); }, "array, volume or mip");
        target.arrayLayers = 1;
        target.dccControl |= 1u << 10u;
        Refuses([&] { DecodeMultisampleInitialContents(target, {}, dcc, cmask); }, "lossy mode");
        target.dccControl = 0x100038u;
        target.bytes -= 1u;
        Refuses([&] { DecodeMultisampleInitialContents(target, {}, dcc, cmask); }, "footprint mismatch");
    }
}

void RegisterClearsAndUncompressedDcc() {
    for (const auto fragments : {2u, 4u, 8u}) {
        auto target = Target(2u, fragments, VK_FORMAT_R16_UINT);
        target.cmaskAddress = 0x20000u;
        target.cmaskBytes = CmaskBytes(target.extent.width, target.extent.height);
        target.cmaskFastClear = true;
        target.clearWords = {0xffffabcdu, 0xffffffffu};
        std::vector<std::byte> cmask(target.cmaskBytes, std::byte{0});
        const auto clear = DecodeMultisampleInitialContents(target, {}, {}, cmask);
        Require(clear.clear && clear.samples.empty() && clear.clear->uint32[0] == 0xabcdu, "CMASK register clear lost the unsigned component or read unused clear bits");
        target.cmaskFastClear = false;
        Refuses([&] { DecodeMultisampleInitialContents(target, {}, {}, cmask); }, "register clear is not implemented");
        target.cmaskFastClear = true;
        target.cmaskAddress = 0;
        target.dccAddress = 0x30000u;
        target.dccPipeAligned = true;
        target.dccControl = 0x100038u;
        std::vector<std::byte> dcc(MultisampleDccBytes_nid_no_patch(2u, fragments, target.extent.width, target.extent.height, 1u), std::byte{0xff});
        std::vector<std::byte> tiled(target.bytes, std::byte{0x5a});
        const auto raw = DecodeMultisampleInitialContents(target, tiled, dcc, {});
        const auto linearBytes = static_cast<std::size_t>(target.extent.width) * target.extent.height * fragments * target.elementBytes;
        Require(!raw.clear && raw.samples == std::vector<std::byte>(linearBytes, std::byte{0x5a}), "uncompressed DCC did not preserve every raw sample byte");
        target.fmaskAddress = 0x40000u;
        Refuses([&] { DecodeMultisampleInitialContents(target, tiled, dcc, {}); }, "FMASK mapping");
        target.fmaskAddress = 0;
        for (const auto code : {0x10u, 0x20u, 0x55u}) {
            std::fill(dcc.begin(), dcc.end(), static_cast<std::byte>(code));
            Refuses([&] { DecodeMultisampleInitialContents(target, tiled, dcc, {}); }, "compression code is not implemented");
        }
        std::fill(dcc.begin(), dcc.end(), std::byte{0xff});
        target.dccPipeAligned = false;
        Refuses([&] { DecodeMultisampleInitialContents(target, tiled, dcc, {}); }, "alignment");
        target.dccPipeAligned = true;
        target.dccControl |= 1u << 7u;
        Refuses([&] { DecodeMultisampleInitialContents(target, tiled, dcc, {}); }, "color transform");
        target.dccControl = 0x100038u;
        target.mip = 1;
        Refuses([&] { DecodeMultisampleInitialContents(target, tiled, dcc, {}); }, "array, volume or mip");
    }
}

int main() {
    try {
        RawSamples();
        DccClears();
        RegisterClearsAndUncompressedDcc();
        std::cout << "Initial multisample raw fragments, CMASK mapping and numeric DCC clear states passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
