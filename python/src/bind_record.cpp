#include "bind_reflected.hpp"
#include "bindings.hpp"
#include "json_convert.hpp"

#include "lossylab/core/record.hpp"
#include "lossylab/core/result.hpp"

#include <nanobind/operators.h>

namespace lossylab::pybind
{
    using namespace nb::literals;

    void bind_record(nb::module_& m)
    {
        nb::enum_<StageKind>(m, "StageKind")
            .value("DecodeImage", StageKind::DecodeImage)
            .value("DecodeVideo", StageKind::DecodeVideo)
            .value("Convert", StageKind::Convert)
            .value("ChromaRoundtrip", StageKind::ChromaRoundtrip)
            .value("Reinterpret", StageKind::Reinterpret)
            .value("Resize", StageKind::Resize)
            .value("Filter", StageKind::Filter)
            .value("EncodeImage", StageKind::EncodeImage)
            .value("EncodeVideo", StageKind::EncodeVideo)
            .value("RoundtripImage", StageKind::RoundtripImage)
            .value("RoundtripVideo", StageKind::RoundtripVideo)
            .value("AnimateStill", StageKind::AnimateStill)
            .value("Measure", StageKind::Measure)
            .value("Compare", StageKind::Compare)
            .value("RecompressionCurve", StageKind::RecompressionCurve)
            .value("CompressionHistory", StageKind::CompressionHistory);

        nb::enum_<PictureType>(m, "PictureType")
            .value("Unknown", PictureType::Unknown)
            .value("I", PictureType::I)
            .value("P", PictureType::P)
            .value("B", PictureType::B);

        bind_reflected_rw<FormatDescription>(m, "FormatDescription")
            .def(nb::self == nb::self)
            .def(nb::self != nb::self)
            .def_static("from_dict",
                        [](nb::dict value) { return FormatDescription::from_json(to_json(value)); });

        bind_reflected_rw<FrameStats>(m, "FrameStats")
            .def_static("from_dict", [](nb::dict value) { return FrameStats::from_json(to_json(value)); });

        bind_reflected_rw<StageRecord>(m, "StageRecord")
            .def_prop_ro("kind", &StageRecord::kind, "The kind of operation, which the type of `evidence` states.")
            .def_static("from_dict", [](nb::dict value) { return StageRecord::from_json(to_json(value)); });

        nb::class_<ProcessingRecord>(m, "ProcessingRecord")
            .def(nb::init<>())
            .def_static("for_this_build", &ProcessingRecord::for_this_build)
            .def("append", &ProcessingRecord::append, "stage"_a, "configuration"_a,
                 "Adds a stage with the configuration it ran with. Raises ConfigError when the configuration "
                 "belongs to another kind of stage.")
            .def("stages", &ProcessingRecord::stages)
            .def("configurations", &ProcessingRecord::configurations)
            .def("empty", &ProcessingRecord::empty)
            .def("__len__", &ProcessingRecord::size)
            .def_prop_ro("build", [](const ProcessingRecord& self) { return to_python(self.build()); })
            .def_prop_ro("diagnostics", [](const ProcessingRecord& self) { return to_python(self.diagnostics()); })
            .def_prop_rw("origin", &ProcessingRecord::origin, &ProcessingRecord::set_origin, nb::arg("origin").none())
            .def("end_to_end_transform", &ProcessingRecord::end_to_end_transform)
            .def("effective_block_grid", &ProcessingRecord::effective_block_grid)
            .def("is_reproducible", &ProcessingRecord::is_reproducible)
            .def("compression_generations", &ProcessingRecord::compression_generations)
            .def("all_conversions", &ProcessingRecord::all_conversions)
            .def("validate_continuity", &ProcessingRecord::validate_continuity)
            .def("to_dict", [](const ProcessingRecord& self) { return to_python(self.to_json()); })
            .def_static("from_dict",
                        [](nb::dict value) { return ProcessingRecord::from_json(to_json(value)); });

        nb::class_<FrameResult>(m, "FrameResult")
            .def_rw("frame", &FrameResult::frame)
            .def_rw("record", &FrameResult::record)
            .def_ro("configuration", &FrameResult::configuration);
    }
}
