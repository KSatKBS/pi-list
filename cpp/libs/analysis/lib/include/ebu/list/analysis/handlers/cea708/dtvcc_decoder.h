#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// CEA-708 (DTVCC) closed caption decoder with Korean support.
//
// Input : the cc_data triplets of a CDP (SMPTE 334-2, DID 0x61 / SDID 0x01),
//         as carried by ST 2110-40.
// Output: the caption text of each caption service, as UTF-8.
//
// Korean captions (TTA TTAK.KO-07.0093) encode Hangul/Hanja as KS X 1001
// (EUC-KR) two-byte codes placed in the G1 code space (0xA1..0xFE pairs).
// The P16 command (EXT1 0x18 hi lo) is also accepted for 16-bit characters.
//
// Window positioning, pen attributes and timing are not rendered: the decoder
// produces a text transcript where window boundaries / carriage returns become
// line breaks.

namespace ebu_list::analysis::cea708
{
    enum class korean_mode
    {
        auto_detect, // a G1 byte pair which is a valid KS X 1001 code is decoded as Korean, otherwise Latin-1
        always,      // G1 byte pairs are always decoded as KS X 1001
        never        // plain CEA-708: G1 is ISO 8859-1 (Latin-1)
    };

    struct service_text
    {
        int service_number;
        std::string text; // UTF-8
    };

    struct decoder_stats
    {
        uint32_t packets            = 0; // complete caption channel packets decoded
        uint32_t sequence_errors    = 0; // discontinuities in the 2-bit packet sequence number
        uint32_t size_errors        = 0; // packets shorter than announced / service blocks overflowing the packet
        uint32_t korean_characters  = 0; // KS X 1001 / P16 characters decoded
        uint32_t ignored_cc_entries = 0; // DTVCC entries dropped because no packet start was seen
    };

    class dtvcc_decoder
    {
      public:
        explicit dtvcc_decoder(korean_mode mode = korean_mode::auto_detect);

        // Feeds one cc_data entry. Entries with cc_type 0/1 (CEA-608) or cc_valid == 0 are ignored.
        void push(bool cc_valid, uint8_t cc_type, uint8_t cc_data_1, uint8_t cc_data_2);

        // Forces decoding of a pending (incomplete) packet, e.g. at the end of the stream.
        void flush();

        // Returns and clears the text produced since the last call, one entry per service, ordered by service.
        std::vector<service_text> drain();

        [[nodiscard]] const decoder_stats& stats() const noexcept { return stats_; }

        // Decodes one complete caption channel packet (header byte included). Exposed for testing.
        void decode_packet(const uint8_t* data, std::size_t size);

      private:
        struct service_state
        {
            std::string pending;      // text not yet drained
            bool at_line_start = true; // nothing printed on the current output line
            int held_lead      = -1;   // KS X 1001 lead byte waiting for its trail byte (split service blocks)
        };

        void decode_service_block(service_state& svc, const uint8_t* data, std::size_t size);
        void put_char(service_state& svc, char32_t cp);
        void new_line(service_state& svc);
        void clear_line(service_state& svc);

        korean_mode mode_;
        decoder_stats stats_;
        std::vector<uint8_t> packet_;
        std::size_t expected_size_ = 0;
        int last_sequence_         = -1;
        std::map<int, service_state> services_;
    };
} // namespace ebu_list::analysis::cea708
