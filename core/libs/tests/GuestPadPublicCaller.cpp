#include "prx/libScePad/include/Pad.hpp"
#include "prx/libScePad/include/PadState.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

extern "C" {
int APS5_VABI scePadInit_nid_postfix();
int APS5_VABI scePadOpen_nid_postfix(int, int, int, const void*);
int APS5_VABI scePadReadState(int, void*);
}

struct Ps5ScePadData {
    std::uint32_t buttons;
    std::uint8_t leftStickX;
    std::uint8_t leftStickY;
    std::uint8_t rightStickX;
    std::uint8_t rightStickY;
    std::uint8_t analogL2;
    std::uint8_t analogR2;
    std::uint16_t padding;
    float orientation[4];
    float acceleration[3];
    float angularVelocity[3];
    std::uint8_t touchData[32];
    std::uint8_t connected;
    std::uint64_t timestamp;
    std::uint8_t ext[64];
};

static_assert(sizeof(Ps5ScePadData) >= sizeof(PadData));
static_assert(offsetof(Ps5ScePadData, buttons) == offsetof(PadData, buttons));
static_assert(offsetof(Ps5ScePadData, leftStickX) == offsetof(PadData, left_stick_x));
static_assert(offsetof(Ps5ScePadData, leftStickY) == offsetof(PadData, left_stick_y));
static_assert(offsetof(Ps5ScePadData, rightStickX) == offsetof(PadData, right_stick_x));
static_assert(offsetof(Ps5ScePadData, rightStickY) == offsetof(PadData, right_stick_y));
static_assert(offsetof(Ps5ScePadData, analogL2) == offsetof(PadData, analog_buttons_l2));
static_assert(offsetof(Ps5ScePadData, analogR2) == offsetof(PadData, analog_buttons_r2));

static void Require(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}

int main() {
    try {
        Require(scePadInit_nid_postfix() == 0, "pad init failed");
        const int handle = scePadOpen_nid_postfix(0x10000000, 0, 0, nullptr);
        Require(handle > 0, "public caller's standard port did not open");
        std::uint32_t cases = 0;
        const auto check = [&](const PadInputState& input, std::uint8_t l2, std::uint8_t r2) {
            PadPublishInput_nid_postfix(input);
            Ps5ScePadData data{};
            Require(scePadReadState(handle, &data) == 0, "pad read failed");
            Require(data.buttons == input.buttons, "public caller read different button bits");
            Require(std::array{data.leftStickX, data.leftStickY, data.rightStickX, data.rightStickY} == input.sticks, "public caller read different stick bytes");
            Require(data.analogL2 == l2 && data.analogR2 == r2, "public caller read different trigger bytes");
            ++cases;
        };
        check(PadInputState{}, 0, 0);
        for (std::uint32_t button : {0x10u, 0x40u, 0x80u, 0x20u, 0x8u, 0x100000u, 0x2u, 0x4u, 0x400u, 0x800u, 0x4000u, 0x2000u, 0x8000u, 0x1000u}) {
            PadInputState input;
            input.buttons = button;
            check(input, 0, 0);
        }
        for (const auto& sticks : std::array<std::array<std::uint8_t,4>,5>{{{0,0,0,0}, {255,255,255,255}, {128,128,128,128}, {117,118,138,139}, {12,64,192,244}}}) {
            PadInputState input;
            input.sticks = sticks;
            check(input, 0, 0);
        }
        for (std::uint8_t l2 : {0, 1, 73, 254, 255}) {
            for (std::uint8_t r2 : {0, 1, 73, 254, 255}) {
                PadInputState input;
                input.buttons = (l2 != 0 ? 0x100u : 0u) | (r2 != 0 ? 0x200u : 0u);
                input.analogButtonsL2 = l2;
                input.analogButtonsR2 = r2;
                check(input, l2, r2);
            }
        }
        PadInputState digital;
        digital.buttons = 0x300;
        check(digital, 255, 255);
        std::printf("public caller prefix: %u synthetic states passed; caller=%zu producer=%zu bytes\n", cases, sizeof(Ps5ScePadData), sizeof(PadData));
        std::printf("unused connected offsets: caller=%zu producer=%zu; timestamp: caller=%zu producer=%zu\n", offsetof(Ps5ScePadData, connected), offsetof(PadData, connected), offsetof(Ps5ScePadData, timestamp), offsetof(PadData, timestamp));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
