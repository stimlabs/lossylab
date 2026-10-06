#include "bind_reflected.hpp"
#include "bindings.hpp"

#include "lossylab/convert/convert.hpp"
#include "lossylab/transform/transform.hpp"

namespace lossylab::pybind
{
    using namespace nb::literals;

    void bind_convert(nb::module_& m)
    {
        nb::enum_<IccHandling>(m, "IccHandling")
            .value("Convert", IccHandling::Convert)
            .value("Ignore", IccHandling::Ignore);
        nb::enum_<AlphaHandling>(m, "AlphaHandling")
            .value("OverBlack", AlphaHandling::OverBlack)
            .value("Discard", AlphaHandling::Discard);

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

        bind_reflected_rw<CropOptions>(m, "CropOptions");
        bind_reflected_rw<OrientOptions>(m, "OrientOptions");
        bind_reflected_rw<AchromaticOptions>(m, "AchromaticOptions");
        bind_reflected<CropEvidence>(m, "CropEvidence");
        bind_reflected<OrientEvidence>(m, "OrientEvidence");
        bind_reflected<AchromaticEvidence>(m, "AchromaticEvidence");

        m.def("crop", &crop, "frame"_a, "options"_a, "block_grid"_a = nb::none(),
              nb::call_guard<nb::gil_scoped_release>());
        m.def("orient", &orient, "frame"_a, "options"_a, nb::call_guard<nb::gil_scoped_release>());
        m.def("achromatic", &achromatic, "frame"_a, "options"_a = AchromaticOptions{},
              nb::call_guard<nb::gil_scoped_release>());
    }
}
