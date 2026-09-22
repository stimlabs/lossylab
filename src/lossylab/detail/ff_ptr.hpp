#pragma once

/// RAII wrappers for the FFmpeg objects the library owns.
///
/// Internal header: nothing under include/ may include it, which is what keeps
/// libav* out of the public API. Every FFmpeg allocation in the library goes
/// through one of these, so no code path below has to pair an allocation with
/// a free by hand.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <memory>

namespace lossylab::detail
{
    // Most FFmpeg types free through a function taking a pointer-to-pointer so
    // that it can null the caller's handle. The deleters below adapt that to
    // unique_ptr's single-pointer signature.

    struct FrameDeleter
    {
        void operator()(AVFrame* pointer) const noexcept { av_frame_free(&pointer); }
    };

    struct PacketDeleter
    {
        void operator()(AVPacket* pointer) const noexcept { av_packet_free(&pointer); }
    };

    struct CodecContextDeleter
    {
        void operator()(AVCodecContext* pointer) const noexcept { avcodec_free_context(&pointer); }
    };

    struct FormatContextDeleter
    {
        void operator()(AVFormatContext* pointer) const noexcept { avformat_close_input(&pointer); }
    };

    /// Output contexts are built, not opened, so they free differently from the
    /// input ones above.
    struct OutputFormatContextDeleter
    {
        void operator()(AVFormatContext* pointer) const noexcept
        {
            if (pointer == nullptr)
            {
                return;
            }
            if (pointer->pb != nullptr && (pointer->oformat->flags & AVFMT_NOFILE) == 0)
            {
                avio_closep(&pointer->pb);
            }
            avformat_free_context(pointer);
        }
    };

    struct FilterGraphDeleter
    {
        void operator()(AVFilterGraph* pointer) const noexcept { avfilter_graph_free(&pointer); }
    };

    struct FilterInOutDeleter
    {
        void operator()(AVFilterInOut* pointer) const noexcept { avfilter_inout_free(&pointer); }
    };

    struct SwsContextDeleter
    {
        void operator()(SwsContext* pointer) const noexcept { sws_freeContext(pointer); }
    };

    struct DictionaryDeleter
    {
        void operator()(AVDictionary* pointer) const noexcept { av_dict_free(&pointer); }
    };

    struct BufferRefDeleter
    {
        void operator()(AVBufferRef* pointer) const noexcept { av_buffer_unref(&pointer); }
    };

    struct BsfContextDeleter
    {
        void operator()(AVBSFContext* pointer) const noexcept { av_bsf_free(&pointer); }
    };

    struct AvIoContextDeleter
    {
        void operator()(AVIOContext* pointer) const noexcept
        {
            if (pointer == nullptr)
            {
                return;
            }
            // The buffer is reallocated by avio, so the current one is freed
            // rather than whatever was handed in at construction.
            av_freep(&pointer->buffer);
            avio_context_free(&pointer);
        }
    };

    /// Frees memory obtained from av_malloc and friends.
    struct AvFreeDeleter
    {
        void operator()(void* pointer) const noexcept { av_free(pointer); }
    };

    using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
    using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
    using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
    using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
    using OutputFormatContextPtr = std::unique_ptr<AVFormatContext, OutputFormatContextDeleter>;
    using FilterGraphPtr = std::unique_ptr<AVFilterGraph, FilterGraphDeleter>;
    using FilterInOutPtr = std::unique_ptr<AVFilterInOut, FilterInOutDeleter>;
    using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;
    using DictionaryPtr = std::unique_ptr<AVDictionary, DictionaryDeleter>;
    using BufferRefPtr = std::unique_ptr<AVBufferRef, BufferRefDeleter>;
    using BsfContextPtr = std::unique_ptr<AVBSFContext, BsfContextDeleter>;
    using AvIoContextPtr = std::unique_ptr<AVIOContext, AvIoContextDeleter>;
    using AvBufferPtr = std::unique_ptr<void, AvFreeDeleter>;

    /// Allocating constructors. Each throws FFmpegError on allocation failure,
    /// so callers never have to null-check.
    [[nodiscard]] FramePtr make_frame();
    [[nodiscard]] PacketPtr make_packet();
    [[nodiscard]] FilterGraphPtr make_filter_graph();

    /// Deep-copies a frame's properties and data into a new frame.
    [[nodiscard]] FramePtr clone_frame(const AVFrame* source);

    /// A new reference to the same underlying buffers: cheap, and the basis of
    /// Frame's copy semantics.
    [[nodiscard]] FramePtr ref_frame(const AVFrame* source);
}
