#include "prx/libSceNpWebApi/include/Api.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

constexpr int InvalidArgument = static_cast<int>(0x80552902u);
constexpr int LibContextNotFound = static_cast<int>(0x80552904u);
constexpr int UserContextNotFound = static_cast<int>(0x80552905u);
constexpr int InvalidContentParameter = static_cast<int>(0x80552908u);
const char onlineId[20] = "player";

struct IntInitializeArgs {
    std::uint32_t libHttpCtxId;
    std::uint8_t reserved[4];
    std::uint64_t poolSize;
    const char* name;
    std::uint64_t structSize;
};
static_assert(sizeof(IntInitializeArgs) == 0x20);

using InitializeFn = int (APS5_VABI*)(int, std::size_t);
using IntInitializeFn = int (APS5_VABI*)(const IntInitializeArgs*);
using RegisterFn = int (APS5_VABI*)(int, void*, void*);
using UnregisterFn = int (APS5_VABI*)(int);
using IntCreateRequestFn = int (APS5_VABI*)(int, const char*, const char*, int, const void*, const void*, std::int64_t*);

struct ContentParameter {
    std::uint64_t contentLength;
    const char* contentType;
    std::uint8_t reserved[16];
};

template <typename Function>
Function Resolve(const char* name) {
#ifdef _WIN32
    void* symbol = reinterpret_cast<void*>(GetProcAddress(LoadLibraryA("libSceNpWebApi.prx"), name));
#else
    void* symbol = dlsym(RTLD_DEFAULT, name);
#endif
    if (!symbol) {
        std::fprintf(stderr, "%s is not exported by libSceNpWebApi\n", name);
        std::abort();
    }
    return reinterpret_cast<Function>(symbol);
}

void Require(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "NpWebApi initialize: %s\n", message);
    std::abort();
}

void APS5_VABI Notification() {}

bool InternalArgumentsThrow(IntCreateRequestFn intCreateRequest, int user, const void* internalArgs, std::int64_t* request) {
    try {
        intCreateRequest(user, "group", "/v1", 0, nullptr, internalArgs, request);
    } catch (const std::runtime_error& error) {
        return std::string(error.what()).find("not implemented") != std::string::npos;
    }
    return false;
}

}

int main() {
    const auto initializeForPresence = Resolve<InitializeFn>("sceNpWebApiInitializeForPresence");
    const auto vshInitialize = Resolve<InitializeFn>("sceNpWebApiVshInitialize");
    const auto intInitialize = Resolve<IntInitializeFn>("sceNpWebApiIntInitialize");
    const auto registerNotification = Resolve<RegisterFn>("sceNpWebApiRegisterNotificationCallback");
    const auto unregisterNotification = Resolve<UnregisterFn>("sceNpWebApiUnregisterNotificationCallback");
    const auto intCreateRequest = Resolve<IntCreateRequestFn>("sceNpWebApiIntCreateRequest");

    const int presence = initializeForPresence(1, 0x10000);
    Require(presence > 0, "the presence library context was not created");
    const int vsh = vshInitialize(1, 0x10000);
    Require(vsh > 0 && vsh != presence, "the VSH library context was not a new context");
    IntInitializeArgs args{1, {}, 0x10000, "internal", sizeof(IntInitializeArgs)};
    Require(intInitialize(nullptr) == InvalidArgument, "null internal arguments were taken");
    args.structSize = 0x18;
    Require(intInitialize(&args) == InvalidArgument, "internal arguments of the wrong size were taken");
    args.structSize = sizeof(IntInitializeArgs);
    const int internal = intInitialize(&args);
    Require(internal > 0 && internal != presence && internal != vsh, "the internal library context was not a new context");
    for (const int context : {presence, vsh, internal}) {
        const int user = sceNpWebApiCreateContext(context, onlineId);
        Require(user > 0, "a user context could not be created in the new library context");
        Require(sceNpWebApiDeleteContext(user) == 0, "the user context could not be deleted");
        Require(sceNpWebApiTerminate(context) == 0, "the library context could not be terminated");
    }

    const int context = sceNpWebApiInitialize(1, 0x10000);
    const int user = sceNpWebApiCreateContext(context, onlineId);
    Require(context > 0 && user > 0, "the contexts for the notification checks were not created");
    Require(registerNotification(user, nullptr, nullptr) == InvalidArgument, "a null notification callback was taken");
    Require(registerNotification(user + 1, nullptr, nullptr) == InvalidArgument, "the callback was not checked before the user context");
    auto* callback = reinterpret_cast<void*>(&Notification);
    Require(registerNotification(user + 1, callback, nullptr) == UserContextNotFound, "an unknown user context was taken");
    Require(registerNotification((0x7fff << 16) | 1, callback, nullptr) == LibContextNotFound, "an unknown library context was taken");
    Require(registerNotification(user, callback, nullptr) == 0, "registering the notification callback failed");
    Require(registerNotification(user, callback, nullptr) == 0, "registering the notification callback again failed");
    Require(unregisterNotification(user + 1) == UserContextNotFound, "unregistering for an unknown user context was taken");
    Require(unregisterNotification(user) == 0 && unregisterNotification(user) == 0, "unregistering the notification callback failed");

    std::int64_t request = 77;
    ContentParameter typeless{8, nullptr, {}};
    Require(intCreateRequest(user, nullptr, "/v1", 0, nullptr, nullptr, &request) == InvalidArgument && intCreateRequest(user, "group", nullptr, 0, nullptr, nullptr, &request) == InvalidArgument, "an internal request without a group or path was taken");
    Require(intCreateRequest(user, "group", "/v1", 5, &typeless, nullptr, &request) == InvalidArgument, "a method past PATCH was not refused before the content parameter");
    Require(intCreateRequest(user, "group", "/v1", 0, &typeless, nullptr, &request) == InvalidContentParameter, "content without a type was taken");
    Require(intCreateRequest(user + 1, "group", "/v1", 4, nullptr, nullptr, &request) == UserContextNotFound && request == 77, "an internal request for an unknown user context was taken");
    Require(intCreateRequest(user, "group", "/v1", 4, nullptr, nullptr, &request) == 0 && (request >> 32) == user, "a PATCH internal request was not created in the user context");
    Require(sceNpWebApiDeleteRequest(request) == 0, "the internal request could not be deleted");
    const void* nullFields[3] = {};
    Require(intCreateRequest(user, "group", "/v1", 0, nullptr, nullFields, &request) == 0 && (request >> 32) == user, "internal arguments with only null fields were refused");
    Require(sceNpWebApiDeleteRequest(request) == 0, "the internal request with null fields could not be deleted");
    const void* setFields[3] = {nullptr, onlineId, nullptr};
    request = 77;
    Require(intCreateRequest(user + 1, "group", "/v1", 0, nullptr, setFields, &request) == UserContextNotFound && request == 77, "the user context was not checked before the internal arguments");
    Require(InternalArgumentsThrow(intCreateRequest, user, setFields, &request) && request == 77, "internal arguments with a field set did not stay unsupported");
    Require(sceNpWebApiDeleteContext(user) == 0, "the user context could not be deleted");
    Require(registerNotification(user, callback, nullptr) == UserContextNotFound, "a deleted user context was taken");
    Require(sceNpWebApiTerminate(context) == 0, "the library context could not be terminated");
    Require(unregisterNotification(user) == LibContextNotFound, "a terminated library context was taken");

    std::puts("NpWebApi initialize and notification tests passed");
    return 0;
}
