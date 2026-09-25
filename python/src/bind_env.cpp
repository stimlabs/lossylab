#include "bind_reflected.hpp"
#include "bindings.hpp"
#include "json_convert.hpp"

#include "lossylab/env/build_info.hpp"
#include "lossylab/env/capabilities.hpp"
#include "lossylab/env/log.hpp"

#include <nanobind/stl/function.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

namespace lossylab::pybind
{
    using namespace nb::literals;

    void bind_env(nb::module_& m)
    {
        // ---- build_info.hpp --------------------------------------------------
        nb::enum_<License>(m, "License")
            .value("Lgpl21", License::Lgpl21)
            .value("Lgpl3", License::Lgpl3)
            .value("Gpl2", License::Gpl2)
            .value("Gpl3", License::Gpl3)
            .value("Nonfree", License::Nonfree)
            .value("Unknown", License::Unknown);

        m.def("permits_proprietary_distribution", &permits_proprietary_distribution, "license"_a);

        bind_reflected<LibraryVersion>(m, "LibraryVersion")
            .def("to_string", &LibraryVersion::to_string)
            .def("matches_compiled", &LibraryVersion::matches_compiled);

        bind_reflected<LossylabBuild>(m, "LossylabBuild");

        bind_reflected<FfmpegBuild>(m, "FfmpegBuild").def("is_consistent", &FfmpegBuild::is_consistent);

        bind_reflected<BuildInfo>(m, "BuildInfo");

        m.def("build_info", &build_info, nb::rv_policy::reference);

        bind_reflected<Diagnostics>(m, "Diagnostics");

        m.def("diagnostics", &diagnostics, nb::rv_policy::reference);

        m.def(
            "build_diff", [](nb::dict a, nb::dict b) { return to_python(build_diff(to_json(a), to_json(b))); },
            "a"_a, "b"_a,
            "Every leaf that differs between two builds' to_dict(), keyed by its dotted path, as [in_a, in_b].");

        // ---- capabilities.hpp --------------------------------------------------
        bind_reflected<OptionSchema>(m, "OptionSchema");

        bind_reflected<CodecInfo>(m, "CodecInfo")
            .def("accepts", &CodecInfo::accepts, "pixel_format"_a)
            .def("find_option", &CodecInfo::find_option, "name"_a, nb::rv_policy::reference_internal,
                 nb::sig("def find_option(self, name: str) -> OptionSchema | None"));

        bind_reflected<FilterInfo>(m, "FilterInfo");

        bind_reflected<HardwareDeviceInfo>(m, "HardwareDeviceInfo");

        m.def("hardware_device_usable", &hardware_device_usable, "name"_a,
              "Opens the device and may load vendor drivers. Call in the parent process, before "
              "forking DataLoader workers, never inside one.");

        nb::class_<Capabilities>(m, "Capabilities")
            .def("encoders", &Capabilities::encoders)
            .def("decoders", &Capabilities::decoders)
            .def("filters", &Capabilities::filters)
            .def("hardware_devices", &Capabilities::hardware_devices)
            .def("find_encoder", &Capabilities::find_encoder, "name"_a, nb::rv_policy::reference_internal,
                 nb::sig("def find_encoder(self, name: str) -> CodecInfo | None"))
            .def("find_decoder", &Capabilities::find_decoder, "name"_a, nb::rv_policy::reference_internal,
                 nb::sig("def find_decoder(self, name: str) -> CodecInfo | None"))
            .def("find_filter", &Capabilities::find_filter, "name"_a, nb::rv_policy::reference_internal,
                 nb::sig("def find_filter(self, name: str) -> FilterInfo | None"))
            .def("has_encoder", &Capabilities::has_encoder, "name"_a)
            .def("has_decoder", &Capabilities::has_decoder, "name"_a)
            .def("has_filter", &Capabilities::has_filter, "name"_a)
            .def("has_hardware_device", &Capabilities::has_hardware_device, "name"_a)
            .def("select_encoder",
                 nb::overload_cast<ImageCodec>(&Capabilities::select_encoder, nb::const_), "codec"_a,
                 nb::rv_policy::reference_internal,
                 nb::sig("def select_encoder(self, codec: ImageCodec) -> CodecInfo | None"))
            .def("select_encoder",
                 nb::overload_cast<VideoCodec, EncoderBackend>(&Capabilities::select_encoder, nb::const_),
                 "codec"_a, "backend"_a = EncoderBackend::Software, nb::rv_policy::reference_internal,
                 nb::sig("def select_encoder(self, codec: VideoCodec, "
                         "backend: EncoderBackend = EncoderBackend.Software) -> CodecInfo | None"))
            .def("select_decoder",
                 nb::overload_cast<ImageCodec>(&Capabilities::select_decoder, nb::const_), "codec"_a,
                 nb::rv_policy::reference_internal,
                 nb::sig("def select_decoder(self, codec: ImageCodec) -> CodecInfo | None"))
            .def("select_decoder", nb::overload_cast<VideoCodec>(&Capabilities::select_decoder, nb::const_),
                 "codec"_a, nb::rv_policy::reference_internal,
                 nb::sig("def select_decoder(self, codec: VideoCodec) -> CodecInfo | None"))
            .def("require_encoder",
                 nb::overload_cast<ImageCodec>(&Capabilities::require_encoder, nb::const_), "codec"_a,
                 nb::rv_policy::reference_internal)
            .def("require_encoder",
                 nb::overload_cast<VideoCodec, EncoderBackend>(&Capabilities::require_encoder, nb::const_),
                 "codec"_a, "backend"_a = EncoderBackend::Software, nb::rv_policy::reference_internal)
            .def("require_decoder",
                 nb::overload_cast<ImageCodec>(&Capabilities::require_decoder, nb::const_), "codec"_a,
                 nb::rv_policy::reference_internal)
            .def("require_decoder", nb::overload_cast<VideoCodec>(&Capabilities::require_decoder, nb::const_),
                 "codec"_a, nb::rv_policy::reference_internal)
            .def("require_filter", &Capabilities::require_filter, "name"_a, nb::rv_policy::reference_internal)
            .def("require_resize_backend", &Capabilities::require_resize_backend, "backend"_a)
            .def("require_metric", &Capabilities::require_metric, "metric"_a)
            .def("supports", nb::overload_cast<ImageCodec>(&Capabilities::supports, nb::const_), "codec"_a)
            .def("supports", nb::overload_cast<VideoCodec, EncoderBackend>(&Capabilities::supports, nb::const_),
                 "codec"_a, "backend"_a = EncoderBackend::Software)
            .def("supports", nb::overload_cast<ResizeBackend>(&Capabilities::supports, nb::const_), "backend"_a)
            .def("supports", nb::overload_cast<Metric>(&Capabilities::supports, nb::const_), "metric"_a)
            .def("to_dict", [](const Capabilities& self) { return to_python(self.to_json()); })
            .def("to_dict_full", [](const Capabilities& self) { return to_python(self.to_json_full()); });

        m.def("capabilities", &capabilities, nb::rv_policy::reference);

        // ---- log.hpp --------------------------------------------------
        nb::enum_<LogLevel>(m, "LogLevel")
            .value("Quiet", LogLevel::Quiet)
            .value("Panic", LogLevel::Panic)
            .value("Fatal", LogLevel::Fatal)
            .value("Error", LogLevel::Error)
            .value("Warning", LogLevel::Warning)
            .value("Info", LogLevel::Info)
            .value("Verbose", LogLevel::Verbose)
            .value("Debug", LogLevel::Debug)
            .value("Trace", LogLevel::Trace);

        bind_reflected<LogMessage>(m, "LogMessage")
            .def("__eq__", [](const LogMessage& self, const LogMessage& other) { return self == other; });

        // set_log_handler installs a process-global callback that FFmpeg calls
        // from arbitrary internal threads, not just from a thread that released
        // the GIL on the way in here. nanobind's std::function caster acquires
        // the GIL on every invocation and keeps the Python callable alive, so
        // no manual trampoline is needed.
        m.def("set_log_handler", &set_log_handler, "handler"_a.none(), "level"_a = LogLevel::Info,
              "Passing None restores FFmpeg's default output.");
        m.def("mute_log", &mute_log);
        m.def("log_level", &log_level);
    }
}
