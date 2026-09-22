#include "lossylab/env/capabilities.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/core/json_io.hpp"
#include "lossylab/env/build_info.hpp"

#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace lossylab
{
    namespace
    {
        std::string option_type_name(const AVOptionType type)
        {
            switch (type)
            {
            case AV_OPT_TYPE_FLAGS: return "flags";
            case AV_OPT_TYPE_INT:
            case AV_OPT_TYPE_INT64:
            case AV_OPT_TYPE_UINT:
            case AV_OPT_TYPE_UINT64: return "int";
            case AV_OPT_TYPE_DOUBLE:
            case AV_OPT_TYPE_FLOAT: return "double";
            case AV_OPT_TYPE_STRING: return "string";
            case AV_OPT_TYPE_RATIONAL: return "rational";
            case AV_OPT_TYPE_BOOL: return "bool";
            case AV_OPT_TYPE_CONST: return "const";
            case AV_OPT_TYPE_PIXEL_FMT: return "pix_fmt";
            case AV_OPT_TYPE_COLOR: return "color";
            case AV_OPT_TYPE_DICT: return "dict";
            case AV_OPT_TYPE_BINARY: return "binary";
            default: return "other";
            }
        }

        std::string default_value_text(const AVOption& option)
        {
            switch (option.type)
            {
            case AV_OPT_TYPE_FLAGS:
            case AV_OPT_TYPE_INT:
            case AV_OPT_TYPE_INT64:
            case AV_OPT_TYPE_UINT:
            case AV_OPT_TYPE_UINT64:
            case AV_OPT_TYPE_BOOL:
                return std::to_string(option.default_val.i64);
            case AV_OPT_TYPE_DOUBLE:
            case AV_OPT_TYPE_FLOAT:
                return std::to_string(option.default_val.dbl);
            case AV_OPT_TYPE_STRING:
                return option.default_val.str != nullptr ? option.default_val.str : "";
            default:
                return "";
            }
        }

        /// Reads an AVClass's option table into the schema the API exposes.
        ///
        /// AVOption represents an enum as the parent option plus a run of
        /// AV_OPT_TYPE_CONST entries sharing its unit, so the constants are
        /// folded back into the option they belong to.
        std::vector<OptionSchema> read_options(const AVClass* klass)
        {
            std::vector<OptionSchema> schemas;
            if (klass == nullptr)
            {
                return schemas;
            }

            const AVOption* option = nullptr;
            while ((option = av_opt_next(&klass, option)) != nullptr)
            {
                if (option->type == AV_OPT_TYPE_CONST)
                {
                    // Attach to the most recent option sharing this unit.
                    if (option->unit == nullptr)
                    {
                        continue;
                    }
                    for (auto it = schemas.rbegin(); it != schemas.rend(); ++it)
                    {
                        if (it->type == "enum" || it->type == "flags" || it->type == "int")
                        {
                            it->choices.emplace_back(option->name);
                            break;
                        }
                    }
                    continue;
                }

                OptionSchema schema;
                schema.name = option->name;
                schema.help = option->help != nullptr ? option->help : "";
                schema.type = option_type_name(option->type);
                schema.default_value = default_value_text(*option);
                if (option->min != -INFINITY) { schema.min_value = option->min; }
                if (option->max != INFINITY) { schema.max_value = option->max; }
                schemas.push_back(std::move(schema));
            }
            return schemas;
        }

        std::vector<PixelFormat> read_pixel_formats(const AVCodec& codec)
        {
            std::vector<PixelFormat> formats;

            // FFmpeg 7.1 replaced codec->pix_fmts with this query, which also
            // covers codecs that compute their format list at runtime.
            const void* config = nullptr;
            int count = 0;
            if (avcodec_get_supported_config(nullptr, &codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                             &config, &count) < 0 ||
                config == nullptr)
            {
                return formats;
            }

            const auto* list = static_cast<const AVPixelFormat*>(config);
            for (int i = 0; i < count; ++i)
            {
                formats.push_back(PixelFormat::from_raw(list[i]));
            }
            return formats;
        }

        bool is_hardware_codec(const AVCodec& codec)
        {
            return (codec.capabilities & AV_CODEC_CAP_HARDWARE) != 0 ||
                   (codec.capabilities & AV_CODEC_CAP_HYBRID) != 0;
        }

        CodecInfo read_codec(const AVCodec& codec, const bool encoder)
        {
            CodecInfo info;
            info.name = codec.name != nullptr ? codec.name : "";
            info.long_name = codec.long_name != nullptr ? codec.long_name : "";
            const char* codec_name = avcodec_get_name(codec.id);
            info.codec_name = codec_name != nullptr ? codec_name : "";
            info.is_encoder = encoder;
            info.is_hardware = is_hardware_codec(codec);
            info.experimental = (codec.capabilities & AV_CODEC_CAP_EXPERIMENTAL) != 0;
            info.pixel_formats = read_pixel_formats(codec);
            info.options = read_options(codec.priv_class);
            return info;
        }

        template <typename T>
        const T* find_by_name(const std::vector<T>& items, const std::string_view name) noexcept
        {
            const auto it = std::find_if(items.begin(), items.end(),
                                         [name](const T& item) { return item.name == name; });
            return it == items.end() ? nullptr : &*it;
        }

        /// Walks a codec's candidate encoder or decoder names in preference
        /// order and returns the first this build provides.
        const CodecInfo* first_available(const std::vector<CodecInfo>& pool,
                                         const std::vector<std::string>& candidates) noexcept
        {
            for (const std::string& name : candidates)
            {
                if (const CodecInfo* found = find_by_name(pool, std::string_view(name)))
                {
                    return found;
                }
            }
            return nullptr;
        }
    }

    // -----------------------------------------------------------------------
    // Serialization
    // -----------------------------------------------------------------------

    json::Value OptionSchema::to_json() const
    {
        return json::object({
            {"name", name},
            {"type", type},
            {"help", help},
            {"default", default_value},
            {"min", json::optional_or_null(min_value)},
            {"max", json::optional_or_null(max_value)},
            {"choices", json::to_array(choices)},
        });
    }

    bool CodecInfo::accepts(const PixelFormat& format) const noexcept
    {
        if (pixel_formats.empty())
        {
            // No declared list means the codec did not narrow it, not that it
            // rejects everything.
            return true;
        }
        return std::find(pixel_formats.begin(), pixel_formats.end(), format) !=
               pixel_formats.end();
    }

    const OptionSchema* CodecInfo::find_option(const std::string_view option_name) const noexcept
    {
        return find_by_name(options, option_name);
    }

    json::Value CodecInfo::to_json() const
    {
        return json::object({
            {"name", name},
            {"long_name", long_name},
            {"codec", codec_name},
            {"encoder", is_encoder},
            {"hardware", is_hardware},
            {"experimental", experimental},
            {"pixel_formats", json::to_array(pixel_formats)},
            {"options", json::to_array(options)},
        });
    }

    json::Value FilterInfo::to_json() const
    {
        return json::object({
            {"name", name},
            {"description", description},
            {"inputs", input_count},
            {"outputs", output_count},
            {"dynamic_inputs", dynamic_inputs},
            {"dynamic_outputs", dynamic_outputs},
            {"slice_threads", supports_slice_threads},
            {"options", json::to_array(options)},
        });
    }

    // -----------------------------------------------------------------------
    // Lookup
    // -----------------------------------------------------------------------

    const CodecInfo* Capabilities::find_encoder(const std::string_view name) const noexcept
    {
        return find_by_name(m_encoders, name);
    }

    const CodecInfo* Capabilities::find_decoder(const std::string_view name) const noexcept
    {
        return find_by_name(m_decoders, name);
    }

    const FilterInfo* Capabilities::find_filter(const std::string_view name) const noexcept
    {
        return find_by_name(m_filters, name);
    }

    bool Capabilities::has_encoder(const std::string_view name) const noexcept
    {
        return find_encoder(name) != nullptr;
    }

    bool Capabilities::has_decoder(const std::string_view name) const noexcept
    {
        return find_decoder(name) != nullptr;
    }

    bool Capabilities::has_filter(const std::string_view name) const noexcept
    {
        return find_filter(name) != nullptr;
    }

    bool Capabilities::has_hardware_device(const std::string_view name) const noexcept
    {
        return find_by_name(m_hardware_devices, name) != nullptr;
    }

    bool hardware_device_usable(const std::string_view name)
    {
        const AVHWDeviceType type = av_hwdevice_find_type_by_name(std::string(name).c_str());
        if (type == AV_HWDEVICE_TYPE_NONE)
        {
            return false;
        }

        AVBufferRef* device = nullptr;
        const bool opened = av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0) >= 0;
        av_buffer_unref(&device);
        return opened;
    }

    const CodecInfo* Capabilities::select_encoder(const ImageCodec codec) const noexcept
    {
        return first_available(m_encoders, encoder_candidates(codec));
    }

    const CodecInfo* Capabilities::select_encoder(const VideoCodec codec,
                                                  const EncoderBackend backend) const noexcept
    {
        return first_available(m_encoders, encoder_candidates(codec, backend));
    }

    const CodecInfo* Capabilities::select_decoder(const ImageCodec codec) const noexcept
    {
        return first_available(m_decoders, decoder_candidates(codec));
    }

    const CodecInfo* Capabilities::select_decoder(const VideoCodec codec) const noexcept
    {
        return first_available(m_decoders, decoder_candidates(codec));
    }

    const CodecInfo& Capabilities::require_encoder(const ImageCodec codec) const
    {
        if (const CodecInfo* found = select_encoder(codec))
        {
            return *found;
        }
        throw UnsupportedCapability("image encoder", to_string(codec), build_info().build_id);
    }

    const CodecInfo& Capabilities::require_encoder(const VideoCodec codec,
                                                   const EncoderBackend backend) const
    {
        if (const CodecInfo* found = select_encoder(codec, backend))
        {
            return *found;
        }
        throw UnsupportedCapability("video encoder",
                                    to_string(codec) + " (" + to_string(backend) + ")",
                                    build_info().build_id);
    }

    const CodecInfo& Capabilities::require_decoder(const ImageCodec codec) const
    {
        if (const CodecInfo* found = select_decoder(codec))
        {
            return *found;
        }
        throw UnsupportedCapability("image decoder", to_string(codec), build_info().build_id);
    }

    const CodecInfo& Capabilities::require_decoder(const VideoCodec codec) const
    {
        if (const CodecInfo* found = select_decoder(codec))
        {
            return *found;
        }
        throw UnsupportedCapability("video decoder", to_string(codec), build_info().build_id);
    }

    const FilterInfo& Capabilities::require_filter(const std::string_view name) const
    {
        if (const FilterInfo* found = find_filter(name))
        {
            return *found;
        }
        throw UnsupportedCapability("filter", std::string(name), build_info().build_id);
    }

    void Capabilities::require_resize_backend(const ResizeBackend backend) const
    {
        // Called for the throw, not the value.
        static_cast<void>(require_filter(filter_name(backend)));
    }

    void Capabilities::require_metric(const Metric metric) const
    {
        if (!supports(metric))
        {
            throw UnsupportedCapability("metric", to_string(metric), build_info().build_id);
        }
    }

    bool Capabilities::supports(const ImageCodec codec) const noexcept
    {
        return select_encoder(codec) != nullptr;
    }

    bool Capabilities::supports(const VideoCodec codec,
                                const EncoderBackend backend) const noexcept
    {
        return select_encoder(codec, backend) != nullptr;
    }

    bool Capabilities::supports(const ResizeBackend backend) const noexcept
    {
        return has_filter(filter_name(backend));
    }

    bool Capabilities::supports(const Metric metric) const noexcept
    {
        switch (metric)
        {
        case Metric::Psnr: return has_filter("psnr");
        case Metric::Ssim: return has_filter("ssim");
        case Metric::Vmaf: return has_filter("libvmaf");
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // Enumeration
    // -----------------------------------------------------------------------

    Capabilities Capabilities::probe_build()
    {
        Capabilities available;

        void* codec_iter = nullptr;
        const AVCodec* codec = nullptr;
        while ((codec = av_codec_iterate(&codec_iter)) != nullptr)
        {
            if (codec->type != AVMEDIA_TYPE_VIDEO)
            {
                // The library models image and video processing; audio codecs
                // are linked in but never selected.
                continue;
            }
            if (av_codec_is_encoder(codec))
            {
                available.m_encoders.push_back(read_codec(*codec, true));
            }
            if (av_codec_is_decoder(codec))
            {
                available.m_decoders.push_back(read_codec(*codec, false));
            }
        }

        void* filter_iter = nullptr;
        const AVFilter* filter = nullptr;
        while ((filter = av_filter_iterate(&filter_iter)) != nullptr)
        {
            FilterInfo info;
            info.name = filter->name != nullptr ? filter->name : "";
            info.description = filter->description != nullptr ? filter->description : "";
            info.input_count = static_cast<int>(avfilter_filter_pad_count(filter, 0));
            info.output_count = static_cast<int>(avfilter_filter_pad_count(filter, 1));
            info.dynamic_inputs = (filter->flags & AVFILTER_FLAG_DYNAMIC_INPUTS) != 0;
            info.dynamic_outputs = (filter->flags & AVFILTER_FLAG_DYNAMIC_OUTPUTS) != 0;
            info.supports_slice_threads = (filter->flags & AVFILTER_FLAG_SLICE_THREADS) != 0;
            info.options = read_options(filter->priv_class);
            available.m_filters.push_back(std::move(info));
        }

        AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_NONE;
        while ((hw_type = av_hwdevice_iterate_types(hw_type)) != AV_HWDEVICE_TYPE_NONE)
        {
            // Only what the build knows about. Actually opening a device is
            // left to hardware_device_usable(), because it loads vendor
            // drivers and may start threads, which this cached enumeration
            // promises not to do.
            HardwareDeviceInfo device;
            const char* name = av_hwdevice_get_type_name(hw_type);
            device.name = name != nullptr ? name : "";
            available.m_hardware_devices.push_back(std::move(device));
        }

        return available;
    }

    json::Value Capabilities::to_json() const
    {
        json::Array image_array;
        for (const ImageCodec codec : all_image_codecs())
        {
            const CodecInfo* encoder = select_encoder(codec);
            const CodecInfo* decoder = select_decoder(codec);
            image_array.push_back(json::object({
                {"codec", to_string(codec)},
                {"encoder", encoder != nullptr ? json::Value(encoder->name) : json::Value()},
                {"decoder", decoder != nullptr ? json::Value(decoder->name) : json::Value()},
            }));
        }

        json::Array video_array;
        for (const VideoCodec codec : all_video_codecs())
        {
            json::Array backend_array;
            for (const EncoderBackend backend :
                 {EncoderBackend::Software, EncoderBackend::Vaapi, EncoderBackend::Nvenc,
                  EncoderBackend::Qsv, EncoderBackend::VideoToolbox})
            {
                if (const CodecInfo* encoder = select_encoder(codec, backend))
                {
                    backend_array.push_back(json::object({
                        {"backend", to_string(backend)},
                        {"encoder", encoder->name},
                    }));
                }
            }
            const CodecInfo* decoder = select_decoder(codec);
            video_array.push_back(json::object({
                {"codec", to_string(codec)},
                {"encoders", json::array(std::move(backend_array))},
                {"decoder", decoder != nullptr ? json::Value(decoder->name) : json::Value()},
            }));
        }

        json::Array backend_array;
        for (const ResizeBackend backend : {ResizeBackend::Swscale, ResizeBackend::Zscale})
        {
            backend_array.push_back(json::object({
                {"backend", to_string(backend)},
                {"available", supports(backend)},
            }));
        }

        json::Array metric_array;
        for (const Metric metric : {Metric::Psnr, Metric::Ssim, Metric::Vmaf})
        {
            metric_array.push_back(json::object({
                {"metric", to_string(metric)},
                {"available", supports(metric)},
            }));
        }

        return json::object({
            {"image_codecs", json::array(std::move(image_array))},
            {"video_codecs", json::array(std::move(video_array))},
            {"resize_backends", json::array(std::move(backend_array))},
            {"metrics", json::array(std::move(metric_array))},

            // Names only: whether a device opens is a separate, heavyweight probe.
            {"hardware_devices", json::to_array(m_hardware_devices,
                                                [](const HardwareDeviceInfo& device) {
                                                    return device.name;
                                                })},
            {"encoder_count", m_encoders.size()},
            {"decoder_count", m_decoders.size()},
            {"filter_count", m_filters.size()},
        });
    }

    json::Value Capabilities::to_json_full() const
    {
        return json::object({
            {"encoders", json::to_array(m_encoders)},
            {"decoders", json::to_array(m_decoders)},
            {"filters", json::to_array(m_filters)},
        });
    }

    const Capabilities& capabilities()
    {
        // Enumerated once, on first use. FFmpeg's codec and filter tables are
        // static, so this is pure computation: no threads are started and no
        // global state is mutated, which is what keeps forked dataloader
        // workers safe.
        static const Capabilities available = Capabilities::probe_build();
        return available;
    }
}
