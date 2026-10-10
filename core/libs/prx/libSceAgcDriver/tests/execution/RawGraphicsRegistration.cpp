#include "VulkanTestDevice.hpp"
#include "SceTypes.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <string_view>

alignas(256) std::array<std::byte, 64 * 64 * 4> colorMemory{};
alignas(256) constexpr std::array<std::uint32_t, 64> vertexCode{0x7e000280u, 0x7e020280u, 0x7e040280u, 0x7e0602f2u, 0xf80008cfu, 0x03020100u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 64> pixelCode{0x7e0002f2u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u};
AgcDriver::QueueState makeState() {
    AgcDriver::QueueState queue;
    queue.userConfig[0x242] = 4;
    queue.context = {
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u},
        {0x3b8, 0x9000000}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040}
    };
    const auto address = reinterpret_cast<std::uintptr_t>(colorMemory.data());
    queue.context[0x318] = static_cast<std::uint32_t>(address >> 8u);
    queue.context[0x390] = static_cast<std::uint32_t>(address >> 40u);
    queue.context[0x10f] = std::bit_cast<std::uint32_t>(32.0f);
    queue.context[0x110] = std::bit_cast<std::uint32_t>(32.0f);
    queue.context[0x111] = std::bit_cast<std::uint32_t>(-2.0f);
    queue.context[0x112] = std::bit_cast<std::uint32_t>(2.0f);
    queue.context[0x113] = std::bit_cast<std::uint32_t>(1.0f);
    queue.context[0x114] = 0;
    queue.context[0xb4] = 0;
    queue.context[0xb5] = std::bit_cast<std::uint32_t>(1.0f);
    queue.shader[0x008] = 0x100;
    queue.shader[0x009] = 0;
    return queue;
}

void RegisterCompute() {
    alignas(256) static const std::array<std::uint32_t, 64> code{0xbf810000u};
    struct Header {
        Shader shader{};
        std::array<ShaderRegister, 7> registers{};
        ShaderSpecialRegs specials{};
    };
    static Header header{};
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = sizeof(code);
    header.shader.code = code.data();
    const auto address = reinterpret_cast<std::uintptr_t>(code.data());
    header.registers = {{{0x20c, static_cast<std::uint32_t>(address >> 8u)}, {0x20d, static_cast<std::uint32_t>(address >> 40u)}, {0x207, 1}, {0x208, 1}, {0x209, 1}, {0x212, 0}, {0x213, 0}}};
    header.specials.dispatch_modifier = 0x8000u;
    header.shader.sh_registers = header.registers.data();
    header.shader.num_sh_registers = header.registers.size();
    header.shader.specials = &header.specials;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}


template<typename TAction>
void ExpectFailure(TAction action, const char* reason) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        if (std::string(error.what()).find(reason) == std::string::npos) throw;
        return;
    }
    throw std::runtime_error("expected raw graphics policy rejection");
}

int main(int argc, char** argv) {
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        const std::string_view mode = argc == 2 ? argv[1] : "raw-first";
        if (mode != "raw-first" && mode != "empty-draw" && mode != "registered" && mode != "registration-before-draw") throw std::runtime_error("unknown raw graphics policy test mode");
        auto queue = makeState();
        queue.userConfig[0x24a] = 0;
        const auto bind = [&](const void* code, std::uint32_t program) {
            const auto address = reinterpret_cast<std::uintptr_t>(code);
            queue.shader[program] = static_cast<std::uint32_t>(address >> 8u);
            queue.shader[program + 1u] = static_cast<std::uint32_t>(address >> 40u);
        };
        bind(vertexCode.data(), 0xc8);
        bind(pixelCode.data(), 0x8);
        queue.shader[0x8b] = 0;
        queue.shader[0xb] = 0;
        alignas(4) static volatile std::uint32_t gate = 0;
        std::vector<std::uint32_t> words;
        if (mode == "registration-before-draw") {
            const auto address = reinterpret_cast<std::uintptr_t>(&gate);
            words.insert(words.end(), {0xc0053c00u, 0x13, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 1, 0xffffffffu, 0x19});
        }
        const auto registers = [&](const AgcDriver::Registers& values, std::uint32_t opcode) {
            for (const auto& [offset, value] : values) words.insert(words.end(), {0xc0010000u | (opcode << 8u), offset, value});
        };
        registers(queue.context, 0x69);
        registers(queue.shader, 0x76);
        registers(queue.userConfig, 0x79);
        words.insert(words.end(), {0xc0002f00u, 1, 0xc0012d00u, mode == "empty-draw" ? 0u : 3u, 2});
        Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
        if (mode == "registered") RegisterCompute();
        if (sceAgcDriverSubmitDcb(&packet) != 0) throw std::runtime_error("draw submission failed");
        if (mode == "registration-before-draw") {
            try {
                RegisterCompute();
            } catch (...) {
                gate = 1;
                throw;
            }
            gate = 1;
            ExpectFailure([] { AgcDriverWaitIdle_nid_postfix(); }, "raw graphics draw follows shader registration");
            ExpectFailure([] { AgcDriverShutdown_nid_postfix(); }, "raw graphics draw follows shader registration");
        } else if (mode == "registered") {
            ExpectFailure([] { AgcDriverWaitIdle_nid_postfix(); }, "registered shader");
            ExpectFailure([] { AgcDriverShutdown_nid_postfix(); }, "registered shader");
        } else {
            AgcDriverWaitIdle_nid_postfix();
            if (mode == "empty-draw") {
                RegisterCompute();
                RegisterCompute();
            } else {
                if (sceAgcDriverSubmitDcb(&packet) != 0) throw std::runtime_error("second raw draw submission failed");
                AgcDriverWaitIdle_nid_postfix();
                ExpectFailure([] { RegisterCompute(); }, "after raw graphics draws");
                ExpectFailure([] { AgcDriverRegisterShader_nid_postfix(nullptr); }, "after raw graphics draws");
            }
            AgcDriverShutdown_nid_postfix();
        }
        std::cout << "raw graphics registration policy passed: " << mode << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); }
        catch (const std::exception& shutdown) { std::cerr << "shutdown: " << shutdown.what() << '\n'; }
        return 1;
    }
}
