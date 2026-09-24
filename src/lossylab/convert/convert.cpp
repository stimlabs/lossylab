#include "lossylab/convert/convert.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/detail/ff_error.hpp"
#include "lossylab/detail/ff_ptr.hpp"
#include "lossylab/env/build_info.hpp"
#include "lossylab/env/capabilities.hpp"


extern "C" {
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace lossylab
{
    namespace
    {
        /// swscale selects its kernel through a flag on the context.
        int sws_flag_for(const Kernel kernel)
        {
            switch (kernel)
            {
            case Kernel::Nearest: return SWS_POINT;
            case Kernel::Bilinear: return SWS_BILINEAR;
            case Kernel::Bicubic: return SWS_BICUBIC;
            case Kernel::Lanczos: return SWS_LANCZOS;
            case Kernel::Area: return SWS_AREA;
            case Kernel::Gaussian: return SWS_GAUSS;
            case Kernel::Sinc: return SWS_SINC;
            case Kernel::Spline: return SWS_SPLINE;
            case Kernel::Bicublin: return SWS_BICUBLIN;
            }
            throw ConfigError("kernel '" + to_string(kernel) + "' has no swscale equivalent");
        }

        /// swscale's coefficient tables are indexed by AVColorSpace.
        const int* coefficients_for(const ColorMatrix matrix)
        {
            return sws_getCoefficients(static_cast<int>(matrix));
        }

        bool is_limited(const ColorRange range)
        {
            return range != ColorRange::Full;
        }

        /// Compares the properties a conversion can change, and funnels each
        /// difference through the strict-mode policy.
        ///
        /// Routing every property through one place is what makes the
        /// no-hidden-conversion guarantee structural rather than a promise each
        /// call site has to keep.
        ///
        /// What Strict::Refuse means here: the pixel format may change, because
        /// the caller named the target, but the color interpretation may not.
        /// That separates repackaging the samples from reinterpreting them, and
        /// forces the second to be asked for.
        ///
        /// The exception is the change a layout switch entails. Moving between
        /// RGB and YUV necessarily changes the matrix away from (or to) the RGB
        /// identity and gives the chroma planes a siting, so refusing those
        /// would make every RGB-to-YUV conversion impossible under the default.
        /// Range, primaries and transfer are never entailed by a layout change
        /// and stay subject to the policy.
        void account_for_changes(const Frame& source, const ConvertOptions& options,
                                 ConversionList& conversions, const char* performed_by)
        {
            const PixelFormat from_format = source.pixel_format();
            const PixelFormat& to_format = options.pixel_format;
            const ColorSpec& from_color = source.color();
            const ColorSpec& to_color = options.color;

            constexpr ConversionCause cause = ConversionCause::Requested;
            constexpr std::string_view context = "convert";

            record_or_refuse(Strict::AllowRecorded, conversions, "pix_fmt", from_format.name(),
                             to_format.name(), cause, performed_by, context);

            if (from_format.is_valid() && to_format.is_valid())
            {
                record_or_refuse(Strict::AllowRecorded, conversions, "bit_depth",
                                 std::to_string(from_format.bit_depth()),
                                 std::to_string(to_format.bit_depth()), cause, performed_by,
                                 context);

                record_or_refuse(Strict::AllowRecorded, conversions, "subsampling",
                                 to_string(from_format.subsampling()),
                                 to_string(to_format.subsampling()), cause, performed_by,
                                 context);
            }

            const bool layout_class_changed =
                from_format.is_valid() && to_format.is_valid() &&
                from_format.is_rgb() != to_format.is_rgb();
            const Strict entailed = layout_class_changed ? Strict::AllowRecorded : options.strict;

            record_or_refuse(entailed, conversions, "color_matrix", to_string(from_color.matrix),
                             to_string(to_color.matrix), cause, performed_by, context);
            record_or_refuse(entailed, conversions, "chroma_location",
                             to_string(from_color.chroma_location),
                             to_string(to_color.chroma_location), cause, performed_by, context);

            record_or_refuse(options.strict, conversions, "color_range",
                             to_string(from_color.range), to_string(to_color.range), cause,
                             performed_by, context);
            record_or_refuse(options.strict, conversions, "primaries",
                             to_string(from_color.primaries), to_string(to_color.primaries),
                             cause, performed_by, context);
            record_or_refuse(options.strict, conversions, "transfer",
                             to_string(from_color.transfer), to_string(to_color.transfer), cause,
                             performed_by, context);
        }

        /// True when chroma planes get smaller, which decides whether the
        /// downsampling or the upsampling kernel applies.
        bool chroma_shrinks(const PixelFormat& from, const PixelFormat& to)
        {
            if (!from.is_valid() || !to.is_valid())
            {
                return false;
            }
            const int from_area = from.log2_chroma_width() + from.log2_chroma_height();
            const int to_area = to.log2_chroma_width() + to.log2_chroma_height();
            return to_area > from_area;
        }

        void validate(const Frame& frame, const ConvertOptions& options)
        {
            if (frame.empty())
            {
                throw ConfigError("convert() received an empty frame");
            }
            if (!options.pixel_format.is_valid())
            {
                throw ConfigError("convert() requires a valid target pixel format");
            }

            frame.color().require_fully_specified("convert() source");
            options.color.require_fully_specified("convert() target");

            // swscale's matrix-and-range path leaves gamut and tone curve
            // alone. Labeling its output with other primaries or another
            // transfer would claim a conversion that never happened.
            if (frame.color().primaries != options.color.primaries ||
                frame.color().transfer != options.color.transfer)
            {
                throw NotImplemented("convert() between primaries or transfer characteristics (from " +
                                     to_string(frame.color().primaries) + "/" + to_string(frame.color().transfer) +
                                     " to " + to_string(options.color.primaries) + "/" +
                                     to_string(options.color.transfer) +
                                     "); reinterpret() relabels them without converting");
            }

            if (options.backend == ResizeBackend::Zscale)
            {
                // zscale is a libavfilter filter rather than a swscale mode, so
                // it goes through the filter path instead of this one.
                capabilities().require_resize_backend(options.backend);
                throw NotImplemented("convert() with ResizeBackend::Zscale");
            }
        }
    }

    FrameResult convert(const Frame& frame, const ConvertOptions& options)
    {
        const detail::StageClock clock;
        validate(frame, options);

        const PixelFormat source_format = frame.pixel_format();
        const KernelSpec& kernel =
            chroma_shrinks(source_format, options.pixel_format) ? options.chroma_down
                                                                : options.chroma_up;

        StageRecord record;
        record.kind = StageKind::Convert;
        record.implementation = "swscale";
        record.input = frame.describe();
        record.transform = CoordinateTransform::identity();

        // Conversions are accounted for before anything happens, so a refusal
        // costs nothing and leaves no partial work behind.
        account_for_changes(frame, options, record.conversions, "swscale");

        Frame output = Frame::allocate(frame.width(), frame.height(), options.pixel_format,
                                       options.color);

        detail::SwsContextPtr scaler(LL_FF_TIMED(sws_getContext(
            frame.width(), frame.height(), static_cast<AVPixelFormat>(source_format.raw()),
            frame.width(), frame.height(),
            static_cast<AVPixelFormat>(options.pixel_format.raw()),
            sws_flag_for(kernel.kernel), nullptr, nullptr, nullptr)));
        if (!scaler)
        {
            throw ConfigError("swscale cannot convert " + source_format.name() + " to " +
                              options.pixel_format.name());
        }

        // Color handling is set explicitly rather than left to swscale's
        // defaults, which assume BT.601 limited range regardless of the tags.
        // That assumption is exactly the silent mix-up this library models.
        //
        // swscale rejects the call outright for conversions where it has no
        // YUV side to apply coefficients to, such as RGB to RGB. That is not an
        // error, and there is nothing to set, so it is only enforced where the
        // settings can actually take effect.
        const bool colorspace_applies =
            !source_format.is_rgb() || !options.pixel_format.is_rgb();
        if (colorspace_applies)
        {
            LL_FF_CHECK(sws_setColorspaceDetails(
                scaler.get(), coefficients_for(frame.color().matrix),
                is_limited(frame.color().range) ? 0 : 1, coefficients_for(options.color.matrix),
                is_limited(options.color.range) ? 0 : 1, 0, 1 << 16, 1 << 16));
        }

        LL_FF_CHECK(sws_scale(scaler.get(), frame.raw()->data, frame.raw()->linesize, 0,
                              frame.height(), output.raw()->data, output.raw()->linesize));

        output.set_pts(frame.pts());
        output.set_time_base(frame.time_base());
        output.sync_color_to_av_frame();

        // The primaries and transfer stay as they were (see validate()), so an
        // ICC profile still describes the samples.
        output.copy_embedded_from(frame);

        record.output = output.describe();
        record.params = json::object({
            {"pix_fmt", options.pixel_format.name()},
            {"color", options.color.to_json()},
            {"kernel", kernel.to_json()},
            {"kernel_role", chroma_shrinks(source_format, options.pixel_format)
                                ? "chroma_down"
                                : "chroma_up"},
            {"backend", to_string(options.backend)},
            {"strict", to_string(options.strict)},
        });
        record.duration_ms = clock.duration_ms();
        record.ffmpeg_duration_ms = clock.ffmpeg_duration_ms();

        return FrameResult{std::move(output), std::move(record)};
    }

    FrameResult convert(const Frame& frame, const PixelFormat pixel_format,
                        const ColorSpec& color, const Strict strict)
    {
        ConvertOptions options;
        options.pixel_format = pixel_format;
        options.color = color;
        options.strict = strict;
        return convert(frame, options);
    }

    FrameResult chroma_roundtrip(const Frame& frame, const ChromaRoundtripOptions& options)
    {
        const detail::StageClock clock;

        if (frame.empty())
        {
            throw ConfigError("chroma_roundtrip() received an empty frame");
        }
        if (options.subsampling == Subsampling::Rgb || options.subsampling == Subsampling::Gray)
        {
            throw ConfigError("chroma_roundtrip() needs a chroma-bearing subsampling, got " +
                              to_string(options.subsampling));
        }
        options.color.require_fully_specified("chroma_roundtrip()");

        const PixelFormat source_format = frame.pixel_format();
        const int depth = options.intermediate_bit_depth.value_or(source_format.bit_depth());
        const PixelFormat intermediate = PixelFormat::planar_yuv(options.subsampling, depth);

        // Down to the subsampled intermediate...
        // The trip changes the matrix, range and chroma siting; the samples
        // keep the source's gamut and tone curve throughout.
        ColorSpec intermediate_color = options.color;
        intermediate_color.primaries = frame.color().primaries;
        intermediate_color.transfer = frame.color().transfer;

        ConvertOptions to_intermediate;
        to_intermediate.pixel_format = intermediate;
        to_intermediate.color = intermediate_color;
        to_intermediate.chroma_down = options.chroma_down;
        to_intermediate.chroma_up = options.chroma_down;
        to_intermediate.backend = options.backend;
        to_intermediate.strict = options.strict;
        FrameResult to_yuv = convert(frame, to_intermediate);

        // ...and back to where it started. The source format and color are the
        // target, so the only thing the round trip leaves behind is the chroma
        // information the subsampling discarded.
        ConvertOptions back_to_source;
        back_to_source.pixel_format = source_format;
        back_to_source.color = frame.color();
        back_to_source.chroma_down = options.chroma_up;
        back_to_source.chroma_up = options.chroma_up;
        back_to_source.backend = options.backend;
        back_to_source.strict = options.strict;
        FrameResult back = convert(to_yuv.frame, back_to_source);

        StageRecord record;
        record.kind = StageKind::ChromaRoundtrip;
        record.implementation = to_string(options.backend);
        record.input = frame.describe();
        record.output = back.frame.describe();
        record.transform = CoordinateTransform::identity();

        // Both legs' conversions are kept, in order: the pair is the trace, and
        // reporting only the endpoints would hide that anything happened at all.
        record.conversions = std::move(to_yuv.record.conversions);
        record.conversions.insert(record.conversions.end(), back.record.conversions.begin(),
                                  back.record.conversions.end());

        record.params = json::object({
            {"subsampling", to_string(options.subsampling)},
            {"intermediate_pix_fmt", intermediate.name()},
            {"color", options.color.to_json()},
            {"chroma_down", options.chroma_down.to_json()},
            {"chroma_up", options.chroma_up.to_json()},
            {"backend", to_string(options.backend)},
            {"strict", to_string(options.strict)},
        });
        record.duration_ms = clock.duration_ms();
        record.ffmpeg_duration_ms = clock.ffmpeg_duration_ms();

        return FrameResult{std::move(back.frame), std::move(record)};
    }

    FrameResult reinterpret(const Frame& frame, const ColorSpec& as_color)
    {
        if (frame.empty())
        {
            throw ConfigError("reinterpret() received an empty frame");
        }

        StageRecord record;
        record.kind = StageKind::Reinterpret;
        record.implementation = "relabel";
        record.input = frame.describe();
        record.transform = CoordinateTransform::identity();

        // The samples are untouched, so the frame is shared rather than copied;
        // only the label attached to it changes.
        Frame output = frame;
        const ColorSpec before = output.color();
        output.set_color(as_color);
        output.sync_color_to_av_frame();

        // Recorded unconditionally, and never refused. Relabeling is lossless
        // and is the operation the caller asked for, but it still changes how
        // every downstream stage reads the samples, so it has to be visible.
        const auto note = [&record](const char* property, std::string from, std::string to) {
            if (from != to)
            {
                record.conversions.push_back(ConversionEvent{
                    property, std::move(from), std::move(to), ConversionCause::Requested,
                    "relabel"});
            }
        };
        note("color_matrix", to_string(before.matrix), to_string(as_color.matrix));
        note("color_range", to_string(before.range), to_string(as_color.range));
        note("primaries", to_string(before.primaries), to_string(as_color.primaries));
        note("transfer", to_string(before.transfer), to_string(as_color.transfer));
        note("chroma_location", to_string(before.chroma_location),
             to_string(as_color.chroma_location));

        // A profile describes the primaries and tone curve the samples had;
        // relabeling either makes it describe something else.
        if (const IccProfile* profile = output.icc_profile();
            profile != nullptr && (before.primaries != as_color.primaries || before.transfer != as_color.transfer))
        {
            note("icc_profile", profile->info.name(), "dropped");
            output.clear_icc_profile();
        }

        record.output = output.describe();
        record.params = json::object({
            {"as_color", as_color.to_json()},
            {"samples_modified", false},
        });

        return FrameResult{std::move(output), std::move(record)};
    }
}
