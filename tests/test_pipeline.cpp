#include "lossylab/core/error.hpp"
#include "lossylab/io/decode_image.hpp"
#include "lossylab/pipeline/pipeline.hpp"

#include <cassert>
#include <string>

using namespace lossylab;

namespace
{
    std::string data_path(const std::string_view name)
    {
        return std::string(LOSSYLAB_TEST_DATA_DIR) + "/" + std::string(name);
    }

    ConvertOptions to_srgb24()
    {
        ConvertOptions options;
        options.pixel_format = PixelFormat::from_name("rgb24");
        options.color = ColorSpec::srgb();
        return options;
    }

    /// The equalization chain: decode to sRGB, crop on the JPEG grid, orient,
    /// achromatic, then a JPEG at qscale 2 decoded back the same way.
    PipelineSpec equalization()
    {
        DecodeImageOptions decode;
        decode.conversion = to_srgb24();

        RoundtripImageConfiguration jpeg;
        jpeg.encode.codec = ImageCodec::Mjpeg;
        jpeg.encode.pixel_format = PixelFormat::from_name("yuvj420p");
        jpeg.encode.color = ColorSpec::jpeg();
        jpeg.encode.color->primaries = ColorPrimaries::Bt709;
        jpeg.encode.color->transfer = TransferCharacteristic::Srgb;
        jpeg.encode.rate_control = RateControl::quality(2);
        jpeg.encode.strict = Strict::AllowRecorded;
        jpeg.decode.conversion = to_srgb24();

        PipelineSpec spec;
        spec.add(decode)
            .add(CropOptions{16, 16, 32, 16})
            .add(OrientOptions{6, Strict::AllowRecorded})
            .add(AchromaticOptions{})
            .add(jpeg);
        return spec;
    }

    template <typename Exception, typename Function>
    void expect_throw(Function&& function)
    {
        try
        {
            function();
            assert(false && "expected throw");
        }
        catch (const Exception&)
        {
        }
    }

    void test_the_equalization_chain_runs_from_a_file()
    {
        const Source source = Source::from_path(data_path("testsrc_64x48_q75.jpg"));
        const PipelineResult result = Pipeline(equalization()).run(source, 7);

        // The crop is 32x16; orientation 6 turns it to 16x32.
        assert(result.frame.width() == 16 && result.frame.height() == 32);
        assert(result.frame.pixel_format() == PixelFormat::from_name("rgb24"));
        assert(result.frame.color() == ColorSpec::srgb());

        const ProcessingRecord& record = result.record;
        assert(record.size() == 5);
        assert(record.stages()[0].kind() == StageKind::DecodeImage);
        assert(record.stages()[4].kind() == StageKind::RoundtripImage);
        assert(record.origin().has_value());
        assert(record.seed() == std::optional<std::uint64_t>(7));
        assert(record.output_sha256() == std::optional<std::string>(result.frame.samples_sha256()));

        const CropEvidence& crop = std::get<CropEvidence>(record.stages()[1].evidence);
        assert(crop.block_grid->kind == BlockGridKind::JpegMcu);
        assert(crop.block_grid->phase_x == 0 && crop.block_grid->phase_y == 0 && crop.whole_blocks == true);

        // The new JPEG's grid is the one left at the end.
        assert(record.effective_block_grid()->kind == BlockGridKind::Dct8);
    }

    void test_a_run_and_its_replays_give_the_same_pixels()
    {
        const Source source = Source::from_path(data_path("testsrc_64x48_q75.jpg"));
        const PipelineResult first = Pipeline(equalization()).run(source);
        const PipelineResult second = Pipeline(equalization()).run(source);
        assert(first.record.output_sha256() == second.record.output_sha256());

        // From the record, and from the record after a trip through JSON.
        const PipelineSpec replay = PipelineSpec::from_record(first.record);
        assert(replay.source_sha256() == std::optional<std::string>(source.sha256()));
        assert(Pipeline(replay).run(source).record.output_sha256() == first.record.output_sha256());

        const ProcessingRecord stored = ProcessingRecord::from_json(json::parse(first.record.to_json().dump()));
        assert(stored.output_sha256() == first.record.output_sha256());
        assert(Pipeline(PipelineSpec::from_record(stored)).run(source).record.output_sha256() ==
               first.record.output_sha256());
    }

    void test_a_spec_refuses_another_file_and_the_wrong_start()
    {
        PipelineSpec spec = equalization();
        spec.set_source_sha256(Source::from_path(data_path("testsrc_64x48.jpg")).sha256());
        expect_throw<ConfigError>(
            [&] { (void)Pipeline(spec).run(Source::from_path(data_path("testsrc_64x48_q75.jpg"))); });

        const Frame frame = decode_image(Source::from_path(data_path("testsrc_64x48.png"))).frame;
        expect_throw<ConfigError>([&] { (void)Pipeline(equalization()).run(frame); });

        PipelineSpec without_decode;
        without_decode.add(AchromaticOptions{});
        expect_throw<ConfigError>(
            [&] { (void)Pipeline(without_decode).run(Source::from_path(data_path("testsrc_64x48.png"))); });
        expect_throw<ConfigError>([&] { (void)Pipeline(without_decode).run(Frame()); });
    }

    void test_a_frame_runs_without_a_decode()
    {
        const Frame frame = decode_image(Source::from_path(data_path("testsrc_64x48.png"))).frame;
        PipelineSpec spec;
        spec.add(CropOptions{0, 0, 8, 8}).add(AchromaticOptions{});
        const PipelineResult result = Pipeline(spec).run(frame);
        assert(result.frame.width() == 8);
        assert(result.record.size() == 2);
        assert(!result.record.origin().has_value());
        assert(result.record.output_sha256().has_value());
    }
}

int main()
{
    test_the_equalization_chain_runs_from_a_file();
    test_a_run_and_its_replays_give_the_same_pixels();
    test_a_spec_refuses_another_file_and_the_wrong_start();
    test_a_frame_runs_without_a_decode();
}
