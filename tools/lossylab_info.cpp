/// Prints what the linked FFmpeg provides.
///
/// The library declares the full API surface the design calls for; which parts
/// of it a given build can serve is a runtime question. This is the answer, in
/// a form meant to be read rather than parsed. `--json` emits the same content
/// as a record-compatible document.

#include "lossylab/env/build_info.hpp"
#include "lossylab/env/capabilities.hpp"

#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

namespace
{
    using namespace lossylab;

    constexpr int label_width = 22;

    void print_heading(const char* title)
    {
        std::cout << '\n' << title << '\n' << std::string(std::strlen(title), '-') << '\n';
    }

    const char* mark(const bool available)
    {
        return available ? "yes" : " - ";
    }

    void print_build()
    {
        const BuildInfo& info = build_info();

        print_heading("Build");
        std::cout << std::left << std::setw(label_width) << "  FFmpeg" << info.version << '\n'
                  << std::setw(label_width) << "  build id" << info.build_id << '\n'
                  << std::setw(label_width) << "  license" << to_string(info.license);
        if (!permits_proprietary_distribution(info.license))
        {
            std::cout << "  (restricts redistribution in a closed-source product)";
        }
        std::cout << '\n';

        if (!info.is_consistent())
        {
            std::cout << "\n  WARNING: runtime library versions differ from the headers this\n"
                      << "  library was compiled against. Results are not trustworthy.\n";
        }

        print_heading("Libraries");
        for (const LibraryVersion& library : info.libraries)
        {
            std::cout << "  " << std::left << std::setw(label_width - 2) << library.name
                      << library.major << '.' << library.minor << '.' << library.micro;
            if (!library.matches_compiled())
            {
                std::cout << "   != compiled " << library.compiled_major << '.'
                          << library.compiled_minor << '.' << library.compiled_micro;
            }
            std::cout << '\n';
        }

        print_heading("External libraries");
        if (info.external_libraries.empty())
        {
            std::cout << "  (none)\n";
        }
        for (const std::string& name : info.external_libraries)
        {
            std::cout << "  " << name << '\n';
        }
    }

    void print_capabilities()
    {
        const Capabilities& available = capabilities();

        print_heading("Image codecs");
        for (const ImageCodec codec : all_image_codecs())
        {
            const CodecInfo* encoder = available.select_encoder(codec);
            const CodecInfo* decoder = available.select_decoder(codec);
            std::cout << "  " << std::left << std::setw(8) << to_string(codec)
                      << "encode " << std::setw(16)
                      << (encoder != nullptr ? encoder->name : "-")
                      << "decode " << (decoder != nullptr ? decoder->name : "-") << '\n';
        }

        print_heading("Video codecs");
        std::cout << "  " << std::left << std::setw(8) << "codec" << std::setw(18) << "software"
                  << std::setw(16) << "vaapi" << std::setw(16) << "nvenc" << "decode" << '\n';
        for (const VideoCodec codec : all_video_codecs())
        {
            const CodecInfo* software = available.select_encoder(codec, EncoderBackend::Software);
            const CodecInfo* vaapi = available.select_encoder(codec, EncoderBackend::Vaapi);
            const CodecInfo* nvenc = available.select_encoder(codec, EncoderBackend::Nvenc);
            const CodecInfo* decoder = available.select_decoder(codec);

            std::cout << "  " << std::left << std::setw(8) << to_string(codec)
                      << std::setw(18) << (software != nullptr ? software->name : "-")
                      << std::setw(16) << (vaapi != nullptr ? vaapi->name : "-")
                      << std::setw(16) << (nvenc != nullptr ? nvenc->name : "-")
                      << (decoder != nullptr ? decoder->name : "-") << '\n';
        }

        print_heading("Resize backends");
        for (const ResizeBackend backend : {ResizeBackend::Swscale, ResizeBackend::Zscale})
        {
            std::cout << "  " << std::left << std::setw(label_width - 2) << to_string(backend)
                      << mark(available.supports(backend)) << '\n';
        }

        print_heading("Metrics");
        for (const Metric metric : {Metric::Psnr, Metric::Ssim, Metric::Vmaf})
        {
            std::cout << "  " << std::left << std::setw(label_width - 2) << to_string(metric)
                      << mark(available.supports(metric)) << '\n';
        }

        print_heading("Hardware device types");
        if (available.hardware_devices().empty())
        {
            std::cout << "  (none compiled in)\n";
        }
        for (const HardwareDeviceInfo& device : available.hardware_devices())
        {
            std::cout << "  " << device.name << '\n';
        }
        std::cout << "\n  Compiled in, not probed. Opening a device loads vendor drivers,\n"
                  << "  so availability is checked on demand.\n";

        print_heading("Totals");
        std::cout << "  " << std::left << std::setw(label_width - 2) << "video encoders"
                  << available.encoders().size() << '\n'
                  << "  " << std::setw(label_width - 2) << "video decoders"
                  << available.decoders().size() << '\n'
                  << "  " << std::setw(label_width - 2) << "filters"
                  << available.filters().size() << '\n';
    }
}

int main(const int argc, char** argv)
{
    const bool as_json = argc > 1 && std::strcmp(argv[1], "--json") == 0;
    const bool full = argc > 1 && std::strcmp(argv[1], "--json-full") == 0;

    if (as_json || full)
    {
        json::Value document = json::object({
            {"build", build_info().to_json()},
            {"capabilities", capabilities().to_json()},
        });
        if (full)
        {
            document["codecs"] = capabilities().to_json_full();
        }
        std::cout << document.dump(2) << '\n';
        return 0;
    }

    if (argc > 1)
    {
        std::cerr << "usage: lossylab-info [--json | --json-full]\n";
        return 2;
    }

    print_build();
    print_capabilities();
    std::cout << '\n';
    return 0;
}
