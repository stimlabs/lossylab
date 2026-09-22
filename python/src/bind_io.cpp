#include "bindings.hpp"
#include "json_convert.hpp"

#include "lossylab/io/decode_image.hpp"
#include "lossylab/io/probe.hpp"
#include "lossylab/io/source.hpp"

#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <cstdint>
#include <span>
#include <vector>

namespace lossylab::pybind
{
    using namespace nb::literals;

    void bind_io(nb::module_& m)
    {
        // ---- source.hpp --------------------------------------------------
        auto source = nb::class_<Source>(m, "Source");
        source
            .def_static("from_path", &Source::from_path, "path"_a)
            .def_static(
                "from_bytes",
                [](nb::bytes data)
                {
                    const auto* begin = reinterpret_cast<const std::uint8_t*>(data.c_str());
                    return Source::from_bytes(std::vector<std::uint8_t>(begin, begin + data.size()));
                },
                "data"_a, "Copies the buffer; the Source owns its own bytes afterward.")
            .def_static(
                "from_memory",
                [](nb::bytes data)
                {
                    const auto* begin = reinterpret_cast<const std::uint8_t*>(data.c_str());
                    return Source::from_memory(std::span<const std::uint8_t>(begin, data.size()));
                },
                "data"_a, nb::keep_alive<0, 1>(),
                "Borrows the buffer without copying; the Source holds a reference to it for its "
                "whole lifetime, so it need not be kept alive separately.")
            .def("is_path", &Source::is_path)
            .def("path", &Source::path)
            .def("describe", &Source::describe);

        // ---- probe.hpp --------------------------------------------------
        nb::class_<StreamInfo>(m, "StreamInfo")
            .def_ro("index", &StreamInfo::index)
            .def_ro("type", &StreamInfo::type)
            .def_ro("codec_name", &StreamInfo::codec_name)
            .def_ro("codec_long_name", &StreamInfo::codec_long_name)
            .def_ro("profile", &StreamInfo::profile)
            .def_ro("level", &StreamInfo::level)
            .def_ro("width", &StreamInfo::width)
            .def_ro("height", &StreamInfo::height)
            .def_ro("pixel_format", &StreamInfo::pixel_format)
            .def_ro("bit_depth", &StreamInfo::bit_depth)
            .def_ro("color", &StreamInfo::color)
            .def_ro("color_fully_tagged", &StreamInfo::color_fully_tagged)
            .def_ro("frame_rate", &StreamInfo::frame_rate)
            .def_ro("average_frame_rate", &StreamInfo::average_frame_rate)
            .def_ro("time_base", &StreamInfo::time_base)
            .def_ro("sample_aspect_ratio", &StreamInfo::sample_aspect_ratio)
            .def_ro("rotation", &StreamInfo::rotation)
            .def_ro("frame_count", &StreamInfo::frame_count)
            .def_ro("duration_us", &StreamInfo::duration_us)
            .def_ro("bit_rate", &StreamInfo::bit_rate)
            .def_ro("metadata", &StreamInfo::metadata)
            .def_ro("has_hdr_metadata", &StreamInfo::has_hdr_metadata)
            .def("to_dict", [](const StreamInfo& self) { return to_python(self.to_json()); });

        nb::class_<ProbeResult>(m, "ProbeResult")
            .def_ro("format_name", &ProbeResult::format_name)
            .def_ro("format_long_name", &ProbeResult::format_long_name)
            .def_ro("duration_us", &ProbeResult::duration_us)
            .def_ro("bit_rate", &ProbeResult::bit_rate)
            .def_ro("size_bytes", &ProbeResult::size_bytes)
            .def_ro("streams", &ProbeResult::streams)
            .def_ro("metadata", &ProbeResult::metadata)
            .def_ro("major_brand", &ProbeResult::major_brand)
            .def_ro("compatible_brands", &ProbeResult::compatible_brands)
            .def("encoder_string", &ProbeResult::encoder_string)
            .def("primary_video_stream", &ProbeResult::primary_video_stream, nb::rv_policy::reference_internal)
            .def("to_dict", [](const ProbeResult& self) { return to_python(self.to_json()); });

        m.def("probe", &probe, "source"_a, nb::call_guard<nb::gil_scoped_release>());

        // ---- decode_image.hpp --------------------------------------------------
        nb::class_<DecodeImageOptions>(m, "DecodeImageOptions")
            .def(nb::init<>())
            .def_rw("pixel_format", &DecodeImageOptions::pixel_format)
            .def_rw("color", &DecodeImageOptions::color)
            .def_rw("assumed_color", &DecodeImageOptions::assumed_color)
            .def_rw("strict", &DecodeImageOptions::strict);

        m.def("decode_image", &decode_image, "source"_a, "options"_a = DecodeImageOptions{},
              nb::call_guard<nb::gil_scoped_release>());
    }
}
