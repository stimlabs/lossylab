#include "bindings.hpp"

#include "lossylab/convert/convert.hpp"

#include <nanobind/stl/optional.h>

namespace lossylab::pybind
{
    using namespace nb::literals;

    void bind_convert(nb::module_& m)
    {
        nb::class_<ConvertOptions>(m, "ConvertOptions")
            .def(nb::init<>())
            .def_rw("pixel_format", &ConvertOptions::pixel_format)
            .def_rw("color", &ConvertOptions::color)
            .def_rw("chroma_down", &ConvertOptions::chroma_down)
            .def_rw("chroma_up", &ConvertOptions::chroma_up)
            .def_rw("backend", &ConvertOptions::backend)
            .def_rw("strict", &ConvertOptions::strict);

        nb::class_<ChromaRoundtripOptions>(m, "ChromaRoundtripOptions")
            .def(nb::init<>())
            .def_rw("subsampling", &ChromaRoundtripOptions::subsampling)
            .def_rw("color", &ChromaRoundtripOptions::color)
            .def_rw("chroma_down", &ChromaRoundtripOptions::chroma_down)
            .def_rw("chroma_up", &ChromaRoundtripOptions::chroma_up)
            .def_rw("intermediate_bit_depth", &ChromaRoundtripOptions::intermediate_bit_depth)
            .def_rw("backend", &ChromaRoundtripOptions::backend)
            .def_rw("strict", &ChromaRoundtripOptions::strict);

        m.def("convert", nb::overload_cast<const Frame&, const ConvertOptions&>(&convert), "frame"_a,
              "options"_a, nb::call_guard<nb::gil_scoped_release>());
        m.def("convert",
              nb::overload_cast<const Frame&, PixelFormat, const ColorSpec&, Strict>(&convert), "frame"_a,
              "pixel_format"_a, "color"_a, "strict"_a = Strict::AllowRecorded,
              nb::call_guard<nb::gil_scoped_release>());

        m.def("chroma_roundtrip", &chroma_roundtrip, "frame"_a, "options"_a,
              nb::call_guard<nb::gil_scoped_release>());

        m.def("reinterpret", &reinterpret, "frame"_a, "as_color"_a,
              nb::call_guard<nb::gil_scoped_release>());
    }
}
