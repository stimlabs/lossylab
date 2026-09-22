#include "bindings.hpp"
#include "json_convert.hpp"

#include "lossylab/io/decode_image.hpp"
#include "lossylab/io/probe.hpp"
#include "lossylab/io/read_headers.hpp"
#include "lossylab/io/source.hpp"

#include <nanobind/stl/array.h>
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
        nb::class_<ImageContainerInfo>(m, "ImageContainerInfo")
            .def_ro("has_alpha", &ImageContainerInfo::has_alpha)
            .def_ro("is_animated", &ImageContainerInfo::is_animated)
            .def_ro("is_still_image", &ImageContainerInfo::is_still_image)
            .def_ro("compression", &ImageContainerInfo::compression)
            .def_ro("frame_count", &ImageContainerInfo::frame_count)
            .def_ro("canvas_width", &ImageContainerInfo::canvas_width)
            .def_ro("canvas_height", &ImageContainerInfo::canvas_height)
            .def("to_dict", [](const ImageContainerInfo& self) { return to_python(self.to_json()); });

        auto jpeg_info = nb::class_<JpegInfo>(m, "JpegInfo");
        nb::class_<JpegInfo::Component>(jpeg_info, "Component")
            .def_ro("id", &JpegInfo::Component::id)
            .def_ro("horizontal_sampling", &JpegInfo::Component::horizontal_sampling)
            .def_ro("vertical_sampling", &JpegInfo::Component::vertical_sampling)
            .def_ro("quantization_table", &JpegInfo::Component::quantization_table);
        nb::class_<JpegInfo::QuantizationTable>(jpeg_info, "QuantizationTable")
            .def_ro("id", &JpegInfo::QuantizationTable::id)
            .def_ro("precision", &JpegInfo::QuantizationTable::precision)
            .def_ro("values", &JpegInfo::QuantizationTable::values);
        nb::class_<JpegInfo::Segment>(jpeg_info, "Segment")
            .def_ro("marker", &JpegInfo::Segment::marker)
            .def_ro("identifier", &JpegInfo::Segment::identifier)
            .def_ro("size_bytes", &JpegInfo::Segment::size_bytes);
        jpeg_info.def_ro("process", &JpegInfo::process)
            .def_ro("arithmetic_coding", &JpegInfo::arithmetic_coding)
            .def_ro("precision", &JpegInfo::precision)
            .def_ro("components", &JpegInfo::components)
            .def_ro("quantization_tables", &JpegInfo::quantization_tables)
            .def_ro("ijg_quality", &JpegInfo::ijg_quality)
            .def_ro("ijg_quality_exact", &JpegInfo::ijg_quality_exact)
            .def_ro("huffman_tables", &JpegInfo::huffman_tables)
            .def_ro("restart_interval", &JpegInfo::restart_interval)
            .def_ro("scan_count", &JpegInfo::scan_count)
            .def_ro("segments", &JpegInfo::segments)
            .def_ro("comment", &JpegInfo::comment)
            .def_ro("adobe_transform", &JpegInfo::adobe_transform)
            .def_ro("has_end_of_image", &JpegInfo::has_end_of_image)
            .def_ro("trailing_bytes", &JpegInfo::trailing_bytes)
            .def("to_dict", [](const JpegInfo& self) { return to_python(self.to_json()); });

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
            .def_ro("is_variable_frame_rate", &StreamInfo::is_variable_frame_rate)
            .def_ro("time_base", &StreamInfo::time_base)
            .def_ro("sample_aspect_ratio", &StreamInfo::sample_aspect_ratio)
            .def_ro("rotation", &StreamInfo::rotation)
            .def_ro("frame_count", &StreamInfo::frame_count)
            .def_ro("duration_us", &StreamInfo::duration_us)
            .def_ro("bit_rate", &StreamInfo::bit_rate)
            .def_ro("metadata", &StreamInfo::metadata)
            .def_ro("has_hdr_metadata", &StreamInfo::has_hdr_metadata)
            .def_ro("image_container", &StreamInfo::image_container)
            .def_ro("jpeg", &StreamInfo::jpeg)
            .def_ro("is_default", &StreamInfo::is_default)
            .def_ro("is_dependent", &StreamInfo::is_dependent)
            .def("to_dict", [](const StreamInfo& self) { return to_python(self.to_json()); });

        auto tile_grid = nb::class_<TileGrid>(m, "TileGrid");
        nb::class_<TileGrid::Tile>(tile_grid, "Tile")
            .def_ro("stream_index", &TileGrid::Tile::stream_index)
            .def_ro("x", &TileGrid::Tile::x)
            .def_ro("y", &TileGrid::Tile::y);
        tile_grid.def_ro("id", &TileGrid::id)
            .def_ro("is_primary", &TileGrid::is_primary)
            .def_ro("title", &TileGrid::title)
            .def_ro("width", &TileGrid::width)
            .def_ro("height", &TileGrid::height)
            .def_ro("coded_width", &TileGrid::coded_width)
            .def_ro("coded_height", &TileGrid::coded_height)
            .def_ro("tiles", &TileGrid::tiles)
            .def("to_dict", [](const TileGrid& self) { return to_python(self.to_json()); });

        auto probe_result = nb::class_<ProbeResult>(m, "ProbeResult");
        nb::class_<ProbeResult::AdditionalImages>(probe_result, "AdditionalImages")
            .def_ro("stream_indices", &ProbeResult::AdditionalImages::stream_indices)
            .def_ro("tile_grid_ids", &ProbeResult::AdditionalImages::tile_grid_ids);
        probe_result
            .def_ro("format_name", &ProbeResult::format_name)
            .def_ro("format_long_name", &ProbeResult::format_long_name)
            .def_ro("duration_us", &ProbeResult::duration_us)
            .def_ro("bit_rate", &ProbeResult::bit_rate)
            .def_ro("size_bytes", &ProbeResult::size_bytes)
            .def_ro("streams", &ProbeResult::streams)
            .def_ro("metadata", &ProbeResult::metadata)
            .def_ro("major_brand", &ProbeResult::major_brand)
            .def_ro("compatible_brands", &ProbeResult::compatible_brands)
            .def_ro("claimed_extension", &ProbeResult::claimed_extension)
            .def_ro("format_mismatch", &ProbeResult::format_mismatch)
            .def("encoder_string", &ProbeResult::encoder_string)
            .def_ro("tile_grids", &ProbeResult::tile_grids)
            .def("primary_video_stream", &ProbeResult::primary_video_stream, nb::rv_policy::reference_internal)
            .def("primary_tile_grid", &ProbeResult::primary_tile_grid, nb::rv_policy::reference_internal)
            .def("additional_images", &ProbeResult::additional_images)
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

        // ---- read_headers.hpp --------------------------------------------------
        nb::class_<ParameterSet>(m, "ParameterSet")
            .def_ro("kind", &ParameterSet::kind)
            .def_ro("id", &ParameterSet::id)
            .def_prop_ro("fields", [](const ParameterSet& self) { return to_python(self.fields); })
            .def("to_dict", [](const ParameterSet& self) { return to_python(self.to_json()); });

        nb::class_<SliceInfo>(m, "SliceInfo")
            .def_ro("index", &SliceInfo::index)
            .def_ro("slice_type", &SliceInfo::slice_type)
            .def_ro("qp", &SliceInfo::qp)
            .def_ro("size_bytes", &SliceInfo::size_bytes)
            .def("to_dict", [](const SliceInfo& self) { return to_python(self.to_json()); });

        nb::class_<HeaderInfo>(m, "HeaderInfo")
            .def_ro("codec_name", &HeaderInfo::codec_name)
            .def_ro("parameter_sets", &HeaderInfo::parameter_sets)
            .def_ro("slices", &HeaderInfo::slices)
            .def_ro("embedded_encoder_settings", &HeaderInfo::embedded_encoder_settings)
            .def_ro("embedded_encoder_settings_availability", &HeaderInfo::embedded_encoder_settings_availability)
            .def_ro("encoder_settings", &HeaderInfo::encoder_settings)
            .def_ro("quantizer_indices", &HeaderInfo::quantizer_indices)
            .def_prop_ro("bitstream_color", [](const HeaderInfo& self) { return to_python(self.bitstream_color); })
            .def("to_dict", [](const HeaderInfo& self) { return to_python(self.to_json()); });

        nb::class_<ReadHeadersOptions>(m, "ReadHeadersOptions")
            .def(nb::init<>())
            .def_rw("max_slices", &ReadHeadersOptions::max_slices)
            .def_rw("stream_index", &ReadHeadersOptions::stream_index);

        m.def("read_headers", &read_headers, "source"_a, "options"_a = ReadHeadersOptions{},
              nb::call_guard<nb::gil_scoped_release>());
    }
}
