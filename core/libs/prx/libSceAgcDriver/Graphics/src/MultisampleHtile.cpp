#include "prx/libSceAgcDriver/Graphics/include/MultisampleHtile.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <array>
#include <bit>
#include <limits>

namespace AgcDriver::Graphics {

extern "C" std::size_t MultisampleHtileBytes_nid_no_patch(std::uint32_t width, std::uint32_t height, std::uint32_t layers) {
    Require(width != 0u && width <= 16384u && height != 0u && height <= 16384u && layers != 0u && layers <= 8192u, "invalid multisampled HTILE extent or layer count");
    const auto slice = static_cast<std::uint64_t>((width + 1023u) / 1024u) * ((height + 511u) / 512u) * 32768u;
    Require(slice <= std::numeric_limits<std::size_t>::max() / layers, "multisampled HTILE size overflows");
    return static_cast<std::size_t>(slice * layers);
}

extern "C" std::uint64_t MultisampleHtileOffset_nid_no_patch(std::uint32_t fragments, std::uint32_t width, std::uint32_t height, std::uint32_t layers, std::uint32_t pipeBankXor, std::uint32_t x, std::uint32_t y, std::uint32_t layer) {
    const auto slice = MultisampleHtileBytes_nid_no_patch(width, height, layers) / layers;
    Require(fragments == 2u || fragments == 4u || fragments == 8u, "unsupported multisampled HTILE fragment count");
    Require((pipeBankXor & ~0xcf00u) == 0u, "unsupported multisampled HTILE pipe/bank XOR");
    Require(x < width && y < height && layer < layers, "multisampled HTILE coordinate is outside its extent");
    constexpr std::array<std::uint64_t, 16> pattern4{0u, 0u, 0u, 1ull << 3u, 1ull << 20u, 1ull << 6u, 1ull << 22u, 1ull << 7u, 1ull << 23u, (1ull << 3u) ^ (1ull << 19u) ^ (1ull << 35u), (1ull << 34u) ^ (1ull << 4u) ^ (1ull << 20u), (1ull << 33u) ^ (1ull << 21u) ^ (1ull << 6u), (1ull << 32u) ^ (1ull << 5u) ^ (1ull << 22u), 1ull << 8u, 1ull << 24u, 1ull << 9u};
    constexpr std::array<std::uint64_t, 16> pattern8{0u, 0u, 0u, 1ull << 3u, 1ull << 20u, 1ull << 6u, 1ull << 22u, 1ull << 7u, 1ull << 23u, (1ull << 34u) ^ (1ull << 3u) ^ (1ull << 19u), (1ull << 33u) ^ (1ull << 4u) ^ (1ull << 20u), (1ull << 32u) ^ (1ull << 21u) ^ (1ull << 6u), (1ull << 5u) ^ (1ull << 22u), 1ull << 8u, 1ull << 24u, 1ull << 9u};
    const auto& pattern = fragments == 8u ? pattern8 : pattern4;
    const auto coordinate = static_cast<std::uint64_t>(x) | (static_cast<std::uint64_t>(y) << 16u) | (static_cast<std::uint64_t>(layer) << 32u);
    std::uint32_t nibble = 0u;
    for (std::uint32_t bit = 0u; bit < pattern.size(); ++bit) nibble |= static_cast<std::uint32_t>(std::popcount(coordinate & pattern[bit]) & 1u) << bit;
    const auto block = static_cast<std::uint64_t>(y / 512u) * ((width + 1023u) / 1024u) + x / 1024u;
    return slice * layer + block * 32768u + ((nibble >> 1u) ^ (pipeBankXor & 0xf00u));
}

extern "C" HtileDepthState MultisampleHtileState_nid_no_patch(std::uint32_t word) {
    if ((word & 15u) == 15u) return HtileDepthState::Uncompressed;
    switch (word) {
        case 0x00000000u:
        case 0x000000f0u:
        case 0x00000030u: return HtileDepthState::ClearZero;
        case 0xfffffff0u:
        case 0xfffc00f0u:
        case 0xfffc0030u: return HtileDepthState::ClearOne;
        default: throw std::runtime_error("multisampled HTILE depth compression is not implemented");
    }
}

}
