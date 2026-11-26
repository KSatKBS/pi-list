#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// KS X 1001 (a.k.a. KS C 5601, EUC-KR) support for Korean closed captions.
//
// Korean DTV closed captions (TTA TTAK.KO-07.0093, based on CEA-708) carry
// Hangul / Hanja as two-byte KS X 1001 codes, each byte being in 0xA1..0xFE.

namespace ebu_list::analysis::cea708
{
    namespace detail
    {
        constexpr std::size_t ksx1001_rows = 94;
        extern const char16_t ksx1001_table[ksx1001_rows * ksx1001_rows];
    } // namespace detail

    constexpr bool is_ksx1001_byte(uint8_t b) noexcept
    {
        return b >= 0xA1 && b <= 0xFE;
    }

    // Returns the Unicode code point of a KS X 1001 two-byte code, or 0 if unmapped.
    char32_t ksx1001_to_unicode(uint8_t lead, uint8_t trail) noexcept;

    // Appends the UTF-8 encoding of a code point. Invalid code points become U+FFFD.
    void append_utf8(std::string& out, char32_t cp);

    // Removes the last UTF-8 encoded code point of a string (if any).
    void pop_utf8(std::string& s);
} // namespace ebu_list::analysis::cea708
