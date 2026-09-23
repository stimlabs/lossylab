#include "bindings.hpp"
#include "json_convert.hpp"

#include "lossylab/core/record.hpp"
#include "lossylab/core/result.hpp"

#include <nanobind/operators.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

namespace lossylab::pybind
{
    using namespace nb::literals;

    void bind_record(nb::module_& m)
    {
        nb::enum_<StageKind>(m, "StageKind")
            .value("Decode", StageKind::Decode)
            .value("Probe", StageKind::Probe)
            .value("Convert", StageKind::Convert)
            .value("ChromaRoundtrip", StageKind::ChromaRoundtrip)
            .value("Reinterpret", StageKind::Reinterpret)
            .value("Resize", StageKind::Resize)
            .value("Filter", StageKind::Filter)
            .value("EncodeImage", StageKind::EncodeImage)
            .value("EncodeVideo", StageKind::EncodeVideo)
            .value("Roundtrip", StageKind::Roundtrip)
            .value("AnimateStill", StageKind::AnimateStill)
            .value("Measure", StageKind::Measure)
            .value("Compare", StageKind::Compare)
            .value("RecompressionCurve", StageKind::RecompressionCurve);

        nb::enum_<PictureType>(m, "PictureType")
            .value("Unknown", PictureType::Unknown)
            .value("I", PictureType::I)
            .value("P", PictureType::P)
            .value("B", PictureType::B);

        nb::class_<FormatDescription>(m, "FormatDescription")
            .def(nb::init<>())
            .def_rw("width", &FormatDescription::width)
            .def_rw("height", &FormatDescription::height)
            .def_rw("pixel_format", &FormatDescription::pixel_format)
            .def_rw("color", &FormatDescription::color)
            .def(nb::self == nb::self)
            .def(nb::self != nb::self)
            .def("to_dict", [](const FormatDescription& self) { return to_python(self.to_json()); })
            .def_static("from_dict",
                        [](nb::dict value) { return FormatDescription::from_json(to_json(value)); });

        nb::class_<FrameStats>(m, "FrameStats")
            .def(nb::init<>())
            .def_rw("index", &FrameStats::index)
            .def_rw("pts", &FrameStats::pts)
            .def_rw("picture_type", &FrameStats::picture_type)
            .def_rw("key_frame", &FrameStats::key_frame)
            .def_rw("size_bytes", &FrameStats::size_bytes)
            .def_rw("qp_min", &FrameStats::qp_min)
            .def_rw("qp_max", &FrameStats::qp_max)
            .def_rw("qp_mean", &FrameStats::qp_mean)
            .def("to_dict", [](const FrameStats& self) { return to_python(self.to_json()); })
            .def_static("from_dict", [](nb::dict value) { return FrameStats::from_json(to_json(value)); });

        nb::class_<StageRecord>(m, "StageRecord")
            .def(nb::init<>())
            .def_rw("kind", &StageRecord::kind)
            .def_rw("implementation", &StageRecord::implementation)
            .def_prop_rw(
                "params", [](const StageRecord& self) { return to_python(self.params); },
                [](StageRecord& self, nb::object value) { self.params = to_json(value); })
            .def_rw("input", &StageRecord::input)
            .def_rw("output", &StageRecord::output)
            .def_rw("conversions", &StageRecord::conversions)
            .def_rw("transform", &StageRecord::transform)
            .def_rw("block_grid", &StageRecord::block_grid)
            .def_rw("frames", &StageRecord::frames)
            .def_prop_rw(
                "encoder_settings", [](const StageRecord& self) { return to_python(self.encoder_settings); },
                [](StageRecord& self, nb::object value) { self.encoder_settings = to_json(value); })
            .def_rw("achieved_bpp", &StageRecord::achieved_bpp)
            .def_rw("seed", &StageRecord::seed)
            .def_rw("reproducible", &StageRecord::reproducible)
            .def_rw("duration_ms", &StageRecord::duration_ms)
            .def("to_dict", [](const StageRecord& self) { return to_python(self.to_json()); })
            .def_static("from_dict", [](nb::dict value) { return StageRecord::from_json(to_json(value)); });

        nb::class_<ProcessingRecord>(m, "ProcessingRecord")
            .def(nb::init<>())
            .def(nb::init<std::string>(), "build_id"_a)
            .def("append", &ProcessingRecord::append, "stage"_a)
            .def("stages", &ProcessingRecord::stages)
            .def("empty", &ProcessingRecord::empty)
            .def("__len__", &ProcessingRecord::size)
            .def_prop_rw("build_id", &ProcessingRecord::build_id, &ProcessingRecord::set_build_id)
            .def("end_to_end_transform", &ProcessingRecord::end_to_end_transform)
            .def("effective_block_grid", &ProcessingRecord::effective_block_grid)
            .def("is_reproducible", &ProcessingRecord::is_reproducible)
            .def("compression_generations", &ProcessingRecord::compression_generations)
            .def("all_conversions", &ProcessingRecord::all_conversions)
            .def("validate_continuity", &ProcessingRecord::validate_continuity)
            .def("to_dict", [](const ProcessingRecord& self) { return to_python(self.to_json()); })
            .def_static("from_dict",
                        [](nb::dict value) { return ProcessingRecord::from_json(to_json(value)); });

        nb::class_<FrameResult>(m, "FrameResult").def_rw("frame", &FrameResult::frame).def_rw("record", &FrameResult::record);
    }
}
