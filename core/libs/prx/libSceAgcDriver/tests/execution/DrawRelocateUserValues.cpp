#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "Recompiler.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::DriverDetail::DispatchVariant;
using AgcDriver::DriverDetail::DrawProgram;
using AgcDriver::DriverDetail::Driver;
using AgcDriver::DriverDetail::RelocatableUserWord;
using ShaderRecompiler::RecompileResult;

alignas(256) constexpr std::array<std::uint32_t, 2> SwappcCode{0xbe822100u, 0xbf810000u};
alignas(256) std::array<std::uint32_t, 1024> FirstCallee{};
alignas(256) std::array<std::uint32_t, 1024> SecondCallee{};
alignas(256) std::array<std::uint32_t, 1024> OtherCallee{};
alignas(256) std::array<std::uint32_t, 64> Buffer{};

void Fill(std::array<std::uint32_t, 1024>& callee, std::uint32_t first) {
    callee.fill(0xbf810000u);
    callee[0] = first;
    callee[1] = 0xbe802002u;
}

std::uint32_t Low(const void* address) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(address));
}

std::uint32_t High(const void* address) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(address)) >> 32u);
}

DrawProgram VertexProgram(const void* callee) {
    DrawProgram program{};
    program.binary.stage = ShaderRecompiler::ShaderStage::Vertex;
    program.binary.code = SwappcCode;
    program.firstUserSgpr = 0;
    program.userData = {Low(callee), High(callee), 0u, 0u};
    return program;
}

std::shared_ptr<DispatchVariant> Variant(std::shared_ptr<const RecompileResult> compiled) {
    auto variant = std::make_shared<DispatchVariant>();
    variant->compiled = std::move(compiled);
    return variant;
}

ShaderRecompiler::DescriptorBinding GuestBuffer(std::vector<std::uint32_t> descriptor, bool written) {
    ShaderRecompiler::DescriptorBinding binding{ShaderRecompiler::DescriptorKind::StorageBuffer, ShaderRecompiler::DescriptorRole::GuestBuffers, 0, 0, 1, std::move(descriptor)};
    binding.bufferWritten = {written};
    binding.bufferAtomic = {false};
    binding.bufferRead = {true};
    return binding;
}

void CalleeWordsAreKeyedByTheirCode() {
    ShaderRecompiler::SetGcnGuestShaders(true);
    Fill(FirstCallee, 0x7e000280u);
    Fill(SecondCallee, 0x7e000280u);
    Fill(OtherCallee, 0x7e000281u);
    const auto program = VertexProgram(FirstCallee.data());
    const auto variant = Variant(std::make_shared<RecompileResult>());
    Driver::tagUserValueWords(*variant, program);
    Require(variant->calleeWords == 1u && variant->calleeReads.size() == 1u && variant->calleeReads[0].first == 0u, "the fetch subroutine pointer in s[0:1] was not tagged as a callee word (mask " + std::to_string(variant->calleeWords) + ")");
    Require(variant->calleeReads[0].second == 2u, "the callee was measured as " + std::to_string(variant->calleeReads[0].second) + " words, expected 2");
    const RelocatableUserWord word{0u, {{0, variant->calleeReads[0].second}}, false, High(FirstCallee.data())};
    bool readable = true;
    const auto first = Driver::relocatedContentKey(word, Low(FirstCallee.data()), readable);
    const auto second = Driver::relocatedContentKey(word, Low(SecondCallee.data()), readable);
    const auto other = Driver::relocatedContentKey(word, Low(OtherCallee.data()), readable);
    Require(readable, "a callee in mapped memory was reported unreadable");
    Require(first == second, "two copies of the same callee code at different addresses keyed differently");
    Require(first != other, "callees with different code keyed the same");

    auto bound = std::make_shared<RecompileResult>();
    bound->bindings.push_back(GuestBuffer({Low(FirstCallee.data()), 0u, 16u, 0u}, false));
    const auto boundVariant = Variant(bound);
    Driver::tagUserValueWords(*boundVariant, program);
    Require(boundVariant->calleeWords == 0u, "a call target that also feeds a bound descriptor was tagged as a callee word");

    ShaderRecompiler::SetGcnGuestShaders(false);
    const auto rdna = Variant(std::make_shared<RecompileResult>());
    Driver::tagUserValueWords(*rdna, program);
    Require(rdna->calleeWords == 0u, "a vertex program was tagged for GCN user-data callees outside GCN guest shaders");
}

void BufferValueWordsArePatchedInTheirBinding() {
    ShaderRecompiler::SetGcnGuestShaders(false);
    const std::vector<std::uint32_t> descriptor{Low(Buffer.data()), High(Buffer.data()) | (16u << 16u), 4u, 0x20077facu};
    DrawProgram program{};
    program.binary.stage = ShaderRecompiler::ShaderStage::Fragment;
    program.firstUserSgpr = 0;
    program.userData = {0u, 0u, 0u, 0u, descriptor[0], descriptor[1], descriptor[2], descriptor[3]};
    const auto result = [&](bool written) {
        auto compiled = std::make_shared<RecompileResult>();
        compiled->bindings.push_back(GuestBuffer(descriptor, written));
        return compiled;
    };

    const auto variant = Variant(result(false));
    Driver::tagUserValueWords(*variant, program);
    Require(variant->valueWords == ((1u << 4u) | (1u << 6u)), "the base and record words of a read-only V# in user data were not tagged (mask " + std::to_string(variant->valueWords) + ")");
    Require(variant->valueSlots.size() == 2u && variant->valueSlots[0] == std::array<std::uint32_t, 3>{4u, 0u, 0u} && variant->valueSlots[1] == std::array<std::uint32_t, 3>{6u, 0u, 2u}, "the tagged V# words do not name their binding words");
    variant->descriptorPatches = {{0u, 0u, descriptor[0] + 0x100u}, {0u, 2u, 7u}};
    Driver::applyShaderDataPatches(*variant);
    const auto& patched = variant->compiled->bindings[0].guestDescriptor;
    Require(patched[0] == descriptor[0] + 0x100u && patched[1] == descriptor[1] && patched[2] == 7u && patched[3] == descriptor[3], "the descriptor patches did not move the bound V# base and record count");
    Require(variant->descriptorPatches.empty(), "the descriptor patches were kept after they were applied");

    const auto written = Variant(result(true));
    Driver::tagUserValueWords(*written, program);
    Require(written->valueWords == 0u, "a V# the shader writes through was tagged as relocatable");

    auto pushedResult = result(false);
    pushedResult->pushConstants.resize(sizeof(std::uint32_t));
    std::memcpy(pushedResult->pushConstants.data(), &descriptor[0], sizeof(std::uint32_t));
    const auto pushed = Variant(pushedResult);
    Driver::tagUserValueWords(*pushed, program);
    Require((pushed->valueWords >> 4u & 1u) == 0u, "a V# base that is also a push constant word not forwarded from it was tagged");
    auto forwardedResult = std::make_shared<RecompileResult>(*pushedResult);
    forwardedResult->shaderDataUserWords = {4u};
    const auto forwarded = Variant(forwardedResult);
    Driver::tagUserValueWords(*forwarded, program);
    Require((forwarded->valueWords >> 4u & 1u) != 0u, "a V# base forwarded from its own user word as push data was not tagged");

    const auto captured = Variant(result(false));
    captured->runs = {{reinterpret_cast<std::uintptr_t>(Buffer.data()) + 16u, reinterpret_cast<std::uintptr_t>(Buffer.data()) + 32u}};
    Driver::tagUserValueWords(*captured, program);
    Require(captured->valueWords == 0u, "a V# whose memory the stage captured was tagged as relocatable");

    const auto pointer = Variant(result(false));
    pointer->relocatableWords = 1u << 4u;
    Driver::tagUserValueWords(*pointer, program);
    Require((pointer->valueWords >> 4u & 1u) == 0u, "a user word relocated as a pointer was also tagged as a V# value");
}

void BufferValueWordsKeyByNothing() {
    const RelocatableUserWord word{4u, {}, true, 0u};
    bool readable = true;
    const auto first = Driver::relocatedContentKey(word, 0x1000u, readable);
    const auto second = Driver::relocatedContentKey(word, 0x2002u, readable);
    Require(readable && first == second, "a V# value word changed the relocated content key");
}

}

int main() {
    try {
        CalleeWordsAreKeyedByTheirCode();
        BufferValueWordsArePatchedInTheirBinding();
        BufferValueWordsKeyByNothing();
        std::cout << "User-data V# values and fetch subroutine pointers relocate\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
