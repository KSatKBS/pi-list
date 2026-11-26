#include "ebu/list/analysis/handlers/cea708/dtvcc_decoder.h"
#include "ebu/list/analysis/handlers/cea708/ksx1001.h"

using namespace ebu_list::analysis::cea708;

//------------------------------------------------------------------------------
// CEA-708-E code space
//------------------------------------------------------------------------------

namespace
{
    constexpr uint8_t cc_type_dtvcc_data  = 2;
    constexpr uint8_t cc_type_dtvcc_start = 3;

    // C0
    constexpr uint8_t c0_nul  = 0x00;
    constexpr uint8_t c0_etx  = 0x03;
    constexpr uint8_t c0_bs   = 0x08;
    constexpr uint8_t c0_ff   = 0x0C;
    constexpr uint8_t c0_cr   = 0x0D;
    constexpr uint8_t c0_hcr  = 0x0E;
    constexpr uint8_t c0_ext1 = 0x10;
    constexpr uint8_t c0_p16  = 0x18;

    // C1
    constexpr uint8_t c1_dly = 0x8D;
    constexpr uint8_t c1_dlc = 0x8E;
    constexpr uint8_t c1_spa = 0x90;
    constexpr uint8_t c1_spc = 0x91;

    constexpr char32_t music_note        = 0x266A;
    constexpr char32_t replacement_char  = 0xFFFD;
    constexpr char32_t unsupported_glyph = '_';

    // Number of parameter bytes following a C1 command (0x80..0x9F).
    constexpr std::size_t c1_parameter_count(uint8_t c)
    {
        if(c <= 0x87) return 0;           // CW0..CW7
        if(c <= 0x8D) return 1;           // CLW, DSW, HDW, TGW, DLW, DLY
        if(c <= 0x8F) return 0;           // DLC, RST
        if(c == c1_spa) return 2;         // SPA
        if(c == c1_spc) return 3;         // SPC
        if(c == 0x92) return 2;           // SPL
        if(c <= 0x96) return 0;           // reserved
        if(c == 0x97) return 4;           // SWA
        return 6;                         // DF0..DF7
    }

    // Number of parameter bytes following a C0 command (0x00..0x1F), EXT1 excluded.
    constexpr std::size_t c0_parameter_count(uint8_t c)
    {
        if(c <= 0x0F) return 0;
        if(c <= 0x17) return 1;
        return 2;
    }

    char32_t g2_to_unicode(uint8_t c)
    {
        switch(c)
        {
        case 0x20: return ' ';    // transparent space
        case 0x21: return ' ';    // non-breaking transparent space
        case 0x25: return 0x2026; // U+2026
        case 0x2A: return 0x0160; // U+0160
        case 0x2C: return 0x0152; // U+0152
        case 0x30: return 0x2588; // U+2588
        case 0x31: return 0x2018; // U+2018
        case 0x32: return 0x2019; // U+2019
        case 0x33: return 0x201C; // U+201C
        case 0x34: return 0x201D; // U+201D
        case 0x35: return 0x2022; // U+2022
        case 0x39: return 0x2122; // U+2122
        case 0x3A: return 0x0161; // U+0161
        case 0x3C: return 0x0153; // U+0153
        case 0x3D: return 0x2120; // U+2120
        case 0x3F: return 0x0178; // U+0178
        case 0x76: return 0x215B; // U+215B
        case 0x77: return 0x215C; // U+215C
        case 0x78: return 0x215D; // U+215D
        case 0x79: return 0x215E; // U+215E
        case 0x7A: return 0x2502; // U+2502
        case 0x7B: return 0x2510; // U+2510
        case 0x7C: return 0x2514; // U+2514
        case 0x7D: return 0x2500; // U+2500
        case 0x7E: return 0x2518; // U+2518
        case 0x7F: return 0x250C; // U+250C
        default: return unsupported_glyph;
        }
    }
} // namespace

//------------------------------------------------------------------------------

dtvcc_decoder::dtvcc_decoder(korean_mode mode) : mode_(mode)
{
}

void dtvcc_decoder::push(bool cc_valid, uint8_t cc_type, uint8_t cc_data_1, uint8_t cc_data_2)
{
    if(!cc_valid) return;

    if(cc_type == cc_type_dtvcc_start)
    {
        if(!packet_.empty())
        {
            // the previous packet did not get all the bytes its header announced
            ++stats_.size_errors;
            decode_packet(packet_.data(), packet_.size());
            packet_.clear();
        }

        const int sequence     = cc_data_1 >> 6;
        const auto size_code   = static_cast<std::size_t>(cc_data_1 & 0x3F);
        expected_size_         = size_code == 0 ? 128 : size_code * 2;
        if(last_sequence_ >= 0 && sequence != ((last_sequence_ + 1) & 0x03)) ++stats_.sequence_errors;
        last_sequence_ = sequence;

        packet_.push_back(cc_data_1);
        packet_.push_back(cc_data_2);
    }
    else if(cc_type == cc_type_dtvcc_data)
    {
        if(packet_.empty())
        {
            ++stats_.ignored_cc_entries;
            return;
        }
        packet_.push_back(cc_data_1);
        packet_.push_back(cc_data_2);
    }
    else
    {
        return; // CEA-608
    }

    if(packet_.size() >= expected_size_)
    {
        packet_.resize(expected_size_);
        decode_packet(packet_.data(), packet_.size());
        packet_.clear();
    }
}

void dtvcc_decoder::flush()
{
    if(!packet_.empty())
    {
        ++stats_.size_errors;
        decode_packet(packet_.data(), packet_.size());
        packet_.clear();
    }

    for(auto& [number, svc] : services_)
    {
        if(svc.held_lead < 0) continue;
        put_char(svc, mode_ == korean_mode::always ? replacement_char : static_cast<char32_t>(svc.held_lead));
        svc.held_lead = -1;
    }
}

std::vector<service_text> dtvcc_decoder::drain()
{
    std::vector<service_text> result;
    for(auto& [number, svc] : services_)
    {
        if(svc.pending.empty()) continue;
        result.push_back({number, std::move(svc.pending)});
        svc.pending.clear();
    }
    return result;
}

void dtvcc_decoder::decode_packet(const uint8_t* data, std::size_t size)
{
    if(size == 0) return;
    ++stats_.packets;

    std::size_t pos = 1; // skip the packet header (sequence number + size)
    while(pos < size)
    {
        const uint8_t block_header = data[pos++];
        int service_number         = block_header >> 5;
        auto block_size            = static_cast<std::size_t>(block_header & 0x1F);

        if(service_number == 0) break; // null service block: padding until the end of the packet

        if(service_number == 7) // extended service number
        {
            if(pos >= size)
            {
                ++stats_.size_errors;
                break;
            }
            service_number = data[pos++] & 0x3F;
        }

        if(pos + block_size > size)
        {
            ++stats_.size_errors;
            block_size = size - pos;
        }

        decode_service_block(services_[service_number], data + pos, block_size);
        pos += block_size;
    }
}

void dtvcc_decoder::decode_service_block(service_state& svc, const uint8_t* data, std::size_t size)
{
    std::size_t i = 0;

    // A KS X 1001 character may have been split across two service blocks
    if(svc.held_lead >= 0)
    {
        const auto lead = static_cast<uint8_t>(svc.held_lead);
        svc.held_lead   = -1;
        const auto cp   = size > 0 ? ksx1001_to_unicode(lead, data[0]) : 0;
        if(cp != 0)
        {
            ++stats_.korean_characters;
            put_char(svc, cp);
            i = 1;
        }
        else if(size > 0 && mode_ == korean_mode::always && is_ksx1001_byte(data[0]))
        {
            put_char(svc, replacement_char);
            i = 1;
        }
        else
        {
            put_char(svc, mode_ == korean_mode::always ? replacement_char : static_cast<char32_t>(lead));
        }
    }

    while(i < size)
    {
        const uint8_t c = data[i];

        if(c < 0x20) // C0
        {
            if(c == c0_ext1)
            {
                if(i + 1 >= size)
                {
                    ++stats_.size_errors;
                    return;
                }
                const uint8_t e = data[i + 1];
                if(e < 0x20) // C2: 0, 1, 2 or 3 parameter bytes
                {
                    i += 2 + (e >> 3);
                }
                else if(e < 0x80) // G2
                {
                    put_char(svc, g2_to_unicode(e));
                    i += 2;
                }
                else if(e < 0x88) // C3: 4 parameter bytes
                {
                    i += 2 + 4;
                }
                else if(e < 0x90) // C3: 5 parameter bytes
                {
                    i += 2 + 5;
                }
                else if(e < 0xA0) // C3: variable length
                {
                    if(i + 2 >= size)
                    {
                        ++stats_.size_errors;
                        return;
                    }
                    i += 3 + (data[i + 2] & 0x1F);
                }
                else // G3
                {
                    if(e == 0xA0)
                    {
                        put_char(svc, '[');
                        put_char(svc, 'C');
                        put_char(svc, 'C');
                        put_char(svc, ']');
                    }
                    else
                    {
                        put_char(svc, unsupported_glyph);
                    }
                    i += 2;
                }
                continue;
            }

            if(c == c0_p16) // 16-bit character
            {
                if(i + 2 >= size)
                {
                    ++stats_.size_errors;
                    return;
                }
                const uint8_t hi = data[i + 1];
                const uint8_t lo = data[i + 2];
                if(const auto cp = ksx1001_to_unicode(hi, lo); cp != 0)
                {
                    ++stats_.korean_characters;
                    put_char(svc, cp);
                }
                else
                {
                    put_char(svc, (static_cast<char32_t>(hi) << 8) | lo); // UCS-2
                }
                i += 3;
                continue;
            }

            switch(c)
            {
            case c0_nul: break;
            case c0_etx:
            case c0_ff:
            case c0_cr: new_line(svc); break;
            case c0_bs:
                if(!svc.pending.empty() && svc.pending.back() != '\n') pop_utf8(svc.pending);
                break;
            case c0_hcr: clear_line(svc); break;
            default: break;
            }
            i += 1 + c0_parameter_count(c);
            continue;
        }

        if(c < 0x80) // G0
        {
            put_char(svc, c == 0x7F ? music_note : static_cast<char32_t>(c));
            ++i;
            continue;
        }

        if(c < 0xA0) // C1
        {
            const auto length = 1 + c1_parameter_count(c);
            if(i + length > size) ++stats_.size_errors;

            // Window and pen location commands delimit caption lines.
            // Pen attributes/colors (SPA, SPC) and delays do not.
            if(c != c1_spa && c != c1_spc && c != c1_dly && c != c1_dlc) new_line(svc);

            i += length;
            continue;
        }

        // G1: Latin-1, or the lead byte of a KS X 1001 (Korean) character
        if(mode_ != korean_mode::never && is_ksx1001_byte(c))
        {
            if(i + 1 >= size)
            {
                svc.held_lead = c; // wait for the next service block
                return;
            }

            const uint8_t trail = data[i + 1];
            if(const auto cp = ksx1001_to_unicode(c, trail); cp != 0)
            {
                ++stats_.korean_characters;
                put_char(svc, cp);
                i += 2;
                continue;
            }

            if(mode_ == korean_mode::always)
            {
                put_char(svc, replacement_char);
                i += is_ksx1001_byte(trail) ? 2 : 1;
                continue;
            }
        }

        put_char(svc, c == 0xA0 ? ' ' : static_cast<char32_t>(c)); // ISO 8859-1
        ++i;
    }
}

void dtvcc_decoder::put_char(service_state& svc, char32_t cp)
{
    append_utf8(svc.pending, cp);
    svc.at_line_start = false;
}

void dtvcc_decoder::new_line(service_state& svc)
{
    if(svc.at_line_start) return;
    svc.pending.push_back('\n');
    svc.at_line_start = true;
}

void dtvcc_decoder::clear_line(service_state& svc)
{
    const auto last_new_line = svc.pending.rfind('\n');
    svc.pending.erase(last_new_line == std::string::npos ? 0 : last_new_line + 1);
    svc.at_line_start = svc.pending.empty() || svc.pending.back() == '\n';
}
