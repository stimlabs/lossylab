#include "encoder_session.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/io/icc_profile.hpp"

#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <jpeglib.h>

namespace lossylab::detail
{
    namespace
    {
        constexpr int component_count = 3;

        /// libjpeg's error manager, made to jump back to the caller instead of
        /// exiting the process.
        struct ErrorManager
        {
            // First, since libjpeg reaches the whole struct through cinfo->err.
            jpeg_error_mgr manager;
            std::jmp_buf jump;
            char message[JMSG_LENGTH_MAX];
        };

        [[noreturn]] void jump_out(const j_common_ptr info)
        {
            auto* errors = reinterpret_cast<ErrorManager*>(info->err);
            (*info->err->format_message)(info, errors->message);
            std::longjmp(errors->jump, 1);
        }

        /// A warning (level -1) is an error; trace messages are dropped.
        void warning_is_error(const j_common_ptr info, const int level)
        {
            if (level < 0)
            {
                jump_out(info);
            }
        }

        /// What libjpeg works on, owned outside the function that calls
        /// setjmp, so that its values are defined after a longjmp.
        struct Compression
        {
            jpeg_compress_struct info{};
            ErrorManager errors{};
            unsigned char* output = nullptr;
            unsigned long output_size = 0;
        };

        /// One iMCU row of a component: its rows, each filled out to whole
        /// blocks.
        struct ComponentRows
        {
            ConstPlaneView plane;
            int sampling_rows = 1;
            int padded_width = 0;
            std::vector<JSAMPLE> samples;
            std::vector<JSAMPROW> rows;
        };

        /// The luma sampling factors of a subsampling; chroma's are 1x1.
        std::array<int, 2> luma_sampling(const Subsampling subsampling)
        {
            switch (subsampling)
            {
            case Subsampling::Yuv444: return {1, 1};
            case Subsampling::Yuv440: return {1, 2};
            case Subsampling::Yuv422: return {2, 1};
            case Subsampling::Yuv420: return {2, 2};
            default: break;
            }
            throw ConfigError("libjpeg-turbo takes 4:2:0, 4:2:2, 4:4:0 or 4:4:4 YCbCr, not " +
                              to_string(subsampling));
        }

        /// Copies iMCU row `imcu_row` of each plane into `components`,
        /// repeating a plane's last row and column where its blocks reach
        /// past it.
        void fill_imcu_row(std::array<ComponentRows, component_count>& components, const int imcu_row)
        {
            for (ComponentRows& component : components)
            {
                const ConstPlaneView& plane = component.plane;
                const int row_count = component.sampling_rows * DCTSIZE;
                for (int row = 0; row < row_count; ++row)
                {
                    const int source_row = std::min(imcu_row * row_count + row, plane.height - 1);
                    JSAMPLE* destination = component.rows[static_cast<std::size_t>(row)];
                    std::memcpy(destination, plane.row(source_row), static_cast<std::size_t>(plane.width));
                    std::fill(destination + plane.width, destination + component.padded_width,
                              destination[plane.width - 1]);
                }
            }
        }

        /// Runs libjpeg from start to finish. False when it reported an
        /// error, whose text is then in `compression.errors.message`. Nothing
        /// here may own a resource: a longjmp out of libjpeg lands at the
        /// setjmp without unwinding.
        bool compress(Compression& compression, std::array<ComponentRows, component_count>& components,
                      const std::array<int, 2> sampling, const int width, const int height, const int quality,
                      const IccProfile* icc_profile, const Rational sample_aspect_ratio)
        {
            jpeg_compress_struct& info = compression.info;
            info.err = jpeg_std_error(&compression.errors.manager);
            compression.errors.manager.error_exit = jump_out;
            compression.errors.manager.emit_message = warning_is_error;
            if (setjmp(compression.errors.jump) != 0)
            {
                return false;
            }

            jpeg_create_compress(&info);
            jpeg_mem_dest(&info, &compression.output, &compression.output_size);
            info.image_width = static_cast<JDIMENSION>(width);
            info.image_height = static_cast<JDIMENSION>(height);
            info.input_components = component_count;
            info.in_color_space = JCS_YCbCr;
            jpeg_set_defaults(&info);
            jpeg_set_quality(&info, quality, TRUE);
            info.raw_data_in = TRUE;
            info.dct_method = JDCT_ISLOW;
            info.comp_info[0].h_samp_factor = sampling[0];
            info.comp_info[0].v_samp_factor = sampling[1];
            for (int index = 1; index < component_count; ++index)
            {
                info.comp_info[index].h_samp_factor = 1;
                info.comp_info[index].v_samp_factor = 1;
            }

            // JFIF density without a unit is the pixel aspect ratio.
            const Rational ratio = sample_aspect_ratio.reduced();
            if (ratio.num > 0 && ratio.den > 0 && ratio.num <= 65535 && ratio.den <= 65535)
            {
                info.density_unit = 0;
                info.X_density = static_cast<UINT16>(ratio.num);
                info.Y_density = static_cast<UINT16>(ratio.den);
            }

            jpeg_start_compress(&info, TRUE);
            if (icc_profile != nullptr)
            {
                jpeg_write_icc_profile(&info, icc_profile->bytes.data(),
                                       static_cast<unsigned int>(icc_profile->bytes.size()));
            }

            std::array<JSAMPARRAY, component_count> planes{};
            for (int index = 0; index < component_count; ++index)
            {
                planes[static_cast<std::size_t>(index)] = components[static_cast<std::size_t>(index)].rows.data();
            }
            const int lines_per_imcu_row = sampling[1] * DCTSIZE;
            const int imcu_row_count = (height + lines_per_imcu_row - 1) / lines_per_imcu_row;
            for (int imcu_row = 0; imcu_row < imcu_row_count; ++imcu_row)
            {
                fill_imcu_row(components, imcu_row);
                if (jpeg_write_raw_data(&info, planes.data(), static_cast<JDIMENSION>(lines_per_imcu_row)) !=
                    static_cast<JDIMENSION>(lines_per_imcu_row))
                {
                    std::snprintf(compression.errors.message, sizeof(compression.errors.message),
                                  "accepted only part of iMCU row %d", imcu_row);
                    return false;
                }
            }
            jpeg_finish_compress(&info);
            return true;
        }

        /// libjpeg's settings as the encode left them.
        EncoderResolution resolution_of(const jpeg_compress_struct& info, const int quality)
        {
            EncoderResolution resolved;
            resolved.time_base = Rational{1, 1};
            resolved.thread_count = 1;
            resolved.bitexact = true;

            std::string sampling;
            for (int index = 0; index < info.num_components; ++index)
            {
                const jpeg_component_info& component = info.comp_info[index];
                sampling += (index == 0 ? "" : ",") + std::to_string(component.h_samp_factor) + "x" +
                            std::to_string(component.v_samp_factor);
            }
            resolved.options = {
                {"quality", std::to_string(quality)},
                {"force_baseline", "1"},
                {"dct_method", info.dct_method == JDCT_ISLOW ? "islow" : std::to_string(info.dct_method)},
                {"optimize_coding", info.optimize_coding ? "1" : "0"},
                {"arith_code", info.arith_code ? "1" : "0"},
                {"progressive", info.scan_info != nullptr ? "1" : "0"},
                {"smoothing_factor", std::to_string(info.smoothing_factor)},
                {"restart_interval", std::to_string(info.restart_interval)},
                {"write_JFIF_header", info.write_JFIF_header ? "1" : "0"},
                {"sampling_factors", sampling},
            };
            return resolved;
        }
    }

    LibjpegEncode encode_with_libjpeg(const Frame& frame, const EncoderSetup& setup)
    {
        if (!setup.ijg_quality.has_value())
        {
            throw Error("libjpeg-turbo was set up without an IJG quality");
        }
        const PixelFormat& pixel_format = frame.pixel_format();
        if (pixel_format.bit_depth() != 8 || pixel_format.plane_count() != component_count ||
            pixel_format.is_rgb() || pixel_format.has_alpha())
        {
            throw ConfigError("libjpeg-turbo takes 8-bit planar YCbCr, not " + pixel_format.name());
        }
        const std::array<int, 2> sampling = luma_sampling(pixel_format.subsampling());

        std::array<ComponentRows, component_count> components;
        for (int index = 0; index < component_count; ++index)
        {
            ComponentRows& component = components[static_cast<std::size_t>(index)];
            component.plane = frame.plane(index);
            component.sampling_rows = index == 0 ? sampling[1] : 1;
            component.padded_width = (component.plane.width + DCTSIZE - 1) / DCTSIZE * DCTSIZE;
            const auto row_count = static_cast<std::size_t>(component.sampling_rows * DCTSIZE);
            const auto padded_width = static_cast<std::size_t>(component.padded_width);
            component.samples.resize(padded_width * row_count);
            for (std::size_t row = 0; row < row_count; ++row)
            {
                component.rows.push_back(component.samples.data() + row * padded_width);
            }
        }

        Compression compression;
        const bool compressed = compress(compression, components, sampling, frame.width(), frame.height(),
                                         *setup.ijg_quality, setup.icc_profile, setup.sample_aspect_ratio);
        LibjpegEncode encoded;
        if (compressed)
        {
            encoded.bytes.assign(compression.output, compression.output + compression.output_size);
            encoded.resolved = resolution_of(compression.info, *setup.ijg_quality);
        }
        jpeg_destroy_compress(&compression.info);
        std::free(compression.output);
        if (!compressed)
        {
            throw Error(std::string("libjpeg-turbo: ") + compression.errors.message);
        }
        return encoded;
    }
}
