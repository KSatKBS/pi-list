#include "catch2/catch.hpp"
#include "ebu/list/analysis/handlers/cea708/dtvcc_decoder.h"
#include "ebu/list/analysis/handlers/cea708/ksx1001.h"
#include <initializer_list>

using namespace ebu_list::analysis::cea708;

//------------------------------------------------------------------------------
namespace
{
    // "Korean captions" (U+D55C U+AE00 U+0020 U+C790 U+B9C9) in KS X 1001 / EUC-KR
    const std::vector<uint8_t> hangul_caption = {0xC7, 0xD1, 0xB1, 0xDB, 0x20, 0xC0, 0xDA, 0xB8, 0xB7};

    std::string utf8(const std::u32string& s)
    {
        std::string out;
        for(const auto cp : s)
            append_utf8(out, cp);
        return out;
    }

    // "\uD55C\uAE00 \uC790\uB9C9" (Korean: "Korean captions")
    const std::string hangul_caption_utf8 = utf8(U"\uD55C\uAE00 \uC790\uB9C9");

    std::vector<uint8_t> service_block(int service, const std::vector<uint8_t>& data)
    {
        std::vector<uint8_t> block{static_cast<uint8_t>((service << 5) | data.size())};
        block.insert(block.end(), data.begin(), data.end());
        return block;
    }

    // Splits a caption channel packet (header excluded) into cc_data entries and feeds them to the decoder
    void feed_packet(dtvcc_decoder& decoder, int sequence, std::vector<uint8_t> payload)
    {
        if((payload.size() + 1) % 2) payload.push_back(0x00); // null service block padding
        const auto size_code = static_cast<uint8_t>((payload.size() + 1) / 2);

        std::vector<uint8_t> packet{static_cast<uint8_t>((sequence << 6) | size_code)};
        packet.insert(packet.end(), payload.begin(), payload.end());

        for(std::size_t i = 0; i < packet.size(); i += 2)
        {
            decoder.push(true, i == 0 ? 3 : 2, packet[i], packet[i + 1]);
        }
    }

    std::vector<uint8_t> concat(std::initializer_list<std::vector<uint8_t>> parts)
    {
        std::vector<uint8_t> out;
        for(const auto& p : parts)
            out.insert(out.end(), p.begin(), p.end());
        return out;
    }

    std::string text_of(dtvcc_decoder& decoder, int service = 1)
    {
        std::string text;
        for(const auto& s : decoder.drain())
            if(s.service_number == service) text += s.text;
        return text;
    }
} // namespace
//------------------------------------------------------------------------------

SCENARIO("KS X 1001 to Unicode conversion")
{
    REQUIRE(ksx1001_to_unicode(0xB0, 0xA1) == U'\uAC00');
    REQUIRE(ksx1001_to_unicode(0xC8, 0xFE) == U'\uD79D');
    REQUIRE(ksx1001_to_unicode(0xC7, 0xD1) == U'\uD55C');
    REQUIRE(ksx1001_to_unicode(0xA1, 0xA1) == 0x3000); // ideographic space
    REQUIRE(ksx1001_to_unicode(0xA2, 0xE8) == 0);      // unassigned
    REQUIRE(ksx1001_to_unicode(0x41, 0xA1) == 0);      // out of range

    std::string s;
    append_utf8(s, U'\uD55C');
    append_utf8(s, 'A');
    REQUIRE(s == utf8(U"\uD55CA"));
    pop_utf8(s);
    REQUIRE(s == utf8(U"\uD55C"));
    pop_utf8(s);
    REQUIRE(s.empty());
}

SCENARIO("CEA-708 DTVCC Korean caption decoding")
{
    GIVEN("a caption packet with EUC-KR Hangul")
    {
        dtvcc_decoder decoder;
        auto text = hangul_caption;
        text.push_back(0x0D); // CR
        text.insert(text.end(), {'O', 'K'});
        feed_packet(decoder, 0, service_block(1, text));

        THEN("the Hangul text is decoded as UTF-8")
        {
            REQUIRE(text_of(decoder) == hangul_caption_utf8 + "\nOK");
            REQUIRE(decoder.stats().packets == 1);
            REQUIRE(decoder.stats().korean_characters == 4);
            REQUIRE(decoder.stats().size_errors == 0);
        }
    }

    GIVEN("a Korean character split across two service blocks")
    {
        dtvcc_decoder decoder;
        feed_packet(decoder, 0, concat({service_block(1, {0xC7, 0xD1, 0xB1}), service_block(1, {0xDB})}));

        THEN("it is reassembled") { REQUIRE(text_of(decoder) == utf8(U"\uD55C\uAE00")); }
    }

    GIVEN("a character sent with the P16 command")
    {
        dtvcc_decoder decoder;
        feed_packet(decoder, 0, service_block(1, {0x18, 0xC7, 0xD1, 0x18, 0xAC, 0x00}));

        THEN("KS X 1001 and UCS-2 16-bit codes are decoded")
        {
            REQUIRE(text_of(decoder) == utf8(U"\uD55C\uAC00")); // U+AC00
        }
    }

    GIVEN("window commands with parameters")
    {
        dtvcc_decoder decoder;
        // DF0 (6 parameter bytes that look like text), SPA, SPL, text, CLW, text
        const std::vector<uint8_t> commands = {0x98, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x90, 0x41, 0x42,
                                               0x92, 0x01, 0x02, 0xC7, 0xD1, 0x88, 0x01, 0xB1, 0xDB};
        feed_packet(decoder, 0, service_block(1, commands));

        THEN("the parameters are not printed and windows delimit lines")
        {
            REQUIRE(text_of(decoder) == utf8(U"\uD55C\n\uAE00"));
        }
    }

    GIVEN("several services in one packet")
    {
        dtvcc_decoder decoder;
        feed_packet(decoder, 0, concat({service_block(1, hangul_caption), service_block(2, {'H', 'i'})}));
        const auto services = decoder.drain();

        THEN("each service gets its own text")
        {
            REQUIRE(services.size() == 2);
            REQUIRE(services[0].service_number == 1);
            REQUIRE(services[0].text == hangul_caption_utf8);
            REQUIRE(services[1].service_number == 2);
            REQUIRE(services[1].text == "Hi");
        }
    }

    GIVEN("an extended service number")
    {
        dtvcc_decoder decoder;
        feed_packet(decoder, 0, {static_cast<uint8_t>((7 << 5) | 2), 10, 'O', 'K'});

        THEN("the text goes to that service") { REQUIRE(text_of(decoder, 10) == "OK"); }
    }

    GIVEN("a packet spanning several CDPs")
    {
        dtvcc_decoder decoder;
        auto block = service_block(1, hangul_caption);
        block.push_back(0x00);
        const auto size_code = static_cast<uint8_t>((block.size() + 1) / 2);
        std::vector<uint8_t> packet{size_code};
        packet.insert(packet.end(), block.begin(), block.end());

        decoder.push(true, 3, packet[0], packet[1]);
        decoder.push(true, 2, packet[2], packet[3]);
        REQUIRE(text_of(decoder).empty()); // not complete yet
        decoder.push(false, 2, 0x00, 0x00); // invalid entries are ignored
        for(std::size_t i = 4; i < packet.size(); i += 2)
            decoder.push(true, 2, packet[i], packet[i + 1]);

        THEN("it is decoded once complete") { REQUIRE(text_of(decoder) == hangul_caption_utf8); }
    }

    GIVEN("non consecutive sequence numbers")
    {
        dtvcc_decoder decoder;
        feed_packet(decoder, 0, service_block(1, {'A'}));
        feed_packet(decoder, 1, service_block(1, {'B'}));
        feed_packet(decoder, 3, service_block(1, {'C'}));
        feed_packet(decoder, 0, service_block(1, {'D'}));

        THEN("one discontinuity is reported")
        {
            REQUIRE(text_of(decoder) == "ABCD");
            REQUIRE(decoder.stats().sequence_errors == 1);
        }
    }

    GIVEN("data before any packet start")
    {
        dtvcc_decoder decoder;
        decoder.push(true, 2, 0x21, 'A');
        THEN("it is ignored") { REQUIRE(decoder.stats().ignored_cc_entries == 1); }
    }
}

SCENARIO("CEA-708 character sets")
{
    GIVEN("Latin-1 text with an accent")
    {
        const std::vector<uint8_t> cafe = {'c', 'a', 'f', 0xE9, '!', 0x7F};

        WHEN("auto detection is used")
        {
            dtvcc_decoder decoder(korean_mode::auto_detect);
            feed_packet(decoder, 0, service_block(1, cafe));
            THEN("a lone G1 byte is Latin-1") { REQUIRE(text_of(decoder) == utf8(U"caf\u00E9!\u266A")); }
        }
    }

    GIVEN("a G1 byte pair")
    {
        WHEN("Korean decoding is disabled")
        {
            dtvcc_decoder decoder(korean_mode::never);
            feed_packet(decoder, 0, service_block(1, {0xC7, 0xD1}));
            THEN("it is two Latin-1 characters") { REQUIRE(text_of(decoder) == utf8(U"\u00C7\u00D1")); }
        }

        WHEN("Korean decoding is forced and the code is unassigned")
        {
            dtvcc_decoder decoder(korean_mode::always);
            feed_packet(decoder, 0, service_block(1, {0xA2, 0xE8, 'x'}));
            THEN("a replacement character is printed") { REQUIRE(text_of(decoder) == utf8(U"\uFFFDx")); }
        }
    }

    GIVEN("G2 characters and backspace")
    {
        dtvcc_decoder decoder;
        feed_packet(decoder, 0, service_block(1, {0x10, 0x39, 'a', 'b', 0x08, 0x10, 0x25}));
        THEN("they are decoded") { REQUIRE(text_of(decoder) == utf8(U"\u2122a\u2026")); }
    }

    GIVEN("a Korean lead byte at the very end of the stream")
    {
        dtvcc_decoder decoder;
        feed_packet(decoder, 0, service_block(1, {'a', 0xE9}));
        decoder.flush();
        THEN("it is printed as Latin-1") { REQUIRE(text_of(decoder) == utf8(U"a\u00E9")); }
    }
}
