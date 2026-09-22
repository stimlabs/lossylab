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
        nb::class_<MotionVector>(m, "MotionVector")
            .def_ro("source_x", &MotionVector::source_x)
            .def_ro("source_y", &MotionVector::source_y)
            .def_ro("dest_x", &MotionVector::dest_x)
            .def_ro("dest_y", &MotionVector::dest_y)
            .def_ro("block_width", &MotionVector::block_width)
            .def_ro("block_height", &MotionVector::block_height)
            .def_ro("source_index", &MotionVector::source_index);

        nb::class_<VideoFrame>(m, "VideoFrame")
            .def_ro("frame", &VideoFrame::frame)
            .def_ro("index", &VideoFrame::index)
            .def_ro("stats", &VideoFrame::stats)
            .def_ro("qp_map", &VideoFrame::qp_map)
            .def_ro("qp_map_availability", &VideoFrame::qp_map_availability)
            .def_ro("motion_vectors", &VideoFrame::motion_vectors)
            .def_ro("motion_vector_availability", &VideoFrame::motion_vector_availability);

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

        nb::class_<VideoReaderOptions>(m, "VideoReaderOptions")
            .def(nb::init<>())
            .def_rw("stream_index", &VideoReaderOptions::stream_index)
            .def_rw("pixel_format", &VideoReaderOptions::pixel_format)
            .def_rw("color", &VideoReaderOptions::color)
            .def_rw("assumed_color", &VideoReaderOptions::assumed_color)
            .def_rw("export_qp_maps", &VideoReaderOptions::export_qp_maps)
            .def_rw("export_motion_vectors", &VideoReaderOptions::export_motion_vectors)
            .def_rw("thread_count", &VideoReaderOptions::thread_count)
            .def_rw("strict", &VideoReaderOptions::strict);

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
