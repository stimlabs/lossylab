#pragma once

/// Driving one FFmpeg encoder and muxing its packets in memory. Internal
/// header.

#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/frame.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/rational.hpp"
#include "lossylab/core/record.hpp"
#include "lossylab/detail/ff_ptr.hpp"
#include "lossylab/detail/log_capture.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lossylab::detail
{
    /// Everything an encoder is opened with: the codec context fields the
    /// library sets and the encoder's private options. Fields left unset keep
    /// the encoder's defaults, which the resolved settings then report.
    struct EncoderSetup
    {
        std::string encoder_name;
        int width = 0;
        int height = 0;
        PixelFormat pixel_format;
        ColorSpec color;
        Rational frame_rate{1, 1};
        int thread_count = 1;

        /// A fixed quantizer, in the encoder's quantizer units; sets
        /// AV_CODEC_FLAG_QSCALE.
        std::optional<int> fixed_qscale;
        std::optional<int> qmin;
        std::optional<int> qmax;
        std::int64_t bit_rate = 0;
        std::int64_t max_rate = 0;
        std::int64_t buffer_size = 0;
        std::optional<int> gop_size;
        std::optional<int> keyint_min;
        std::optional<int> max_b_frames;
        bool closed_gop = false;

        /// Set when the muxer wants codec headers out of band.
        bool global_header = false;

        /// Private AVOptions of the encoder, e.g. {"crf", "23"}.
        std::map<std::string, std::string> options;
    };

    /// An open encoder. Frames go in with send(), and finish() drains it; the
    /// packets come out in coded order.
    class EncoderSession
    {
    public:
        explicit EncoderSession(const EncoderSetup& setup);

        void send(const Frame& frame, std::int64_t pts);
        void finish();

        [[nodiscard]] std::vector<PacketPtr>& packets() noexcept { return m_packets; }
        [[nodiscard]] const AVCodecContext& context() const noexcept { return *m_context; }

        /// The context fields and private options as the encoder resolved
        /// them after opening.
        [[nodiscard]] json::Value resolved_settings() const;

    private:
        void drain();

        // Declared ahead of the context so that it outlives it: closing an
        // encoder logs its summary.
        std::optional<ContextLogCapture> m_log_capture;
        CodecContextPtr m_context;
        std::vector<std::string> m_option_names;
        std::vector<PacketPtr> m_packets;
        bool m_global_header = false;
    };

    /// The packets' payloads back to back.
    [[nodiscard]] std::vector<std::uint8_t> concatenate_packets(const std::vector<PacketPtr>& packets);

    /// Muxes the packets into `muxer_name`'s format, in memory. The output is
    /// seekable, so muxers that rewrite their headers at the end work.
    [[nodiscard]] std::vector<std::uint8_t> mux_packets(const std::string& muxer_name, const AVCodecContext& encoder,
                                                        std::vector<PacketPtr>& packets);

    /// Whether `muxer_name` wants codec headers out of band.
    [[nodiscard]] bool muxer_wants_global_header(const std::string& muxer_name);

    /// A packet's picture type, key flag, size and, when the encoder reports
    /// one, the quantizer from its quality statistics.
    [[nodiscard]] FrameStats packet_stats(const AVPacket& packet, bool reports_qp);
}
