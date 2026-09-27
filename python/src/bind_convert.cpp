#include "bind_reflected.hpp"
#include "bindings.hpp"

#include "lossylab/convert/convert.hpp"

namespace lossylab::pybind
{
    using namespace nb::literals;

    void bind_convert(nb::module_& m)
    {
        bind_reflected_rw<ConvertOptions>(m, "ConvertOptions");

        bind_reflected_rw<ChromaRoundtripOptions>(m, "ChromaRoundtripOptions");

        bind_reflected_rw<ReinterpretOptions>(m, "ReinterpretOptions");

        bind_reflected<ConvertEvidence>(m, "ConvertEvidence");
        bind_reflected<ChromaRoundtripEvidence>(m, "ChromaRoundtripEvidence");
        bind_reflected<ReinterpretEvidence>(m, "ReinterpretEvidence");

        m.def("convert", nb::overload_cast<const Frame&, const ConvertOptions&>(&convert), "frame"_a,
              "options"_a, nb::call_guard<nb::gil_scoped_release>());
        m.def("convert",
              nb::overload_cast<const Frame&, PixelFormat, const ColorSpec&, Strict>(&convert), "frame"_a,
              "pixel_format"_a, "color"_a, "strict"_a = Strict::AllowRecorded,
              nb::call_guard<nb::gil_scoped_release>());

        m.def("chroma_roundtrip", &chroma_roundtrip, "frame"_a, "options"_a,
              nb::call_guard<nb::gil_scoped_release>());

        m.def("reinterpret", nb::overload_cast<const Frame&, const ReinterpretOptions&>(&reinterpret), "frame"_a,
              "options"_a, nb::call_guard<nb::gil_scoped_release>());
        m.def("reinterpret", nb::overload_cast<const Frame&, const ColorSpec&>(&reinterpret), "frame"_a,
              "as_color"_a, nb::call_guard<nb::gil_scoped_release>());
    }
}
