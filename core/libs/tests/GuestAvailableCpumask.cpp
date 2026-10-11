#include "SceTypes.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>

extern "C" {
KernelCpumask APS5_VABI sceKernelGetAvailableCpumask(void);
int APS5_VABI scePthreadCreate(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg, const char* name);
int APS5_VABI scePthreadJoin(Pthread thread, void** retval);
Pthread APS5_VABI scePthreadSelf();
int APS5_VABI scePthreadGetaffinity(Pthread thread, KernelCpumask* mask);
int APS5_VABI scePthreadSetaffinity(Pthread thread, KernelCpumask mask);
int APS5_VABI scePthreadAttrInit(PthreadAttr* attr);
int APS5_VABI scePthreadAttrDestroy(PthreadAttr* attr);
int APS5_VABI scePthreadAttrGetaffinity(const PthreadAttr* attr, KernelCpumask* mask);
int APS5_VABI cpuset_getaffinity_nid_postfix(int level, int which, std::int64_t id, std::size_t size, void* mask);
int APS5_VABI cpuset_setaffinity_nid_postfix(int level, int which, std::int64_t id, std::size_t size, const void* mask);
int APS5_VABI getpid_nid_postfix(void);
int* APS5_VABI __error_nid_postfix();
}

static constexpr int SCE_OK = 0;
static constexpr int PS5_LOGICAL_CPUS = 16;

static void Require(bool value) { if (!value) std::abort(); }

static void* APS5_VABI Worker(void* arg) {
    static_cast<std::future<void>*>(arg)->get();
    return nullptr;
}

static void* APS5_VABI ReportCpuset(void* arg) {
    auto* result = static_cast<std::uint64_t*>(arg);
    std::uint64_t set[4] = {~0ull, ~0ull, ~0ull, ~0ull};
    result[0] = cpuset_getaffinity_nid_postfix(3, 1, -1, 8, set);
    result[1] = set[0];
    result[2] = set[1];
    result[3] = cpuset_getaffinity_nid_postfix(3, 1, -1, sizeof(set), set);
    result[4] = set[0] | (set[1] != 0 || set[2] != 0 || set[3] != 0 ? 1ull << 63 : 0);
    result[5] = cpuset_getaffinity_nid_postfix(3, 1, -1, 4, set) == -1 && *__error_nid_postfix() == 34;
    result[6] = cpuset_getaffinity_nid_postfix(3, 1, -1, 33, set) == -1 && *__error_nid_postfix() == 34;
    result[7] = cpuset_getaffinity_nid_postfix(3, 1, -1, 8, nullptr) == -1 && *__error_nid_postfix() == 14;
    return nullptr;
}

struct SetCpusetResult {
    KernelCpumask cpu;
    int results[8];
    KernelCpumask affinity[3];
};

static void* APS5_VABI SetCpuset(void* arg) {
    auto* result = static_cast<SetCpusetResult*>(arg);
    const KernelCpumask cpu = result->cpu;
    std::uint64_t set[4] = {cpu | (1ull << 40), 0, 0, 0};
    result->results[0] = cpuset_setaffinity_nid_postfix(3, 1, -1, 8, set);
    scePthreadGetaffinity(scePthreadSelf(), &result->affinity[0]);
    set[1] = 1;
    result->results[1] = cpuset_setaffinity_nid_postfix(3, 1, -1, 16, set) == -1 && *__error_nid_postfix() == 22;
    set[1] = 0;
    result->results[2] = cpuset_setaffinity_nid_postfix(3, 1, -1, sizeof(set), set);
    result->results[3] = cpuset_setaffinity_nid_postfix(3, 1, -1, 4, set) == -1 && *__error_nid_postfix() == 34;
    result->results[4] = cpuset_setaffinity_nid_postfix(3, 1, -1, 33, set) == -1 && *__error_nid_postfix() == 34;
    result->results[5] = cpuset_setaffinity_nid_postfix(3, 1, -1, 8, nullptr) == -1 && *__error_nid_postfix() == 14;
    const std::uint64_t unavailable[1] = {1ull << 40};
    result->results[6] = cpuset_setaffinity_nid_postfix(3, 1, -1, 8, unavailable) == -1 && *__error_nid_postfix() == 11;
    scePthreadGetaffinity(scePthreadSelf(), &result->affinity[1]);
    const std::uint64_t process[1] = {cpu};
    result->results[7] = cpuset_setaffinity_nid_postfix(3, 2, getpid_nid_postfix(), 8, process);
    scePthreadGetaffinity(scePthreadSelf(), &result->affinity[2]);
    return nullptr;
}

static void* APS5_VABI ReportMask(void* arg) {
    *static_cast<KernelCpumask*>(arg) = sceKernelGetAvailableCpumask();
    return nullptr;
}

int main() {
    const KernelCpumask available = sceKernelGetAvailableCpumask();
    Require(available != 0);
    Require((available >> PS5_LOGICAL_CPUS) == 0);
    Require(sceKernelGetAvailableCpumask() == available);

    PthreadAttr attr = nullptr;
    Require(scePthreadAttrInit(&attr) == SCE_OK);
    KernelCpumask defaultAffinity = 0;
    Require(scePthreadAttrGetaffinity(&attr, &defaultAffinity) == SCE_OK);
    Require(defaultAffinity == available);

    std::promise<void> release;
    auto released = release.get_future();
    Pthread thread = nullptr;
    Require(scePthreadCreate(&thread, &attr, Worker, &released, nullptr) == SCE_OK);
    KernelCpumask threadAffinity = 0;
    Require(scePthreadGetaffinity(thread, &threadAffinity) == SCE_OK);
    Require(threadAffinity == available);

    for (KernelCpumask cpu = 1; cpu != 0; cpu <<= 1) {
        if ((available & cpu) == 0) continue;
        Require(scePthreadSetaffinity(thread, cpu) == SCE_OK);
        Require(scePthreadGetaffinity(thread, &threadAffinity) == SCE_OK);
        Require(threadAffinity == cpu);
    }
    release.set_value();
    Require(scePthreadJoin(thread, nullptr) == SCE_OK);

    KernelCpumask fromThread = 0;
    Require(scePthreadCreate(&thread, &attr, ReportMask, &fromThread, nullptr) == SCE_OK);
    Require(scePthreadJoin(thread, nullptr) == SCE_OK);
    Require(fromThread == available);

    std::uint64_t cpuset[8] = {};
    Require(scePthreadCreate(&thread, &attr, ReportCpuset, cpuset, nullptr) == SCE_OK);
    Require(scePthreadJoin(thread, nullptr) == SCE_OK);
    Require(cpuset[0] == 0 && cpuset[1] == available && cpuset[2] == ~0ull);
    Require(cpuset[3] == 0 && cpuset[4] == available);
    Require(cpuset[5] == 1 && cpuset[6] == 1 && cpuset[7] == 1);

    KernelCpumask first = available & (~available + 1);
    KernelCpumask last = first;
    for (KernelCpumask cpu = first; cpu != 0; cpu <<= 1) if ((available & cpu) != 0) last = cpu;
    std::promise<void> hold;
    auto held = hold.get_future();
    Pthread bystander = nullptr;
    Require(scePthreadCreate(&bystander, &attr, Worker, &held, nullptr) == SCE_OK);
    SetCpusetResult set{last, {}, {}};
    Require(scePthreadCreate(&thread, &attr, SetCpuset, &set, nullptr) == SCE_OK);
    Require(scePthreadJoin(thread, nullptr) == SCE_OK);
    Require(set.results[0] == 0 && set.affinity[0] == last);
    Require(set.results[1] == 1 && set.results[2] == 0);
    Require(set.results[3] == 1 && set.results[4] == 1 && set.results[5] == 1);
    Require(set.results[6] == 1 && set.affinity[1] == last);
    Require(set.results[7] == 0 && set.affinity[2] == last);
    KernelCpumask bystanderAffinity = 0;
    Require(scePthreadGetaffinity(bystander, &bystanderAffinity) == SCE_OK);
    Require(bystanderAffinity == last);
    const std::uint64_t everything[1] = {available};
    Require(cpuset_setaffinity_nid_postfix(3, 2, -1, 8, everything) == 0);
    Require(scePthreadGetaffinity(bystander, &bystanderAffinity) == SCE_OK);
    Require(bystanderAffinity == available);
    hold.set_value();
    Require(scePthreadJoin(bystander, nullptr) == SCE_OK);
    Require(scePthreadAttrDestroy(&attr) == SCE_OK);
}
