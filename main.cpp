#include <iomanip>
#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace
{
    void print_component(const char* name, const unsigned int version)
    {
        std::cout << "  " << std::left << std::setw(14) << name
                  << AV_VERSION_MAJOR(version) << '.'
                  << AV_VERSION_MINOR(version) << '.'
                  << AV_VERSION_MICRO(version) << '\n';
    }
}

int main()
{
    std::cout << "FFmpeg " << av_version_info() << '\n';
    print_component("libavcodec", avcodec_version());
    print_component("libavformat", avformat_version());
    print_component("libavfilter", avfilter_version());
    print_component("libavutil", avutil_version());
    print_component("libswscale", swscale_version());
    print_component("libswresample", swresample_version());

    // Smoke test: the codecs and pixel formats lossylab leans on must be present.
    for (const auto* codec_name : {"libx264", "libx265", "mjpeg"})
    {
        const AVCodec* encoder = avcodec_find_encoder_by_name(codec_name);
        std::cout << "  encoder " << std::left << std::setw(12) << codec_name
                  << (encoder != nullptr ? "ok" : "MISSING") << '\n';
    }

    std::cout << "  pix_fmt yuv420p "
              << av_get_pix_fmt_name(AV_PIX_FMT_YUV420P) << '\n';

    return 0;
}
