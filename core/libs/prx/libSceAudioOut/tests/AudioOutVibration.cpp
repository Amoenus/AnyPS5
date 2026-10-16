#include "prx/libScePad/include/PadState.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string_view>

extern "C" {
int APS5_VABI sceAudioOutOpen(int, int, int, std::uint32_t, std::uint32_t, std::uint32_t);
int APS5_VABI sceAudioOutClose(int);
int APS5_VABI sceAudioOutOutput(int, const void*);
int APS5_VABI sceAudioOutOutputs(AudioOutOutputParam*, std::uint32_t);
}

static void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc == 2 ? argv[1] : "--s16";
        Require(argc <= 2 && (mode == "--s16" || mode == "--f32" || mode == "--batch_s16" || mode == "--batch_f32" || mode == "--pad_control"), "invalid test arguments");
        Pad::Initialize();
        Pad::SetVibrationMode(1);
        std::uint32_t seen = 0;
        PadOutputState output{};
        static_cast<void>(PadFetchOutput_nid_postfix(&seen, &output));
        struct Port {
            int handle = -1;
            ~Port() { if (handle > 0) sceAudioOutClose(handle); }
        } port;
        if (mode == "--pad_control") {
            PadSubmitAudioHaptics_nid_postfix(128, 255);
        } else {
            constexpr std::uint32_t frames = 256;
            const bool floating = mode == "--f32" || mode == "--batch_f32";
            const bool batch = mode == "--batch_s16" || mode == "--batch_f32";
            std::array<std::int16_t, frames * 2> integers{};
            std::array<float, frames * 2> floats{};
            for (std::uint32_t frame = 0; frame < frames; ++frame) {
                integers[frame * 2] = 8192;
                integers[frame * 2 + 1] = -16384;
                floats[frame * 2] = 0.25f;
                floats[frame * 2 + 1] = -0.5f;
            }
            port.handle = sceAudioOutOpen(0x10000000, 10, 0, frames, 48000, floating ? 4u : 1u);
            Require(port.handle > 0, "legacy vibration port did not open");
            const void* samples = floating ? static_cast<const void*>(floats.data()) : static_cast<const void*>(integers.data());
            AudioOutOutputParam param{};
            param.handle = port.handle;
            param.ptr = samples;
            const int result = batch ? sceAudioOutOutputs(&param, 1) : sceAudioOutOutput(port.handle, samples);
            std::printf("legacy port=%d format=%s batch=%d output=%d\n", port.handle, floating ? "f32" : "s16", batch, result);
            Require(result >= 0, "legacy vibration output failed");
        }
        Require(PadFetchOutput_nid_postfix(&seen, &output), "accepted legacy vibration PCM did not publish controller output");
        Require(output.audioVibrationLarge == 128 && output.audioVibrationSmall == 255, "legacy vibration PCM did not reach the existing motor bridge");
        Require(output.vibrationLarge == 0 && output.vibrationSmall == 0, "audio vibration replaced regular motor state");
        std::puts("legacy vibration bridge test passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
