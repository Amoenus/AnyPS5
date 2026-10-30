#ifndef CORE_LIBS_PRX_LIBSCEIME_INCLUDE_PSNDIALOGNOTICE_HPP
#define CORE_LIBS_PRX_LIBSCEIME_INCLUDE_PSNDIALOGNOTICE_HPP

#include <cstdint>
#include <stdexcept>
#include <string>

enum class PsnDialogNoticeMode : std::uint8_t { Silent, Show };

inline PsnDialogNoticeMode ParsePsnDialogNoticeMode(const char* text) {
    if (text == nullptr || *text == '\0') return PsnDialogNoticeMode::Silent;
    const std::string value(text);
    if (value == "silent") return PsnDialogNoticeMode::Silent;
    if (value == "show") return PsnDialogNoticeMode::Show;
    throw std::runtime_error("ANYPS5_PSN_DIALOGS must be show or silent, not '" + value + "'");
}

#endif
