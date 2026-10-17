#include "prx/libkernel/Time/include/Time.hpp"
#include "prx/libkernel/File/include/File.hpp"
#include "prx/libc/include/FileStream.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <chrono>
#include <cstddef>
#include <thread>

extern "C" {
FileStream* APS5_VABI fopen_nid_postfix(const char*, const char*);
std::size_t APS5_VABI fread_nid_postfix(void*, std::size_t, std::size_t, FileStream*);
int APS5_VABI fseeko_nid_postfix(FileStream*, std::int64_t, int);
int APS5_VABI fclose_nid_postfix(FileStream*);
}

int main() {
    const std::filesystem::path path = "anyps5-startup-stdio-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin";
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); }
    } cleanup{path};
    { std::ofstream file(path, std::ios::binary); file << "abcd"; }
    const auto name = path.string();
    auto* stream = fopen_nid_postfix(name.c_str(), "rb");
    if (!stream) return 1;
    std::array<char, 4> contents{};
    if (fread_nid_postfix(contents.data(), 2, 2, stream) != 2 || contents != std::array{'a', 'b', 'c', 'd'}) return 1;
    if (fseeko_nid_postfix(stream, 0, SEEK_SET) != 0 || fread_nid_postfix(contents.data(), 4, 1, stream) != 1) return 1;
    if (fclose_nid_postfix(stream) != 0) return 1;
    if (fopen_nid_postfix((name + ".missing").c_str(), "rb")) return 1;
    bool threw = false;
    try { static_cast<void>(fopen_nid_postfix(nullptr, "rb")); }
    catch (const std::runtime_error& error) { threw = std::string(error.what()).find("fopen_nid_postfix:") != std::string::npos; }
    if (!threw) return 1;
    FileStat status{};
    std::byte byte{};
    if (sceKernelOpen("", 0, 0) >= 0 || sceKernelStat("", &status) >= 0 || sceKernelRead(-1, &byte, 1) >= 0) return 1;
    if (sceKernelStat("/app0/__anyps5_observer_missing_stat_path__", &status) >= 0) return 1;
    const std::array<int, 8> inactive{};
    const int active = 0;
    const int zeroDuration = 0;
    for (const auto& site : inactive) KernelTraceWait_nid_postfix("wait_trace_inactive", &site, 1000000u, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(3100));
    KernelTraceWait_nid_postfix("wait_trace_active", &active, 2000000u, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(3100));
    KernelTraceWait_nid_postfix("wait_trace_active_zero", &zeroDuration, 0u, false);
    return 0;
}
