#include "SceTypes.hpp"
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
using Handler = void (APS5_VABI *)(int);
extern "C" {
int APS5_VABI scePthreadCreate(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg, const char* name);
int APS5_VABI scePthreadJoin(Pthread thread, void** retval);
Handler APS5_VABI signal_nid_postfix(int, Handler);
int APS5_VABI raise_nid_postfix(int);
int APS5_VABI sigaction_nid_postfix(int, const void*, void*);
int APS5_VABI sigprocmask_nid_postfix(int, const void*, void*);
int APS5_VABI pthread_sigmask_nid_postfix(int, const void*, void*);
int* APS5_VABI __error_nid_postfix();
int APS5_VABI _is_signal_return_nid_postfix(std::uint64_t);
}
struct GuestSignalSet {
    std::uint32_t bits[4];
};
struct GuestSigaction {
    std::uintptr_t handler;
    int flags;
    GuestSignalSet mask;
};
static_assert(sizeof(GuestSigaction) == 32);
static bool Same(const GuestSigaction& left, const GuestSigaction& right) {
    return left.handler == right.handler && left.flags == right.flags && std::memcmp(&left.mask, &right.mask, sizeof(left.mask)) == 0;
}
volatile std::sig_atomic_t received = 0;
volatile std::uintptr_t handlerReturn = 0;
std::atomic<int> deliveries{0};
void APS5_VABI InfoCallback(int, void*, void*) {}
void APS5_VABI Callback(int value) {
    received = value;
    deliveries.fetch_add(1);
    handlerReturn = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
}
static void Require(bool value) { if (!value) std::abort(); }
static constexpr std::uint32_t Bit(int signum) { return 1u << (signum - 1); }
static std::atomic<std::uint32_t> childInherited{0};
static std::atomic<std::uint32_t> childInheritedHigh{0};
static std::atomic<std::uint32_t> childUnblocked{0};
static std::atomic<int> childReceived{0};
static std::atomic<int> scopedEntries{0};
static std::atomic<std::uint32_t> scopedSeen{0};
static std::atomic<int> scopedNested{0};
void APS5_VABI Scoped(int value) {
    if (scopedEntries.fetch_add(1) != 0) return;
    GuestSignalSet seen{{}};
    Require(sigprocmask_nid_postfix(1, nullptr, &seen) == 0);
    scopedSeen.store(seen.bits[0]);
    const GuestSignalSet urgent{{Bit(16), 0, 0, 0}};
    Require(sigprocmask_nid_postfix(1, &urgent, nullptr) == 0);
    Require(raise_nid_postfix(value) == 0);
    scopedNested.store(scopedEntries.load());
}
static void* APS5_VABI Child(void*) {
    GuestSignalSet seen{{}};
    Require(sigprocmask_nid_postfix(1, nullptr, &seen) == 0);
    childInherited.store(seen.bits[0]);
    childInheritedHigh.store(seen.bits[1]);
    const GuestSignalSet term{{Bit(15), 0, 0, 0}};
    Require(sigprocmask_nid_postfix(2, &term, &seen) == 0);
    Require(sigprocmask_nid_postfix(1, nullptr, &seen) == 0);
    childUnblocked.store(seen.bits[0]);
    received = 0;
    Require(raise_nid_postfix(15) == 0);
    childReceived.store(received);
    return nullptr;
}
int main() {
    const auto invalid = reinterpret_cast<Handler>(static_cast<std::uintptr_t>(-1));
    const auto ignore = reinterpret_cast<Handler>(std::uintptr_t{1});
    Require(signal_nid_postfix(9, Callback) == invalid);
    Require(*__error_nid_postfix() == 22);
    Require(signal_nid_postfix(15, Callback) != invalid);
    Require(raise_nid_postfix(15) == 0 && received == 15);
    Require(handlerReturn != 0 && _is_signal_return_nid_postfix(handlerReturn) == 0);
    Require(_is_signal_return_nid_postfix(reinterpret_cast<std::uintptr_t>(Callback)) == 0);
    Require(_is_signal_return_nid_postfix(0) == 0);
    received = 0;
    Require(raise_nid_postfix(15) == 0 && received == 15);
    Require(signal_nid_postfix(15, ignore) != invalid);
    received = 0;
    Require(raise_nid_postfix(15) == 0 && received == 0);
    Require(signal_nid_postfix(15, nullptr) == ignore);
    Require(raise_nid_postfix(100) == -1 && *__error_nid_postfix() == 22);
    GuestSignalSet blocked{{0x20, 0, 0, 0}};
    GuestSignalSet previous{{}};
    Require(sigprocmask_nid_postfix(3, &blocked, &previous) == 0);
    Require(previous.bits[0] == 0);
    Require(sigprocmask_nid_postfix(1, nullptr, &previous) == 0);
    Require(previous.bits[0] == 0x20);
    Require(sigprocmask_nid_postfix(2, &blocked, nullptr) == 0);
    Require(sigprocmask_nid_postfix(1, nullptr, &previous) == 0);
    Require(previous.bits[0] == 0);
    *__error_nid_postfix() = 0;
    Require(pthread_sigmask_nid_postfix(1, &blocked, nullptr) == 0);
    Require(pthread_sigmask_nid_postfix(0, &blocked, &previous) == 22);
    Require(pthread_sigmask_nid_postfix(4, &blocked, &previous) == 22);
    Require(*__error_nid_postfix() == 0);
    Require(pthread_sigmask_nid_postfix(0, nullptr, &previous) == 0);
    Require(previous.bits[0] == 0x20);
    Require(sigprocmask_nid_postfix(1, nullptr, &previous) == 0 && previous.bits[0] == 0x20);
    Require(pthread_sigmask_nid_postfix(2, &blocked, &previous) == 0 && previous.bits[0] == 0x20);
    Require(pthread_sigmask_nid_postfix(1, nullptr, &previous) == 0 && previous.bits[0] == 0);

    {
    Require(signal_nid_postfix(15, Callback) != invalid);
    const GuestSignalSet term{{Bit(15), 0, 0, 0}};
    received = 0;
    Require(sigprocmask_nid_postfix(1, &term, nullptr) == 0);
    const int before = deliveries.load();
    Require(raise_nid_postfix(15) == 0 && received == 0);
    Require(raise_nid_postfix(15) == 0 && received == 0);
    Require(sigprocmask_nid_postfix(2, &term, &previous) == 0);
    Require(received == 15 && previous.bits[0] == Bit(15) && deliveries.load() == before + 1);
    received = 0;
    Require(sigprocmask_nid_postfix(1, nullptr, nullptr) == 0 && received == 0);

    const GuestSignalSet urgent{{Bit(16), 0, 0, 0}};
    Require(sigprocmask_nid_postfix(1, &urgent, nullptr) == 0);
    Require(raise_nid_postfix(15) == 0 && received == 15);
    Require(sigprocmask_nid_postfix(2, &urgent, nullptr) == 0);

    const GuestSignalSet wide{{0xffffffffu, 0x1u, 0x80000000u, 0x5u}};
    Require(sigprocmask_nid_postfix(3, &wide, nullptr) == 0);
    Require(sigprocmask_nid_postfix(1, nullptr, &previous) == 0);
    Require(previous.bits[0] == (0xffffffffu & ~(Bit(9) | Bit(17) | Bit(32))));
    Require(previous.bits[1] == 0x1u && previous.bits[2] == 0x80000000u && previous.bits[3] == 0x5u);
    const GuestSignalSet wideApplied = previous;
    Require(sigprocmask_nid_postfix(3, &term, nullptr) == 0);
    GuestSignalSet same = wide;
    Require(sigprocmask_nid_postfix(3, &same, &same) == 0);
    Require(same.bits[0] == Bit(15) && same.bits[1] == 0 && same.bits[2] == 0 && same.bits[3] == 0);
    Require(sigprocmask_nid_postfix(1, nullptr, &previous) == 0);
    Require(previous.bits[0] == wideApplied.bits[0] && previous.bits[3] == 0x5u);
    const GuestSignalSet none{{}};
    Require(sigprocmask_nid_postfix(3, &none, nullptr) == 0);

    received = 0;
    const GuestSignalSet inherited{{Bit(15), 0x1u, 0, 0}};
    Require(sigprocmask_nid_postfix(1, &inherited, nullptr) == 0);
    Pthread child = nullptr;
    Require(scePthreadCreate(&child, nullptr, Child, nullptr, "signals") == 0);
    Require(scePthreadJoin(child, nullptr) == 0);
    Require(childInherited.load() == Bit(15) && childInheritedHigh.load() == 0x1u);
    Require(childUnblocked.load() == 0 && childReceived.load() == 15);
    Require(sigprocmask_nid_postfix(1, nullptr, &previous) == 0);
    Require(previous.bits[0] == Bit(15) && previous.bits[1] == 0x1u);
    received = 0;
    Require(sigprocmask_nid_postfix(2, &inherited, nullptr) == 0 && received == 0);

    Require(signal_nid_postfix(15, ignore) == Callback);
    Require(sigprocmask_nid_postfix(1, &term, nullptr) == 0);
    Require(raise_nid_postfix(15) == 0);
    Require(signal_nid_postfix(15, Callback) == ignore);
    Require(sigprocmask_nid_postfix(2, &term, nullptr) == 0 && received == 0);
    Require(sigprocmask_nid_postfix(1, &term, nullptr) == 0);
    Require(raise_nid_postfix(15) == 0 && received == 0);
    Require(signal_nid_postfix(15, ignore) == Callback);
    Require(signal_nid_postfix(15, Callback) == ignore);
    Require(sigprocmask_nid_postfix(2, &term, nullptr) == 0 && received == 0);

    Require(signal_nid_postfix(15, Scoped) == Callback);
    Require(raise_nid_postfix(15) == 0);
    Require(scopedEntries.load() == 2 && scopedNested.load() == 1 && scopedSeen.load() == Bit(15));
    Require(sigprocmask_nid_postfix(1, nullptr, &previous) == 0);
    Require(previous.bits[0] == 0);
    Require(signal_nid_postfix(15, nullptr) == Scoped);
    }
    {
    const auto callback = reinterpret_cast<std::uintptr_t>(Callback);
    const auto infoCallback = reinterpret_cast<std::uintptr_t>(InfoCallback);
    GuestSigaction current{};
    std::memset(&current, 0xa5, sizeof(current));
    Require(sigaction_nid_postfix(15, nullptr, &current) == 0);
    Require(current.handler == 0 && current.flags == 0x2 && current.mask.bits[0] == 0 && current.mask.bits[3] == 0);
    const GuestSigaction term{callback, 0, {{0x4000, 0, 0, 0x80000000u}}};
    Require(sigaction_nid_postfix(15, &term, &current) == 0 && current.handler == 0);
    received = 0;
    Require(raise_nid_postfix(15) == 0 && received == 15);
    Require(sigaction_nid_postfix(15, nullptr, &current) == 0);
    Require(Same(current, term));
    const GuestSigaction defaults{0, 0, {}};
    Require(sigaction_nid_postfix(15, &defaults, nullptr) == 0);
    Require(signal_nid_postfix(15, Callback) == nullptr);
    Require(sigaction_nid_postfix(15, nullptr, &current) == 0 && current.handler == callback && current.flags == 0x2);
    Require(signal_nid_postfix(15, nullptr) == Callback);

    const GuestSigaction crash{infoCallback, 0x40 | 0x10, {}};
    for (int recorded : {10, 1, 64, 128}) {
        Require(sigaction_nid_postfix(recorded, &crash, nullptr) == 0);
        Require(sigaction_nid_postfix(recorded, nullptr, &current) == 0);
        Require(Same(current, crash));
        Require(sigaction_nid_postfix(recorded, &defaults, nullptr) == 0);
    }
    Require(sigaction_nid_postfix(6, &crash, nullptr) == 0);
    bool rejected = false;
    try {
        raise_nid_postfix(6);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    Require(rejected);
    Require(sigaction_nid_postfix(6, &defaults, &current) == 0 && current.handler == infoCallback && current.flags == 0x50);

    *__error_nid_postfix() = 0;
    Require(sigaction_nid_postfix(0, nullptr, &current) == -1 && *__error_nid_postfix() == 22);
    *__error_nid_postfix() = 0;
    Require(sigaction_nid_postfix(129, nullptr, &current) == -1 && *__error_nid_postfix() == 22);
    for (int fixed : {9, 17}) {
        *__error_nid_postfix() = 0;
        Require(sigaction_nid_postfix(fixed, &term, &current) == -1 && *__error_nid_postfix() == 22);
        Require(sigaction_nid_postfix(fixed, &defaults, &current) == 0 && current.handler == 0);
    }
    Require(sigaction_nid_postfix(15, nullptr, nullptr) == 0);
    Require(signal_nid_postfix(15, Callback) != invalid);
    GuestSignalSet termMask{{1u << 14, 0, 0, 0}};
    received = 0;
    Require(sigprocmask_nid_postfix(1, &termMask, nullptr) == 0);
    Require(raise_nid_postfix(15) == 0 && received == 0);
    Require(sigprocmask_nid_postfix(2, &termMask, nullptr) == 0);
    Require(raise_nid_postfix(15) == 0 && received == 15);
    GuestSignalSet urgMask{{1u << 15, 0, 0, 0}};
    received = 0;
    Require(sigprocmask_nid_postfix(1, &urgMask, nullptr) == 0);
    Require(raise_nid_postfix(15) == 0 && received == 15);
    Require(sigprocmask_nid_postfix(2, &urgMask, nullptr) == 0);
    }
}
