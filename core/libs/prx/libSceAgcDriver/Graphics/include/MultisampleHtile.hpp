#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_MULTISAMPLEHTILE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_MULTISAMPLEHTILE_HPP

#include <cstddef>
#include <cstdint>

namespace AgcDriver::Graphics {

enum class HtileDepthState { Uncompressed, ClearZero, ClearOne };

extern "C" std::size_t MultisampleHtileBytes_nid_no_patch(std::uint32_t width, std::uint32_t height, std::uint32_t layers);
extern "C" std::uint64_t MultisampleHtileOffset_nid_no_patch(std::uint32_t fragments, std::uint32_t width, std::uint32_t height, std::uint32_t layers, std::uint32_t pipeBankXor, std::uint32_t x, std::uint32_t y, std::uint32_t layer);
extern "C" HtileDepthState MultisampleHtileState_nid_no_patch(std::uint32_t word);
extern "C" HtileDepthState MultisampleHtileClearState_nid_no_patch(std::uint32_t word, bool depthOnly);

}

#endif
