#include "prx/libSceAgcDriver/Graphics/include/MultisampleDcc.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include <algorithm>
#include <bit>
#include <limits>

namespace AgcDriver::Graphics {

extern "C" std::size_t MultisampleDccBytes_nid_no_patch(std::uint32_t elementBytes, std::uint32_t fragments, std::uint32_t width, std::uint32_t height, std::uint32_t layers) {
    Require(std::has_single_bit(elementBytes) && elementBytes <= 16u, "unsupported multisampled DCC element size");
    Require(fragments == 2u || fragments == 4u || fragments == 8u, "unsupported multisampled DCC fragment count");
    Require(width != 0u && width <= 16384u && height != 0u && height <= 16384u && layers != 0u && layers <= 8192u, "invalid multisampled DCC extent or layer count");
    const auto pixelBits = 19u - static_cast<std::uint32_t>(std::countr_zero(elementBytes));
    const auto blockWidth = 1u << ((pixelBits + 1u) / 2u);
    const auto blockHeight = 1u << (pixelBits / 2u);
    const auto sliceBytes = static_cast<std::uint64_t>((width + blockWidth - 1u) / blockWidth) * ((height + blockHeight - 1u) / blockHeight) * 4096u;
    Require(sliceBytes <= std::numeric_limits<std::size_t>::max() / layers, "multisampled DCC size overflows");
    return static_cast<std::size_t>(sliceBytes * layers);
}

extern "C" DccKeys MultisampleDccKeys_nid_no_patch(const GuestTextureResource& resource, std::span<const std::byte> metadata) {
    Require(resource.dimension == TextureDimension::k2DMsaa || resource.dimension == TextureDimension::k2DMsaaArray, "multisampled DCC image dimension is not implemented");
    Require(resource.tileMode == TextureTileMode::kR64KBX && (resource.pipeBankXor & ~0xcf00u) == 0u, "multisampled DCC swizzle or pipe/bank XOR is not implemented");
    Require(resource.depthOrLastArray < 8192u && resource.baseArray <= resource.depthOrLastArray, "invalid multisampled DCC array range");
    Require(resource.dimension == TextureDimension::k2DMsaaArray || (resource.depthOrLastArray == 0u && resource.baseArray == 0u), "multisampled DCC 2D image has an array range");
    Require(resource.mipCount == 1u && resource.allocatedMipCount == 1u && resource.baseLevel == 0u && resource.lastLevel == 0u && resource.minLod == 0u, "multisampled DCC mip fields are not implemented");
    Require(resource.dccAddress != 0u && resource.dccPipeAligned, "multisampled DCC metadata must have a pipe-aligned address");
    Require(!IsConvertedTextureFormat(resource.format) && !IsBlockCompressed(resource.format), "multisampled DCC texture format requires an unsupported conversion");
    const auto bytes = MultisampleDccBytes_nid_no_patch(BytesPerElement(resource.format), resource.samples, resource.width, resource.height, resource.depthOrLastArray + 1u);
    Require(metadata.size() == bytes, "multisampled DCC snapshot size mismatch");
    const auto code = metadata.front();
    Require(std::ranges::all_of(metadata, [&](std::byte value) { return value == code; }), "mixed multisampled DCC metadata is not implemented");
    switch (std::to_integer<std::uint32_t>(code)) {
        case 0x00u: return DccKeys::Clear0000;
        case 0x40u: return DccKeys::Clear0001;
        case 0x80u: return DccKeys::Clear1110;
        case 0xc0u: return DccKeys::Clear1111;
        case 0xffu: return DccKeys::Uncompressed;
        default: throw std::runtime_error("multisampled DCC compression code is not implemented");
    }
}

}
