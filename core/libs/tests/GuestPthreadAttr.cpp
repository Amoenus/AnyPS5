#include "SceTypes.hpp"
#include <cstddef>
#include <cstdlib>
#include <future>
#include <stdexcept>
#include <string_view>

extern "C" {
int APS5_VABI scePthreadCreate(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg, const char* name);
int APS5_VABI scePthreadJoin(Pthread thread, void** retval);
int APS5_VABI scePthreadSetprio(Pthread thread, int prio);
int APS5_VABI scePthreadSetaffinity(Pthread thread, KernelCpumask mask);
int APS5_VABI scePthreadAttrInit(PthreadAttr* attr);
int APS5_VABI scePthreadAttrDestroy(PthreadAttr* attr);
int APS5_VABI scePthreadAttrGet(Pthread thread, PthreadAttr* attr);
int APS5_VABI scePthreadAttrSetinheritsched(PthreadAttr* attr, int inheritSched);
int APS5_VABI scePthreadAttrSetschedparam(PthreadAttr* attr, const KernelSchedParam* param);
int APS5_VABI scePthreadAttrSetaffinity(PthreadAttr* attr, KernelCpumask mask);
int APS5_VABI scePthreadAttrSetstacksize(PthreadAttr* attr, std::size_t stackSize);
int APS5_VABI scePthreadAttrGetschedparam(const PthreadAttr* attr, KernelSchedParam* param);
int APS5_VABI scePthreadAttrGetaffinity(const PthreadAttr* attr, KernelCpumask* mask);
int APS5_VABI scePthreadAttrGetstacksize(const PthreadAttr* attr, std::size_t* stackSize);
int APS5_VABI scePthreadAttrGetdetachstate(const PthreadAttr* attr, int* state);
int APS5_VABI pthread_attr_init_nid_postfix(PthreadAttr* attr);
int APS5_VABI pthread_attr_destroy_nid_postfix(PthreadAttr* attr);
int APS5_VABI pthread_attr_getinheritsched_nid_postfix(const PthreadAttr* attr, int* inheritSched);
int APS5_VABI pthread_attr_setinheritsched_nid_postfix(PthreadAttr* attr, int inheritSched);
int APS5_VABI pthread_attr_getscope_nid_postfix(const PthreadAttr* attr, int* scope);
int APS5_VABI pthread_attr_setscope_nid_postfix(PthreadAttr* attr, int scope);
int APS5_VABI pthread_attr_getstackaddr_nid_postfix(const PthreadAttr* attr, void** stackAddress);
int APS5_VABI pthread_attr_get_np_nid_postfix(Pthread thread, PthreadAttr* attr);
int APS5_VABI pthread_create_nid_postfix(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg);
int APS5_VABI pthread_join_nid_postfix(Pthread thread, void** result);
int* APS5_VABI __error_nid_postfix();
int APS5_VABI scePthreadAttrGetsolosched(const PthreadAttr* attr, int* solosched);
int APS5_VABI pthread_attr_setstacksize_nid_postfix(PthreadAttr* attr, std::size_t stackSize);
int APS5_VABI pthread_attr_setsolosched_np_nid_postfix(PthreadAttr* attr, int solosched);
}

static constexpr int SCE_OK = 0;
static constexpr int SCE_KERNEL_ERROR_EINVAL = static_cast<int>(0x80020016);
static constexpr int EXPLICIT_SCHED = 0;
static constexpr int DETACH_JOINABLE = 0;
static constexpr int CREATION_PRIORITY = 256;
static constexpr int UPDATED_PRIORITY = 767;
static constexpr KernelCpumask CREATION_AFFINITY = 0x3;
static constexpr KernelCpumask UPDATED_AFFINITY = 0x1000;
static constexpr std::size_t STACK_SIZE = 2u << 20;
static constexpr std::size_t MIN_STACK_SIZE = 16384;
static constexpr int GUEST_EINVAL = 22;

static void Require(bool value) { if (!value) std::abort(); }

struct ReportedAttributes {
    int priority = 0;
    KernelCpumask affinity = 0;
    std::size_t stackSize = 0;
    int detachState = -1;
};

extern "C" {
int APS5_VABI pthread_attr_setstack_nid_postfix(PthreadAttr* attr, void* addr, std::size_t size);
int APS5_VABI pthread_attr_getstack_nid_postfix(const PthreadAttr* attr, void** addr, std::size_t* size);
}

static ReportedAttributes Query(Pthread thread) {
    PthreadAttr attr = nullptr;
    Require(scePthreadAttrInit(&attr) == SCE_OK);
    Require(scePthreadAttrGet(thread, &attr) == SCE_OK);
    ReportedAttributes reported;
    KernelSchedParam param{};
    Require(scePthreadAttrGetschedparam(&attr, &param) == SCE_OK);
    reported.priority = param.sched_priority;
    Require(scePthreadAttrGetaffinity(&attr, &reported.affinity) == SCE_OK);
    Require(scePthreadAttrGetstacksize(&attr, &reported.stackSize) == SCE_OK);
    Require(scePthreadAttrGetdetachstate(&attr, &reported.detachState) == SCE_OK);
    Require(scePthreadAttrDestroy(&attr) == SCE_OK);
    return reported;
}

static void CheckStackSizeLimit() {
    PthreadAttr attr = nullptr;
    Require(scePthreadAttrInit(&attr) == SCE_OK);
    std::size_t stackSize = 0;
    Require(scePthreadAttrSetstacksize(&attr, MIN_STACK_SIZE) == SCE_OK);
    Require(scePthreadAttrGetstacksize(&attr, &stackSize) == SCE_OK && stackSize == MIN_STACK_SIZE);
    Require(scePthreadAttrSetstacksize(&attr, MIN_STACK_SIZE - 1) == SCE_KERNEL_ERROR_EINVAL);
    Require(scePthreadAttrSetstacksize(&attr, 0) == SCE_KERNEL_ERROR_EINVAL);
    Require(pthread_attr_setstacksize_nid_postfix(&attr, MIN_STACK_SIZE - 1) == GUEST_EINVAL);
    Require(scePthreadAttrGetstacksize(&attr, &stackSize) == SCE_OK && stackSize == MIN_STACK_SIZE);
    Require(scePthreadAttrDestroy(&attr) == SCE_OK);
}

static void CheckSoloSched() {
    PthreadAttr attr = nullptr;
    Require(pthread_attr_setsolosched_np_nid_postfix(nullptr, 1) == GUEST_EINVAL);
    Require(pthread_attr_setsolosched_np_nid_postfix(&attr, 1) == GUEST_EINVAL);
    Require(scePthreadAttrInit(&attr) == SCE_OK);
    int soloSched = -1;
    Require(scePthreadAttrGetsolosched(&attr, &soloSched) == SCE_OK && soloSched == 0);
    Require(pthread_attr_setsolosched_np_nid_postfix(&attr, 1) == 0);
    Require(scePthreadAttrGetsolosched(&attr, &soloSched) == SCE_OK && soloSched == 1);
    Require(pthread_attr_setsolosched_np_nid_postfix(&attr, 0) == 0);
    Require(scePthreadAttrGetsolosched(&attr, &soloSched) == SCE_OK && soloSched == 0);
    Require(scePthreadAttrDestroy(&attr) == SCE_OK);
    Require(pthread_attr_setsolosched_np_nid_postfix(&attr, 1) == GUEST_EINVAL);
}

static void* APS5_VABI Worker(void* arg) {
    static_cast<std::future<void>*>(arg)->get();
    return nullptr;
}

static void* APS5_VABI MarkStarted(void* arg) {
    *static_cast<bool*>(arg) = true;
    return nullptr;
}

static void PosixAttributes() {
    constexpr int Invalid = 22;
    PthreadAttr attr = nullptr;
    Require(pthread_attr_init_nid_postfix(&attr) == 0);
    int value = -1;
    Require(pthread_attr_getinheritsched_nid_postfix(&attr, &value) == 0 && value == 4);
    Require(pthread_attr_setinheritsched_nid_postfix(&attr, EXPLICIT_SCHED) == 0);
    Require(pthread_attr_getinheritsched_nid_postfix(&attr, &value) == 0 && value == EXPLICIT_SCHED);
    Require(pthread_attr_getinheritsched_nid_postfix(&attr, nullptr) == Invalid);

    Require(pthread_attr_getscope_nid_postfix(&attr, &value) == 0 && value == 2);
    Require(pthread_attr_setscope_nid_postfix(&attr, 0) == 0);
    Require(pthread_attr_getscope_nid_postfix(&attr, &value) == 0 && value == 0);
    Require(pthread_attr_setscope_nid_postfix(&attr, 1) == Invalid);
    Require(pthread_attr_getscope_nid_postfix(&attr, &value) == 0 && value == 0);
    Require(pthread_attr_setscope_nid_postfix(&attr, -1) == Invalid);
    Require(pthread_attr_setscope_nid_postfix(&attr, 3) == Invalid);
    Require(pthread_attr_getscope_nid_postfix(&attr, &value) == 0 && value == 0);

    void* address = &value;
    Require(pthread_attr_getstackaddr_nid_postfix(&attr, &address) == 0 && address == nullptr);
    Require(pthread_attr_getstackaddr_nid_postfix(&attr, nullptr) == Invalid);
    char marker;
    Require(pthread_attr_setstack_nid_postfix(&attr, &marker, STACK_SIZE) == 0);
    Require(pthread_attr_getstackaddr_nid_postfix(&attr, &address) == 0 && address == &marker);
    Require(pthread_attr_getscope_nid_postfix(&attr, nullptr) == Invalid);

    PthreadAttr empty = nullptr;
    Require(pthread_attr_getscope_nid_postfix(&empty, &value) == Invalid);
    Require(pthread_attr_getscope_nid_postfix(nullptr, &value) == Invalid);
    Require(pthread_attr_setscope_nid_postfix(&empty, 2) == Invalid);
    Require(pthread_attr_setscope_nid_postfix(nullptr, 2) == Invalid);
    Require(pthread_attr_getinheritsched_nid_postfix(&empty, &value) == Invalid);
    Require(pthread_attr_getinheritsched_nid_postfix(nullptr, &value) == Invalid);
    Require(pthread_attr_getstackaddr_nid_postfix(&empty, &address) == Invalid);
    Require(pthread_attr_getstackaddr_nid_postfix(nullptr, &address) == Invalid);
    Require(pthread_attr_destroy_nid_postfix(&attr) == 0);
}

static void QueriedScope() {
    PthreadAttr created = nullptr;
    PthreadAttr queried = nullptr;
    Require(pthread_attr_init_nid_postfix(&created) == 0);
    Require(pthread_attr_init_nid_postfix(&queried) == 0);
    Require(pthread_attr_setscope_nid_postfix(&created, 0) == 0);
    Require(pthread_attr_setinheritsched_nid_postfix(&created, EXPLICIT_SCHED) == 0);
    Pthread thread = nullptr;
    bool started = false;
    bool rejected = false;
    *__error_nid_postfix() = 77;
    try {
        pthread_create_nid_postfix(&thread, &created, MarkStarted, &started);
    } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()) == "process contention scope not implemented";
    }
    if (thread) Require(pthread_join_nid_postfix(thread, nullptr) == 0);
    Require(rejected && !started && !thread && *__error_nid_postfix() == 77);

    Require(pthread_attr_setinheritsched_nid_postfix(&created, 4) == 0);
    std::promise<void> release;
    auto released = release.get_future();
    Require(pthread_create_nid_postfix(&thread, &created, Worker, &released) == 0);
    Require(pthread_attr_setscope_nid_postfix(&queried, 0) == 0);
    Require(pthread_attr_get_np_nid_postfix(thread, &queried) == 0);
    int scope = -1;
    Require(pthread_attr_getscope_nid_postfix(&queried, &scope) == 0 && scope == 2);
    Require(pthread_attr_setscope_nid_postfix(&queried, 0) == 0);
    Require(scePthreadAttrGet(thread, &queried) == SCE_OK);
    Require(pthread_attr_getscope_nid_postfix(&queried, &scope) == 0 && scope == 2);
    release.set_value();
    Require(pthread_join_nid_postfix(thread, nullptr) == 0);
    Require(pthread_attr_destroy_nid_postfix(&created) == 0);
    Require(pthread_attr_destroy_nid_postfix(&queried) == 0);
}

int main() {
    CheckStackSizeLimit();
    CheckSoloSched();
    PthreadAttr attr = nullptr;
    Require(scePthreadAttrInit(&attr) == SCE_OK);
    Require(scePthreadAttrSetinheritsched(&attr, EXPLICIT_SCHED) == SCE_OK);
    const KernelSchedParam requested{CREATION_PRIORITY};
    Require(scePthreadAttrSetschedparam(&attr, &requested) == SCE_OK);
    Require(scePthreadAttrSetaffinity(&attr, CREATION_AFFINITY) == SCE_OK);
    Require(scePthreadAttrSetstacksize(&attr, STACK_SIZE) == SCE_OK);

    std::promise<void> release;
    auto released = release.get_future();
    Pthread thread = nullptr;
    Require(scePthreadCreate(&thread, &attr, Worker, &released, nullptr) == SCE_OK);
    Require(scePthreadAttrDestroy(&attr) == SCE_OK);

    auto reported = Query(thread);
    Require(reported.priority == CREATION_PRIORITY);
    Require(reported.affinity == CREATION_AFFINITY);
    Require(reported.stackSize == STACK_SIZE);
    Require(reported.detachState == DETACH_JOINABLE);

    Require(scePthreadSetprio(thread, UPDATED_PRIORITY) == SCE_OK);
    Require(scePthreadSetaffinity(thread, UPDATED_AFFINITY) == SCE_OK);
    reported = Query(thread);
    Require(reported.priority == UPDATED_PRIORITY);
    Require(reported.affinity == UPDATED_AFFINITY);
    Require(reported.stackSize == STACK_SIZE);
    Require(reported.detachState == DETACH_JOINABLE);

    PthreadAttr destroyed = nullptr;
    Require(scePthreadAttrGet(thread, &destroyed) == SCE_KERNEL_ERROR_EINVAL);
    Require(scePthreadAttrGet(thread, nullptr) == SCE_KERNEL_ERROR_EINVAL);
    Require(scePthreadAttrInit(&attr) == SCE_OK);
    Require(scePthreadAttrGet(nullptr, &attr) == SCE_KERNEL_ERROR_EINVAL);
    Require(scePthreadAttrDestroy(&attr) == SCE_OK);

    release.set_value();
    Require(scePthreadJoin(thread, nullptr) == SCE_OK);

    PthreadAttr stackAttr = nullptr;
    char stackMarker;
    Require(pthread_attr_setstack_nid_postfix(nullptr, &stackMarker, STACK_SIZE) == 22);
    Require(pthread_attr_setstack_nid_postfix(&stackAttr, &stackMarker, STACK_SIZE) == 22);
    Require(scePthreadAttrInit(&stackAttr) == SCE_OK);
    Require(pthread_attr_setstack_nid_postfix(&stackAttr, &stackMarker, STACK_SIZE) == 0);
    Require(pthread_attr_setstack_nid_postfix(&stackAttr, nullptr, STACK_SIZE) == 22);
    Require(pthread_attr_setstack_nid_postfix(&stackAttr, &stackMarker, 16383) == 22);
    void* reportedAddress = nullptr;
    std::size_t reportedSize = 0;
    Require(pthread_attr_getstack_nid_postfix(&stackAttr, &reportedAddress, &reportedSize) == 0);
    Require(reportedAddress == &stackMarker && reportedSize == STACK_SIZE);
    Require(scePthreadAttrDestroy(&stackAttr) == SCE_OK);
    Require(pthread_attr_setstack_nid_postfix(&stackAttr, &stackMarker, STACK_SIZE) == 22);

    PosixAttributes();
    QueriedScope();
}
