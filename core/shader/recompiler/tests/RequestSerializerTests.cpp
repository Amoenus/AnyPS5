#include "ControlFlow/RequestSerializer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr char kHistoricalVersion21[] =
    "NVNQQRUAAAAFAAADAAAAAAABAAAAAAAAAAAAgb8AAAAAAAAAAAAAAAAAAAAAQAAAAAgAAAADAAAAAAAAAN+bVxPgrGgk776t3gABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAJAAAAAAAAAOQAAAAAAAAAAAAAAAAAAAAAAQEAAAEAAAAAAAAAAAAFAAAAAAAEAAAAAAAAABI0VngAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIAAAALAAAAEAAAAEAAAAAAAAF4VjQSAQEBOTAAAAA=";
constexpr char kVersion21WithF16Tail[] =
    "NVNQQRUAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAQA0DAAEB";

constexpr char kCanonicalVersion22[] =
    "NVNQQRYAAAAFAAADAAAAAAABAAAAAAAAAAAAgb8AAAAAAAAAAAAAAAAAAAAAQAAAAAgAAAADAAAAAAAAAN+bVxPgrGgk776t"
    "3gABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAJAAAAAAAAAOQAAAAAAAAAAAAAAAAAAAAAAQEDAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAkAAAANAAAA"
    "AQUKBQABAAAAAAAAAAAABQAAAAAABAAAAAAAAAABAwUHAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAACAAAACwAAABAAAABAAAAAAAABeFY0EgEBATkwAAAAAA==";

void Require(bool condition) {
    if (!condition) std::abort();
}

std::vector<std::uint8_t> DecodeBase64(std::string_view text) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<std::uint8_t> bytes;
    std::uint32_t chunk = 0;
    std::size_t bits = 0;
    for (const char symbol : text) {
        if (symbol == '=') break;
        const auto value = alphabet.find(symbol);
        Require(value != std::string_view::npos);
        chunk = ((chunk << 6u) | static_cast<std::uint32_t>(value)) & 0xffffffu;
        bits += 6u;
        if (bits >= 8u) {
            bits -= 8u;
            bytes.push_back(static_cast<std::uint8_t>((chunk >> bits) & 0xffu));
        }
    }
    return bytes;
}

std::string EncodeBase64(const std::vector<std::uint8_t>& bytes) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    for (std::size_t index = 0; index < bytes.size(); index += 3u) {
        const std::size_t count = std::min<std::size_t>(3u, bytes.size() - index);
        std::uint32_t chunk = static_cast<std::uint32_t>(bytes[index]) << 16u;
        if (count > 1u) chunk |= static_cast<std::uint32_t>(bytes[index + 1u]) << 8u;
        if (count > 2u) chunk |= bytes[index + 2u];
        for (std::size_t symbol = 0; symbol < 4u; ++symbol) {
            text.push_back(symbol <= count ? alphabet[(chunk >> (18u - 6u * symbol)) & 0x3fu] : '=');
        }
    }
    return text;
}

}

int main() {
    const ShaderRecompiler::RequestSerializer serializer;

    const auto historical = serializer.Deserialize(kHistoricalVersion21);
    Require(!historical.request.target.float16DenormPreserve);

    const auto version21WithTail = serializer.Deserialize(kVersion21WithF16Tail);
    Require(version21WithTail.request.target.float16DenormPreserve);

    const auto canonical22 = DecodeBase64(kCanonicalVersion22);
    Require(canonical22.size() == 1417u);
    for (const bool layer : {false, true}) {
        for (const bool preserve : {false, true}) {
            auto bytes = canonical22;
            bytes[258] = static_cast<std::uint8_t>(layer);
            bytes.back() = static_cast<std::uint8_t>(preserve);
            const auto encoded22 = EncodeBase64(bytes);
            const auto replay = serializer.Deserialize(encoded22);
            Require(replay.request.context.pixel.has_value() && replay.request.context.vertex.has_value());
            Require(replay.request.context.pixel->layerExported == layer);
            Require(!replay.request.context.pixel->ditheredAlphaToCoverage);
            Require(replay.request.context.pixel->rasterPrimitive == ShaderRecompiler::RasterPrimitive::Rectangle);
            Require(replay.request.context.vertex->paClVsOutCntl == 0x00050a05u);
            Require(replay.request.target.float16DenormPreserve == preserve);
            bytes[4] = 23u;
            bytes.insert(bytes.begin() + 259u, 0u);
            Require(serializer.Serialize(replay.request) == EncodeBase64(bytes));

            auto truncated22 = DecodeBase64(encoded22);
            truncated22.pop_back();
            bool rejected = false;
            try {
                static_cast<void>(serializer.Deserialize(EncodeBase64(truncated22)));
            } catch (const std::runtime_error& error) {
                rejected = std::string_view(error.what()) == "RequestSerializer: truncated data";
            }
            Require(rejected);
        }
    }

    ShaderRecompiler::RecompileRequest request{};
    for (const bool preserve : {false, true}) {
        request.target.float16DenormPreserve = preserve;
        const auto encoded = serializer.Serialize(request);
        const auto replay = serializer.Deserialize(encoded);
        Require(replay.request.target.float16DenormPreserve == preserve);
        auto bytes = DecodeBase64(encoded);
        Require(bytes.size() >= 8u && bytes[4] == 23u && bytes[5] == 0u && bytes[6] == 0u && bytes[7] == 0u);
        auto future = bytes;
        future[4] = 24u;
        bool futureRejected = false;
        try {
            static_cast<void>(serializer.Deserialize(EncodeBase64(future)));
        } catch (const std::runtime_error& error) {
            futureRejected = std::string_view(error.what()) == "unsupported recompile request serialization version";
        }
        Require(futureRejected);
        bytes.pop_back();
        bool rejected = false;
        try {
            static_cast<void>(serializer.Deserialize(EncodeBase64(bytes)));
        } catch (const std::runtime_error& error) {
            rejected = std::string_view(error.what()) == "RequestSerializer: truncated data";
        }
        Require(rejected);
    }

    return 0;
}
