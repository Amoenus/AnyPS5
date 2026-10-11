#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_MULTISAMPLEINITIALCONTENTS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_MULTISAMPLEINITIALCONTENTS_HPP

#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <optional>
#include <span>
#include <vector>

namespace AgcDriver::Graphics {

struct MultisampleInitialContents {
    std::optional<VkClearColorValue> clear;
    std::vector<std::byte> samples;
};

std::size_t MultisampleFmaskBytes(const ColorTarget& target);
std::uint32_t MultisampleExpandedFmask(std::uint32_t samples);
MultisampleInitialContents DecodeMultisampleInitialContents(const ColorTarget& target, std::span<const std::byte> color, std::span<const std::byte> dcc, std::span<const std::byte> cmask, std::span<const std::byte> fmask = {});

}

#endif
