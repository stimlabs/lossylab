#include "lossylab/core/error.hpp"
#include "lossylab/io/video_reader.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace lossylab;

namespace
{
    std::string data_path(std::string_view name)
    {
        return std::string(LOSSYLAB_TEST_DATA_DIR) + "/" + std::string(name);
    }

    std::vector<std::uint8_t> read_file(const std::string& path)
    {
        std::FILE* file = std::fopen(path.c_str(), "rb");
        assert(file != nullptr && "could not open fixture");
        std::vector<std::uint8_t> bytes;
        std::uint8_t buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        {
            bytes.insert(bytes.end(), buffer, buffer + read);
        }
        std::fclose(file);
        return bytes;
    }

    // Five H.264 frames at 5 fps, so frame n sits at n * 0.2 s. Picture types
    // in presentation order are I B P I P.
    const char* video_fixture = "testsrc_64x48.mp4";

    VideoReader fixture_reader(const VideoReaderOptions& options = {})
    {
        return VideoReader(Source::from_path(data_path(video_fixture)), options);
    }

    std::vector<int> indices_of(const std::vector<VideoFrame>& frames)
    {
        std::vector<int> indices;
        for (const VideoFrame& video_frame : frames)
        {
            indices.push_back(video_frame.index);
        }
        return indices;
    }

    void test_all_reads_every_frame_in_presentation_order()
    {
        VideoReader reader = fixture_reader();
        const std::vector<VideoFrame> frames = reader.frames(FrameSelector::all());

        assert(indices_of(frames) == (std::vector<int>{0, 1, 2, 3, 4}));
        const std::vector<PictureType> expected_types{PictureType::I, PictureType::B, PictureType::P,
                                                      PictureType::I, PictureType::P};
        for (std::size_t i = 0; i < frames.size(); ++i)
        {
            assert(frames[i].stats.picture_type == expected_types[i]);
            assert(frames[i].frame.width() == 64);
            assert(frames[i].frame.height() == 48);
        }
        assert(frames[0].stats.key_frame);
    }

    void test_frames_carry_their_timestamps()
    {
        VideoReader reader = fixture_reader();
        const std::vector<VideoFrame> frames = reader.frames(FrameSelector::all());
        for (const VideoFrame& video_frame : frames)
        {
            const std::optional<double> seconds = video_frame.frame.timestamp_seconds();
            assert(seconds.has_value());
            assert(*seconds > video_frame.index * 0.2 - 1e-9);
            assert(*seconds < video_frame.index * 0.2 + 1e-9);
        }
    }

    void test_the_record_describes_the_read()
    {
        VideoReader reader = fixture_reader();
        const FrameSelector select = FrameSelector::stride(2);
        static_cast<void>(reader.frames(select));

        const StageRecord& record = reader.record();
        assert(record.kind == StageKind::Decode);
        assert(record.implementation == "h264");
        assert(record.transform.is_identity());
        assert(record.frames.size() == std::size_t{3});
        assert(record.params.at("selector") == select.to_json());
        assert(record.params.at("frames_selected") == 3);
        assert(record.input == record.output);
        assert(record.input.pixel_format == PixelFormat::from_name("yuv420p"));
    }

    void test_indices_select_those_frames_and_stop_reading_after_the_last()
    {
        VideoReader reader = fixture_reader();
        assert(indices_of(reader.frames(FrameSelector::indices({3, 1}))) == (std::vector<int>{1, 3}));
        assert(reader.record().params.at("frames_decoded") == 4);
    }

    void test_stride_selects_every_nth_frame_from_its_offset()
    {
        VideoReader reader = fixture_reader();
        assert(indices_of(reader.frames(FrameSelector::stride(2))) == (std::vector<int>{0, 2, 4}));
        assert(indices_of(reader.frames(FrameSelector::stride(2, 1))) == (std::vector<int>{1, 3}));
    }

    void test_timestamps_select_the_first_frame_at_or_after_each_time()
    {
        VideoReader reader = fixture_reader();
        const std::vector<VideoFrame> frames = reader.frames(FrameSelector::timestamps({0.8, 0.3, 0.35}));
        assert(indices_of(frames) == (std::vector<int>{2, 4}));
    }

    void test_picture_types_select_only_those_types()
    {
        VideoReader reader = fixture_reader();
        assert(indices_of(reader.frames(FrameSelector::picture_types({PictureType::I}))) ==
               (std::vector<int>{0, 3}));
    }

    void test_evenly_spaced_spreads_over_the_clip()
    {
        VideoReader reader = fixture_reader();
        assert(indices_of(reader.frames(FrameSelector::evenly_spaced(2))) == (std::vector<int>{1, 3}));
        assert(indices_of(reader.frames(FrameSelector::evenly_spaced(10))) == (std::vector<int>{0, 1, 2, 3, 4}));
    }

    void test_a_predicate_sees_the_native_frame()
    {
        VideoReaderOptions options;
        options.pixel_format = PixelFormat::from_name("rgb24");
        options.color = ColorSpec::srgb();
        VideoReader reader = fixture_reader(options);

        const std::vector<VideoFrame> frames = reader.frames(FrameSelector::where(
            [](const VideoFrame& candidate)
            {
                assert(candidate.frame.pixel_format() == PixelFormat::from_name("yuv420p"));
                return candidate.index % 2 == 1;
            }));
        assert(indices_of(frames) == (std::vector<int>{1, 3}));
        assert(frames[0].frame.pixel_format() == PixelFormat::from_name("rgb24"));
    }

    void test_a_timestamp_in_a_conjunction_waits_for_the_other_side()
    {
        VideoReader reader = fixture_reader();
        const FrameSelector select =
            FrameSelector::picture_types({PictureType::I}).and_also(FrameSelector::timestamps({0.1}));
        assert(indices_of(reader.frames(select)) == (std::vector<int>{3}));
    }

    void test_a_selection_that_matches_nothing_describes_the_stream()
    {
        VideoReader reader = fixture_reader();
        assert(reader.frames(FrameSelector::stride(1, 99)).empty());
        assert(reader.record().params.at("frames_decoded") == 5);
        assert(reader.record().input.width == 64);
        assert(reader.record().frames.empty());
    }

    void test_for_each_stops_when_the_callback_returns_false()
    {
        VideoReader reader = fixture_reader();
        int calls = 0;
        reader.for_each(FrameSelector::all(),
                        [&](const VideoFrame&)
                        {
                            ++calls;
                            return false;
                        });
        assert(calls == 1);
        assert(reader.record().frames.size() == std::size_t{1});
    }

    void test_repeated_reads_start_from_the_beginning()
    {
        VideoReader reader = fixture_reader();
        const std::vector<VideoFrame> first = reader.frames(FrameSelector::all());
        const std::vector<VideoFrame> second = reader.frames(FrameSelector::all());
        assert(indices_of(first) == indices_of(second));

        const ConstPlaneView first_luma = first[2].frame.plane(0);
        const ConstPlaneView second_luma = second[2].frame.plane(0);
        for (int row = 0; row < first_luma.height; ++row)
        {
            for (int column = 0; column < first_luma.width; ++column)
            {
                assert(first_luma.row(row)[column] == second_luma.row(row)[column]);
            }
        }
    }

    void test_qp_maps_are_exported_on_request()
    {
        VideoReaderOptions options;
        options.export_qp_maps = true;
        VideoReader reader = fixture_reader(options);

        for (const VideoFrame& video_frame : reader.frames(FrameSelector::all()))
        {
            assert(video_frame.qp_map_availability == Availability::Present);
            assert(video_frame.qp_map.has_value());
            assert(video_frame.qp_map->width == 4);
            assert(video_frame.qp_map->height == 3);
            assert(video_frame.stats.qp_mean.has_value());
            assert(*video_frame.stats.qp_min <= *video_frame.stats.qp_mean);
            assert(*video_frame.stats.qp_mean <= *video_frame.stats.qp_max);
        }
        assert(reader.record().frames[0].qp_mean.has_value());
    }

    void test_qp_maps_are_absent_unless_requested()
    {
        VideoReader reader = fixture_reader();
        const std::vector<VideoFrame> frames = reader.frames(FrameSelector::all());
        assert(!frames[0].qp_map.has_value());
        assert(frames[0].qp_map_availability == Availability::NotPresent);
        assert(!frames[0].stats.qp_mean.has_value());
    }

    void test_motion_vectors_are_exported_for_predicted_frames()
    {
        VideoReaderOptions options;
        options.export_motion_vectors = true;
        VideoReader reader = fixture_reader(options);

        for (const VideoFrame& video_frame : reader.frames(FrameSelector::all()))
        {
            if (video_frame.stats.picture_type == PictureType::I)
            {
                // Intra frames carry no motion, and an H.264 decoder that can
                // export vectors says so rather than claiming it cannot.
                assert(video_frame.motion_vector_availability == Availability::NotPresent);
                assert(video_frame.motion_vectors.empty());
            }
            else
            {
                assert(video_frame.motion_vector_availability == Availability::Present);
                assert(!video_frame.motion_vectors.empty());
            }
        }
    }

    void test_a_requested_conversion_is_applied_and_recorded()
    {
        VideoReaderOptions options;
        options.pixel_format = PixelFormat::from_name("rgb24");
        options.color = ColorSpec::srgb();
        VideoReader reader = fixture_reader(options);

        const std::vector<VideoFrame> frames = reader.frames(FrameSelector::indices({0}));
        assert(frames.size() == std::size_t{1});
        assert(frames[0].frame.pixel_format() == PixelFormat::from_name("rgb24"));

        const StageRecord& record = reader.record();
        assert(record.input.pixel_format == PixelFormat::from_name("yuv420p"));
        assert(record.output.pixel_format == PixelFormat::from_name("rgb24"));
        assert(!record.conversions.empty());
    }

    void test_reading_from_memory_matches_reading_from_a_path()
    {
        VideoReader from_path = fixture_reader();
        VideoReader from_memory(Source::from_bytes(read_file(data_path(video_fixture))));
        assert(indices_of(from_path.frames(FrameSelector::all())) ==
               indices_of(from_memory.frames(FrameSelector::all())));
    }

    void test_an_explicit_stream_index_is_checked()
    {
        VideoReaderOptions options;
        options.stream_index = 0;
        VideoReader reader = fixture_reader(options);
        assert(reader.stream().index == 0);

        options.stream_index = 7;
        try
        {
            static_cast<void>(fixture_reader(options));
            assert(false && "expected ConfigError");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_a_still_image_reads_as_a_one_frame_clip()
    {
        VideoReader reader(Source::from_path(data_path("testsrc_64x48.png")));
        assert(reader.frames(FrameSelector::all()).size() == std::size_t{1});
    }

    void test_for_each_rejects_an_empty_callback()
    {
        VideoReader reader = fixture_reader();
        try
        {
            reader.for_each(FrameSelector::all(), nullptr);
            assert(false && "expected ConfigError");
        }
        catch (const ConfigError&)
        {
        }
    }
}

int main()
{
    test_all_reads_every_frame_in_presentation_order();
    test_frames_carry_their_timestamps();
    test_the_record_describes_the_read();
    test_indices_select_those_frames_and_stop_reading_after_the_last();
    test_stride_selects_every_nth_frame_from_its_offset();
    test_timestamps_select_the_first_frame_at_or_after_each_time();
    test_picture_types_select_only_those_types();
    test_evenly_spaced_spreads_over_the_clip();
    test_a_predicate_sees_the_native_frame();
    test_a_timestamp_in_a_conjunction_waits_for_the_other_side();
    test_a_selection_that_matches_nothing_describes_the_stream();
    test_for_each_stops_when_the_callback_returns_false();
    test_repeated_reads_start_from_the_beginning();
    test_qp_maps_are_exported_on_request();
    test_qp_maps_are_absent_unless_requested();
    test_motion_vectors_are_exported_for_predicted_frames();
    test_a_requested_conversion_is_applied_and_recorded();
    test_reading_from_memory_matches_reading_from_a_path();
    test_an_explicit_stream_index_is_checked();
    test_a_still_image_reads_as_a_one_frame_clip();
    test_for_each_rejects_an_empty_callback();
    return 0;
}
