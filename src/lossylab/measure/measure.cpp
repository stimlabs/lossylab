#include "lossylab/measure/measure.hpp"

#include "lossylab/codec/encode.hpp"
#include "lossylab/convert/convert.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/core/json_io.hpp"
#include "lossylab/core/schema_version.hpp"
#include "lossylab/detail/ff_error.hpp"
#include "lossylab/detail/ff_ptr.hpp"
#include "lossylab/detail/log_capture.hpp"
#include "lossylab/env/capabilities.hpp"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <numbers>
#include <optional>
#include <utility>

namespace lossylab
{
    namespace
    {
        /// The libavfilter filter each analyzer is built on, or an empty name
        /// for one computed here. Checking these up front means a build without
        /// one says so by name.
        std::string filter_for(const Analyzer analyzer)
        {
            switch (analyzer)
            {
            case Analyzer::SignalLevels: return "signalstats";
            case Analyzer::Blockiness: return "blockdetect";
            case Analyzer::Blurriness: return "blurdetect";
            case Analyzer::Noise: return "";
            case Analyzer::Letterbox: return "cropdetect";
            case Analyzer::Interlacing: return "idet";
            case Analyzer::SpatialTemporalInfo: return "siti";
            case Analyzer::SceneChange: return "scdet";
            case Analyzer::DuplicateFrames: return "mpdecimate";
            }
            return "";
        }

        /// The options each filter runs with, all spelled out so that a change
        /// of FFmpeg's defaults cannot change a result.
        std::string filter_arguments(const Analyzer analyzer)
        {
            switch (analyzer)
            {
            case Analyzer::SignalLevels: return "stat=brng";
            case Analyzer::Blockiness: return "period_min=3:period_max=24:planes=1";
            case Analyzer::Blurriness:
                return "high=30/255:low=15/255:radius=50:block_pct=80:block_width=-1:block_height=-1:planes=1";
            case Analyzer::Letterbox: return "limit=24/255:skip=0:reset_count=1:max_outliers=0:mode=black";
            default: return "";
            }
        }

        bool is_implemented(const Analyzer analyzer)
        {
            switch (analyzer)
            {
            case Analyzer::SignalLevels:
            case Analyzer::Blockiness:
            case Analyzer::Blurriness:
            case Analyzer::Noise:
            case Analyzer::Letterbox: return true;
            default: return false;
            }
        }

        std::vector<PixelFormat> formats_named(const std::initializer_list<const char*> names)
        {
            std::vector<PixelFormat> formats;
            formats.reserve(names.size());
            for (const char* name : names)
            {
                formats.push_back(PixelFormat::from_name(name));
            }
            return formats;
        }

        /// The formats each analyzer measures, for the FFmpeg-backed ones as
        /// listed in FFmpeg n8.1.3's filter sources, nearest first where two
        /// cost the same to reach. Automatic conversion is off in the graph,
        /// so a filter that no longer takes one of these fails to configure.
        const std::vector<PixelFormat>& measurable_formats(const Analyzer analyzer)
        {
            static const std::vector<PixelFormat> signal_levels = formats_named({
                "yuv444p",   "yuv422p",   "yuv420p",   "yuv411p",   "yuv440p",   "yuvj444p",  "yuvj422p",
                "yuvj420p",  "yuvj411p",  "yuvj440p",  "yuv444p9",  "yuv422p9",  "yuv420p9",  "yuv444p10",
                "yuv422p10", "yuv420p10", "yuv440p10", "yuv444p12", "yuv422p12", "yuv420p12", "yuv440p12",
                "yuv444p14", "yuv422p14", "yuv420p14", "yuv444p16", "yuv422p16", "yuv420p16",
            });
            static const std::vector<PixelFormat> eight_bit_edges = formats_named({
                "gray",     "gbrp",     "gbrap",    "yuv444p",  "yuv422p",   "yuv420p",  "yuv440p",
                "yuv411p",  "yuv410p",  "yuvj444p", "yuvj422p", "yuvj420p",  "yuvj440p", "yuvj411p",
                "yuva444p", "yuva422p", "yuva420p",
            });
            static const std::vector<PixelFormat> letterbox = formats_named({
                "yuv444p",   "yuv422p",   "yuv420p",   "yuv411p",   "yuv440p",   "yuv410p",   "yuvj444p",
                "yuvj422p",  "yuvj420p",  "gray",      "yuv444p9",  "yuv422p9",  "yuv420p9",  "yuv444p10",
                "yuv422p10", "yuv420p10", "yuv444p12", "yuv422p12", "yuv420p12", "yuv444p14", "yuv422p14",
                "yuv420p14", "yuv444p16", "yuv422p16", "yuv420p16", "nv12",      "nv21",      "rgb24",
                "bgr24",     "rgba",      "bgra",
            });
            // Noise takes any format whose first plane is plain luma (see
            // has_plain_luma_plane); these are what others convert to.
            static const std::vector<PixelFormat> noise = formats_named({"yuv444p", "yuv444p16", "gray", "gray16"});

            switch (analyzer)
            {
            case Analyzer::SignalLevels: return signal_levels;
            case Analyzer::Blockiness:
            case Analyzer::Blurriness: return eight_bit_edges;
            case Analyzer::Letterbox: return letterbox;
            default: return noise;
            }
        }

        /// Whether the first plane holds luma alone, as one unshifted
        /// little-endian sample of up to 16 bits per pixel.
        bool has_plain_luma_plane(const PixelFormat& format)
        {
            const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(av_get_pix_fmt(format.name().c_str()));
            if (descriptor == nullptr)
            {
                return false;
            }
            constexpr std::uint64_t excluded = AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_FLOAT |
                                               AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BAYER | AV_PIX_FMT_FLAG_HWACCEL |
                                               AV_PIX_FMT_FLAG_BITSTREAM;
            const AVComponentDescriptor& luma = descriptor->comp[0];
            return (descriptor->flags & excluded) == 0 && luma.plane == 0 && luma.shift == 0 && luma.offset == 0 &&
                   luma.depth <= 16 && luma.step == (luma.depth > 8 ? 2 : 1);
        }

        bool accepts(const Analyzer analyzer, const PixelFormat& format)
        {
            if (analyzer == Analyzer::Noise)
            {
                return has_plain_luma_plane(format);
            }
            const std::vector<PixelFormat>& formats = measurable_formats(analyzer);
            return std::find(formats.begin(), formats.end(), format) != formats.end();
        }

        bool is_full_range_variant(const PixelFormat& format)
        {
            return format.name().starts_with("yuvj");
        }

        /// How much converting between two formats changes: moving between
        /// RGB, gray and YUV costs most, then losing bit depth or changing the
        /// chroma subsampling, then gaining bit depth, then the full-range
        /// "yuvj" variants and alpha.
        int conversion_cost(const PixelFormat& from, const PixelFormat& to)
        {
            const auto family = [](const Subsampling subsampling)
            { return subsampling == Subsampling::Rgb ? 0 : subsampling == Subsampling::Gray ? 1 : 2; };
            const Subsampling from_subsampling = from.subsampling();
            const Subsampling to_subsampling = to.subsampling();

            int cost = 0;
            if (family(from_subsampling) != family(to_subsampling))
            {
                cost += 1000;
                if (family(to_subsampling) == 2 && to_subsampling != Subsampling::Yuv444)
                {
                    cost += 100;
                }
            }
            else if (from_subsampling != to_subsampling)
            {
                cost += 100;
            }
            if (to.bit_depth() < from.bit_depth())
            {
                cost += 100;
            }
            else if (to.bit_depth() > from.bit_depth())
            {
                cost += 10;
            }
            if (is_full_range_variant(from) != is_full_range_variant(to))
            {
                cost += 5;
            }
            if (from.has_alpha() != to.has_alpha())
            {
                cost += 1;
            }
            return cost;
        }

        PixelFormat nearest_format(const std::vector<PixelFormat>& formats, const PixelFormat& from)
        {
            return *std::min_element(formats.begin(), formats.end(),
                                     [&from](const PixelFormat& left, const PixelFormat& right)
                                     { return conversion_cost(from, left) < conversion_cost(from, right); });
        }

        /// The color a conversion targets: the frame's own, with a BT.709
        /// matrix and centered chroma when RGB becomes YUV or gray.
        ColorSpec measured_color(const ColorSpec& color, const PixelFormat& from, const PixelFormat& to)
        {
            ColorSpec target = color;
            if (from.is_rgb() && !to.is_rgb())
            {
                target.matrix = ColorMatrix::Bt709;
                target.chroma_location = ChromaLocation::Center;
            }
            return target;
        }

        /// The frames each analyzer or metric measures: the originals when it
        /// accepts their format, and otherwise the originals converted to the
        /// nearest format it does accept, converted once per target format.
        class MeasuredFrames
        {
        public:
            MeasuredFrames(const std::vector<Frame>& frames, const Strict strict, ConversionList& conversions,
                           std::string operation)
                : m_frames(frames), m_strict(strict), m_conversions(conversions), m_operation(std::move(operation))
            {
            }

            const std::vector<Frame>& for_analyzer(const Analyzer analyzer)
            {
                const PixelFormat source_format = m_frames.front().pixel_format();
                if (accepts(analyzer, source_format))
                {
                    return m_frames;
                }
                const std::string measured_by =
                    analyzer == Analyzer::Noise ? std::string("the noise estimator") : filter_for(analyzer);
                return in_format(nearest_format(measurable_formats(analyzer), source_format),
                                 measured_by + " for " + to_string(analyzer));
            }

            /// The frames in one of `formats`, which `measured_by` accepts.
            const std::vector<Frame>& in_one_of(const std::vector<PixelFormat>& formats,
                                                const std::string& measured_by)
            {
                const PixelFormat source_format = m_frames.front().pixel_format();
                if (std::find(formats.begin(), formats.end(), source_format) != formats.end())
                {
                    return m_frames;
                }
                return in_format(nearest_format(formats, source_format), measured_by);
            }

        private:
            const std::vector<Frame>& in_format(const PixelFormat& target_format, const std::string& measured_by)
            {
                const PixelFormat source_format = m_frames.front().pixel_format();
                if (m_strict == Strict::Refuse)
                {
                    throw ConversionRefused("pix_fmt " + source_format.name(), "pix_fmt " + target_format.name(),
                                            m_operation + ": " + measured_by + " does not accept " +
                                                source_format.name());
                }

                const auto [converted, inserted] = m_converted.try_emplace(target_format.name());
                if (inserted)
                {
                    const ColorSpec target_color =
                        measured_color(m_frames.front().color(), source_format, target_format);
                    for (const Frame& frame : m_frames)
                    {
                        FrameResult result = convert(frame, target_format, target_color, Strict::AllowRecorded);
                        if (converted->second.empty())
                        {
                            for (ConversionEvent& event : result.record.conversions)
                            {
                                event.cause = ConversionCause::CodecConstraint;
                                m_conversions.push_back(std::move(event));
                            }
                        }
                        converted->second.push_back(std::move(result.frame));
                    }
                }
                return converted->second;
            }

            const std::vector<Frame>& m_frames;
            Strict m_strict;
            ConversionList& m_conversions;
            std::string m_operation;
            std::map<std::string, std::vector<Frame>> m_converted;
        };

        using FrameMetadata = std::map<std::string, std::string>;

        /// A buffer source taking frames shaped like `first_frame`.
        AVFilterContext* make_buffer_source(AVFilterGraph& graph, const Frame& first_frame, const char* name)
        {
            const AVFrame& raw = *first_frame.raw();
            AVFilterContext* source =
                LL_FF_ALLOC(avfilter_graph_alloc_filter(&graph, avfilter_get_by_name("buffer"), name));
            const detail::AvBufferPtr parameters_owner(LL_FF_ALLOC(av_buffersrc_parameters_alloc()));
            auto* parameters = static_cast<AVBufferSrcParameters*>(parameters_owner.get());
            parameters->format = raw.format;
            parameters->width = raw.width;
            parameters->height = raw.height;
            parameters->time_base = AVRational{1, 25};
            parameters->sample_aspect_ratio =
                raw.sample_aspect_ratio.num > 0 ? raw.sample_aspect_ratio : AVRational{1, 1};
            parameters->color_space = raw.colorspace;
            parameters->color_range = raw.color_range;
            parameters->alpha_mode = raw.alpha_mode;
            LL_FF_CHECK(av_buffersrc_parameters_set(source, parameters));
            LL_FF_CHECK(avfilter_init_str(source, nullptr));
            return source;
        }

        /// Copies the metadata a filter attached to a frame.
        FrameMetadata metadata_of(const AVFrame& frame)
        {
            FrameMetadata entries;
            const AVDictionaryEntry* entry = nullptr;
            while ((entry = av_dict_iterate(frame.metadata, entry)) != nullptr)
            {
                entries.emplace(entry->key, entry->value);
            }
            return entries;
        }

        /// One analyzer filter between a buffer source and a buffer sink, with
        /// automatic format conversion off, one thread, and the filter's own
        /// Info-level reporting of what it attaches to each frame kept out of
        /// the log.
        class AnalyzerGraph
        {
        public:
            AnalyzerGraph(const Frame& first_frame, const std::string& filter_name, const std::string& arguments)
                : m_graph(detail::make_filter_graph())
            {
                m_graph->nb_threads = 1;
                avfilter_graph_set_auto_convert(m_graph.get(), AVFILTER_AUTO_CONVERT_NONE);
                m_source = make_buffer_source(*m_graph, first_frame, "in");

                AVFilterContext* analyzer = LL_FF_ALLOC(avfilter_graph_alloc_filter(
                    m_graph.get(), avfilter_get_by_name(filter_name.c_str()), "analyzer"));
                m_log_capture.emplace(analyzer, [](const int level, const char*, va_list)
                                      { return level >= AV_LOG_INFO; });
                LL_FF_CHECK(avfilter_init_str(analyzer, arguments.c_str()));

                m_sink = LL_FF_ALLOC(avfilter_graph_alloc_filter(m_graph.get(), avfilter_get_by_name("buffersink"),
                                                                 "out"));
                LL_FF_CHECK(avfilter_init_str(m_sink, nullptr));

                LL_FF_CHECK(avfilter_link(m_source, 0, analyzer, 0));
                LL_FF_CHECK(avfilter_link(analyzer, 0, m_sink, 0));
                LL_FF_CHECK(avfilter_graph_config(m_graph.get(), nullptr));
            }

            /// The metadata the filter attached to each frame, in order.
            std::vector<FrameMetadata> run(const std::vector<Frame>& frames)
            {
                std::vector<FrameMetadata> metadata;
                metadata.reserve(frames.size());
                const detail::FramePtr filtered = detail::make_frame();

                const auto drain = [&]
                {
                    while (true)
                    {
                        const int status = av_buffersink_get_frame(m_sink, filtered.get());
                        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF)
                        {
                            return;
                        }
                        LL_FF_CHECK(status);
                        metadata.push_back(metadata_of(*filtered));
                        av_frame_unref(filtered.get());
                    }
                };

                for (std::size_t index = 0; index < frames.size(); ++index)
                {
                    const detail::FramePtr input = detail::ref_frame(frames[index].raw());
                    input->pts = static_cast<std::int64_t>(index);
                    LL_FF_CHECK(av_buffersrc_add_frame_flags(m_source, input.get(), 0));
                    drain();
                }
                LL_FF_CHECK(av_buffersrc_add_frame_flags(m_source, nullptr, 0));
                drain();

                if (metadata.size() != frames.size())
                {
                    throw Error("measure(): the filter graph returned " + std::to_string(metadata.size()) +
                                " frames for " + std::to_string(frames.size()));
                }
                return metadata;
            }

        private:
            // Declared ahead of the graph so that it outlives it: freeing the
            // graph runs each filter's uninit, which logs a summary.
            std::optional<detail::ContextLogCapture> m_log_capture;
            detail::FilterGraphPtr m_graph;
            AVFilterContext* m_source = nullptr;
            AVFilterContext* m_sink = nullptr;
        };

        /// A full-reference metric filter fed the distorted frames on its main
        /// input and the reference frames on its second, between buffer
        /// sources and a buffer sink, set up as AnalyzerGraph is.
        class MetricGraph
        {
        public:
            MetricGraph(const Frame& first_reference, const Frame& first_distorted, const std::string& filter_name)
                : m_graph(detail::make_filter_graph())
            {
                m_graph->nb_threads = 1;
                avfilter_graph_set_auto_convert(m_graph.get(), AVFILTER_AUTO_CONVERT_NONE);
                m_distorted_source = make_buffer_source(*m_graph, first_distorted, "distorted");
                m_reference_source = make_buffer_source(*m_graph, first_reference, "reference");

                AVFilterContext* metric = LL_FF_ALLOC(avfilter_graph_alloc_filter(
                    m_graph.get(), avfilter_get_by_name(filter_name.c_str()), "metric"));
                m_log_capture.emplace(metric, [](const int level, const char*, va_list)
                                      { return level >= AV_LOG_INFO; });
                LL_FF_CHECK(avfilter_init_str(metric, nullptr));

                m_sink = LL_FF_ALLOC(avfilter_graph_alloc_filter(m_graph.get(), avfilter_get_by_name("buffersink"),
                                                                 "out"));
                LL_FF_CHECK(avfilter_init_str(m_sink, nullptr));

                LL_FF_CHECK(avfilter_link(m_distorted_source, 0, metric, 0));
                LL_FF_CHECK(avfilter_link(m_reference_source, 0, metric, 1));
                LL_FF_CHECK(avfilter_link(metric, 0, m_sink, 0));
                LL_FF_CHECK(avfilter_graph_config(m_graph.get(), nullptr));
            }

            /// The metadata the filter attached to each distorted frame, in
            /// order.
            std::vector<FrameMetadata> run(const std::vector<Frame>& reference, const std::vector<Frame>& distorted)
            {
                std::vector<FrameMetadata> metadata;
                metadata.reserve(distorted.size());
                const detail::FramePtr filtered = detail::make_frame();

                const auto drain = [&]
                {
                    while (true)
                    {
                        const int status = av_buffersink_get_frame(m_sink, filtered.get());
                        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF)
                        {
                            return;
                        }
                        LL_FF_CHECK(status);
                        metadata.push_back(metadata_of(*filtered));
                        av_frame_unref(filtered.get());
                    }
                };

                for (std::size_t index = 0; index < distorted.size(); ++index)
                {
                    const detail::FramePtr reference_input = detail::ref_frame(reference[index].raw());
                    const detail::FramePtr distorted_input = detail::ref_frame(distorted[index].raw());
                    reference_input->pts = static_cast<std::int64_t>(index);
                    distorted_input->pts = static_cast<std::int64_t>(index);
                    LL_FF_CHECK(av_buffersrc_add_frame_flags(m_reference_source, reference_input.get(), 0));
                    LL_FF_CHECK(av_buffersrc_add_frame_flags(m_distorted_source, distorted_input.get(), 0));
                    drain();
                }
                LL_FF_CHECK(av_buffersrc_add_frame_flags(m_reference_source, nullptr, 0));
                LL_FF_CHECK(av_buffersrc_add_frame_flags(m_distorted_source, nullptr, 0));
                drain();

                if (metadata.size() != distorted.size())
                {
                    throw Error("compare(): the filter graph returned " + std::to_string(metadata.size()) +
                                " frames for " + std::to_string(distorted.size()));
                }
                return metadata;
            }

        private:
            std::optional<detail::ContextLogCapture> m_log_capture;
            detail::FilterGraphPtr m_graph;
            AVFilterContext* m_reference_source = nullptr;
            AVFilterContext* m_distorted_source = nullptr;
            AVFilterContext* m_sink = nullptr;
        };

        std::string filter_for(const Metric metric)
        {
            return metric == Metric::Psnr ? "psnr" : metric == Metric::Ssim ? "ssim" : "libvmaf";
        }

        /// The formats each metric's filter takes, as listed in FFmpeg
        /// n8.1.3's vf_psnr.c and vf_ssim.c.
        const std::vector<PixelFormat>& comparable_formats(const Metric metric)
        {
            static const std::vector<PixelFormat> psnr = formats_named({
                "gray",       "gray9",       "gray10",      "gray12",      "gray14",      "gray16",
                "yuv420p",    "yuv422p",     "yuv444p",     "yuva420p",    "yuva422p",    "yuva444p",
                "yuv420p9",   "yuv422p9",    "yuv444p9",    "yuva420p9",   "yuva422p9",   "yuva444p9",
                "yuv420p10",  "yuv422p10",   "yuv444p10",   "yuva420p10",  "yuva422p10",  "yuva444p10",
                "yuv420p12",  "yuv422p12",   "yuv444p12",   "yuv420p14",   "yuv422p14",   "yuv444p14",
                "yuv420p16",  "yuv422p16",   "yuv444p16",   "yuva420p16",  "yuva422p16",  "yuva444p16",
                "yuv440p",    "yuv411p",     "yuv410p",     "yuvj411p",    "yuvj420p",    "yuvj422p",
                "yuvj440p",   "yuvj444p",    "gbrp",        "gbrp9",       "gbrp10",      "gbrp12",
                "gbrp14",     "gbrp16",      "gbrap",       "gbrap10",     "gbrap12",     "gbrap16",
            });
            static const std::vector<PixelFormat> ssim = formats_named({
                "gray",      "gray9",     "gray10",    "gray12",    "gray14",    "gray16",    "yuv420p",
                "yuv422p",   "yuv444p",   "yuv440p",   "yuv411p",   "yuv410p",   "yuvj411p",  "yuvj420p",
                "yuvj422p",  "yuvj440p",  "yuvj444p",  "gbrp",      "yuv420p9",  "yuv422p9",  "yuv444p9",
                "gbrp9",     "yuv420p10", "yuv422p10", "yuv444p10", "gbrp10",    "yuv420p12", "yuv422p12",
                "yuv444p12", "gbrp12",    "yuv420p14", "yuv422p14", "yuv444p14", "gbrp14",    "yuv420p16",
                "yuv422p16", "yuv444p16", "gbrp16",
            });
            return metric == Metric::Ssim ? ssim : psnr;
        }

        /// Reads the values under `prefix` + a component letter into
        /// `name` + "_" + the letter in lowercase.
        void read_components(const FrameMetadata& metadata, const std::string& prefix, const std::string& name,
                             std::map<std::string, double>& values)
        {
            for (const auto& [key, text] : metadata)
            {
                if (key.size() != prefix.size() + 1 || !key.starts_with(prefix))
                {
                    continue;
                }
                const char component = static_cast<char>(std::tolower(static_cast<unsigned char>(key.back())));
                values[name + "_" + component] = std::strtod(text.c_str(), nullptr);
            }
        }

        /// A value the filter attaches to every frame. Throws when it is
        /// missing or not a number, which means FFmpeg changed its output. The
        /// value can be NaN or infinite; callers decide what that means.
        double attached_number(const FrameMetadata& metadata, const std::string& key)
        {
            const auto it = metadata.find(key);
            char* end = nullptr;
            const double value = it == metadata.end() ? 0.0 : std::strtod(it->second.c_str(), &end);
            if (it == metadata.end() || end == it->second.c_str())
            {
                throw Error("the filter attached no numeric '" + key + "' to a frame");
            }
            return value;
        }

        void read_metric_values(const Metric metric, const FrameMetadata& metadata,
                                std::map<std::string, double>& values)
        {
            if (metric == Metric::Psnr)
            {
                values["psnr"] = attached_number(metadata, "lavfi.psnr.psnr_avg");
                values["mse"] = attached_number(metadata, "lavfi.psnr.mse_avg");
                read_components(metadata, "lavfi.psnr.psnr.", "psnr", values);
                read_components(metadata, "lavfi.psnr.mse.", "mse", values);
                return;
            }
            values["ssim"] = attached_number(metadata, "lavfi.ssim.All");
            values["ssim_db"] = attached_number(metadata, "lavfi.ssim.dB");
            read_components(metadata, "lavfi.ssim.", "ssim", values);
        }

        void read_signal_levels(const FrameMetadata& metadata, FrameMeasurement& measurement)
        {
            static const std::pair<const char*, const char*> keys[] = {
                {"YMIN", "luma_min"},
                {"YLOW", "luma_low"},
                {"YAVG", "luma_mean"},
                {"YHIGH", "luma_high"},
                {"YMAX", "luma_max"},
                {"UMIN", "u_min"},
                {"ULOW", "u_low"},
                {"UAVG", "u_mean"},
                {"UHIGH", "u_high"},
                {"UMAX", "u_max"},
                {"VMIN", "v_min"},
                {"VLOW", "v_low"},
                {"VAVG", "v_mean"},
                {"VHIGH", "v_high"},
                {"VMAX", "v_max"},
                {"SATMIN", "saturation_min"},
                {"SATLOW", "saturation_low"},
                {"SATAVG", "saturation_mean"},
                {"SATHIGH", "saturation_high"},
                {"SATMAX", "saturation_max"},
                {"HUEMED", "hue_median"},
                {"HUEAVG", "hue_mean"},
                {"YBITDEPTH", "luma_bit_depth"},
                {"UBITDEPTH", "u_bit_depth"},
                {"VBITDEPTH", "v_bit_depth"},
                {"BRNG", "outside_limited_range"},
            };
            for (const auto& [key, name] : keys)
            {
                measurement.values[name] = attached_number(metadata, std::string("lavfi.signalstats.") + key);
            }
        }

        void read_letterbox(const FrameMetadata& metadata, const Frame& frame, FrameMeasurement& measurement)
        {
            const double left = attached_number(metadata, "lavfi.cropdetect.x1");
            const double right = attached_number(metadata, "lavfi.cropdetect.x2");
            const double top = attached_number(metadata, "lavfi.cropdetect.y1");
            const double bottom = attached_number(metadata, "lavfi.cropdetect.y2");

            // cropdetect leaves the far edge before the near one when every
            // row and column is black.
            if (right < left || bottom < top)
            {
                measurement.content_rect = Rect{};
                measurement.values["content_fraction"] = 0.0;
                return;
            }

            const double width = frame.width();
            const double height = frame.height();
            const Rect content{left, top, right - left + 1.0, bottom - top + 1.0};
            measurement.content_rect = content;
            measurement.values["letterbox_top"] = top;
            measurement.values["letterbox_bottom"] = height - 1.0 - bottom;
            measurement.values["letterbox_left"] = left;
            measurement.values["letterbox_right"] = width - 1.0 - right;
            measurement.values["content_fraction"] = content.width * content.height / (width * height);
        }

        void read_filter_values(const Analyzer analyzer, const FrameMetadata& metadata, const Frame& frame,
                                FrameMeasurement& measurement)
        {
            switch (analyzer)
            {
            case Analyzer::SignalLevels: read_signal_levels(metadata, measurement); break;
            case Analyzer::Blockiness:
                measurement.values["blockiness"] = attached_number(metadata, "lavfi.block");
                break;
            case Analyzer::Blurriness:
            {
                // blurdetect divides by the number of blocks with edges, so a
                // frame without any comes back as NaN.
                const double blurriness = attached_number(metadata, "lavfi.blur");
                if (std::isfinite(blurriness))
                {
                    measurement.values["blurriness"] = blurriness;
                }
                break;
            }
            case Analyzer::Letterbox: read_letterbox(metadata, frame, measurement); break;
            default: break;
            }
        }

        template <typename Sample>
        int sample_at(const ConstPlaneView& plane, const int x, const int y)
        {
            Sample value;
            std::memcpy(&value, plane.row(y) + static_cast<std::size_t>(x) * sizeof(Sample), sizeof(Sample));
            return value;
        }

        /// Tai and Yang's edge-masked Immerkaer estimate ("A fast method for
        /// image noise estimation using Laplacian operator and adaptive edge
        /// detection", 2008), in the plane's own code values.
        template <typename Sample>
        double edge_masked_noise_sigma(const ConstPlaneView& plane, const int max_value)
        {
            const auto at = [&plane](const int x, const int y) { return sample_at<Sample>(plane, x, y); };
            const auto sobel_magnitude = [&at](const int x, const int y)
            {
                const int horizontal = at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1) - at(x - 1, y - 1) -
                                       2 * at(x - 1, y) - at(x - 1, y + 1);
                const int vertical = at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1) - at(x - 1, y - 1) -
                                     2 * at(x, y - 1) - at(x + 1, y - 1);
                return std::abs(horizontal) + std::abs(vertical);
            };

            std::vector<std::uint64_t> histogram(static_cast<std::size_t>(8 * max_value) + 1);
            for (int y = 1; y < plane.height - 1; ++y)
            {
                for (int x = 1; x < plane.width - 1; ++x)
                {
                    ++histogram[static_cast<std::size_t>(sobel_magnitude(x, y))];
                }
            }

            // The 10% of pixels with the strongest gradient are edges.
            const std::uint64_t interior =
                static_cast<std::uint64_t>(plane.width - 2) * static_cast<std::uint64_t>(plane.height - 2);
            const std::uint64_t kept = (interior * 9 + 9) / 10;
            std::size_t threshold = 0;
            for (std::uint64_t cumulative = histogram[0]; cumulative < kept; cumulative += histogram[++threshold])
            {
            }

            double total = 0.0;
            std::uint64_t used = 0;
            for (int y = 1; y < plane.height - 1; ++y)
            {
                for (int x = 1; x < plane.width - 1; ++x)
                {
                    if (static_cast<std::size_t>(sobel_magnitude(x, y)) > threshold)
                    {
                        continue;
                    }
                    const int response = at(x - 1, y - 1) - 2 * at(x, y - 1) + at(x + 1, y - 1) -
                                         2 * at(x - 1, y) + 4 * at(x, y) - 2 * at(x + 1, y) + at(x - 1, y + 1) -
                                         2 * at(x, y + 1) + at(x + 1, y + 1);
                    total += std::abs(response);
                    ++used;
                }
            }
            return std::sqrt(std::numbers::pi / 2.0) * total / (6.0 * static_cast<double>(used));
        }

        /// The noise sigma on the first plane, in 8-bit code values.
        std::optional<double> noise_sigma(const Frame& frame)
        {
            const ConstPlaneView plane = frame.plane(0);
            if (plane.width < 3 || plane.height < 3)
            {
                return std::nullopt;
            }
            const int bit_depth = frame.pixel_format().bit_depth();
            if (bit_depth <= 8)
            {
                return edge_masked_noise_sigma<std::uint8_t>(plane, (1 << bit_depth) - 1);
            }
            return edge_masked_noise_sigma<std::uint16_t>(plane, (1 << bit_depth) - 1) /
                   static_cast<double>(1 << (bit_depth - 8));
        }

        const std::map<std::string, double>& values_of(const FrameMeasurement& frame) { return frame.values; }
        const std::map<std::string, double>& values_of(const std::map<std::string, double>& frame) { return frame; }

        template <typename FrameValues>
        void pool_into(std::map<std::string, double>& pooled, const std::vector<FrameValues>& frames)
        {
            std::map<std::string, double> totals;
            std::map<std::string, int> counts;

            for (const FrameValues& frame : frames)
            {
                for (const auto& [name, value] : values_of(frame))
                {
                    totals[name] += value;
                    counts[name] += 1;

                    auto& minimum = pooled[name + "_min"];
                    auto& maximum = pooled[name + "_max"];
                    if (counts[name] == 1)
                    {
                        minimum = value;
                        maximum = value;
                    }
                    else
                    {
                        minimum = std::min(minimum, value);
                        maximum = std::max(maximum, value);
                    }
                }
            }

            for (const auto& [name, total] : totals)
            {
                pooled[name + "_mean"] = total / counts[name];
            }
        }
    }

    std::string to_string(const Analyzer analyzer)
    {
        switch (analyzer)
        {
        case Analyzer::SignalLevels: return "signal_levels";
        case Analyzer::Blockiness: return "blockiness";
        case Analyzer::Blurriness: return "blurriness";
        case Analyzer::Noise: return "noise";
        case Analyzer::Letterbox: return "letterbox";
        case Analyzer::Interlacing: return "interlacing";
        case Analyzer::SpatialTemporalInfo: return "spatial_temporal_info";
        case Analyzer::SceneChange: return "scene_change";
        case Analyzer::DuplicateFrames: return "duplicate_frames";
        }
        return "unknown";
    }

    Analyzer analyzer_from_string(const std::string_view name)
    {
        if (name == "signal_levels") { return Analyzer::SignalLevels; }
        if (name == "blockiness") { return Analyzer::Blockiness; }
        if (name == "blurriness") { return Analyzer::Blurriness; }
        if (name == "noise") { return Analyzer::Noise; }
        if (name == "letterbox") { return Analyzer::Letterbox; }
        if (name == "interlacing") { return Analyzer::Interlacing; }
        if (name == "spatial_temporal_info") { return Analyzer::SpatialTemporalInfo; }
        if (name == "scene_change") { return Analyzer::SceneChange; }
        if (name == "duplicate_frames") { return Analyzer::DuplicateFrames; }
        throw ConfigError("unknown analyzer '" + std::string(name) + "'");
    }

    std::optional<double> FrameMeasurement::value(const std::string_view name) const
    {
        const auto it = values.find(std::string(name));
        return it == values.end() ? std::nullopt : std::optional<double>(it->second);
    }

    json::Value FrameMeasurement::to_json() const
    {
        json::Value document = json::object({
            {"index", index},
            {"values", json::to_object(values)},
        });

        if (content_rect.has_value())
        {
            document["content_rect"] = content_rect->to_json();
        }
        return document;
    }

    json::Value MeasureResult::to_json() const
    {
        return json::object({
            {"schema_version", schema_version},
            {"frames", json::to_array(frames)},
            {"pooled", json::to_object(pooled)},
            {"record", record.to_json()},
        });
    }

    json::Value CompareResult::to_json() const
    {
        return json::object({
            {"schema_version", schema_version},
            {"frames", json::to_array(frames, [](const std::map<std::string, double>& metrics) {
                 return json::to_object(metrics);
             })},
            {"pooled", json::to_object(pooled)},
            {"record", record.to_json()},
        });
    }

    json::Value RecompressionPoint::to_json() const
    {
        return json::object({
            {"quality_parameter", quality_parameter},
            {"error", error},
            {"bits_per_pixel", bits_per_pixel},
        });
    }

    json::Value RecompressionCurve::to_json() const
    {
        return json::object({
            {"schema_version", schema_version},
            {"points", json::to_array(points)},
            {"estimated_prior_parameter", json::optional_or_null(estimated_prior_parameter)},
            {"confidence", confidence},
            {"record", record.to_json()},
        });
    }

    MeasureResult measure(const std::vector<Frame>& frames,
                          const std::vector<Analyzer>& analyzers,
                          const MeasureOptions& options)
    {
        const auto started = std::chrono::steady_clock::now();

        if (frames.empty())
        {
            throw ConfigError("measure() received no frames");
        }
        if (analyzers.empty())
        {
            throw ConfigError("measure() received no analyzers");
        }
        if (frames.front().empty())
        {
            throw ConfigError("measure() received an empty frame");
        }
        const FormatDescription format = frames.front().describe();
        for (std::size_t index = 1; index < frames.size(); ++index)
        {
            if (frames[index].empty() || frames[index].describe() != format)
            {
                throw ConfigError("measure() frame " + std::to_string(index) +
                                  " differs from frame 0 in size, pixel format or color");
            }
        }

        std::vector<Analyzer> distinct_analyzers;
        for (const Analyzer analyzer : analyzers)
        {
            if (std::find(distinct_analyzers.begin(), distinct_analyzers.end(), analyzer) ==
                distinct_analyzers.end())
            {
                distinct_analyzers.push_back(analyzer);
            }
        }

        for (const Analyzer analyzer : distinct_analyzers)
        {
            if (!is_implemented(analyzer))
            {
                throw NotImplemented("measure() for " + to_string(analyzer));
            }
            // Each FFmpeg-backed analyzer is a libavfilter filter, so a build
            // without one is reported by name before any work starts.
            if (const std::string filter = filter_for(analyzer); !filter.empty())
            {
                static_cast<void>(capabilities().require_filter(filter));
            }
        }

        MeasureResult result;
        result.frames.resize(frames.size());
        for (std::size_t index = 0; index < frames.size(); ++index)
        {
            result.frames[index].index = static_cast<int>(index);
        }

        StageRecord& record = result.record;
        record.kind = StageKind::Measure;
        record.implementation = "libavfilter";
        record.input = format;
        record.output = format;
        record.transform = CoordinateTransform::identity();

        json::Value analyzer_names = json::Value::array();
        json::Value measured_as = json::Value::object();
        json::Value methods = json::Value::object();
        MeasuredFrames measured_frames(frames, options.strict, record.conversions, "measure()");

        for (const Analyzer analyzer : distinct_analyzers)
        {
            const std::vector<Frame>& measured = measured_frames.for_analyzer(analyzer);
            const std::string name = to_string(analyzer);
            analyzer_names.push_back(name);
            measured_as[name] = measured.front().pixel_format().name();

            if (analyzer == Analyzer::Noise)
            {
                methods[name] = "tai_yang_edge_masked_immerkaer";
                for (std::size_t index = 0; index < measured.size(); ++index)
                {
                    if (const std::optional<double> sigma = noise_sigma(measured[index]))
                    {
                        result.frames[index].values["noise_sigma"] = *sigma;
                    }
                }
                continue;
            }

            const std::string filter = filter_for(analyzer);
            const std::string arguments = filter_arguments(analyzer);
            methods[name] = filter + "=" + arguments;

            AnalyzerGraph graph(measured.front(), filter, arguments);
            const std::vector<FrameMetadata> metadata = graph.run(measured);
            for (std::size_t index = 0; index < measured.size(); ++index)
            {
                read_filter_values(analyzer, metadata[index], measured[index], result.frames[index]);
            }
        }

        pool_into(result.pooled, result.frames);

        record.params = json::object({
            {"analyzers", analyzer_names},
            {"frame_count", frames.size()},
            {"methods", methods},
            {"measured_as", measured_as},
            {"strict", to_string(options.strict)},
        });
        record.duration_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        return result;
    }

    MeasureResult measure(const Frame& frame, const std::vector<Analyzer>& analyzers, const MeasureOptions& options)
    {
        return measure(std::vector<Frame>{frame}, analyzers, options);
    }

    CompareResult compare(const std::vector<Frame>& reference,
                          const std::vector<Frame>& distorted,
                          const std::vector<Metric>& metrics,
                          const CompareOptions& options)
    {
        const auto started = std::chrono::steady_clock::now();

        if (reference.empty() || distorted.empty())
        {
            throw ConfigError("compare() received no frames");
        }
        if (reference.size() != distorted.size())
        {
            throw ConfigError("compare() received " + std::to_string(reference.size()) +
                              " reference frames and " + std::to_string(distorted.size()) +
                              " distorted frames");
        }
        if (metrics.empty())
        {
            throw ConfigError("compare() received no metrics");
        }

        for (std::size_t i = 0; i < reference.size(); ++i)
        {
            if (reference[i].empty() || distorted[i].empty())
            {
                throw ConfigError("compare() frame " + std::to_string(i) + " is empty");
            }
            if (reference[i].width() != distorted[i].width() ||
                reference[i].height() != distorted[i].height())
            {
                throw ConfigError("compare() frame " + std::to_string(i) +
                                  " differs in size between reference and distorted; "
                                  "resize explicitly first");
            }
        }

        const FormatDescription format = reference.front().describe();
        for (std::size_t i = 0; i < reference.size(); ++i)
        {
            if (reference[i].describe() != format || distorted[i].describe() != format)
            {
                throw ConfigError("compare() frame " + std::to_string(i) +
                                  " differs from reference frame 0 in size, pixel format or color; "
                                  "convert explicitly first");
            }
        }

        std::vector<Metric> distinct_metrics;
        for (const Metric metric : metrics)
        {
            capabilities().require_metric(metric);
            if (std::find(distinct_metrics.begin(), distinct_metrics.end(), metric) == distinct_metrics.end())
            {
                distinct_metrics.push_back(metric);
            }
        }
        for (const Metric metric : distinct_metrics)
        {
            if (metric == Metric::Vmaf)
            {
                throw NotImplemented("compare() for vmaf");
            }
            if (metric == Metric::Ssim && (format.width < 8 || format.height < 8))
            {
                throw ConfigError("compare() needs frames of at least 8x8 for SSIM, not " +
                                  std::to_string(format.width) + "x" + std::to_string(format.height));
            }
        }

        CompareResult result;
        result.frames.resize(reference.size());

        StageRecord& record = result.record;
        record.kind = StageKind::Compare;
        record.implementation = "libavfilter";
        record.input = format;
        record.output = format;
        record.transform = CoordinateTransform::identity();

        // Both sides go through the same conversion, so it is recorded once.
        ConversionList distorted_conversions;
        MeasuredFrames reference_frames(reference, options.strict, record.conversions, "compare()");
        MeasuredFrames distorted_frames(distorted, options.strict, distorted_conversions, "compare()");

        json::Value metric_names = json::Value::array();
        json::Value measured_as = json::Value::object();
        for (const Metric metric : distinct_metrics)
        {
            const std::string filter = filter_for(metric);
            const std::vector<PixelFormat>& formats = comparable_formats(metric);
            const std::vector<Frame>& measured_reference = reference_frames.in_one_of(formats, filter);
            const std::vector<Frame>& measured_distorted = distorted_frames.in_one_of(formats, filter);
            metric_names.push_back(to_string(metric));
            measured_as[to_string(metric)] = measured_reference.front().pixel_format().name();

            MetricGraph graph(measured_reference.front(), measured_distorted.front(), filter);
            const std::vector<FrameMetadata> metadata = graph.run(measured_reference, measured_distorted);
            for (std::size_t index = 0; index < metadata.size(); ++index)
            {
                read_metric_values(metric, metadata[index], result.frames[index]);
            }
        }

        pool_into(result.pooled, result.frames);

        record.params = json::object({
            {"metrics", metric_names},
            {"frame_count", reference.size()},
            {"measured_as", measured_as},
            {"strict", to_string(options.strict)},
        });
        record.duration_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        return result;
    }

    CompareResult compare(const Frame& reference, const Frame& distorted, const std::vector<Metric>& metrics,
                          const CompareOptions& options)
    {
        return compare(std::vector<Frame>{reference}, std::vector<Frame>{distorted}, metrics, options);
    }

    namespace
    {
        /// How far each point's log error falls below the straight line
        /// between its two neighbors' log errors, over the parameter axis.
        /// Zero for the first and last points, which have no two neighbors.
        std::vector<double> notch_depths(const std::vector<RecompressionPoint>& points)
        {
            // An exact re-encode has zero error; the floor keeps its logarithm
            // finite while still making it the deepest point by far.
            constexpr double error_floor = 1e-9;
            std::vector<double> log_errors;
            for (const RecompressionPoint& point : points)
            {
                log_errors.push_back(std::log(std::max(point.error, error_floor)));
            }

            std::vector<double> depths(points.size(), 0.0);
            for (std::size_t i = 1; i + 1 < points.size(); ++i)
            {
                const double previous = points[i - 1].quality_parameter;
                const double next = points[i + 1].quality_parameter;
                const double position = (points[i].quality_parameter - previous) / (next - previous);
                const double line = log_errors[i - 1] + position * (log_errors[i + 1] - log_errors[i - 1]);
                depths[i] = line - log_errors[i];
            }
            return depths;
        }

        double median(std::vector<double> values)
        {
            if (values.empty())
            {
                return 0.0;
            }
            std::sort(values.begin(), values.end());
            const std::size_t middle = values.size() / 2;
            return values.size() % 2 == 1 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
        }

        /// The format a codec re-encodes a frame in when the caller names
        /// none, as RecompressionOptions::pixel_format lists them.
        PixelFormat default_recompression_format(const ImageCodec codec, const PixelFormat& format)
        {
            const Subsampling subsampling = format.subsampling();
            switch (codec)
            {
            case ImageCodec::Mjpeg:
                switch (subsampling)
                {
                case Subsampling::Gray:
                case Subsampling::Yuv444: return PixelFormat::from_name("yuvj444p");
                case Subsampling::Yuv422: return PixelFormat::from_name("yuvj422p");
                default: return PixelFormat::from_name("yuvj420p");
                }
            case ImageCodec::WebP: return PixelFormat::from_name("yuv420p");
            case ImageCodec::Avif:
                switch (subsampling)
                {
                case Subsampling::Gray: return PixelFormat::from_name("gray");
                case Subsampling::Yuv444: return PixelFormat::from_name("yuv444p");
                case Subsampling::Yuv422: return PixelFormat::from_name("yuv422p");
                default: return PixelFormat::from_name("yuv420p");
                }
            case ImageCodec::Jxl:
            case ImageCodec::Jpeg2000:
                return PixelFormat::from_name(subsampling == Subsampling::Gray ? "gray" : "rgb24");
            case ImageCodec::Png:
            case ImageCodec::Heif: break;
            }
            throw ConfigError("recompression_curve() needs a lossy codec to sweep: MJPEG, WebP, AVIF, JPEG XL or "
                              "JPEG 2000, not " + to_string(codec));
        }

        /// The color a codec's bitstream implies for what a frame leaves
        /// unspecified, on sRGB primaries and transfer.
        ColorSpec codec_implied_color(const ImageCodec codec, const PixelFormat& format)
        {
            ColorSpec color = ColorSpec::srgb();
            if (format.is_rgb())
            {
                return color;
            }
            color.matrix = ColorMatrix::Bt470bg;
            color.range = codec == ImageCodec::WebP ? ColorRange::Limited : ColorRange::Full;
            color.chroma_location = codec == ImageCodec::Avif ? ChromaLocation::Left : ChromaLocation::Center;
            return color;
        }

        /// The color to re-encode in when the caller names none: the frame's
        /// own, with the fields the codec's bitstream fixes put in place.
        ColorSpec default_recompression_color(const ImageCodec codec, const PixelFormat& target_format,
                                              const ColorSpec& frame_color)
        {
            ColorSpec color = frame_color;
            if (target_format.is_rgb())
            {
                color.matrix = ColorMatrix::Rgb;
                color.range = ColorRange::Full;
                return color;
            }
            const bool fixed_by_codec = codec == ImageCodec::Mjpeg || codec == ImageCodec::WebP;
            if (fixed_by_codec || frame_color.is_rgb())
            {
                const ColorSpec implied = codec_implied_color(codec, target_format);
                color.matrix = implied.matrix;
                color.range = implied.range;
                color.chroma_location = implied.chroma_location;
            }
            return color;
        }

        /// The metric value a point's error is taken from.
        std::string error_key(const Metric metric, const RecompressionPlanes planes)
        {
            const std::string base = metric == Metric::Psnr ? "mse" : "ssim";
            return planes == RecompressionPlanes::Luma ? base + "_y" : base;
        }
    }

    std::string to_string(const RecompressionPlanes planes)
    {
        return planes == RecompressionPlanes::Luma ? "luma" : "all";
    }

    RecompressionPlanes recompression_planes_from_string(const std::string_view name)
    {
        if (name == "all") { return RecompressionPlanes::All; }
        if (name == "luma") { return RecompressionPlanes::Luma; }
        throw ConfigError("unknown recompression planes '" + std::string(name) + "'");
    }

    RecompressionCurve recompression_curve(const Frame& frame,
                                           const RecompressionOptions& options)
    {
        const auto started = std::chrono::steady_clock::now();

        if (frame.empty())
        {
            throw ConfigError("recompression_curve() received an empty frame");
        }
        std::vector<double> parameters = options.parameter_range;
        std::sort(parameters.begin(), parameters.end());
        parameters.erase(std::unique(parameters.begin(), parameters.end()), parameters.end());
        if (parameters.size() < 3)
        {
            throw ConfigError("recompression_curve() needs at least three distinct parameters to find "
                              "a minimum");
        }

        static_cast<void>(capabilities().require_encoder(options.codec));
        static_cast<void>(capabilities().require_decoder(options.codec));
        capabilities().require_metric(options.metric);
        if (options.metric == Metric::Vmaf)
        {
            throw NotImplemented("recompression_curve() with vmaf");
        }

        RecompressionCurve curve;
        StageRecord& record = curve.record;
        record.kind = StageKind::RecompressionCurve;
        record.input = frame.describe();
        record.transform = CoordinateTransform::identity();

        // Unspecified fields are filled before anything reads the color, since
        // neither a conversion nor an encode can proceed without them.
        Frame source = frame;
        const ColorSpec tagged = frame.color();
        if (!tagged.is_fully_specified())
        {
            const ColorSpec filled =
                tagged.with_defaults_from(codec_implied_color(options.codec, frame.pixel_format()));
            source.set_color(filled);
            source.sync_color_to_av_frame();
            record.conversions.push_back(ConversionEvent{"color_tags", tagged.describe(), filled.describe(),
                                                         ConversionCause::Requested, "codec_implied_color"});
        }

        const PixelFormat pixel_format =
            options.pixel_format.value_or(default_recompression_format(options.codec, frame.pixel_format()));
        const ColorSpec color =
            options.color.value_or(default_recompression_color(options.codec, pixel_format, source.color()));
        if (options.planes == RecompressionPlanes::Luma && pixel_format.is_rgb())
        {
            throw ConfigError("recompression_curve() measures luma error in a YUV or gray format, not " +
                              pixel_format.name());
        }
        Frame reference = source;
        if (pixel_format != source.pixel_format() || color != source.color())
        {
            FrameResult converted = convert(source, pixel_format, color, Strict::AllowRecorded);
            record.conversions.insert(record.conversions.end(), converted.record.conversions.begin(),
                                      converted.record.conversions.end());
            reference = std::move(converted.frame);
        }
        record.output = reference.describe();

        EncodeImageOptions encode_options;
        encode_options.codec = options.codec;
        encode_options.pixel_format = pixel_format;
        encode_options.encoder_options = options.encoder_options;

        DecodeSpec decode_spec;
        decode_spec.pixel_format = pixel_format;
        decode_spec.color = color;
        decode_spec.strict = Strict::AllowRecorded;

        CompareOptions compare_options;
        compare_options.strict = Strict::AllowRecorded;

        json::Value quality_scale;
        for (const double parameter : parameters)
        {
            encode_options.rate_control = RateControl::quality(parameter);
            const FrameResult decoded = roundtrip(reference, encode_options, decode_spec);
            const CompareResult compared = compare(reference, decoded.frame, {options.metric}, compare_options);
            const std::map<std::string, double>& values = compared.frames.front();
            const double measured = values.at(error_key(options.metric, options.planes));
            const double error = options.metric == Metric::Psnr ? measured : 1.0 - measured;
            curve.points.push_back(RecompressionPoint{parameter, error, decoded.record.achieved_bpp.value_or(0.0)});

            // Every point converts the same way, so the first one speaks for all.
            if (curve.points.size() == 1)
            {
                record.implementation = decoded.record.implementation;
                quality_scale = decoded.record.encoder_settings.at("quality_scale");
                for (const ConversionList* conversions : {&decoded.record.conversions, &compared.record.conversions})
                {
                    record.conversions.insert(record.conversions.end(), conversions->begin(), conversions->end());
                }
            }
        }

        const std::vector<double> depths = notch_depths(curve.points);
        const auto deepest = std::max_element(depths.begin() + 1, depths.end() - 1);
        constexpr double minimum_noise_scale = 0.05;
        double noise_scale = minimum_noise_scale;
        if (*deepest > 0.0)
        {
            std::vector<double> other_depths;
            for (auto it = depths.begin() + 1; it != depths.end() - 1; ++it)
            {
                if (it != deepest)
                {
                    other_depths.push_back(std::abs(*it));
                }
            }
            noise_scale = std::max(median(other_depths), minimum_noise_scale);
            curve.confidence = *deepest / (*deepest + 3.0 * noise_scale);
            if (curve.confidence >= 0.5)
            {
                curve.estimated_prior_parameter =
                    curve.points[static_cast<std::size_t>(deepest - depths.begin())].quality_parameter;
            }
        }

        record.params = json::object({
            {"codec", to_string(options.codec)},
            {"metric", to_string(options.metric)},
            {"error", options.metric == Metric::Psnr ? error_key(options.metric, options.planes)
                                                     : "1 - " + error_key(options.metric, options.planes)},
            {"planes", to_string(options.planes)},
            {"alpha", frame.pixel_format().has_alpha() && !pixel_format.has_alpha() ? "dropped" : "kept"},
            {"parameters", parameters},
            {"quality_scale", quality_scale},
            {"notch_depths", depths},
            {"noise_scale", noise_scale},
            {"pixel_format", pixel_format.name()},
            {"color", color.to_json()},
            {"encoder_options", json::to_object(options.encoder_options)},
        });
        record.duration_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        return curve;
    }
}
