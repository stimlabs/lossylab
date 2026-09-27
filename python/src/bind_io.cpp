#include "bind_reflected.hpp"
#include "bindings.hpp"
#include "json_convert.hpp"

#include "lossylab/io/decode_image.hpp"
#include "lossylab/io/icc_profile.hpp"
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
            .def("describe", &Source::describe)
            .def("sha256", &Source::sha256,
                "\"sha256:\" and the lowercase hex SHA-256 of this source's bytes. Identifies the "
                "source for a stored record, in place of its path.");

        // ---- icc_profile.hpp --------------------------------------------------
        bind_reflected<Chromaticity>(m, "Chromaticity");

        // Colorants is a nested Python class, scoped under IccProfileInfo rather than
        // the module; bind_reflected takes any nb::handle scope, so this works the
        // same as a top-level type.
        auto icc_profile_info = bind_reflected<IccProfileInfo>(m, "IccProfileInfo");
        bind_reflected<IccProfileInfo::Colorants>(icc_profile_info, "Colorants");
        icc_profile_info.def("is_expressible_as_tags", &IccProfileInfo::is_expressible_as_tags)
            .def("agrees_with", &IccProfileInfo::agrees_with, "tagged"_a);

        m.def(
            "describe_icc_profile",
            [](nb::bytes data)
            {
                const auto* begin = reinterpret_cast<const std::uint8_t*>(data.c_str());
                return describe_icc_profile(std::span<const std::uint8_t>(begin, data.size()));
            },
            "data"_a, "Reads an ICC profile's header and tags. Malformed input is reported in `problems`, not raised.");

        // ---- probe.hpp --------------------------------------------------
        bind_reflected<ImageContainerInfo>(m, "ImageContainerInfo");

        // Component/QuantizationTable/Segment are nested Python classes, scoped under
        // JpegInfo rather than the module; bind_reflected takes any nb::handle scope.
        auto jpeg_info = bind_reflected<JpegInfo>(m, "JpegInfo");
        bind_reflected<JpegInfo::Component>(jpeg_info, "Component");
        bind_reflected<JpegInfo::QuantizationTable>(jpeg_info, "QuantizationTable");
        bind_reflected<JpegInfo::Segment>(jpeg_info, "Segment");

        bind_reflected<StreamInfo>(m, "StreamInfo");

        // Tile is a nested Python class, scoped under TileGrid.
        auto tile_grid = bind_reflected<TileGrid>(m, "TileGrid");
        bind_reflected<TileGrid::Tile>(tile_grid, "Tile");

        // AdditionalImages is a nested Python class, scoped under ProbeResult.
        auto probe_result = bind_reflected<ProbeResult>(m, "ProbeResult");
        bind_reflected<ProbeResult::AdditionalImages>(probe_result, "AdditionalImages");
        probe_result.def("encoder_string", &ProbeResult::encoder_string)
            .def("primary_video_stream", &ProbeResult::primary_video_stream, nb::rv_policy::reference_internal,
                 nb::sig("def primary_video_stream(self) -> StreamInfo | None"))
            .def("primary_tile_grid", &ProbeResult::primary_tile_grid, nb::rv_policy::reference_internal,
                 nb::sig("def primary_tile_grid(self) -> TileGrid | None"))
            .def("additional_images", &ProbeResult::additional_images);

        m.def("probe", &probe, "source"_a, nb::call_guard<nb::gil_scoped_release>());

        // ---- decode_image.hpp --------------------------------------------------
        nb::enum_<OrientationHandling>(m, "OrientationHandling")
            .value("Report", OrientationHandling::Report)
            .value("Apply", OrientationHandling::Apply);

        bind_reflected_rw<DecodeImageOptions>(m, "DecodeImageOptions");

        bind_reflected<DecodeImageEvidence>(m, "DecodeImageEvidence");

        bind_reflected<DecodedImage>(m, "DecodedImage")
            .def_prop_ro("evidence", &DecodedImage::evidence, nb::rv_policy::reference_internal)
            .def("stream", &DecodedImage::stream, nb::rv_policy::reference_internal)
            .def("tile_grid", &DecodedImage::tile_grid, nb::rv_policy::reference_internal,
                 nb::sig("def tile_grid(self) -> TileGrid | None"))
            .def("processing_record", &DecodedImage::processing_record);

        m.def("decode_image", &decode_image, "source"_a, "options"_a = DecodeImageOptions{},
              nb::call_guard<nb::gil_scoped_release>());

        // ---- read_headers.hpp --------------------------------------------------
        bind_reflected<ParameterSet>(m, "ParameterSet");

        bind_reflected<SliceInfo>(m, "SliceInfo");

        // Fields come from HeaderInfo's LOSSYLAB_REFLECT list (read_headers.hpp), so a
        // member added there without a matching name in that list fails to build instead
        // of silently missing here.
        bind_reflected<HeaderInfo>(m, "HeaderInfo");

        bind_reflected_rw<ReadHeadersOptions>(m, "ReadHeadersOptions");

        m.def("read_headers", &read_headers, "source"_a, "options"_a = ReadHeadersOptions{},
              nb::call_guard<nb::gil_scoped_release>());
    }
}
