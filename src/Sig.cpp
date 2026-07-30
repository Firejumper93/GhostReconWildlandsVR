// Sig.cpp - see Sig.h for the rules this exists to enforce.

#include "Sig.h"

#include "Log.h"

#include <cstring>
#include <vector>

namespace grwxr {
namespace sig {
namespace {

struct Token {
    uint8_t value;
    bool    wildcard;
};

// "48 89 ?? E0" -> tokens. Accepts ? and ?? for a wildcard.
std::vector<Token> parse(const char* pattern) {
    std::vector<Token> out;
    for (const char* p = pattern; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') {
            out.push_back({0, true});
            while (*p == '?') ++p;
            continue;
        }
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nib(p[0]);
        const int lo = p[1] ? nib(p[1]) : -1;
        if (hi < 0 || lo < 0) { out.clear(); return out; }
        out.push_back({(uint8_t)((hi << 4) | lo), false});
        p += 2;
    }
    return out;
}

}  // namespace

std::optional<Image> main_image() {
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    if (!base) return std::nullopt;

    const auto* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return std::nullopt;
    const auto* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return std::nullopt;

    Image img;
    img.base = base;
    img.size = nt->OptionalHeader.SizeOfImage;
    return img;
}

std::optional<uint8_t*> find_unique(const Image& img, const char* pattern,
                                    size_t* matches_out) {
    if (matches_out) *matches_out = 0;

    const std::vector<Token> pat = parse(pattern);
    if (pat.empty() || img.size < pat.size()) return std::nullopt;

    // Skip any page that is not committed and readable. The image is 369 MB and
    // parts of it are not backed; touching those would fault.
    uint8_t* first = nullptr;
    size_t   count = 0;

    const size_t last = img.size - pat.size();
    MEMORY_BASIC_INFORMATION mbi{};
    uint8_t* region_end = img.base;   // forces a query on the first iteration

    for (size_t i = 0; i <= last; ++i) {
        uint8_t* here = img.base + i;
        if (here >= region_end) {
            if (!VirtualQuery(here, &mbi, sizeof(mbi))) break;
            region_end = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
            const bool readable =
                mbi.State == MEM_COMMIT &&
                !(mbi.Protect & PAGE_NOACCESS) &&
                !(mbi.Protect & PAGE_GUARD);
            if (!readable) {
                // Jump past the whole unreadable region.
                i = (size_t)(region_end - img.base) - 1;
                continue;
            }
        }
        // A match must not straddle the end of the readable region.
        if (here + pat.size() > region_end) continue;

        size_t k = 0;
        for (; k < pat.size(); ++k) {
            if (!pat[k].wildcard && here[k] != pat[k].value) break;
        }
        if (k != pat.size()) continue;

        if (count == 0) first = here;
        ++count;
    }

    if (matches_out) *matches_out = count;
    if (count != 1) return std::nullopt;
    return first;
}

}  // namespace sig
}  // namespace grwxr
