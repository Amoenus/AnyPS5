#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_FMASKTEXTURE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_FMASKTEXTURE_HPP

#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include <cstddef>
#include <span>
#include <vector>

namespace AgcDriver::Graphics {

extern "C" GuestTextureResource FmaskSnapshotResource_nid_no_patch(const GuestTextureResource& resource);
extern "C" std::vector<std::byte> FmaskSnapshot_nid_no_patch(const GuestTextureResource& resource, std::span<const std::byte> fmask, std::span<const std::byte> cmask);

}

#endif
