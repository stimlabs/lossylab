#include "lossylab/core/error.hpp"
#include "lossylab/io/file_result.hpp"
#include "lossylab/io/probe.hpp"
#include "lossylab/resample/resize.hpp"

#include <cassert>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lossylab;

namespace
{
    std::string data_path(std::string_view name)
    {
        return std::string(LOSSYLAB_TEST_DATA_DIR) + "/" + std::string(name);
    }

    const char* png_fixture = "testsrc_64x48.png";
    const char* video_fixture = "testsrc_64x48.mp4";

    const Source in_memory_source = Source::from_bytes({1, 2, 3});

    template <typename Exception>
    FileError error_from_throwing(const Exception& exception)
    {
        const FileResult<int> result = capture("operation", in_memory_source,
                                               [&]() -> int { throw exception; });
        assert(!result.ok());
        return result.error();
    }

    void test_capture_returns_the_value_on_success()
    {
        const Source source = Source::from_path(data_path(png_fixture));
        const FileResult<ProbeResult> result = capture("probe", source, [&] { return probe(source); });
        assert(result.ok());
        assert(result.value().primary_video_stream() != nullptr);
    }

    void test_capture_attributes_the_error_to_its_operation_and_source()
    {
        const FileError error = error_from_throwing(ConfigError("bad request"));
        assert(error.operation == "operation");
        assert(error.source == in_memory_source.describe());
        assert(error.message == "bad request");
    }

    void test_garbage_bytes_become_an_ffmpeg_error()
    {
        const Source source = Source::from_bytes(std::vector<std::uint8_t>(256, 0x5a));
        const FileResult<ProbeResult> result = capture("probe", source, [&] { return probe(source); });
        assert(!result.ok());
        assert(result.error().kind == FileErrorKind::FFmpeg);
        assert(result.error().details.at("averror").get<int>() < 0);
        assert(!result.error().details.at("call").get<std::string>().empty());
    }

    void test_a_missing_file_becomes_an_ffmpeg_error()
    {
        const Source source = Source::from_path(data_path("does_not_exist.png"));
        const FileResult<ProbeResult> result = capture("probe", source, [&] { return probe(source); });
        assert(!result.ok());
        assert(result.error().kind == FileErrorKind::FFmpeg);
        assert(result.error().source == source.describe());
    }

    void test_an_unimplemented_operation_becomes_a_not_implemented_error()
    {
        const Source source = Source::from_path(data_path(video_fixture));
        const Frame frame =
            Frame::allocate(64, 48, PixelFormat::from_name("yuv420p"), ColorSpec::bt709_limited());
        const FileResult<ResizeResult> result = capture("resize", source, [&] { return resize(frame, 32, 24); });
        assert(!result.ok());
        assert(result.error().kind == FileErrorKind::NotImplemented);
        assert(!result.error().details.at("symbol").get<std::string>().empty());
    }

    void test_config_error_maps_to_config()
    {
        const FileError error = error_from_throwing(ConfigError("bad request"));
        assert(error.kind == FileErrorKind::Config);
        assert(error.details.empty());
    }

    void test_unsupported_capability_keeps_its_fields()
    {
        const FileError error = error_from_throwing(UnsupportedCapability("decoder", "libdav1d", "build-1"));
        assert(error.kind == FileErrorKind::UnsupportedCapability);
        assert(error.details.at("capability_kind") == "decoder");
        assert(error.details.at("name") == "libdav1d");
        assert(error.details.at("build_id") == "build-1");
    }

    void test_conversion_refused_keeps_its_fields()
    {
        const FileError error = error_from_throwing(ConversionRefused("rgb24", "yuv420p", "encode_video"));
        assert(error.kind == FileErrorKind::ConversionRefused);
        assert(error.details.at("from") == "rgb24");
        assert(error.details.at("to") == "yuv420p");
        assert(error.details.at("context") == "encode_video");
    }

    void test_ffmpeg_error_keeps_its_fields()
    {
        const FileError error = error_from_throwing(FFmpegError(-22, "avcodec_open2", "invalid argument"));
        assert(error.kind == FileErrorKind::FFmpeg);
        assert(error.details.at("averror") == -22);
        assert(error.details.at("call") == "avcodec_open2");
    }

    void test_not_implemented_keeps_its_symbol()
    {
        const FileError error = error_from_throwing(NotImplemented("resize"));
        assert(error.kind == FileErrorKind::NotImplemented);
        assert(error.details.at("symbol") == "resize");
    }

    void test_a_plain_library_error_maps_to_library()
    {
        const FileError error = error_from_throwing(Error("something"));
        assert(error.kind == FileErrorKind::Library);
    }

    void test_bad_alloc_maps_to_out_of_memory()
    {
        const FileError error = error_from_throwing(std::bad_alloc());
        assert(error.kind == FileErrorKind::OutOfMemory);
    }

    void test_a_standard_exception_maps_to_internal()
    {
        const FileError error = error_from_throwing(std::logic_error("broken invariant"));
        assert(error.kind == FileErrorKind::Internal);
        assert(error.message == "broken invariant");
    }

    void test_a_non_standard_exception_maps_to_internal()
    {
        const FileError error = error_from_throwing(42);
        assert(error.kind == FileErrorKind::Internal);
        assert(!error.message.empty());
    }

    void test_value_on_a_failed_result_throws_config_error()
    {
        const FileResult<int> result(error_from_throwing(NotImplemented("resize")));
        try
        {
            static_cast<void>(result.value());
            assert(false && "expected ConfigError");
        }
        catch (const ConfigError& error)
        {
            assert(std::string(error.what()).find("resize") != std::string::npos);
        }
    }

    void test_error_on_a_successful_result_throws_config_error()
    {
        const FileResult<int> result(7);
        try
        {
            static_cast<void>(result.error());
            assert(false && "expected ConfigError");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_file_error_json_round_trip()
    {
        const FileError error = error_from_throwing(FFmpegError(-22, "avcodec_open2", "invalid argument"));
        assert(FileError::from_json(error.to_json()) == error);
    }

    void test_file_error_kind_string_round_trip()
    {
        for (const FileErrorKind kind :
             {FileErrorKind::Config, FileErrorKind::UnsupportedCapability, FileErrorKind::ConversionRefused,
              FileErrorKind::FFmpeg, FileErrorKind::NotImplemented, FileErrorKind::Library,
              FileErrorKind::OutOfMemory, FileErrorKind::Internal})
        {
            assert(file_error_kind_from_string(to_string(kind)) == kind);
        }
    }

    void test_file_result_json_marks_success()
    {
        const Source source = Source::from_path(data_path(png_fixture));
        const FileResult<ProbeResult> result = capture("probe", source, [&] { return probe(source); });
        const json::Value value = result.to_json();
        assert(value.at("ok") == true);
        assert(value.at("value") == result.value().to_json());
    }

    void test_file_result_json_marks_failure()
    {
        const FileResult<ProbeResult> result(error_from_throwing(NotImplemented("resize")));
        const json::Value value = result.to_json();
        assert(value.at("ok") == false);
        assert(value.at("error") == result.error().to_json());
    }
}

int main()
{
    test_capture_returns_the_value_on_success();
    test_capture_attributes_the_error_to_its_operation_and_source();
    test_garbage_bytes_become_an_ffmpeg_error();
    test_a_missing_file_becomes_an_ffmpeg_error();
    test_an_unimplemented_operation_becomes_a_not_implemented_error();
    test_config_error_maps_to_config();
    test_unsupported_capability_keeps_its_fields();
    test_conversion_refused_keeps_its_fields();
    test_ffmpeg_error_keeps_its_fields();
    test_not_implemented_keeps_its_symbol();
    test_a_plain_library_error_maps_to_library();
    test_bad_alloc_maps_to_out_of_memory();
    test_a_standard_exception_maps_to_internal();
    test_a_non_standard_exception_maps_to_internal();
    test_value_on_a_failed_result_throws_config_error();
    test_error_on_a_successful_result_throws_config_error();
    test_file_error_json_round_trip();
    test_file_error_kind_string_round_trip();
    test_file_result_json_marks_success();
    test_file_result_json_marks_failure();
    return 0;
}
