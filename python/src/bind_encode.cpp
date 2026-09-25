#include "bind_reflected.hpp"
#include "bindings.hpp"
#include "json_convert.hpp"

#include "lossylab/codec/encode.hpp"

#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

namespace lossylab::pybind
{
    using namespace nb::literals;

    namespace
    {
        nb::bytes to_bytes(const std::vector<std::uint8_t>& bytes)
        {
            return nb::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
    }

    void bind_encode(nb::module_& m)
    {
        auto rate_control = nb::class_<RateControl>(m, "RateControl");
        nb::enum_<RateControl::Mode>(rate_control, "Mode")
            .value("Crf", RateControl::Mode::Crf)
            .value("ConstantQp", RateControl::Mode::ConstantQp)
            .value("Bitrate", RateControl::Mode::Bitrate)
            .value("Constrained", RateControl::Mode::Constrained)
            .value("Quality", RateControl::Mode::Quality);
        rate_control.def_static("crf", &RateControl::crf, "value"_a)
            .def_static("constant_qp", &RateControl::constant_qp, "qp"_a)
            .def_static("bitrate", &RateControl::bitrate, "bits_per_second"_a)
            .def_static("constrained", &RateControl::constrained, "bits_per_second"_a, "max_rate"_a,
                        "buffer_size"_a)
            .def_static("quality", &RateControl::quality, "value"_a)
            .def_prop_ro("mode", &RateControl::mode)
            .def_prop_ro("value", &RateControl::value)
            .def_prop_ro("rate", &RateControl::rate)
            .def_prop_ro("max_rate", &RateControl::max_rate)
            .def_prop_ro("buffer_size", &RateControl::buffer_size)
            .def("quality_parameter", &RateControl::quality_parameter)
            .def("with_quality_parameter", &RateControl::with_quality_parameter, "value"_a)
            .def("describe", &RateControl::describe)
            .def("__repr__", [](const RateControl& self) { return "RateControl(" + self.describe() + ")"; })
            .def("to_dict", [](const RateControl& self) { return to_python(self.to_json()); })
            .def_static("from_dict", [](nb::dict value) { return RateControl::from_json(to_json(value)); });

        bind_reflected_rw<GopStructure>(m, "GopStructure")
            .def_static("intra_only", &GopStructure::intra_only)
            .def_static("from_dict", [](nb::dict value) { return GopStructure::from_json(to_json(value)); });

        bind_reflected_rw<EncodeVideoOptions>(m, "EncodeVideoOptions");

        bind_reflected_rw<EncodeImageOptions>(m, "EncodeImageOptions");

        bind_reflected_rw<DecodeSpec>(m, "DecodeSpec");

        auto target = bind_reflected_rw<EncodeTarget>(m, "EncodeTarget");
        nb::enum_<EncodeTarget::Kind>(target, "Kind")
            .value("BitsPerPixel", EncodeTarget::Kind::BitsPerPixel)
            .value("Psnr", EncodeTarget::Kind::Psnr)
            .value("Ssim", EncodeTarget::Kind::Ssim)
            .value("Vmaf", EncodeTarget::Kind::Vmaf);
        target.def("describe", &EncodeTarget::describe)
            .def_static("from_dict", [](nb::dict value) { return EncodeTarget::from_json(to_json(value)); });

        nb::class_<EncodedResult>(m, "EncodedResult")
            .def_prop_ro("bytes", [](const EncodedResult& self) { return to_bytes(self.bytes); })
            .def_ro("record", &EncodedResult::record)
            .def_prop_ro("configuration", [](const EncodedResult& self) { return to_python(self.configuration); })
            .def("bits_per_pixel", &EncodedResult::bits_per_pixel);

        nb::class_<FramesResult>(m, "FramesResult")
            .def_ro("frames", &FramesResult::frames)
            .def_ro("record", &FramesResult::record)
            .def_prop_ro("configuration", [](const FramesResult& self) { return to_python(self.configuration); });

        nb::class_<EncodeToTargetResult>(m, "EncodeToTargetResult")
            .def_prop_ro("bytes", [](const EncodeToTargetResult& self) { return to_bytes(self.bytes); })
            .def_ro("record", &EncodeToTargetResult::record)
            .def_prop_ro("configuration",
                         [](const EncodeToTargetResult& self) { return to_python(self.configuration); })
            .def_ro("quality_parameter", &EncodeToTargetResult::quality_parameter)
            .def_ro("achieved", &EncodeToTargetResult::achieved)
            .def_ro("iterations", &EncodeToTargetResult::iterations)
            .def_ro("converged", &EncodeToTargetResult::converged);

        m.def("encode_video", &encode_video, "frames"_a, "options"_a, nb::call_guard<nb::gil_scoped_release>());
        m.def("encode_image", &encode_image, "frame"_a, "options"_a, nb::call_guard<nb::gil_scoped_release>());
        m.def("roundtrip",
              nb::overload_cast<const std::vector<Frame>&, const EncodeVideoOptions&, const DecodeSpec&>(&roundtrip),
              "frames"_a, "encode_spec"_a, "decode_spec"_a = DecodeSpec{}, nb::call_guard<nb::gil_scoped_release>());
        m.def("roundtrip", nb::overload_cast<const Frame&, const EncodeImageOptions&, const DecodeSpec&>(&roundtrip),
              "frame"_a, "encode_spec"_a, "decode_spec"_a = DecodeSpec{}, nb::call_guard<nb::gil_scoped_release>());
        m.def("encode_to_target",
              nb::overload_cast<const std::vector<Frame>&, const EncodeVideoOptions&, const EncodeTarget&>(
                  &encode_to_target),
              "frames"_a, "options"_a, "target"_a, nb::call_guard<nb::gil_scoped_release>());
        m.def("encode_to_target",
              nb::overload_cast<const Frame&, const EncodeImageOptions&, const EncodeTarget&>(&encode_to_target),
              "frame"_a, "options"_a, "target"_a, nb::call_guard<nb::gil_scoped_release>());
    }
}
