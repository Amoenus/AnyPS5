#ifndef CORE_LIBS_PRX_LIBKERNEL_FILE_STARTUPFILEOBSERVATION_HPP
#define CORE_LIBS_PRX_LIBKERNEL_FILE_STARTUPFILEOBSERVATION_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace StartupFileObservation {

enum class Operation { Open, Read, PositionedRead, Seek, Stat, PosixRead, PosixPositionedRead, Count };

struct Counters {
    std::atomic<std::uint64_t> started{0};
    std::atomic<std::uint64_t> returned{0};
    std::atomic<std::uint64_t> active{0};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<std::uint64_t> exceptions{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> lastRequest{0};
    std::atomic<std::int64_t> lastResult{0};
    std::atomic<int> lastDescriptor{-1};
};

inline std::array<Counters, static_cast<std::size_t>(Operation::Count)> counters_nid_no_patch{};

template<class TAction>
auto Observe_nid_no_patch(Operation operation, int descriptor, std::uint64_t requested, TAction&& action) {
    auto& counters = counters_nid_no_patch[static_cast<std::size_t>(operation)];
    counters.started.fetch_add(1, std::memory_order_relaxed);
    counters.active.fetch_add(1, std::memory_order_relaxed);
    counters.lastDescriptor.store(descriptor, std::memory_order_relaxed);
    counters.lastRequest.store(requested, std::memory_order_relaxed);
    struct Active {
        Counters& counters;
        bool returned = false;
        ~Active() {
            if (!returned) counters.exceptions.fetch_add(1, std::memory_order_relaxed);
            counters.active.fetch_sub(1, std::memory_order_relaxed);
        }
    } active{counters};
    const auto result = action();
    counters.lastResult.store(result, std::memory_order_relaxed);
    counters.returned.fetch_add(1, std::memory_order_relaxed);
    if (result < 0) counters.errors.fetch_add(1, std::memory_order_relaxed);
    else if (operation == Operation::Read || operation == Operation::PositionedRead || operation == Operation::PosixRead || operation == Operation::PosixPositionedRead) counters.bytes.fetch_add(static_cast<std::uint64_t>(result), std::memory_order_relaxed);
    active.returned = true;
    return result;
}

inline void Report_nid_no_patch() {
    constexpr std::array names{"open", "read", "pread", "seek", "stat", "posix_read", "posix_pread"};
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto& counters = counters_nid_no_patch[index];
        std::fprintf(stderr, "[startup-file] %s started=%llu returned=%llu active=%llu errors=%llu exceptions=%llu bytes=%llu last_fd=%d last_request=%llu last_result=%lld\n", names[index], static_cast<unsigned long long>(counters.started.load(std::memory_order_relaxed)), static_cast<unsigned long long>(counters.returned.load(std::memory_order_relaxed)), static_cast<unsigned long long>(counters.active.load(std::memory_order_relaxed)), static_cast<unsigned long long>(counters.errors.load(std::memory_order_relaxed)), static_cast<unsigned long long>(counters.exceptions.load(std::memory_order_relaxed)), static_cast<unsigned long long>(counters.bytes.load(std::memory_order_relaxed)), counters.lastDescriptor.load(std::memory_order_relaxed), static_cast<unsigned long long>(counters.lastRequest.load(std::memory_order_relaxed)), static_cast<long long>(counters.lastResult.load(std::memory_order_relaxed)));
    }
}

}

#endif
