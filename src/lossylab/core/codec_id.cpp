#include "lossylab/core/codec_id.hpp"

#include "lossylab/core/error.hpp"

namespace lossylab
{
    std::string to_string(const ImageCodec codec)
    {
        switch (codec)
        {
        case ImageCodec::Png: return "png";
        case ImageCodec::Mjpeg: return "mjpeg";
        case ImageCodec::WebP: return "webp";
        case ImageCodec::Avif: return "avif";
        case ImageCodec::Jxl: return "jxl";
        case ImageCodec::Heif: return "heif";
        }
        return "unknown";
    }

    std::string to_string(const VideoCodec codec)
    {
        switch (codec)
        {
        case VideoCodec::H264: return "h264";
        case VideoCodec::Hevc: return "hevc";
        case VideoCodec::Vp9: return "vp9";
        case VideoCodec::Av1: return "av1";
        }
        return "unknown";
    }

    std::string to_string(const EncoderBackend backend)
    {
        switch (backend)
        {
        case EncoderBackend::Software: return "software";
        case EncoderBackend::Vaapi: return "vaapi";
        case EncoderBackend::Nvenc: return "nvenc";
        case EncoderBackend::Qsv: return "qsv";
        case EncoderBackend::VideoToolbox: return "videotoolbox";
        }
        return "unknown";
    }

    std::string to_string(const ResizeBackend backend)
    {
        return backend == ResizeBackend::Swscale ? "swscale" : "zscale";
    }

    std::string to_string(const Metric metric)
    {
        switch (metric)
        {
        case Metric::Psnr: return "psnr";
        case Metric::Ssim: return "ssim";
        case Metric::Vmaf: return "vmaf";
        }
        return "unknown";
    }

    ImageCodec image_codec_from_string(const std::string_view name)
    {
        if (name == "png") { return ImageCodec::Png; }
        if (name == "mjpeg" || name == "jpeg") { return ImageCodec::Mjpeg; }
        if (name == "webp") { return ImageCodec::WebP; }
        if (name == "avif") { return ImageCodec::Avif; }
        if (name == "jxl" || name == "jpegxl") { return ImageCodec::Jxl; }
        if (name == "heif" || name == "heic") { return ImageCodec::Heif; }
        throw ConfigError("unknown image codec '" + std::string(name) + "'");
    }

    VideoCodec video_codec_from_string(const std::string_view name)
    {
        if (name == "h264" || name == "avc") { return VideoCodec::H264; }
        if (name == "hevc" || name == "h265") { return VideoCodec::Hevc; }
        if (name == "vp9") { return VideoCodec::Vp9; }
        if (name == "av1") { return VideoCodec::Av1; }
        throw ConfigError("unknown video codec '" + std::string(name) + "'");
    }

    EncoderBackend encoder_backend_from_string(const std::string_view name)
    {
        if (name == "software") { return EncoderBackend::Software; }
        if (name == "vaapi") { return EncoderBackend::Vaapi; }
        if (name == "nvenc") { return EncoderBackend::Nvenc; }
        if (name == "qsv") { return EncoderBackend::Qsv; }
        if (name == "videotoolbox") { return EncoderBackend::VideoToolbox; }
        throw ConfigError("unknown encoder backend '" + std::string(name) + "'");
    }

    ResizeBackend resize_backend_from_string(const std::string_view name)
    {
        if (name == "swscale") { return ResizeBackend::Swscale; }
        if (name == "zscale") { return ResizeBackend::Zscale; }
        throw ConfigError("unknown resize backend '" + std::string(name) + "'");
    }

    Metric metric_from_string(const std::string_view name)
    {
        if (name == "psnr") { return Metric::Psnr; }
        if (name == "ssim") { return Metric::Ssim; }
        if (name == "vmaf") { return Metric::Vmaf; }
        throw ConfigError("unknown metric '" + std::string(name) + "'");
    }

    std::vector<ImageCodec> all_image_codecs()
    {
        return {ImageCodec::Png, ImageCodec::Mjpeg, ImageCodec::WebP,
                ImageCodec::Avif, ImageCodec::Jxl, ImageCodec::Heif};
    }

    std::vector<VideoCodec> all_video_codecs()
    {
        return {VideoCodec::H264, VideoCodec::Hevc, VideoCodec::Vp9, VideoCodec::Av1};
    }

    std::vector<std::string> encoder_candidates(const ImageCodec codec)
    {
        switch (codec)
        {
        case ImageCodec::Png: return {"png"};
        case ImageCodec::Mjpeg: return {"mjpeg"};
        case ImageCodec::WebP: return {"libwebp", "libwebp_anim"};
        // libaom first: it is the encoder libavif uses, and the only one that
        // takes every chroma subsampling.
        case ImageCodec::Avif: return {"libaom-av1", "libsvtav1", "librav1e"};
        case ImageCodec::Jxl: return {"libjxl"};

        // FFmpeg has no HEIF muxer, so no encoder can produce a HEIF file.
        case ImageCodec::Heif: return {};
        }
        return {};
    }

    std::vector<std::string> encoder_candidates(const VideoCodec codec,
                                                const EncoderBackend backend)
    {
        switch (backend)
        {
        case EncoderBackend::Software:
            switch (codec)
            {
            case VideoCodec::H264: return {"libx264", "libopenh264"};
            case VideoCodec::Hevc: return {"libx265"};
            case VideoCodec::Vp9: return {"libvpx-vp9"};
            case VideoCodec::Av1: return {"libsvtav1", "libaom-av1", "librav1e"};
            }
            return {};

        case EncoderBackend::Vaapi:
            switch (codec)
            {
            case VideoCodec::H264: return {"h264_vaapi"};
            case VideoCodec::Hevc: return {"hevc_vaapi"};
            case VideoCodec::Vp9: return {"vp9_vaapi"};
            case VideoCodec::Av1: return {"av1_vaapi"};
            }
            return {};

        case EncoderBackend::Nvenc:
            switch (codec)
            {
            case VideoCodec::H264: return {"h264_nvenc"};
            case VideoCodec::Hevc: return {"hevc_nvenc"};
            case VideoCodec::Vp9: return {};
            case VideoCodec::Av1: return {"av1_nvenc"};
            }
            return {};

        case EncoderBackend::Qsv:
            switch (codec)
            {
            case VideoCodec::H264: return {"h264_qsv"};
            case VideoCodec::Hevc: return {"hevc_qsv"};
            case VideoCodec::Vp9: return {"vp9_qsv"};
            case VideoCodec::Av1: return {"av1_qsv"};
            }
            return {};

        case EncoderBackend::VideoToolbox:
            switch (codec)
            {
            case VideoCodec::H264: return {"h264_videotoolbox"};
            case VideoCodec::Hevc: return {"hevc_videotoolbox"};
            case VideoCodec::Vp9: return {};
            case VideoCodec::Av1: return {};
            }
            return {};
        }
        return {};
    }

    std::vector<std::string> decoder_candidates(const ImageCodec codec)
    {
        switch (codec)
        {
        case ImageCodec::Png: return {"png"};
        case ImageCodec::Mjpeg: return {"mjpeg"};
        case ImageCodec::WebP: return {"webp", "libwebp"};
        case ImageCodec::Avif: return {"libdav1d", "av1"};
        case ImageCodec::Jxl: return {"libjxl", "jpegxl"};
        case ImageCodec::Heif: return {"hevc"};
        }
        return {};
    }

    std::vector<std::string> decoder_candidates(const VideoCodec codec)
    {
        switch (codec)
        {
        case VideoCodec::H264: return {"h264"};
        case VideoCodec::Hevc: return {"hevc"};
        case VideoCodec::Vp9: return {"vp9", "libvpx-vp9"};
        case VideoCodec::Av1: return {"libdav1d", "av1", "libaom-av1"};
        }
        return {};
    }

    std::string filter_name(const ResizeBackend backend)
    {
        return backend == ResizeBackend::Swscale ? "scale" : "zscale";
    }
}
