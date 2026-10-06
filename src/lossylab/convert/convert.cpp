#include "lossylab/convert/convert.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/detail/ff_error.hpp"
#include "lossylab/detail/ff_ptr.hpp"
#include "lossylab/env/build_info.hpp"
#include "lossylab/env/capabilities.hpp"
#include "lossylab/io/icc_profile.hpp"

#include <lcms2.h>

#include <array>
#include <cstdint>
#include <memory>
#include <type_traits>

extern "C" {
#include <libavutil/csp.h>
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

        /// Without SWS_ACCURATE_RND, swscale converts 4:2:0 and 4:2:2 of even
        /// height to RGB through a shortcut that repeats chroma samples
        /// whatever kernel was asked for; without SWS_FULL_CHR_H_INT, RGB
        /// output interpolates chroma horizontally only at odd widths.
        int sws_flags_for(const KernelSpec& kernel, const PixelFormat& from, const PixelFormat& to)
        {
            int flags = sws_flag_for(kernel.kernel) | SWS_ACCURATE_RND | SWS_BITEXACT;
            if (from.is_rgb() || to.is_rgb())
            {
                flags |= SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP;
            }
            return flags;
        }

        /// True for RGB, gray and palette formats of at most 8 bits, whose
        /// samples are already the values an 8-bit RGB output holds.
        bool is_narrow_rgb_or_gray(const PixelFormat& format)
        {
            const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(format.raw()));
            return format.bit_depth() <= 8 &&
                   ((descriptor->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)) != 0 || format.is_gray());
        }

        /// rgb24 or rgba to rgb48 or rgba64, each value v becoming v * 257,
        /// which swscale's own 8-to-16-bit expansion is not.
        Frame widen_to_16_bits(const Frame& narrow)
        {
            const bool alpha = narrow.pixel_format().has_alpha();
            const int channels = alpha ? 4 : 3;
            Frame wide = Frame::allocate(narrow.width(), narrow.height(),
                                         PixelFormat::from_name(alpha ? "rgba64" : "rgb48"), narrow.color());
            for (int y = 0; y < narrow.height(); ++y)
            {
                const std::uint8_t* narrow_row = narrow.raw()->data[0] + y * narrow.raw()->linesize[0];
                auto* wide_row = reinterpret_cast<std::uint16_t*>(wide.raw()->data[0] + y * wide.raw()->linesize[0]);
                for (int i = 0; i < narrow.width() * channels; ++i)
                {
                    wide_row[i] = static_cast<std::uint16_t>(narrow_row[i] * 257U);
                }
            }
            return wide;
        }

        bool is_srgb(const ColorSpec& color)
        {
            return color.primaries == ColorPrimaries::Bt709 && color.transfer == TransferCharacteristic::Srgb;
        }

        PixelFormat rgb24() { return PixelFormat::from_name("rgb24"); }

        /// Where the colors to convert to sRGB are described.
        enum class ColorSource
        {
            None,
            IccProfile,
            ColorTags,
            CmykFoldedByDecoder
        };

        /// An lcms2 parametric curve: its type and parameters.
        struct ToneCurve
        {
            int type = 1;
            std::array<double, 5> parameters{};
        };

        /// The tone curve of a transfer characteristic, decoding to linear
        /// light. The BT.709 family is the inverse of its camera curve.
        std::optional<ToneCurve> tone_curve_for(const TransferCharacteristic transfer)
        {
            switch (transfer)
            {
            case TransferCharacteristic::Srgb:
                return ToneCurve{4, {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045}};
            case TransferCharacteristic::Bt709:
            case TransferCharacteristic::Smpte170m:
            case TransferCharacteristic::Bt2020_10:
            case TransferCharacteristic::Bt2020_12:
                return ToneCurve{4, {1.0 / 0.45, 1.0 / 1.099, 0.099 / 1.099, 1.0 / 4.5, 0.081}};
            case TransferCharacteristic::Gamma22: return ToneCurve{1, {2.2}};
            case TransferCharacteristic::Gamma28: return ToneCurve{1, {2.8}};
            case TransferCharacteristic::Linear: return ToneCurve{1, {1.0}};
            default: return std::nullopt;
            }
        }

        /// What describes the source colors when converting to sRGB rgb24.
        ColorSource color_source_for(const Frame& frame, const ConvertOptions& options)
        {
            if (options.pixel_format != rgb24() || options.icc != IccHandling::Convert || !is_srgb(options.color))
            {
                return ColorSource::None;
            }
            if (const IccProfile* profile = frame.icc_profile())
            {
                const IccProfileInfo& info = profile->info;
                if (info.data_color_space == "CMYK")
                {
                    return ColorSource::CmykFoldedByDecoder;
                }
                const bool srgb_equivalent =
                    info.known_as == "sRGB" ||
                    (info.primaries == ColorPrimaries::Bt709 && info.transfer == TransferCharacteristic::Srgb) ||
                    (info.data_color_space == "GRAY" && info.transfer == TransferCharacteristic::Srgb);
                if (srgb_equivalent)
                {
                    return ColorSource::None;
                }
                if (info.data_color_space == "RGB" ||
                    (info.data_color_space == "GRAY" && frame.pixel_format().is_gray()))
                {
                    return ColorSource::IccProfile;
                }
                throw NotImplemented("convert() to sRGB from a " + info.data_color_space + " ICC profile on " +
                                     frame.pixel_format().name());
            }
            if (is_srgb(frame.color()))
            {
                return ColorSource::None;
            }
            if (!tone_curve_for(frame.color().transfer).has_value())
            {
                throw NotImplemented("convert() to sRGB from transfer " + to_string(frame.color().transfer));
            }
            return ColorSource::ColorTags;
        }

        struct LcmsContextDeleter
        {
            void operator()(cmsContext context) const { cmsDeleteContext(context); }
        };
        struct LcmsProfileDeleter
        {
            void operator()(void* profile) const { cmsCloseProfile(profile); }
        };
        struct LcmsTransformDeleter
        {
            void operator()(void* transform) const { cmsDeleteTransform(transform); }
        };
        using LcmsContext = std::unique_ptr<std::remove_pointer_t<cmsContext>, LcmsContextDeleter>;
        using LcmsProfile = std::unique_ptr<void, LcmsProfileDeleter>;
        using LcmsTransform = std::unique_ptr<void, LcmsTransformDeleter>;

        /// A matrix/shaper profile built from a frame's primaries and transfer.
        LcmsProfile profile_from_tags(cmsContext context, const ColorSpec& color)
        {
            const AVColorPrimariesDesc* description =
                av_csp_primaries_desc_from_id(static_cast<AVColorPrimaries>(color.primaries));
            const std::optional<ToneCurve> tone = tone_curve_for(color.transfer);
            if (description == nullptr || !tone.has_value())
            {
                throw NotImplemented("convert() to sRGB from " + color.describe());
            }
            const cmsCIExyY white = {av_q2d(description->wp.x), av_q2d(description->wp.y), 1.0};
            const cmsCIExyYTRIPLE primaries = {
                {av_q2d(description->prim.r.x), av_q2d(description->prim.r.y), 1.0},
                {av_q2d(description->prim.g.x), av_q2d(description->prim.g.y), 1.0},
                {av_q2d(description->prim.b.x), av_q2d(description->prim.b.y), 1.0},
            };
            cmsToneCurve* curve = cmsBuildParametricToneCurve(context, tone->type, tone->parameters.data());
            if (curve == nullptr)
            {
                throw ConfigError("lcms2 could not build the tone curve of " + to_string(color.transfer));
            }
            cmsToneCurve* curves[3] = {curve, curve, curve};
            LcmsProfile profile(cmsCreateRGBProfileTHR(context, &white, &primaries, curves));
            cmsFreeToneCurve(curve);
            if (!profile)
            {
                throw ConfigError("lcms2 could not build a profile for " + color.describe());
            }
            return profile;
        }

        /// Converts the 16-bit RGB samples of `wide` (rgb48 or rgba64; for a
        /// gray profile, the gray value is read from the red channel) to sRGB
        /// rgb48 in `target`, relative colorimetric with black point
        /// compensation.
        void convert_to_srgb(const Frame& wide, Frame& target, const ColorSource source, const Frame& frame)
        {
            const LcmsContext context(cmsCreateContext(nullptr, nullptr));
            if (!context)
            {
                throw ConfigError("lcms2 could not create a context");
            }
            LcmsProfile source_profile;
            bool gray = false;
            if (source == ColorSource::IccProfile)
            {
                const std::vector<std::uint8_t>& bytes = frame.icc_profile()->bytes;
                source_profile.reset(cmsOpenProfileFromMemTHR(context.get(), bytes.data(),
                                                              static_cast<cmsUInt32Number>(bytes.size())));
                if (!source_profile)
                {
                    throw ConfigError("lcms2 cannot read the ICC profile " + frame.icc_profile()->info.name());
                }
                gray = frame.icc_profile()->info.data_color_space == "GRAY";
            }
            else
            {
                source_profile = profile_from_tags(context.get(), frame.color());
            }
            const LcmsProfile srgb(cmsCreate_sRGBProfileTHR(context.get()));

            const cmsUInt32Number channels = wide.pixel_format().has_alpha() ? 4U : 3U;
            const cmsUInt32Number input_format =
                gray ? (COLORSPACE_SH(PT_GRAY) | CHANNELS_SH(1) | BYTES_SH(2) | EXTRA_SH(channels - 1))
                     : (COLORSPACE_SH(PT_RGB) | CHANNELS_SH(3) | BYTES_SH(2) | EXTRA_SH(channels - 3));
            // For 16-bit samples lcms2 would otherwise precompute a 33-point
            // lookup table, whose interpolation is off by up to 11 of 255
            // where saturated colors clip at the sRGB gamut.
            const LcmsTransform transform(
                cmsCreateTransformTHR(context.get(), source_profile.get(), input_format, srgb.get(), TYPE_RGB_16,
                                      INTENT_RELATIVE_COLORIMETRIC,
                                      cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_NOOPTIMIZE));
            if (!transform)
            {
                throw ConfigError("lcms2 cannot convert from the source colors to sRGB");
            }
            cmsDoTransformLineStride(transform.get(), wide.raw()->data[0], target.raw()->data[0],
                                     static_cast<cmsUInt32Number>(wide.width()),
                                     static_cast<cmsUInt32Number>(wide.height()),
                                     static_cast<cmsUInt32Number>(wide.raw()->linesize[0]),
                                     static_cast<cmsUInt32Number>(target.raw()->linesize[0]), 0, 0);
        }

        /// Rounds 16-bit RGB to 8 bits once, multiplying by alpha first when
        /// `alpha` is given: round(value * alpha / 65535 * 255 / 65535).
        void round_to_rgb24(const Frame& colors, const Frame* alpha, Frame& target)
        {
            const int color_channels = colors.pixel_format().has_alpha() ? 4 : 3;
            constexpr std::uint64_t full_alpha = 65535ULL * 65535ULL;
            for (int y = 0; y < target.height(); ++y)
            {
                const auto* color_row =
                    reinterpret_cast<const std::uint16_t*>(colors.raw()->data[0] + y * colors.raw()->linesize[0]);
                const auto* alpha_row = alpha != nullptr ? reinterpret_cast<const std::uint16_t*>(
                                                               alpha->raw()->data[0] + y * alpha->raw()->linesize[0])
                                                         : nullptr;
                std::uint8_t* output_row = target.raw()->data[0] + y * target.raw()->linesize[0];
                for (int x = 0; x < target.width(); ++x)
                {
                    for (int channel = 0; channel < 3; ++channel)
                    {
                        const std::uint32_t value = color_row[x * color_channels + channel];
                        if (alpha_row == nullptr)
                        {
                            output_row[x * 3 + channel] = static_cast<std::uint8_t>((value * 255U + 32767U) / 65535U);
                        }
                        else
                        {
                            const std::uint64_t weighted = std::uint64_t{value} * alpha_row[x * 4 + 3] * 255U;
                            output_row[x * 3 + channel] =
                                static_cast<std::uint8_t>((weighted + full_alpha / 2) / full_alpha);
                        }
                    }
                }
            }
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

        /// Converts the frame with swscale at its own size, one thread.
        Frame run_swscale(const Frame& frame, const PixelFormat target_format, const ColorSpec& target_color,
                          const KernelSpec& kernel)
        {
            const PixelFormat source_format = frame.pixel_format();
            Frame output = Frame::allocate(frame.width(), frame.height(), target_format, target_color);

            double parameters[2] = {kernel.params.param_a.value_or(SWS_PARAM_DEFAULT),
                                    kernel.params.param_b.value_or(SWS_PARAM_DEFAULT)};
            detail::SwsContextPtr scaler(LL_FF_TIMED(sws_getContext(
                frame.width(), frame.height(), static_cast<AVPixelFormat>(source_format.raw()), frame.width(),
                frame.height(), static_cast<AVPixelFormat>(target_format.raw()),
                sws_flags_for(kernel, source_format, target_format), nullptr, nullptr, parameters)));
            if (!scaler)
            {
                throw ConfigError("swscale cannot convert " + source_format.name() + " to " + target_format.name());
            }

            // Color handling is set explicitly rather than left to swscale's
            // defaults, which assume BT.601 limited range regardless of the tags.
            //
            // swscale rejects the call outright for conversions where it has no
            // YUV side to apply coefficients to, such as RGB to RGB. There is
            // nothing to set then.
            if (!source_format.is_rgb() || !target_format.is_rgb())
            {
                LL_FF_CHECK(sws_setColorspaceDetails(
                    scaler.get(), coefficients_for(frame.color().matrix), is_limited(frame.color().range) ? 0 : 1,
                    coefficients_for(target_color.matrix), is_limited(target_color.range) ? 0 : 1, 0, 1 << 16,
                    1 << 16));
            }

            LL_FF_CHECK(sws_scale(scaler.get(), frame.raw()->data, frame.raw()->linesize, 0, frame.height(),
                                  output.raw()->data, output.raw()->linesize));
            return output;
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

        void validate(const Frame& frame, const ConvertOptions& options, const ColorSource source)
        {
            // swscale's matrix-and-range path leaves gamut and tone curve
            // alone. Labeling its output with other primaries or another
            // transfer would claim a conversion that never happened.
            const bool colors_converted = source == ColorSource::IccProfile || source == ColorSource::ColorTags;
            if (!colors_converted && (frame.color().primaries != options.color.primaries ||
                                      frame.color().transfer != options.color.transfer))
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

    std::string to_string(const IccHandling handling)
    {
        switch (handling)
        {
        case IccHandling::Convert: return "convert";
        case IccHandling::Ignore: return "ignore";
        }
        return "convert";
    }

    IccHandling icc_handling_from_string(const std::string_view name)
    {
        if (name == "convert") { return IccHandling::Convert; }
        if (name == "ignore") { return IccHandling::Ignore; }
        throw ConfigError("unknown ICC handling '" + std::string(name) + "'");
    }

    std::string to_string(const AlphaHandling handling)
    {
        switch (handling)
        {
        case AlphaHandling::OverBlack: return "over_black";
        case AlphaHandling::Discard: return "discard";
        }
        return "over_black";
    }

    AlphaHandling alpha_handling_from_string(const std::string_view name)
    {
        if (name == "over_black") { return AlphaHandling::OverBlack; }
        if (name == "discard") { return AlphaHandling::Discard; }
        throw ConfigError("unknown alpha handling '" + std::string(name) + "'");
    }

    FrameResult convert(const Frame& frame, const ConvertOptions& options)
    {
        const detail::StageClock clock;
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
        const ColorSource color_source = color_source_for(frame, options);
        validate(frame, options, color_source);

        const PixelFormat source_format = frame.pixel_format();
        const bool chroma_down = chroma_shrinks(source_format, options.pixel_format);
        const KernelSpec& kernel = chroma_down ? options.chroma_down : options.chroma_up;
        const bool to_rgb24 = options.pixel_format == rgb24();

        ConvertEvidence evidence;
        evidence.kernel_role = chroma_down ? "chroma_down" : "chroma_up";
        StageRecord record;
        record.implementation = "swscale";
        record.input = frame.describe();
        record.transform = CoordinateTransform::identity();

        // Conversions are accounted for before anything happens, so a refusal
        // costs nothing and leaves no partial work behind.
        account_for_changes(frame, options, record.conversions, "swscale");

        const IccProfile* profile = frame.icc_profile();
        if (color_source == ColorSource::IccProfile)
        {
            record_or_refuse(options.strict, record.conversions, "icc_profile", profile->info.name(), "sRGB",
                             ConversionCause::Requested, "lcms2", "convert");
            evidence.color_transform = "icc_profile";
            evidence.icc_profile_sha256 = detail::sha256_hex(profile->bytes);
        }
        else if (color_source == ColorSource::ColorTags)
        {
            evidence.color_transform = "color_tags";
        }
        else if (color_source == ColorSource::CmykFoldedByDecoder)
        {
            record.conversions.push_back(ConversionEvent{"icc_profile", profile->info.name(),
                                                         "not applied: CMYK folded to RGB by the decoder",
                                                         ConversionCause::Requested, "ffmpeg"});
        }

        const bool drops_alpha = source_format.has_alpha() && !options.pixel_format.has_alpha();
        const bool premultiplied = frame.raw()->alpha_mode == AVALPHA_MODE_PREMULTIPLIED;
        const bool over_black = to_rgb24 && drops_alpha && options.alpha == AlphaHandling::OverBlack && !premultiplied;
        if (drops_alpha)
        {
            evidence.alpha = over_black ? "over_black" : (premultiplied ? "premultiplied" : "discarded");
            record_or_refuse(Strict::AllowRecorded, record.conversions, "alpha",
                             premultiplied ? "premultiplied" : "straight",
                             over_black ? "over black" : "dropped", ConversionCause::Requested,
                             over_black ? "lossylab" : "swscale", "convert");
        }

        Frame output;
        if (to_rgb24)
        {
            // One path for every source: swscale to 16-bit RGB, which never
            // dithers, then colors, alpha and a single rounding here.
            ColorSpec wide_color = frame.color();
            wide_color.matrix = ColorMatrix::Rgb;
            wide_color.range = ColorRange::Full;
            wide_color.chroma_location = ChromaLocation::Unspecified;
            const bool alpha = source_format.has_alpha();
            const Frame wide =
                is_narrow_rgb_or_gray(source_format)
                    ? widen_to_16_bits(
                          run_swscale(frame, PixelFormat::from_name(alpha ? "rgba" : "rgb24"), wide_color, kernel))
                    : run_swscale(frame, PixelFormat::from_name(alpha ? "rgba64" : "rgb48"), wide_color, kernel);

            Frame colors = wide;
            if (color_source == ColorSource::IccProfile || color_source == ColorSource::ColorTags)
            {
                colors = Frame::allocate(frame.width(), frame.height(), PixelFormat::from_name("rgb48"),
                                         options.color);
                convert_to_srgb(wide, colors, color_source, frame);
            }
            output = Frame::allocate(frame.width(), frame.height(), options.pixel_format, options.color, 1);
            round_to_rgb24(colors, over_black ? &wide : nullptr, output);
        }
        else
        {
            output = run_swscale(frame, options.pixel_format, options.color, kernel);
        }

        output.set_pts(frame.pts());
        output.set_time_base(frame.time_base());
        output.sync_color_to_av_frame();

        // A profile that was applied, or that describes CMYK the decoder
        // already folded to RGB, no longer describes the samples.
        output.copy_embedded_from(frame);
        if (color_source != ColorSource::None)
        {
            output.clear_icc_profile();
        }

        record.evidence = std::move(evidence);
        record.output = output.describe();
        record.duration_ms = clock.duration_ms();
        record.ffmpeg_duration_ms = clock.ffmpeg_duration_ms();

        return FrameResult{std::move(output), std::move(record), options};
    }

    FrameResult convert(const Frame& frame, const PixelFormat pixel_format,
                        const ColorSpec& color, const Strict strict)
    {
        ConvertOptions options;
        options.pixel_format = pixel_format;
        options.color = color;
        options.icc = IccHandling::Ignore;
        options.alpha = AlphaHandling::Discard;
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
        record.evidence = ChromaRoundtripEvidence{intermediate};
        record.implementation = to_string(options.backend);
        record.input = frame.describe();
        record.output = back.frame.describe();
        record.transform = CoordinateTransform::identity();

        // Both legs' conversions are kept, in order: the pair is the trace, and
        // reporting only the endpoints would hide that anything happened at all.
        record.conversions = std::move(to_yuv.record.conversions);
        record.conversions.insert(record.conversions.end(), back.record.conversions.begin(),
                                  back.record.conversions.end());

        ChromaRoundtripOptions configuration = options;
        configuration.intermediate_bit_depth = depth;
        record.duration_ms = clock.duration_ms();
        record.ffmpeg_duration_ms = clock.ffmpeg_duration_ms();

        return FrameResult{std::move(back.frame), std::move(record), std::move(configuration)};
    }

    FrameResult reinterpret(const Frame& frame, const ReinterpretOptions& options)
    {
        if (frame.empty())
        {
            throw ConfigError("reinterpret() received an empty frame");
        }
        const ColorSpec& as_color = options.as_color;

        StageRecord record;
        record.evidence = ReinterpretEvidence{};
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

        return FrameResult{std::move(output), std::move(record), options};
    }

    FrameResult reinterpret(const Frame& frame, const ColorSpec& as_color)
    {
        return reinterpret(frame, ReinterpretOptions{as_color});
    }
}
