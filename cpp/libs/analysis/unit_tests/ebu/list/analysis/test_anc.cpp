#include "catch2/catch.hpp"
#include "ebu/list/analysis/full_analysis.h"
#include "ebu/list/analysis/handlers/cea708/ksx1001.h"
#include "ebu/list/test_lib/sample_files.h"
#include "fs_handler_factory.h"
#include <fstream>
#include <iterator>
#include <map>

using namespace ebu_list;
using namespace ebu_list::media;
using namespace ebu_list::analysis;
using json = nlohmann::json;

//------------------------------------------------------------------------------
namespace
{
    template <typename InfoGetter>
    void run_analysis(const path& pcap_file, pcap_info& pcap, const path& storage_folder, InfoGetter get_stream_info)
    {
        fs_handler_factory handler_factory(storage_folder);
        fs_updater updater(storage_folder);
        analysis_profile profile;
        profile.timestamps.source = timestamps_source::pcap;
        auto callback             = [](float) {};

        constexpr auto is_srt = false;
        auto context          = processing_context{pcap_file,        profile,  storage_folder, pcap,  get_stream_info,
                                          &handler_factory, &updater, callback,       false, is_srt};

        run_full_analysis(context);
    }

    json load_json(path full_path)
    {
        std::ifstream ifs(full_path);
        LIST_ENFORCE(ifs.is_open() && !ifs.fail(), std::runtime_error, "{} not found", full_path.string());
        return json::parse(ifs);
    }

    json load_stream_json(path storage_folder)
    {
        const auto full_path = storage_folder / "stream.json";
        return load_json(full_path);
    }

    std::string read_text_file(const path& full_path)
    {
        std::ifstream ifs(full_path, std::ios::binary);
        LIST_ENFORCE(ifs.is_open(), std::runtime_error, "{} not found", full_path.string());
        return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }

    std::string utf8(const std::u32string& s)
    {
        std::string out;
        for(const auto cp : s)
            analysis::cea708::append_utf8(out, cp);
        return out;
    }
} // namespace
//------------------------------------------------------------------------------

SCENARIO("Ancillary stream analysis")
{
    std::vector<std::string> stream_ids;

    auto get_stream_info = [&stream_ids](const bool /*is_srt*/, const udp::datagram&
                                         /*first_packet*/) -> std::optional<stream_with_details> {
        auto stream_info      = serializable_stream_info{};
        stream_info.type      = media::media_type::ANCILLARY_DATA;
        stream_info.full_type = media::full_media_type::SMPTE291;
        auto details          = anc_stream_details{};
        details.anc.rate      = fraction64(60); // not happy with default 0 denominator
        details.anc.scan_type = video::scan_type::INTERLACED;

        stream_ids.push_back(stream_info.id);

        return stream_with_details{stream_info, details};
    };

    GIVEN("a pcap file a with error: 2 wrong bit markers, 2 wrong fields")
    {
        const auto pcap_file = test_lib::sample_file("pcap/st2110/2110-40/anc_with_wrong_2markers_and_2fields.pcap");
        const auto storage_folder = path("/tmp");
        auto pcap                 = pcap_info{};

        WHEN("the file is analysed")
        {
            run_analysis(pcap_file, pcap, storage_folder, get_stream_info);

            REQUIRE(stream_ids.size() == 1);
            const auto stream = load_stream_json(storage_folder / stream_ids[0]);
            REQUIRE(stream["id"] == stream_ids[0]);
            REQUIRE(stream["state"] == to_string(stream_state::ANALYZED));
            REQUIRE(stream["statistics"]["wrong_marker_count"] == 2);
            REQUIRE(stream["statistics"]["wrong_field_count"] == 4);
        }
    }

    GIVEN("a pcap file a with errors: 1 unknown DID/SDID, and 2 payload bytes")
    {
        const auto pcap_file      = test_lib::sample_file("pcap/st2110/2110-40/anc_with_wrong_DID_and_payload.pcap");
        const auto storage_folder = path("/tmp");
        auto pcap                 = pcap_info{};

        WHEN("the file is analysed")
        {
            run_analysis(pcap_file, pcap, storage_folder, get_stream_info);

            REQUIRE(stream_ids.size() == 1);
            const auto stream = load_stream_json(storage_folder / stream_ids[0]);
            REQUIRE(stream["id"] == stream_ids[0]);
            REQUIRE(stream["state"] == to_string(stream_state::ANALYZED));
            REQUIRE(stream["statistics"]["payload_error_count"] == 3);
        }
    }
    GIVEN("a pcap file with Korean (KS X 1001) CEA-708 captions")
    {
        // generated with scripts/make_korean_cc_test_pcap.py
        const auto pcap_file      = test_lib::sample_file("pcap/st2110/2110-40/anc_cea708_korean.pcap");
        const auto storage_folder = path("/tmp");
        auto pcap                 = pcap_info{};

        WHEN("the file is analysed")
        {
            run_analysis(pcap_file, pcap, storage_folder, get_stream_info);

            REQUIRE(stream_ids.size() == 1);
            const auto stream = load_stream_json(storage_folder / stream_ids[0]);
            REQUIRE(stream["state"] == to_string(stream_state::ANALYZED));
            REQUIRE(stream["statistics"]["payload_error_count"] == 0);

            std::map<std::string, std::string> files; // sub-sub-stream type -> file name
            for(const auto& sub_stream : stream["media_specific"]["sub_streams"])
            {
                if(sub_stream["did_sdid"] != 0x6101) continue;
                for(const auto& ss : sub_stream["sub_sub_streams"])
                    files[ss["type"].get<std::string>()] = ss["filename"].get<std::string>();
            }

            THEN("the caption text of service 1 is decoded as UTF-8")
            {
                REQUIRE(files.count("DTVCC Service 1") == 1);
                const auto text = read_text_file(storage_folder / stream_ids[0] / files["DTVCC Service 1"]);

                // Korean caption lines written by scripts/make_korean_cc_test_pcap.py (music note = G0 0x7F)
                const auto expected = utf8(U"\uC548\uB155\uD558\uC138\uC694. \uD55C\uAE00 \uC790\uB9C9 "
                                           U"\uD14C\uC2A4\uD2B8\uC785\uB2C8\uB2E4.\n"
                                           U"ST 2110-40 CEA-708 \uB514\uCF54\uB354\n"
                                           U"\uC790\uB9C9 \uBC29\uC1A1 \u266A\n");
                REQUIRE(text == expected);
            }

            THEN("the decoding statistics are reported")
            {
                REQUIRE(files.count("DTVCC Statistics") == 1);
                const auto stats = read_text_file(storage_folder / stream_ids[0] / files["DTVCC Statistics"]);
                REQUIRE(stats.find("caption channel packets = 3\n") != std::string::npos);
                REQUIRE(stats.find("sequence errors = 0\n") != std::string::npos);
                REQUIRE(stats.find("korean (KS X 1001) characters = 22\n") != std::string::npos);
            }
        }
    }
}
