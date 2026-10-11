#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_MULTISAMPLETARGET_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_MULTISAMPLETARGET_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace AgcDriver::Graphics {

class StorageTexture;

enum class MultisampleCmaskState { ColorClear, FmaskClear, Expanded, Unsupported };

class MultisampledCmask {
public:
    MultisampledCmask() = default;
    MultisampledCmask(std::uint64_t cmaskAddress, std::uint64_t cmaskBytes, std::uint32_t samples, bool colorFastClear, bool dcc);
    void NoteFill(std::uint64_t fillAddress, std::size_t fillBytes, std::uint32_t pattern);
    void Seed(std::span<const std::byte> metadata);
    void CheckAddress(std::uint64_t cmaskAddress) const;
    void CheckMode(std::uint32_t samples, bool colorFastClear, bool dcc) const;
    bool TakeClear();
    MultisampleCmaskState State() const { return state; }
    bool ClearPending() const { return clearPending; }
    const std::string& Refusal() const { return refusal; }

private:
    void setPattern(std::uint32_t pattern);

    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
    std::uint32_t samples = 8;
    bool colorClearAllowed = false;
    bool clearPending = false;
    MultisampleCmaskState state = MultisampleCmaskState::Expanded;
    std::string refusal;
};

VkImageView MultisampleTargetView(const Context& context, const ColorTarget& target);
void ResolveMultisampleTarget(const Context& context, const ColorTarget& source, StorageTexture& destination, VkImageView destinationView);
void NoteColorMetadataFill(std::uint64_t address, std::size_t bytes, std::uint32_t pattern);
void ClearMultisampleTargets(VkDevice device);

}

#endif
