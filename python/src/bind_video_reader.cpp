#include "bind_reflected.hpp"
#include "bindings.hpp"
#include "json_convert.hpp"

#include "lossylab/io/video_reader.hpp"

#include <nanobind/stl/function.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

namespace lossylab::pybind
{
    using namespace nb::literals;

    void bind_video_reader(nb::module_& m)
    {
        bind_reflected<MotionVector>(m, "MotionVector");

        bind_reflected<VideoFrame>(m, "VideoFrame");

        nb::class_<FrameSelector>(m, "FrameSelector")
            .def_static("all", &FrameSelector::all)
            .def_static("indices", &FrameSelector::indices, "indices"_a)
            .def_static("stride", &FrameSelector::stride, "step"_a, "offset"_a = 0)
            .def_static("timestamps", &FrameSelector::timestamps, "seconds"_a)
            .def_static("picture_types", &FrameSelector::picture_types, "types"_a)
            .def_static("evenly_spaced", &FrameSelector::evenly_spaced, "count"_a)
            .def_static("where", &FrameSelector::where, "predicate"_a,
                        "The predicate runs with the GIL held, once per decoded frame.")
            .def("and_also", &FrameSelector::and_also, "other"_a)
            .def("to_dict", [](const FrameSelector& self) { return to_python(self.to_json()); });

        bind_reflected_rw<VideoReaderOptions>(m, "VideoReaderOptions");

        // The reader keeps its Source alive, since it rereads it on every call
        // and a Source from from_memory borrows its buffer.
        nb::class_<VideoReader>(m, "VideoReader")
            .def(nb::init<const Source&, const VideoReaderOptions&>(), "source"_a,
                 "options"_a = VideoReaderOptions{}, nb::keep_alive<1, 2>(),
                 nb::call_guard<nb::gil_scoped_release>())
            .def("stream", &VideoReader::stream, nb::rv_policy::reference_internal)
            .def("frames", &VideoReader::frames, "select"_a, nb::call_guard<nb::gil_scoped_release>())
            .def("for_each", &VideoReader::for_each, "select"_a, "callback"_a,
                 nb::call_guard<nb::gil_scoped_release>(),
                 "The callback runs with the GIL held, once per selected frame, and returns False to stop.")
            .def("record", &VideoReader::record, nb::rv_policy::reference_internal);
    }
}
