#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_MULTISAMPLEDCC_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_MULTISAMPLEDCC_HPP

#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"

namespace AgcDriver::Graphics {

extern "C" std::size_t MultisampleDccBytes_nid_no_patch(std::uint32_t elementBytes, std::uint32_t fragments, std::uint32_t width, std::uint32_t height, std::uint32_t layers);
extern "C" DccKeys MultisampleDccKeys_nid_no_patch(const GuestTextureResource& resource, std::span<const std::byte> metadata);

}

#endif
