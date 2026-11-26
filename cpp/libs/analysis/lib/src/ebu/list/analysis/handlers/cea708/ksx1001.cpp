#include "ebu/list/analysis/handlers/cea708/ksx1001.h"

using namespace ebu_list::analysis::cea708;

char32_t ebu_list::analysis::cea708::ksx1001_to_unicode(uint8_t lead, uint8_t trail) noexcept
{
    if(!is_ksx1001_byte(lead) || !is_ksx1001_byte(trail)) return 0;
    const auto index = static_cast<std::size_t>(lead - 0xA1) * detail::ksx1001_rows + (trail - 0xA1);
    return detail::ksx1001_table[index];
}

void ebu_list::analysis::cea708::append_utf8(std::string& out, char32_t cp)
{
    if(cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;

    if(cp < 0x80)
    {
        out.push_back(static_cast<char>(cp));
    }
    else if(cp < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if(cp < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

void ebu_list::analysis::cea708::pop_utf8(std::string& s)
{
    while(!s.empty())
    {
        const auto c = static_cast<uint8_t>(s.back());
        s.pop_back();
        if((c & 0xC0) != 0x80) return; // removed the lead byte (or an ASCII char)
    }
}
