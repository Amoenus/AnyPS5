#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#define NOMINMAX
#include <windows.h>

extern "C" {
int APS5_VABI sceKernelStat(const char*, FileStat*);
int APS5_VABI stat_nid_postfix(const char*, FileStat*);
int* APS5_VABI __error_nid_postfix();
}

namespace {

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::filesystem::path Extended(const std::filesystem::path& path) {
    const auto& native = path.native();
    return native.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + native.substr(2) : L"\\\\?\\" + native;
}

FileStat Stat(const char* path) {
    FileStat result{};
    const auto error = sceKernelStat(path, &result);
    if (error != 0) {
        std::fprintf(stderr, "sceKernelStat failed: result=0x%08x, resolved characters=%zu\n", static_cast<unsigned>(error), ResolvePath_nid_no_patch(path).native().size());
        throw std::runtime_error("stat rejected a valid mapped long host path");
    }
    return result;
}

void Run() {
    auto temp = std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal();
    if (temp.filename().empty()) temp = temp.parent_path();
    const auto root = temp / ("anyps5-stat-long-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Require(root.parent_path() == temp && root.filename().string().starts_with("anyps5-stat-long-"), "invalid fixture root");
    Require(!std::filesystem::exists(root), "fixture root already exists");
    auto deep = root;
    while (deep.native().size() < 290) deep /= std::string(40, 'd');
    Require(deep.native().starts_with(root.native() + L"\\"), "fixture escapes its root");
    struct Cleanup {
        std::filesystem::path root;
        std::filesystem::path deep;
        ~Cleanup() {
            RemovePathAlias_nid_no_patch("/long-host-stat");
            RemovePathAlias_nid_no_patch("/extended-host-stat");
            RemovePathAlias_nid_no_patch("/relative-host-stat");
            DeleteFileW(Extended(deep / "payload.bin").c_str());
            for (auto path = deep; ; path = path.parent_path()) {
                RemoveDirectoryW(Extended(path).c_str());
                if (path == root) break;
            }
        }
    } cleanup{root, deep};
    Require(std::filesystem::create_directories(Extended(deep)), "cannot create fixture directories");
    const auto handle = CreateFileW(Extended(deep / "payload.bin").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(handle != INVALID_HANDLE_VALUE, "cannot prepare long-path file");
    DWORD written = 0;
    const bool prepared = WriteFile(handle, "abcd", 4, &written, nullptr) && written == 4;
    CloseHandle(handle);
    Require(prepared, "cannot write fixture payload");
    AddPathAlias_nid_no_patch("/long-host-stat", deep.string().c_str());
    AddPathAlias_nid_no_patch("/extended-host-stat", Extended(deep).string().c_str());
    Require(ResolvePath_nid_no_patch("/long-host-stat/payload.bin").native().size() > MAX_PATH, "fixture does not cross MAX_PATH");
    const auto reference = Stat("/extended-host-stat/payload.bin");
    Require(reference.st_size == 4 && (reference.st_mode & 0170000) == 0100000, "extended control metadata differs");
    const auto file = Stat("/long-host-stat/payload.bin");
    Require(file.st_size == reference.st_size && file.st_mode == reference.st_mode, "long-path file metadata differs");
    Require(file.st_dev == reference.st_dev && file.st_ino == reference.st_ino && file.st_nlink == reference.st_nlink, "long-path file identity differs");
    FileStat posix{};
    Require(stat_nid_postfix("/long-host-stat/payload.bin", &posix) == 0 && posix.st_size == 4, "POSIX long-path stat failed");
    Require((Stat("/long-host-stat/").st_mode & 0170000) == 0040000, "long-path directory metadata differs");
    Require(sceKernelStat("/long-host-stat/missing.bin", &posix) == static_cast<int>(0x80020002u), "missing long-path file did not report ENOENT");
    Require(sceKernelStat("/long-host-stat/payload.bin/", &posix) == static_cast<int>(0x80020014u), "long-path file slash did not report ENOTDIR");
    Require(sceKernelStat("/long-host-stat/payload.bin", nullptr) == static_cast<int>(0x8002000eu), "long-path null output did not report EFAULT");
    *__error_nid_postfix() = 0;
    Require(stat_nid_postfix("/long-host-stat/missing.bin", &posix) == -1 && *__error_nid_postfix() == 2, "POSIX missing long-path file did not report ENOENT");
    const auto originalDirectory = std::filesystem::current_path();
    {
        struct RestoreDirectory {
            std::filesystem::path original;
            ~RestoreDirectory() {
                std::error_code error;
                std::filesystem::current_path(original, error);
            }
        } restoreDirectory{originalDirectory};
        std::filesystem::current_path(root);
        const auto relative = deep.lexically_relative(root).string();
        Require(!relative.empty() && !std::filesystem::path(relative).is_absolute(), "fixture alias is not relative");
        AddPathAlias_nid_no_patch("/relative-host-stat", relative.c_str());
        Require(Stat("/relative-host-stat/payload.bin").st_size == 4, "relative long-path metadata differs");
    }
    Require(std::filesystem::current_path() == originalDirectory, "fixture did not restore working directory");
    std::puts("long host path stat file/directory/identity/POSIX/missing/slash/null/relative checks passed");
}

}

int main() {
    try { Run(); }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
    return 0;
}
