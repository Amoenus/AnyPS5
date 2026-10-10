#ifndef CORE_SHADER_RECOMPILER_BDAABI_HPP
#define CORE_SHADER_RECOMPILER_BDAABI_HPP

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace ShaderRecompiler::BdaAbi {

inline constexpr std::uint32_t Version = 2;
inline constexpr std::uint32_t Read = 1;
inline constexpr std::uint32_t Write = 2;

struct Header {
    std::uint32_t version;
    std::uint32_t count;
    std::uint32_t entryBytes;
    std::uint32_t writtenWords;
};

struct Range {
    std::uint64_t begin;
    std::uint64_t end;
    std::uint64_t deviceAddress;
    std::uint32_t permissions;
    std::uint32_t writtenWord;
};

enum class FaultState : std::uint32_t { Empty, Writing, Ready };
enum class FaultReason : std::uint32_t { Unmapped = 1, Permission, Overflow, InvalidTable, InvalidRectangle, LoopLimit, Unaligned, InvalidDescriptor };

struct Fault {
    FaultState state;
    FaultReason reason;
    std::uint64_t address;
    std::uint32_t bytes;
    std::uint32_t stage;
    std::uint32_t instruction;
    std::uint32_t reserved;
};

static_assert(std::is_standard_layout_v<Header> && std::is_trivially_copyable_v<Header> && sizeof(Header) == 16);
static_assert(std::is_standard_layout_v<Range> && std::is_trivially_copyable_v<Range> && sizeof(Range) == 32);
static_assert(offsetof(Range, deviceAddress) == 16 && offsetof(Range, permissions) == 24);
static_assert(std::is_standard_layout_v<Fault> && std::is_trivially_copyable_v<Fault> && sizeof(Fault) == 32);
static_assert(offsetof(Fault, address) == 8 && offsetof(Fault, instruction) == 24);

inline constexpr std::uint32_t WrittenPageShift = 12;
inline constexpr std::uint32_t WrittenErrorWord = sizeof(Fault) / sizeof(std::uint32_t);
inline constexpr std::uint32_t WrittenBitmapWord = WrittenErrorWord + 1;
inline constexpr std::size_t FaultBufferBytes = WrittenBitmapWord * sizeof(std::uint32_t);

static constexpr std::uint64_t WrittenWords(const Range& range) {
    if (range.begin >= range.end) throw std::runtime_error("invalid BDA range bounds");
    if ((range.permissions & Write) == 0) return 0;
    const auto pages = ((range.end - 1u) >> WrittenPageShift) - (range.begin >> WrittenPageShift) + 1u;
    return (pages + 31u) / 32u;
}

static constexpr std::size_t FaultBufferBytesForWords(std::uint32_t words) {
    return FaultBufferBytes + static_cast<std::size_t>(words) * sizeof(std::uint32_t);
}

}

#endif
