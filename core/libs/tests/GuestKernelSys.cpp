#include "prx/libc/include/general/VabiMacros.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>

extern "C" {
std::int64_t APS5_VABI pathconf_nid_postfix(const char*, int);
int APS5_VABI link_nid_postfix(const char*, const char*);
int APS5_VABI fork_nid_postfix(void);
int APS5_VABI symlink_nid_postfix(const char*, const char*);
std::int64_t APS5_VABI readlink_nid_postfix(const char*, char*, std::size_t);
int APS5_VABI setsid_nid_postfix(void);
std::uint16_t APS5_VABI umask_nid_postfix(std::uint16_t);
int* APS5_VABI __error_nid_postfix();
}

void Require(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(stderr, "Kernel sys check failed at line %u (guest errno %d)\n", location.line(), *__error_nid_postfix());
        std::abort();
    }
}

int Error() {
    return *__error_nid_postfix();
}

void RejectsSymlink(const char* target, const char* link) {
    *__error_nid_postfix() = 73;
    bool unsupported = false;
    try {
        symlink_nid_postfix(target, link);
    } catch (const std::runtime_error& error) {
        unsupported = std::string_view(error.what()) == "symlink_nid_postfix not implemented";
    }
    Require(unsupported && Error() == 73);
}

void RejectsLink(const char* source, const char* destination) {
    *__error_nid_postfix() = 73;
    bool unsupported = false;
    try {
        link_nid_postfix(source, destination);
    } catch (const std::runtime_error& error) {
        unsupported = std::string_view(error.what()) == "link_nid_postfix not implemented";
    }
    Require(unsupported && Error() == 73);
}

int main() {
    char pathconfPath[] = "unchanged-path";
    const auto rejectsPathconf = [](const char* path, int name) {
        *__error_nid_postfix() = 71;
        bool rejected = false;
        try {
            static_cast<void>(pathconf_nid_postfix(path, name));
        } catch (const std::runtime_error& error) {
            rejected = std::strcmp(error.what(), "pathconf_nid_postfix not implemented") == 0;
        }
        return rejected && *__error_nid_postfix() == 71;
    };
    Require(rejectsPathconf(pathconfPath, 4));
    Require(std::strcmp(pathconfPath, "unchanged-path") == 0);
    Require(rejectsPathconf(nullptr, 4));
    Require(rejectsPathconf(reinterpret_cast<const char*>(std::uintptr_t{1}), 4));
    Require(rejectsPathconf(pathconfPath, -1));
    RejectsLink("source", "destination");
    RejectsLink(nullptr, "destination");
    RejectsLink("source", nullptr);
    RejectsLink(nullptr, nullptr);
    *__error_nid_postfix() = 73;
    bool forkUnsupported = false;
    try {
        fork_nid_postfix();
    } catch (const std::runtime_error& error) {
        forkUnsupported = std::string_view(error.what()) == "fork: child processes not implemented";
    }
    Require(forkUnsupported);
    Require(Error() == 73);
    RejectsSymlink("target", "link");
    RejectsSymlink(nullptr, "link");
    RejectsSymlink("target", nullptr);
    RejectsSymlink(nullptr, nullptr);
    Require(setsid_nid_postfix() == -1 && Error() == 1);
    Require(umask_nid_postfix(0) == 022);
    Require(umask_nid_postfix(07777) == 0);
    Require(umask_nid_postfix(022) == 07777);
    const auto root = std::filesystem::path("anyps5-kernel-sys-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Require(std::filesystem::create_directory(root));
    const auto file = (root / "file.txt").string();
    std::ofstream(file).put('x');
    char buffer[64];
    Require(readlink_nid_postfix(file.c_str(), buffer, sizeof(buffer)) == -1 && Error() == 22);
    Require(readlink_nid_postfix(root.string().c_str(), buffer, sizeof(buffer)) == -1 && Error() == 22);
    Require(readlink_nid_postfix((root / "missing").string().c_str(), buffer, sizeof(buffer)) == -1 && Error() == 2);
    Require(readlink_nid_postfix("", buffer, sizeof(buffer)) == -1 && Error() == 2);
    Require(readlink_nid_postfix(nullptr, buffer, sizeof(buffer)) == -1 && Error() == 14);
    std::error_code error;
    const auto link = (root / "link").string();
    std::filesystem::create_symlink("sub/target.txt", link, error);
    if (!error) {
        std::memset(buffer, '#', sizeof(buffer));
        Require(readlink_nid_postfix(link.c_str(), buffer, sizeof(buffer)) == 14);
        Require(std::memcmp(buffer, "sub/target.txt", 14) == 0 && buffer[14] == '#');
        std::memset(buffer, '#', sizeof(buffer));
        Require(readlink_nid_postfix(link.c_str(), buffer, 3) == 3);
        Require(std::memcmp(buffer, "sub", 3) == 0 && buffer[3] == '#');
        Require(readlink_nid_postfix(link.c_str(), nullptr, 0) == 0);
        Require(readlink_nid_postfix(link.c_str(), nullptr, sizeof(buffer)) == -1 && Error() == 14);
        Require(readlink_nid_postfix((root / "LINK").string().c_str(), buffer, sizeof(buffer)) == 14);
    } else {
        std::fprintf(stderr, "symbolic links unavailable: the link cases not tested\n");
    }
    std::filesystem::remove_all(root);
    return 0;
}
