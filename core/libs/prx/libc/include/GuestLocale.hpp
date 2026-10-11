#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GUESTLOCALE_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GUESTLOCALE_HPP

#include <cstddef>
#include <cstdint>
#include "general/VabiMacros.hpp"

namespace GuestLocale {

struct LocinfoStorage {
    alignas(std::uint64_t) unsigned char bytes[64];
};

struct Facet;

struct FacetVtable {
    void (APS5_VABI *destroy)(Facet* self);
    void (APS5_VABI *deleteObject)(Facet* self);
    void (APS5_VABI *retain)(Facet* self);
    Facet* (APS5_VABI *release)(Facet* self);
};

struct Facet {
    const FacetVtable* vtable;
    std::uint32_t references;
    std::uint32_t reserved;
};

struct Implementation {
    Facet base;
    Facet** facets;
    std::uint64_t facetCount;
    std::uint32_t category;
    bool transparent;
    const char* name;
};

struct String {
    std::uint64_t reserved;
    union {
        char buffer[16];
        char* pointer;
    };
    std::uint64_t size;
    std::uint64_t capacity;
};

struct CollateFacet {
    Facet base;
    const void* collation;
    const void* wideCollation;
};

struct CollateVtable {
    FacetVtable facet;
    int (APS5_VABI *compare)(const CollateFacet* self, const char* first1, const char* last1, const char* first2, const char* last2);
    String* (APS5_VABI *transform)(String* result, const CollateFacet* self, const char* first, const char* last);
    std::int64_t (APS5_VABI *hash)(const CollateFacet* self, const char* first, const char* last);
};

struct IosBase {
    const void* vtable;
    std::int64_t standardStream;
    std::int32_t state;
    std::int32_t exceptions;
    std::int32_t flags;
    std::int32_t precision;
    std::int32_t width;
    std::uint32_t reserved;
    void* storage;
    void* callbacks;
    Implementation** locale;
};

static_assert(sizeof(Facet) == 0x10);
static_assert(offsetof(Implementation, facets) == 0x10);
static_assert(offsetof(Implementation, facetCount) == 0x18);
static_assert(offsetof(Implementation, transparent) == 0x24);
static_assert(offsetof(Implementation, name) == 0x28);
static_assert(offsetof(IosBase, state) == 0x10);
static_assert(offsetof(IosBase, storage) == 0x28);
static_assert(offsetof(IosBase, callbacks) == 0x30);
static_assert(offsetof(IosBase, locale) == 0x38);
static_assert(sizeof(IosBase) == 0x40);
static_assert(offsetof(String, buffer) == 0x8);
static_assert(offsetof(String, size) == 0x18);
static_assert(sizeof(String) == 0x28);
static_assert(offsetof(CollateFacet, collation) == 0x10);
static_assert(sizeof(CollateFacet) == 0x20);
static_assert(offsetof(CollateVtable, compare) == 0x20);
static_assert(offsetof(CollateVtable, hash) == 0x30);
static_assert(alignof(LocinfoStorage) == 8);

struct GuestLconv {
    char* decimal_point;
    char* thousands_sep;
    char* grouping;
    char* int_curr_symbol;
    char* currency_symbol;
    char* mon_decimal_point;
    char* mon_thousands_sep;
    char* mon_grouping;
    char* positive_sign;
    char* negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
    char int_p_cs_precedes;
    char int_n_cs_precedes;
    char int_p_sep_by_space;
    char int_n_sep_by_space;
    char int_p_sign_posn;
    char int_n_sign_posn;
};

static_assert(sizeof(GuestLconv) == 0x60);
static_assert(offsetof(GuestLconv, decimal_point) == 0x0);
static_assert(offsetof(GuestLconv, thousands_sep) == 0x8);
static_assert(offsetof(GuestLconv, grouping) == 0x10);
static_assert(offsetof(GuestLconv, int_curr_symbol) == 0x18);
static_assert(offsetof(GuestLconv, currency_symbol) == 0x20);
static_assert(offsetof(GuestLconv, mon_decimal_point) == 0x28);
static_assert(offsetof(GuestLconv, mon_thousands_sep) == 0x30);
static_assert(offsetof(GuestLconv, mon_grouping) == 0x38);
static_assert(offsetof(GuestLconv, positive_sign) == 0x40);
static_assert(offsetof(GuestLconv, negative_sign) == 0x48);
static_assert(offsetof(GuestLconv, int_frac_digits) == 0x50);
static_assert(offsetof(GuestLconv, frac_digits) == 0x51);
static_assert(offsetof(GuestLconv, p_cs_precedes) == 0x52);
static_assert(offsetof(GuestLconv, p_sep_by_space) == 0x53);
static_assert(offsetof(GuestLconv, n_cs_precedes) == 0x54);
static_assert(offsetof(GuestLconv, n_sep_by_space) == 0x55);
static_assert(offsetof(GuestLconv, p_sign_posn) == 0x56);
static_assert(offsetof(GuestLconv, n_sign_posn) == 0x57);
static_assert(offsetof(GuestLconv, int_p_cs_precedes) == 0x58);
static_assert(offsetof(GuestLconv, int_n_cs_precedes) == 0x59);
static_assert(offsetof(GuestLconv, int_p_sep_by_space) == 0x5a);
static_assert(offsetof(GuestLconv, int_n_sep_by_space) == 0x5b);
static_assert(offsetof(GuestLconv, int_p_sign_posn) == 0x5c);
static_assert(offsetof(GuestLconv, int_n_sign_posn) == 0x5d);

}

#endif
