#include "prx/libSceAgcDriver/Graphics/include/MultisampleInitialContents.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleDcc.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTextureLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include <algorithm>
#include <array>
#include <cstring>

namespace AgcDriver::Graphics {

std::size_t MultisampleFmaskBytes(const ColorTarget& target) {
    Require(target.samples == 2u || target.samples == 4u || target.samples == 8u, "unsupported multisample FMASK sample count");
    Require(target.fmaskSwizzle == 0x18u, "initial multisample FMASK footprint requires SW_64KB_Z_X");
    return ComputeSurfaceSize(ComputeElementMipLayout(TextureTileMode::kZ64KBX, target.samples == 8u ? 4u : 1u, target.extent.width, target.extent.height, 1u), 1u);
}

std::uint32_t MultisampleExpandedFmask(std::uint32_t samples) {
    Require(samples == 2u || samples == 4u || samples == 8u, "unsupported expanded multisample FMASK sample count");
    return samples == 2u ? 0x02020202u : samples == 4u ? 0xe4e4e4e4u : 0x76543210u;
}

MultisampleInitialContents DecodeMultisampleInitialContents(const ColorTarget& target, std::span<const std::byte> color, std::span<const std::byte> dcc, std::span<const std::byte> cmask, std::span<const std::byte> fmask, bool includePadding) {
    Require(target.samples == 2u || target.samples == 4u || target.samples == 8u, "unsupported initial multisample fragment count");
    Require(target.tileMode == ColorTileMode::RenderTarget, "initial multisample color layout is not implemented");
    Require(target.arrayLayers == 1u && target.arraySlice == 0u && target.depth == 1u && target.depthSlice == 0u && target.mipCount == 1u && target.mip == 0u && !target.mipTail, "initial multisample array, volume or mip contents are not implemented");
    const MultisampleTextureLayout layout(TextureTileMode::kR64KBX, target.elementBytes, target.samples, target.extent.width, target.extent.height, 1u, target.pipeBankXor);
    Require(target.bytes == layout.Bytes(), "initial multisample target footprint mismatch");
    Require(target.cmaskAddress == 0u ? cmask.empty() : cmask.size() == target.cmaskBytes && target.cmaskBytes == CmaskBytes(target.extent.width, target.extent.height), "initial multisample CMASK snapshot size mismatch");
    std::uint32_t cmaskCode = 0xffu;
    if (!cmask.empty()) {
        const auto first = cmask.front();
        Require(std::ranges::all_of(cmask, [&](std::byte b) { return b == first; }), "mixed initial multisample CMASK is not implemented");
        cmaskCode = std::to_integer<std::uint32_t>(first);
        const auto expanded = target.samples == 2u ? 0xddu : target.samples == 4u ? 0xeeu : 0xffu;
        Require(cmaskCode == 0u || cmaskCode == 0xccu || cmaskCode == expanded, "initial multisample CMASK compression code is not implemented");
    }
    MultisampleInitialContents result;
    if (target.dccAddress != 0u) {
        Require(target.dccPipeAligned && (target.dccControl & 0x3ff80u) == 0u, "initial multisample DCC alignment, color transform or lossy mode is not implemented");
        Require(cmaskCode != 0u, "CMASK register clear with DCC is invalid");
        const auto format = FindGuestColorTargetFormat(target.format, target.elementBytes);
        Require(format.has_value(), "initial multisample DCC format is not implemented");
        GuestTextureResource resource{};
        resource.dimension = TextureDimension::k2DMsaa;
        resource.tileMode = TextureTileMode::kR64KBX;
        resource.width = target.extent.width;
        resource.height = target.extent.height;
        resource.fragments = target.samples;
        resource.mipCount = resource.allocatedMipCount = 1u;
        resource.pipeBankXor = target.pipeBankXor;
        resource.dccAddress = target.dccAddress;
        resource.dccPipeAligned = true;
        resource.dccAlphaOnMsb = target.dccAlphaOnMsb;
        resource.format = *format;
        const auto keys = MultisampleDccKeys_nid_no_patch(resource, dcc);
        if (keys != DccKeys::Uncompressed) {
            std::array<std::byte, 16> texels{};
            Require(FillDccClear(target.format, keys, target.dccAlphaOnMsb, texels), "initial multisample DCC clear format is not implemented");
            std::array<std::uint32_t, 4> pattern{};
            std::memcpy(pattern.data(), texels.data(), texels.size());
            VkClearColorValue clear{};
            Require(ClearColorForTexel(target.format, target.elementBytes, pattern, clear), "initial multisample DCC clear has no exact host value");
            result.clear = clear;
            return result;
        }
    } else {
        Require(dcc.empty(), "initial multisample target without DCC has a metadata snapshot");
        if (!cmask.empty() && cmaskCode == 0u) {
            Require(target.cmaskFastClear && target.elementBytes <= sizeof(target.clearWords), "initial multisample register clear is not implemented");
            std::array<std::uint32_t, 4> pattern{};
            auto bytes = std::as_writable_bytes(std::span(pattern));
            for (std::size_t offset = 0; offset < bytes.size(); offset += target.elementBytes) std::memcpy(bytes.data() + offset, target.clearWords.data(), target.elementBytes);
            VkClearColorValue clear{};
            Require(ClearColorForTexel(target.format, target.elementBytes, pattern, clear), "initial multisample register clear has no exact host value");
            result.clear = clear;
            return result;
        }
    }
    Require(color.size() == layout.Bytes(), "initial multisample color snapshot size mismatch");
    const auto expanded = target.samples == 2u ? 0xddu : target.samples == 4u ? 0xeeu : 0xffu;
    if (!cmask.empty() && cmaskCode == expanded && target.fmaskAddress != 0u) {
        Require(fmask.size() == MultisampleFmaskBytes(target), "initial multisample FMASK mapping snapshot size mismatch");
        const auto pattern = MultisampleExpandedFmask(target.samples);
        for (std::size_t byte = 0; byte < fmask.size(); ++byte) Require(fmask[byte] == static_cast<std::byte>((pattern >> (byte % 4u * 8u)) & 0xffu), "initial multisample FMASK mapping is not expanded identity");
    } else Require((target.fmaskAddress == 0u && cmask.empty() && fmask.empty()) || (!cmask.empty() && cmaskCode == 0xccu), "initial multisample FMASK mapping is not implemented");
    const MultisampleTextureLayout contentsLayout(TextureTileMode::kR64KBX, target.elementBytes, target.samples, includePadding ? layout.Pitch() : target.extent.width, includePadding ? layout.PaddedHeight() : target.extent.height, 1u, target.pipeBankXor);
    result.samples.resize(static_cast<std::size_t>(contentsLayout.LinearBytes()));
    contentsLayout.Detile(color, result.samples);
    if (!cmask.empty() && cmaskCode == 0xccu) {
        const auto pixelBytes = static_cast<std::size_t>(target.elementBytes) * target.samples;
        for (std::size_t offset = 0; offset < result.samples.size(); offset += pixelBytes) {
            for (std::uint32_t sample = 1u; sample < target.samples; ++sample) std::memcpy(result.samples.data() + offset + sample * target.elementBytes, result.samples.data() + offset, target.elementBytes);
        }
    }
    return result;
}

}
