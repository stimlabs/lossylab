#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/core/schema_version.hpp"
#include "lossylab/env/log.hpp"
#include "lossylab/io/read_headers.hpp"

#include <cassert>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace lossylab;

namespace
{
    std::string data_path(std::string_view name)
    {
        return std::string(LOSSYLAB_TEST_DATA_DIR) + "/" + std::string(name);
    }

    // Five frames each, encoded with testsrc at 64x48. The H.264 clip was
    // made by x264 with BT.709 tags; the others by x265, libvpx-vp9 and
    // FFmpeg's mpeg2video, tagged BT.709 matrix and limited range.
    const char* h264_fixture = "testsrc_64x48.mp4";
    const char* hevc_fixture = "testsrc_64x48_hevc.mp4";
    const char* vp9_fixture = "testsrc_64x48_vp9.webm";
    const char* mpeg2_fixture = "testsrc_64x48_mpeg2.m2v";

    HeaderInfo read_fixture(const char* fixture, const ReadHeadersOptions& options = {})
    {
        return read_headers(Source::from_path(data_path(fixture)), options);
    }

    const ParameterSet* find_parameter_set(const HeaderInfo& info, const std::string& kind)
    {
        for (const ParameterSet& parameter_set : info.parameter_sets)
        {
            if (parameter_set.kind == kind)
            {
                return &parameter_set;
            }
        }
        return nullptr;
    }

    void test_h264_parameter_sets_are_recovered()
    {
        const HeaderInfo info = read_fixture(h264_fixture);
        assert(info.codec_name == "h264");

        const ParameterSet* sps = find_parameter_set(info, "sps");
        assert(sps != nullptr);
        assert(sps->id == 0);
        assert(sps->fields.at("profile_idc") == 100);
        assert(sps->fields.at("pic_width_in_mbs_minus1") == 3);

        const ParameterSet* pps = find_parameter_set(info, "pps");
        assert(pps != nullptr);
        assert(pps->fields.at("pic_init_qp_minus26") == 2);
    }

    void test_h264_slices_carry_type_and_qp()
    {
        const HeaderInfo info = read_fixture(h264_fixture);
        assert(info.slices.size() == std::size_t{5});

        // Bitstream order: the second I-frame is coded before the B-frame
        // that precedes it on the timeline.
        assert(info.slices[0].slice_type == "I");
        for (const SliceInfo& slice : info.slices)
        {
            assert(slice.qp.has_value());
            assert(*slice.qp >= 0 && *slice.qp <= 51);
            assert(slice.size_bytes.has_value());
        }
        // pic_init_qp_minus26 = 2 and the second slice's slice_qp_delta = 5.
        assert(info.slices[1].qp == 33);
    }

    void test_x264_settings_are_extracted_and_parsed()
    {
        const HeaderInfo info = read_fixture(h264_fixture);
        assert(info.embedded_encoder_settings_availability == Availability::Present);
        assert(info.embedded_encoder_settings.has_value());
        assert(info.embedded_encoder_settings->starts_with("x264 - core"));
        assert(info.encoder_settings.at("cabac") == "1");
        assert(info.encoder_settings.count("crf") == std::size_t{1});
    }

    void test_h264_vui_color_is_reported()
    {
        const HeaderInfo info = read_fixture(h264_fixture);
        const ColorSpec color = ColorSpec::from_json(info.bitstream_color);
        assert(color.matrix == ColorMatrix::Bt709);
        assert(color.primaries == ColorPrimaries::Bt709);
        assert(color.transfer == TransferCharacteristic::Bt709);
        assert(color.range == ColorRange::Limited);
    }

    void test_hevc_headers_are_recovered()
    {
        const HeaderInfo info = read_fixture(hevc_fixture);
        assert(info.codec_name == "hevc");
        assert(find_parameter_set(info, "vps") != nullptr);
        assert(find_parameter_set(info, "sps") != nullptr);
        assert(find_parameter_set(info, "pps") != nullptr);

        assert(info.slices.size() == std::size_t{5});
        assert(info.slices[0].slice_type == "I");
        assert(info.slices[0].qp == 28);

        assert(info.embedded_encoder_settings.has_value());
        assert(info.embedded_encoder_settings->starts_with("x265"));
        assert(info.encoder_settings.count("no-wpp") == std::size_t{1});
        assert(info.encoder_settings.at("no-wpp").empty());

        const ColorSpec color = ColorSpec::from_json(info.bitstream_color);
        assert(color.matrix == ColorMatrix::Bt709);
        assert(color.range == ColorRange::Limited);
    }

    void test_vp9_quantizer_indices_are_recovered()
    {
        const HeaderInfo info = read_fixture(vp9_fixture);
        assert(info.codec_name == "vp9");
        assert(info.slices.empty());
        assert(info.quantizer_indices.size() == std::size_t{5});
        assert(info.quantizer_indices[0] == 37);
        for (const int index : info.quantizer_indices)
        {
            assert(index >= 0 && index <= 255);
        }

        // VP9 has no user-data mechanism for encoder settings.
        assert(info.embedded_encoder_settings_availability == Availability::NotPresent);

        const ColorSpec color = ColorSpec::from_json(info.bitstream_color);
        assert(color.matrix == ColorMatrix::Bt709);
        assert(color.range == ColorRange::Limited);
    }

    void test_mpeg2_slices_take_their_picture_type()
    {
        const HeaderInfo info = read_fixture(mpeg2_fixture);
        assert(info.codec_name == "mpeg2video");
        assert(find_parameter_set(info, "sequence_header") != nullptr);

        // Three macroblock rows, so three slices per picture.
        assert(info.slices.size() == std::size_t{30});
        assert(info.slices[0].slice_type == "I");
        assert(info.slices[2].slice_type == "I");
        assert(info.slices[0].qp.has_value());
        assert(!info.slices[0].size_bytes.has_value());

        const ColorSpec color = ColorSpec::from_json(info.bitstream_color);
        assert(color.matrix == ColorMatrix::Bt709);
    }

    void test_max_slices_stops_reading()
    {
        ReadHeadersOptions options;
        options.max_slices = 2;
        const HeaderInfo info = read_fixture(h264_fixture, options);
        assert(info.slices.size() == std::size_t{2});
    }

    void test_zero_max_slices_still_finds_parameter_sets_and_settings()
    {
        ReadHeadersOptions options;
        options.max_slices = 0;
        const HeaderInfo info = read_fixture(h264_fixture, options);
        assert(info.slices.empty());
        assert(find_parameter_set(info, "sps") != nullptr);
        assert(info.embedded_encoder_settings.has_value());
    }

    void test_repeated_parameter_sets_are_recorded_once()
    {
        // The MPEG-2 clip repeats its sequence header before every GOP.
        const HeaderInfo info = read_fixture(mpeg2_fixture);
        int sequence_headers = 0;
        for (const ParameterSet& parameter_set : info.parameter_sets)
        {
            if (parameter_set.kind == "sequence_header")
            {
                ++sequence_headers;
            }
        }
        assert(sequence_headers == 1);
    }

    void test_an_uninterpreted_codec_is_unsupported()
    {
        try
        {
            static_cast<void>(read_fixture("testsrc_64x48.png"));
            assert(false && "expected UnsupportedCapability");
        }
        catch (const UnsupportedCapability& error)
        {
            assert(error.name() == "png");
            assert(std::string(error.what()).starts_with("read_headers() does not interpret 'png'"));
        }
    }

    void test_a_missing_stream_is_a_config_error()
    {
        ReadHeadersOptions options;
        options.stream_index = 9;
        try
        {
            static_cast<void>(read_fixture(h264_fixture, options));
            assert(false && "expected ConfigError");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_trace_lines_never_reach_the_log_handler()
    {
        std::mutex mutex;
        std::vector<LogMessage> messages;
        set_log_handler(
            [&](const LogMessage& message)
            {
                const std::lock_guard<std::mutex> lock(mutex);
                messages.push_back(message);
            },
            LogLevel::Trace);

        static_cast<void>(read_fixture(h264_fixture));
        set_log_handler(nullptr);

        for (const LogMessage& message : messages)
        {
            assert(message.text.find("profile_idc") == std::string::npos);
            assert(message.text.find("Sequence Parameter Set") == std::string::npos);
        }
    }

    void test_reading_works_while_ffmpeg_is_muted()
    {
        mute_log();
        const HeaderInfo info = read_fixture(h264_fixture);
        set_log_handler(nullptr);
        assert(info.slices.size() == std::size_t{5});
    }

    void test_concurrent_reads_do_not_mix_their_traces()
    {
        const HeaderInfo h264_expected = read_fixture(h264_fixture);
        const HeaderInfo hevc_expected = read_fixture(hevc_fixture);

        std::vector<std::thread> threads;
        std::vector<HeaderInfo> results(8);
        for (std::size_t i = 0; i < results.size(); ++i)
        {
            threads.emplace_back([&results, i]
                                 { results[i] = read_fixture(i % 2 == 0 ? h264_fixture : hevc_fixture); });
        }
        for (std::thread& thread : threads)
        {
            thread.join();
        }

        for (std::size_t i = 0; i < results.size(); ++i)
        {
            const HeaderInfo& expected = i % 2 == 0 ? h264_expected : hevc_expected;
            assert(results[i].to_json() == expected.to_json());
        }
    }

    void test_header_info_carries_the_schema_version()
    {
        const HeaderInfo info = read_fixture(h264_fixture);
        assert(info.to_json().at("schema_version") == schema_version);
    }
}

int main()
{
    test_h264_parameter_sets_are_recovered();
    test_h264_slices_carry_type_and_qp();
    test_x264_settings_are_extracted_and_parsed();
    test_h264_vui_color_is_reported();
    test_hevc_headers_are_recovered();
    test_vp9_quantizer_indices_are_recovered();
    test_mpeg2_slices_take_their_picture_type();
    test_max_slices_stops_reading();
    test_zero_max_slices_still_finds_parameter_sets_and_settings();
    test_repeated_parameter_sets_are_recorded_once();
    test_an_uninterpreted_codec_is_unsupported();
    test_a_missing_stream_is_a_config_error();
    test_trace_lines_never_reach_the_log_handler();
    test_reading_works_while_ffmpeg_is_muted();
    test_concurrent_reads_do_not_mix_their_traces();
    test_header_info_carries_the_schema_version();
    return 0;
}
