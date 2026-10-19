#include "prx/libSceAgcDriver/Graphics/include/FmaskTexture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureSwizzleEquations.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "IntermediateRepresentation/IrMetadata/BufferFormat.hpp"
#include <bit>
#include <cstring>

namespace AgcDriver::Graphics {

extern "C" GuestTextureResource FmaskSnapshotResource_nid_no_patch(const GuestTextureResource& resource) {
    Require(resource.format == static_cast<std::uint32_t>(ShaderRecompiler::IrBufferFormat::Format32UInt) && resource.fmaskCmaskAddress != 0u, "FMASK image format read semantics are not implemented");
    Require(resource.dimension == TextureDimension::k2D || resource.dimension == TextureDimension::k2DArray, "FMASK image dimension is not implemented");
    Require(resource.tileMode == TextureTileMode::kZ64KBX && (resource.pipeBankXor & ~0xcf00u) == 0u, "FMASK image swizzle or pipe/bank XOR is not implemented");
    Require(resource.width != 0u && resource.width <= 16384u && resource.height != 0u && resource.height <= 16384u && resource.depthOrLastArray < 8192u && resource.baseArray <= resource.depthOrLastArray, "invalid FMASK image extent or layer range");
    Require(resource.dimension == TextureDimension::k2DArray || (resource.depthOrLastArray == 0u && resource.baseArray == 0u), "FMASK 2D image has an array range");
    Require(resource.mipCount == 1u && resource.allocatedMipCount == 1u && resource.baseLevel == 0u && resource.lastLevel == 0u && resource.minLod == 0u, "FMASK image mip fields are not implemented");
    Require(resource.fmaskCmaskAddress == 0u || resource.dccPipeAligned, "FMASK image CMASK must be pipe aligned");
    for (const auto selector : {resource.dstSelX, resource.dstSelY, resource.dstSelZ, resource.dstSelW}) Require(selector == 0u || selector == 1u || selector == 4u, "FMASK image selects an unverified channel");
    auto snapshot = resource;
    snapshot.format = static_cast<std::uint32_t>(ShaderRecompiler::IrBufferFormat::Format32UInt);
    snapshot.tileMode = TextureTileMode::kLinear;
    snapshot.pipeBankXor = 0u;
    snapshot.dccAddress = 0u;
    snapshot.fmaskCmaskAddress = 0u;
    snapshot.dccAlphaOnMsb = false;
    snapshot.dccPipeAligned = false;
    return snapshot;
}

extern "C" std::vector<std::byte> FmaskSnapshot_nid_no_patch(const GuestTextureResource& resource, std::span<const std::byte> fmask, std::span<const std::byte> cmask) {
    const auto snapshot = FmaskSnapshotResource_nid_no_patch(resource);
    const auto rawGeometry = DescribeSurface(resource);
    const auto outputGeometry = DescribeSurface(snapshot);
    const auto layers = rawGeometry.layers;
    const CmaskLayout metadata(resource.width, resource.height);
    Require(fmask.size() == rawGeometry.guestBytes, "FMASK snapshot size mismatch");
    Require(cmask.size() == (resource.fmaskCmaskAddress != 0u ? FmaskCmaskBytes_nid_no_patch(metadata, layers) : 0u), "FMASK CMASK snapshot size mismatch");
    const auto* equation = FindTextureSwizzleEquation(XorSwizzleMode(TextureTileMode::kZ64KBX), 4u);
    Require(equation != nullptr, "FMASK swizzle equation is unavailable");
    const auto& rawMip = rawGeometry.mips.front();
    const auto& outputMip = outputGeometry.mips.front();
    const auto block = ThinBlockLayout(TextureTileMode::kZ64KBX, 4u);
    std::vector<std::byte> output(outputGeometry.guestBytes);
    for (std::uint32_t layer = 0u; layer < layers; ++layer) {
        for (std::uint32_t y = 0u; y < resource.height; ++y) {
            for (std::uint32_t x = 0u; x < resource.width; ++x) {
                std::uint32_t value = 0u;
                std::uint32_t code = 0xfu;
                if (!cmask.empty()) {
                    const auto nibble = FmaskCmaskNibble_nid_no_patch(metadata, x / 8u, y / 8u, layer, layers, resource.pipeBankXor);
                    code = (std::to_integer<std::uint32_t>(cmask[nibble / 2u]) >> ((nibble & 1u) * 4u)) & 15u;
                }
                Require(code == 0xcu || code == 0xfu, "FMASK CMASK compression code is not implemented");
                if (code == 0xfu) {
                    std::uint32_t offset = resource.pipeBankXor;
                    const auto coordinate = ((x + (rawMip.tail ? rawMip.tailX : 0u)) & 0xfffu) | (((y + (rawMip.tail ? rawMip.tailY : 0u)) & 0xfffu) << 12u) | ((layer & 0xffu) << 24u);
                    for (std::uint32_t bit = 0u; bit < equation->bits.size(); ++bit) offset ^= (static_cast<std::uint32_t>(std::popcount(coordinate & equation->bits[bit])) & 1u) << bit;
                    const auto tile = rawMip.tail ? 0u : static_cast<std::uint64_t>(y / block[2]) * rawMip.blocksPerRow + x / block[1];
                    const auto address = rawGeometry.GuestLayerOffset(layer) + rawMip.tiledOffset + tile * block[0] + offset;
                    Require(address <= fmask.size() && fmask.size() - address >= sizeof(value), "FMASK address exceeds its snapshot");
                    std::memcpy(&value, fmask.data() + address, sizeof(value));
                    Require((value & 0x88888888u) == 0u, "FMASK contains an unverified fragment index");
                }
                const auto destination = outputGeometry.GuestLayerOffset(layer) + outputMip.tiledOffset + static_cast<std::uint64_t>(y) * outputMip.pitchBytes + static_cast<std::uint64_t>(x) * sizeof(value);
                Require(destination <= output.size() && output.size() - destination >= sizeof(value), "FMASK linear address exceeds its snapshot");
                std::memcpy(output.data() + destination, &value, sizeof(value));
            }
        }
    }
    return output;
}

}
