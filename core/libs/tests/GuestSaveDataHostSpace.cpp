#include "prx/libSceSaveData/include/Api.hpp"
#include "prx/libSceSaveData.native/include/Api.hpp"
#include "SceTypes.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr std::uint32_t MountReadWrite = 2;
constexpr std::uint32_t MountCreate2 = 32;
constexpr int SaveDataErrorParameter = -2137063424;

int failures = 0;

void Check(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "savedata host space check failed: %s\n", what.c_str());
    ++failures;
}

void SetFreeSpaceVariable(const char* value) {
#ifdef _WIN32
    _putenv_s("ANYPS5_FREE_SPACE", value != nullptr ? value : "");
#else
    if (value == nullptr) {
        unsetenv("ANYPS5_FREE_SPACE");
    } else {
        setenv("ANYPS5_FREE_SPACE", value, 1);
    }
#endif
}

int Mount(const char* name, std::uint32_t mode, std::uint64_t blocks, SaveDataMountResult* result) {
    SceSaveDataDirName dirName{};
    std::snprintf(dirName.data, sizeof(dirName.data), "%s", name);
    SaveDataMount3 mount{};
    mount.user_id = 1;
    mount.dir_name = &dirName;
    mount.mount_mode = mode;
    mount.blocks = blocks;
    return sceSaveDataMount3(&mount, result);
}

void CheckValidValuesMount() {
    const char* values[] = {nullptr, "fixed", "host"};
    int index = 0;
    for (const char* value : values) {
        SetFreeSpaceVariable(value);
        const std::string name = "HS_VALID_" + std::to_string(index++);
        SaveDataMountResult result{};
        const int code = Mount(name.c_str(), MountReadWrite | MountCreate2, 96, &result);
        Check(code == 0, "a valid ANYPS5_FREE_SPACE value lets a save be created (" + name + ")");
        if (code == 0) Check(sceSaveDataUmount2(0, &result.mount_point) == 0, "the created save unmounts (" + name + ")");
    }
    SetFreeSpaceVariable(nullptr);
}

void CheckInvalidValueStopsMount() {
    SetFreeSpaceVariable("bogus");
    SaveDataMountResult result{};
    std::string message;
    try {
        Mount("HS_BOGUS", MountReadWrite | MountCreate2, 96, &result);
    } catch (const std::runtime_error& error) {
        message = error.what();
    }
    Check(message.find("ANYPS5_FREE_SPACE") != std::string::npos, "an invalid ANYPS5_FREE_SPACE throws an error naming the variable");
    Check(!std::filesystem::exists("_sd/HS_BOGUS"), "an invalid ANYPS5_FREE_SPACE stops the mount before a save is created");

    message.clear();
    try {
        Mount("HS_BOGUS", MountReadWrite, 0, &result);
    } catch (const std::runtime_error& error) {
        message = error.what();
    }
    Check(message.find("ANYPS5_FREE_SPACE") != std::string::npos, "an open mount with an invalid ANYPS5_FREE_SPACE also throws");
    SetFreeSpaceVariable(nullptr);
}

#ifndef _WIN32
void CheckEmptyValueStopsMount() {
    SetFreeSpaceVariable("");
    SaveDataMountResult result{};
    std::string message;
    try {
        Mount("HS_EMPTY", MountReadWrite | MountCreate2, 96, &result);
    } catch (const std::runtime_error& error) {
        message = error.what();
    }
    Check(message.find("ANYPS5_FREE_SPACE") != std::string::npos, "an empty ANYPS5_FREE_SPACE throws an error naming the variable");
    Check(!std::filesystem::exists("_sd/HS_EMPTY"), "an empty ANYPS5_FREE_SPACE stops the mount before a save is created");
    SetFreeSpaceVariable(nullptr);
}
#endif

}  // namespace

int main() {
    const auto previous = std::filesystem::current_path();
    std::mt19937_64 random(std::random_device{}());
    const auto root = std::filesystem::temp_directory_path() / ("anyps5-savedata-host-space-" + std::to_string(random()));
    std::filesystem::create_directories(root);
    std::filesystem::current_path(root);
    sceSaveDataInitialize3(nullptr);

    CheckValidValuesMount();
    CheckInvalidValueStopsMount();
#ifndef _WIN32
    CheckEmptyValueStopsMount();
#endif

    sceSaveDataTerminate();
    std::filesystem::current_path(previous);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    if (failures != 0) {
        std::fprintf(stderr, "%d savedata host space check(s) failed\n", failures);
        return 1;
    }
    std::printf("savedata host space tests passed\n");
    return 0;
}
