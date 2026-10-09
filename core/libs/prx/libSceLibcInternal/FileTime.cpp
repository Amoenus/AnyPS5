#include "SceTypes.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstdint>

struct LibcUtimbuf {
    std::int64_t actime;
    std::int64_t modtime;
};
static_assert(sizeof(LibcUtimbuf) == 16);

extern "C" int APS5_VABI utimes_nid_postfix(const char*, const KernelTimeval*);

extern "C" int APS5_VABI utime_nid_postfix(const char* path, const LibcUtimbuf* times) {
    if (times == nullptr) return utimes_nid_postfix(path, nullptr);
    const KernelTimeval values[2] = {{times->actime, 0}, {times->modtime, 0}};
    return utimes_nid_postfix(path, values);
}
