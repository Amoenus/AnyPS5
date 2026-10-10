#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string_view>

extern "C" {
int APS5_VABI sceKernelGetCpuTemperature(int*);
int APS5_VABI sceKernelGetSocSensorTemperature(int, int*);
int* APS5_VABI __error_nid_postfix();
}

static void Require(bool value) { if (!value) std::abort(); }

template<typename TCallback>
static void RequireUnsupported(TCallback callback, std::string_view message) {
    bool unsupported = false;
    try {
        callback();
    } catch (const std::runtime_error& error) {
        unsupported = std::string_view(error.what()) == message;
    }
    Require(unsupported);
}

int main() {
    struct Buffer {
        int before;
        int temperature;
        int after;
    } buffer{0x12345678, std::numeric_limits<int>::min(), 0x23456789};
    *__error_nid_postfix() = 73;
    for (int* output : {&buffer.temperature, static_cast<int*>(nullptr)}) {
        RequireUnsupported([&] { sceKernelGetCpuTemperature(output); },
            "sceKernelGetCpuTemperature not implemented");
        for (int sensor : {-1, 0, 1, 15, 16, std::numeric_limits<int>::max()}) {
            RequireUnsupported([&] { sceKernelGetSocSensorTemperature(sensor, output); },
                "sceKernelGetSocSensorTemperature not implemented");
        }
    }
    Require(buffer.before == 0x12345678);
    Require(buffer.temperature == std::numeric_limits<int>::min());
    Require(buffer.after == 0x23456789);
    Require(*__error_nid_postfix() == 73);
}
