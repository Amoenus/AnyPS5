#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_MULTISAMPLETARGET_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_MULTISAMPLETARGET_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace AgcDriver::GuestMemory {
struct UnchangedQuery;
}

namespace AgcDriver::Graphics {

class StorageTexture;
class Texture;

class MultisampledCmask {
public:
    MultisampledCmask() = default;
    MultisampledCmask(std::uint64_t cmaskAddress, std::uint64_t cmaskBytes) : address(cmaskAddress), bytes(cmaskBytes) {}
    void NoteFill(std::uint64_t fillAddress, std::size_t fillBytes, std::uint32_t pattern);
    void SeedKeys(DccKeys seeded);
    void CheckAddress(std::uint64_t cmaskAddress) const;
    bool TakeClear();
    DccKeys Keys() const { return keys; }
    const std::string& Refusal() const { return refusal; }

private:
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
    DccKeys keys = DccKeys::Uncompressed;
    std::string refusal;
};

VkImageView MultisampleTargetView(const Context& context, const ColorTarget& target);
std::vector<std::byte> ReadMultisampleTargetSamples(const Context& context, const ColorTarget& target);
std::shared_ptr<Texture> NativeMultisampleSnapshot_nid_no_patch(const Context& context, const GuestTextureResource& resource, VkComponentMapping components);
void MarkMultisampleTargetDirty(const Context& context, const ColorTarget& target);
void SynchronizeMultisampleTargetRead(std::uint64_t address, std::size_t bytes);
void InstallResidentImageReadGuard_nid_no_patch();
void GuardResidentDepthRanges_nid_no_patch(const Context& context, std::span<const GuestMemory::UnchangedQuery> ranges, const std::shared_ptr<std::atomic<bool>>& active);
void ClearResidentDepthGuards_nid_no_patch(VkDevice device);
void ResolveMultisampleTarget(const Context& context, const ColorTarget& source, StorageTexture& destination, VkImageView destinationView);
void NoteColorMetadataFill(std::uint64_t address, std::size_t bytes, std::uint32_t pattern);
void NoteNativeColorClear_nid_no_patch(std::uint64_t address, std::uint64_t bytes, std::uint32_t value, std::uint32_t mask);
void ClearMultisampleTargets(VkDevice device);
void RequireNoMultisampleTarget(std::uint64_t address, std::size_t bytes, const char* access);

}

#endif
