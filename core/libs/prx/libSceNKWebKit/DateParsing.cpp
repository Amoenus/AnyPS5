#include "prx/libSceNKWebKit/include/InventoryExports.hpp"
#include "prx/libc/include/Api.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

constexpr double SecondsPerMinute = 60.0;
constexpr double SecondsPerHour = SecondsPerMinute * 60.0;
constexpr double SecondsPerDay = SecondsPerHour * 24.0;
constexpr double MsPerSecond = 1000.0;
constexpr std::int64_t LongMin = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t LongMax = std::numeric_limits<std::int64_t>::max();
constexpr int IntMin = std::numeric_limits<int>::min();
constexpr int IntMax = std::numeric_limits<int>::max();

constexpr int FirstDayOfMonth[2][12] = {
    {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334},
    {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335},
};

struct KnownZone {
    const char* name;
    std::size_t length;
    int offset;
};

constexpr KnownZone KnownZones[] = {
    {"ut", 2, 0},
    {"gmt", 3, 0},
    {"est", 3, -300},
    {"edt", 3, -240},
    {"cst", 3, -360},
    {"cdt", 3, -300},
    {"mst", 3, -420},
    {"mdt", 3, -360},
    {"pst", 3, -480},
    {"pdt", 3, -420},
};

double NaN() {
    return std::numeric_limits<double>::quiet_NaN();
}

bool IsSpace(char character) {
    return character <= ' ' && (character == ' ' || (character <= 0xD && character >= 0x9));
}

bool IsDigit(char character) {
    return character >= '0' && character <= '9';
}

char ToLower(char character) {
    return static_cast<char>(character | ((character >= 'A' && character <= 'Z') << 5));
}

bool StartsWithLetters(const char* string, const char* letters, std::size_t length) {
    for (std::size_t i = 0; i < length; ++i) {
        if (static_cast<char>(string[i] | 0x20) != letters[i]) return false;
    }
    return true;
}

bool IsLeapYear(int year) {
    if (year % 4 != 0) return false;
    if (year % 400 == 0) return true;
    if (year % 100 == 0) return false;
    return true;
}

double DaysFrom1970ToYear(int year) {
    const double yearMinusOne = year - 1;
    const double yearsToAddBy4Rule = std::floor(yearMinusOne / 4.0) - 1970 / 4;
    const double yearsToExcludeBy100Rule = std::floor(yearMinusOne / 100.0) - 1970 / 100;
    const double yearsToAddBy400Rule = std::floor(yearMinusOne / 400.0) - 1970 / 400;
    return 365.0 * (year - 1970.0) + yearsToAddBy4Rule - yearsToExcludeBy100Rule + yearsToAddBy400Rule;
}

double YmdhmsToSeconds(int year, std::int64_t month, std::int64_t day, std::int64_t hour, std::int64_t minute, double second) {
    const int monthDay = FirstDayOfMonth[IsLeapYear(year)][month - 1];
    const double yearDays = DaysFrom1970ToYear(year);
    std::int64_t days = 0;
    if (__builtin_add_overflow(static_cast<std::int64_t>(monthDay), day, &days) || __builtin_sub_overflow(days, std::int64_t{1}, &days))
        throw std::overflow_error("WTF date parsing: the day count overflows long, which is undefined in WebKit");
    const double dateSeconds = second + minute * SecondsPerMinute + hour * SecondsPerHour + (days + yearDays) * SecondsPerDay;
    if (dateSeconds < -8640000000000.0 || dateSeconds > 8640000000000.0) return NaN();
    return dateSeconds;
}

void SkipSpacesAndComments(const char*& string) {
    int nesting = 0;
    char character;
    while ((character = *string)) {
        if (!IsSpace(character)) {
            if (character == '(') {
                if (nesting == IntMax) throw std::overflow_error("WTF date parsing: the comment nesting overflows int, which is undefined in WebKit");
                ++nesting;
            } else if (character == ')' && nesting > 0) {
                --nesting;
            } else if (nesting == 0) {
                break;
            }
        }
        ++string;
    }
}

int FindMonth(const char* monthString) {
    char needle[4];
    for (int i = 0; i < 3; ++i) {
        if (!*monthString) return -1;
        needle[i] = ToLower(*monthString++);
    }
    needle[3] = '\0';
    const char* haystack = "janfebmaraprmayjunjulaugsepoctnovdec";
    const char* found = std::strstr(haystack, needle);
    if (found) {
        const auto position = static_cast<int>(found - haystack);
        if (position % 3 == 0) return position / 3;
    }
    return -1;
}

bool ParseInt(const char* string, char** stopPosition, int* result) {
    const std::int64_t value = strtol_nid_postfix(string, stopPosition, 10);
    if (string == *stopPosition || value <= IntMin || value >= IntMax) return false;
    *result = static_cast<int>(value);
    return true;
}

bool ParseLong(const char* string, char** stopPosition, std::int64_t* result) {
    *result = strtol_nid_postfix(string, stopPosition, 10);
    return !(string == *stopPosition || *result == LongMin || *result == LongMax);
}

void RequireArguments(const char* dateString, const bool* isLocalTime, const char* function) {
    if (isLocalTime == nullptr) throw std::invalid_argument(std::string(function) + ": null isLocalTime reference faults in WebKit");
    if (dateString == nullptr) throw std::invalid_argument(std::string(function) + ": null date string faults in WebKit");
}

double ParseDate(const char* dateString, bool& isLocalTime) {
    isLocalTime = true;
    int offset = 0;
    SkipSpacesAndComments(dateString);
    std::int64_t month = -1;
    const char* wordStart = dateString;
    while (*dateString && !IsDigit(*dateString)) {
        if (IsSpace(*dateString) || *dateString == '(') {
            if (dateString - wordStart >= 3) month = FindMonth(wordStart);
            SkipSpacesAndComments(dateString);
            wordStart = dateString;
        } else {
            ++dateString;
        }
    }
    if (month == -1 && wordStart != dateString) month = FindMonth(wordStart);
    SkipSpacesAndComments(dateString);
    if (!*dateString) return NaN();
    char* end = nullptr;
    std::int64_t day;
    if (!ParseLong(dateString, &end, &day)) return NaN();
    dateString = end;
    if (day < 0) return NaN();
    std::optional<int> year;
    if (day > 31) {
        if (*dateString != '/') return NaN();
        if (!*++dateString) return NaN();
        if (day <= IntMin || day >= IntMax) return NaN();
        year = static_cast<int>(day);
        if (!ParseLong(dateString, &end, &month)) return NaN();
        month -= 1;
        dateString = end;
        if (*dateString++ != '/' || !*dateString) return NaN();
        if (!ParseLong(dateString, &end, &day)) return NaN();
        dateString = end;
    } else if (*dateString == '/' && month == -1) {
        ++dateString;
        month = day - 1;
        if (!ParseLong(dateString, &end, &day)) return NaN();
        if (day < 1 || day > 31) return NaN();
        dateString = end;
        if (*dateString == '/') ++dateString;
        if (!*dateString) return NaN();
    } else {
        if (*dateString == '-') ++dateString;
        SkipSpacesAndComments(dateString);
        if (*dateString == ',') ++dateString;
        if (month == -1) {
            month = FindMonth(dateString);
            if (month == -1) return NaN();
            while (*dateString && *dateString != '-' && *dateString != ',' && !IsSpace(*dateString)) ++dateString;
            if (!*dateString) return NaN();
            if (*dateString != '-' && *dateString != '/' && *dateString != ',' && !IsSpace(*dateString)) return NaN();
            ++dateString;
        }
    }
    if (month < 0 || month > 11) return NaN();
    if (*dateString && !year) {
        int result = 0;
        if (!ParseInt(dateString, &end, &result)) return NaN();
        year = result;
    }
    std::int64_t hour = 0;
    std::int64_t minute = 0;
    std::int64_t second = 0;
    if (!*end) {
        dateString = end;
    } else {
        if (!(IsSpace(*end) || *end == ',')) {
            if (*end != ':') return NaN();
            year.reset();
        } else {
            dateString = ++end;
            SkipSpacesAndComments(dateString);
        }
        ParseLong(dateString, &end, &hour);
        if (end != dateString) {
            dateString = end;
            if (hour < 0 || hour > 23) return NaN();
            if (!*dateString) return NaN();
            if (*dateString++ != ':') return NaN();
            if (!ParseLong(dateString, &end, &minute)) return NaN();
            dateString = end;
            if (minute < 0 || minute > 59) return NaN();
            if (*dateString && *dateString != ':' && !IsSpace(*dateString)) return NaN();
            if (*dateString == ':') {
                ++dateString;
                if (!ParseLong(dateString, &end, &second)) return NaN();
                dateString = end;
                if (second < 0 || second > 59) return NaN();
            }
            SkipSpacesAndComments(dateString);
            if (StartsWithLetters(dateString, "am", 2)) {
                if (hour > 12) return NaN();
                if (hour == 12) hour = 0;
                dateString += 2;
                SkipSpacesAndComments(dateString);
            } else if (StartsWithLetters(dateString, "pm", 2)) {
                if (hour > 12) return NaN();
                if (hour != 12) hour += 12;
                dateString += 2;
                SkipSpacesAndComments(dateString);
            }
        }
    }
    if (IsDigit(*dateString) && !year) {
        int result = 0;
        if (!ParseInt(dateString, &end, &result)) return NaN();
        year = result;
        dateString = end;
        SkipSpacesAndComments(dateString);
    }
    if (*dateString) {
        if (StartsWithLetters(dateString, "gmt", 3) || StartsWithLetters(dateString, "utc", 3)) {
            dateString += 3;
            isLocalTime = false;
        }
        if (*dateString == '+' || *dateString == '-') {
            int zone;
            if (!ParseInt(dateString, &end, &zone)) return NaN();
            dateString = end;
            if (zone < -9959 || zone > 9959) return NaN();
            const int sign = zone < 0 ? -1 : 1;
            zone = zone < 0 ? -zone : zone;
            if (*dateString != ':') {
                if (zone >= 24)
                    offset = ((zone / 100) * 60 + (zone % 100)) * sign;
                else
                    offset = zone * 60 * sign;
            } else {
                ++dateString;
                int zoneMinutes;
                if (!ParseInt(dateString, &end, &zoneMinutes)) return NaN();
                dateString = end;
                int total = 0;
                if (__builtin_add_overflow(zone * 60, zoneMinutes, &total))
                    throw std::overflow_error("WTF date parsing: the time zone offset overflows int, which is undefined in WebKit");
                offset = total * sign;
            }
            isLocalTime = false;
        } else {
            for (const auto& zone : KnownZones) {
                if (StartsWithLetters(dateString, zone.name, zone.length)) {
                    offset = zone.offset;
                    dateString += zone.length;
                    isLocalTime = false;
                    break;
                }
            }
        }
    }
    SkipSpacesAndComments(dateString);
    if (*dateString && !year) {
        int result = 0;
        if (!ParseInt(dateString, &end, &result)) return NaN();
        year = result;
        dateString = end;
        SkipSpacesAndComments(dateString);
    }
    if (*dateString) return NaN();
    if (year) {
        int yearValue = *year;
        if (yearValue >= 0 && yearValue < 100) yearValue += yearValue < 50 ? 2000 : 1900;
        year = yearValue;
    } else {
        year = 2000;
    }
    const double dateSeconds = YmdhmsToSeconds(*year, month + 1, day, hour, minute, static_cast<double>(second)) - offset * SecondsPerMinute;
    return dateSeconds * MsPerSecond;
}

}

extern "C" {

double APS5_VABI _ZN3WTF37parseDateFromNullTerminatedCharactersEPKcRb(const char* dateString, bool* isLocalTime) {
    RequireArguments(dateString, isLocalTime, __func__);
    return ParseDate(dateString, *isLocalTime);
}

}
