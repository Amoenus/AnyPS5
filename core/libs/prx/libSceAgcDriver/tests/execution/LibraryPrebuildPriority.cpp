#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("library prebuild priority: " + message);
}

unsigned PrebuildThreads() {
    const char* text = std::getenv("APS5_PREBUILD_THREADS");
    return text != nullptr ? std::max(1u, static_cast<unsigned>(std::strtoul(text, nullptr, 10))) : std::max(2u, std::thread::hardware_concurrency() / 2u);
}

std::vector<std::uint32_t> Pm4(std::uint32_t opcode, std::initializer_list<std::uint32_t> body) {
    std::vector<std::uint32_t> words{0xc0000000u | (static_cast<std::uint32_t>(body.size() - 1) << 16u) | (opcode << 8u)};
    words.insert(words.end(), body);
    return words;
}

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned blocked = 0;
    unsigned released = 0;
    std::vector<std::uint64_t> order;
};

struct ReleaseOnExit {
    std::shared_ptr<Gate> gate;
    ~ReleaseOnExit() {
        {
            std::lock_guard lock(gate->mutex);
            gate->released = ~0u;
        }
        gate->changed.notify_all();
        AgcDriver::DriverDetail::AwaitLibraryPrebuilds();
    }
};

constexpr std::uint64_t first = 0x0012345600ull;
constexpr std::uint64_t second = 0x120023456700ull;

std::vector<std::uint32_t> Bind(std::uint32_t reg, std::uint64_t program) {
    return Pm4(0x76, {reg, static_cast<std::uint32_t>(program >> 8u), static_cast<std::uint32_t>(program >> 40u)});
}

std::uint64_t FirstPrebuildAfter(std::vector<std::uint32_t> words) {
    const unsigned threads = PrebuildThreads();
    const auto gate = std::make_shared<Gate>();
    const ReleaseOnExit release{gate};
    for (unsigned index = 0; index < threads; ++index) {
        AgcDriver::DriverDetail::SubmitLibraryPrebuild([gate] {
            std::unique_lock lock(gate->mutex);
            ++gate->blocked;
            gate->changed.notify_all();
            gate->changed.wait(lock, [&] { return gate->released != 0; });
            if (gate->released != ~0u) --gate->released;
        });
    }
    std::unique_lock lock(gate->mutex);
    Require(gate->changed.wait_for(lock, std::chrono::seconds(10), [&] { return gate->blocked == threads; }), std::to_string(gate->blocked) + " of " + std::to_string(threads) + " prebuild threads were blocked");
    lock.unlock();
    for (const auto program : {first, second}) {
        AgcDriver::DriverDetail::SubmitLibraryPrebuild([gate, program] {
            std::lock_guard ran(gate->mutex);
            gate->order.push_back(program);
            gate->changed.notify_all();
        }, program);
    }
    Packet dcb{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&dcb) == 0, "the submission binding a pixel program failed");
    AgcDriverWaitIdle_nid_postfix();
    lock.lock();
    gate->released = 1;
    gate->changed.notify_all();
    Require(gate->changed.wait_for(lock, std::chrono::seconds(10), [&] { return !gate->order.empty(); }), "no queued prebuild ran after one prebuild thread was released");
    return gate->order.front();
}

}

int main() {
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        AgcDriver::DriverDetail::Driver::Get();
        Require(FirstPrebuildAfter(Bind(0x008, second)) == second, "the prebuild of the pixel program a submission binds did not run before an earlier queued prebuild");
        Require(FirstPrebuildAfter(Pm4(0x76, {0x008, static_cast<std::uint32_t>(second >> 8u)})) == second, "a pixel program bound with a retained high address register was not prioritized");
        Require(FirstPrebuildAfter(Bind(0x048, second)) == second, "the prebuild of the legacy vertex program a submission binds did not run before an earlier queued prebuild");
        Require(FirstPrebuildAfter(Bind(0x008, 0x120034567800ull)) == first, "a submission binding another program changed the order of queued prebuilds");
        AgcDriver::DriverDetail::CloseRegistrationPreparation();
        AgcDriverShutdown_nid_postfix();
        std::puts("library prebuild priority tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
