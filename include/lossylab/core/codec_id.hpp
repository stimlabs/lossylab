#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace lossylab
{
    /// Still-image codecs the library can encode to or decode from.
    ///
    /// The enum names the complete surface the design calls for, independent of
    /// what any particular FFmpeg build provides. Whether a given member is
    /// usable is a runtime question answered by `capabilities()`, not a
    /// compile-time one.
    enum class ImageCodec
    {
        Png,
        Mjpeg,  ///< FFmpeg's JPEG implementation; a second encoder alongside PIL
        WebP,
        Avif,
        Jxl,
        Heif,
        Jpeg2000  ///< FFmpeg's native JPEG 2000 encoder and decoder
    };

    /// Video codecs for encode and decode.
    enum class VideoCodec
    {
        H264,
        Hevc,
        Vp9,
        Av1
    };

    /// Which implementation carries out an encode. Hardware encoders are
    /// recorded as non-reproducible.
    enum class EncoderBackend
    {
        Software,
        Vaapi,
        Nvenc,
        Qsv,
        VideoToolbox
    };

    /// Scaler implementation behind `resize`. They differ in kernel behavior
    /// and in how they treat color, so the choice is explicit and recorded.
    enum class ResizeBackend
    {
        Swscale,
        Zscale
    };

    /// Full-reference quality metrics.
    enum class Metric
    {
        Psnr,
        Ssim,
        Vmaf
    };

    std::string to_string(ImageCodec codec);
    std::string to_string(VideoCodec codec);
    std::string to_string(EncoderBackend backend);
    std::string to_string(ResizeBackend backend);
    std::string to_string(Metric metric);

    ImageCodec image_codec_from_string(std::string_view name);
    VideoCodec video_codec_from_string(std::string_view name);
    EncoderBackend encoder_backend_from_string(std::string_view name);
    ResizeBackend resize_backend_from_string(std::string_view name);
    Metric metric_from_string(std::string_view name);

    [[nodiscard]] std::vector<ImageCodec> all_image_codecs();
    [[nodiscard]] std::vector<VideoCodec> all_video_codecs();

    /// The FFmpeg encoder names that can serve a codec, best first.
    ///
    /// Several encoders may implement one codec (libaom-av1, libsvtav1 and
    /// librav1e all produce AV1), so selection is a lookup against the build
    /// rather than a fixed name. `capabilities()` walks this list and picks the
    /// first that is present.
    [[nodiscard]] std::vector<std::string> encoder_candidates(ImageCodec codec);
    [[nodiscard]] std::vector<std::string> encoder_candidates(VideoCodec codec,
                                                              EncoderBackend backend);

    /// The FFmpeg decoder names that can serve a codec, best first.
    [[nodiscard]] std::vector<std::string> decoder_candidates(ImageCodec codec);
    [[nodiscard]] std::vector<std::string> decoder_candidates(VideoCodec codec);

    /// The name of the libavfilter scaler a backend maps to.
    [[nodiscard]] std::string filter_name(ResizeBackend backend);
}
