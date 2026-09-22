#pragma once

#include "lossylab/core/codec_id.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/pixel_format.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lossylab
{
    /// One option an encoder or filter accepts, as FFmpeg's AVOption system
    /// describes it.
    ///
    /// Exposing the schema lets a pipeline spec be checked before it runs,
    /// rather than failing halfway through a dataset because one encoder in one
    /// build spells a parameter differently.
    struct OptionSchema
    {
        std::string name;
        std::string help;

        /// "int", "double", "string", "rational", "flags", "enum", "bool".
        std::string type;

        std::optional<double> min_value;
        std::optional<double> max_value;
        std::string default_value;

        /// For enum-typed options, the accepted constant names.
        std::vector<std::string> choices;

        [[nodiscard]] json::Value to_json() const;
    };

    /// An encoder or decoder present in the build.
    struct CodecInfo
    {
        std::string name;
        std::string long_name;

        /// The FFmpeg codec id name, e.g. "h264". Several encoders can share
        /// one: libx264 and h264_nvenc both encode "h264".
        std::string codec_name;

        bool is_encoder = false;
        bool is_hardware = false;
        bool experimental = false;

        /// Pixel formats the codec accepts. Empty means it did not declare a
        /// list, not that it accepts none.
        std::vector<PixelFormat> pixel_formats;

        std::vector<OptionSchema> options;

        /// True when the codec declares it can accept `format`, or when it
        /// declared no list at all.
        [[nodiscard]] bool accepts(const PixelFormat& format) const noexcept;

        [[nodiscard]] const OptionSchema* find_option(std::string_view name) const noexcept;

        [[nodiscard]] json::Value to_json() const;
    };

    struct FilterInfo
    {
        std::string name;
        std::string description;
        int input_count = 0;
        int output_count = 0;

        /// True when the filter accepts a variable number of inputs, as the
        /// overlay and concat families do.
        bool dynamic_inputs = false;
        bool dynamic_outputs = false;

        bool supports_slice_threads = false;

        std::vector<OptionSchema> options;

        [[nodiscard]] json::Value to_json() const;
    };

    /// A hardware device type the build knows about.
    ///
    /// Being compiled in is not the same as being present: a build can know
    /// about CUDA on a machine with no GPU. Whether a device can actually be
    /// opened is answered by `hardware_device_usable`, which is deliberately
    /// not part of the cached enumeration because opening a device loads
    /// vendor drivers and may start threads.
    struct HardwareDeviceInfo
    {
        /// "cuda", "vaapi", "qsv", "videotoolbox", ...
        std::string name;
    };

    /// Tries to open a device of this type, returning whether it succeeded.
    ///
    /// Heavyweight and not cached: it loads vendor drivers and may start
    /// threads, so it must not be called on a path that will later fork. Call
    /// it once during validation, in the parent, before workers are spawned.
    [[nodiscard]] bool hardware_device_usable(std::string_view name);

    /// What the linked FFmpeg can actually do.
    ///
    /// The library's API declares the complete surface the design calls for;
    /// this is how a caller finds out which parts of it this particular build
    /// supports. Requesting something absent raises UnsupportedCapability
    /// rather than falling back to a substitute, because a silent substitution
    /// is exactly the kind of difference that separates two classes of data.
    class Capabilities
    {
    public:
        [[nodiscard]] const std::vector<CodecInfo>& encoders() const noexcept { return m_encoders; }
        [[nodiscard]] const std::vector<CodecInfo>& decoders() const noexcept { return m_decoders; }
        [[nodiscard]] const std::vector<FilterInfo>& filters() const noexcept { return m_filters; }
        [[nodiscard]] const std::vector<HardwareDeviceInfo>& hardware_devices() const noexcept
        {
            return m_hardware_devices;
        }

        [[nodiscard]] const CodecInfo* find_encoder(std::string_view name) const noexcept;
        [[nodiscard]] const CodecInfo* find_decoder(std::string_view name) const noexcept;
        [[nodiscard]] const FilterInfo* find_filter(std::string_view name) const noexcept;

        [[nodiscard]] bool has_encoder(std::string_view name) const noexcept;
        [[nodiscard]] bool has_decoder(std::string_view name) const noexcept;
        [[nodiscard]] bool has_filter(std::string_view name) const noexcept;
        [[nodiscard]] bool has_hardware_device(std::string_view name) const noexcept;

        /// Picks the first encoder from the codec's candidate list that this
        /// build provides. Returns nullptr when none is available.
        [[nodiscard]] const CodecInfo* select_encoder(ImageCodec codec) const noexcept;
        [[nodiscard]] const CodecInfo* select_encoder(
            VideoCodec codec, EncoderBackend backend = EncoderBackend::Software) const noexcept;
        [[nodiscard]] const CodecInfo* select_decoder(ImageCodec codec) const noexcept;
        [[nodiscard]] const CodecInfo* select_decoder(VideoCodec codec) const noexcept;

        /// As select_encoder, but throws UnsupportedCapability naming the codec
        /// and the build instead of returning nullptr.
        [[nodiscard]] const CodecInfo& require_encoder(ImageCodec codec) const;
        [[nodiscard]] const CodecInfo& require_encoder(
            VideoCodec codec, EncoderBackend backend = EncoderBackend::Software) const;
        [[nodiscard]] const CodecInfo& require_decoder(ImageCodec codec) const;
        [[nodiscard]] const CodecInfo& require_decoder(VideoCodec codec) const;
        [[nodiscard]] const FilterInfo& require_filter(std::string_view name) const;

        /// Throws unless the resize backend's filter is present.
        void require_resize_backend(ResizeBackend backend) const;

        /// Throws unless the metric can be computed by this build. VMAF needs
        /// libvmaf; PSNR and SSIM are always available.
        void require_metric(Metric metric) const;

        [[nodiscard]] bool supports(ImageCodec codec) const noexcept;
        [[nodiscard]] bool supports(VideoCodec codec,
                                    EncoderBackend backend = EncoderBackend::Software) const noexcept;
        [[nodiscard]] bool supports(ResizeBackend backend) const noexcept;
        [[nodiscard]] bool supports(Metric metric) const noexcept;

        /// A summary suitable for storing with experiment metadata. The full
        /// listing is large, so this reports availability per design-level
        /// capability rather than every codec FFmpeg happens to have.
        [[nodiscard]] json::Value to_json() const;

        /// The complete listing, for tooling that needs it.
        [[nodiscard]] json::Value to_json_full() const;

    private:
        friend const Capabilities& capabilities();
        static Capabilities probe_build();

        std::vector<CodecInfo> m_encoders;
        std::vector<CodecInfo> m_decoders;
        std::vector<FilterInfo> m_filters;
        std::vector<HardwareDeviceInfo> m_hardware_devices;
    };

    /// Enumerates what the linked FFmpeg provides. Computed once and cached;
    /// the enumeration is pure computation over FFmpeg's static tables, so it
    /// starts no threads and is safe to call before a fork.
    [[nodiscard]] const Capabilities& capabilities();
}
