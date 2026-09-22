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

        nb::class_<LibraryVersion>(m, "LibraryVersion")
            .def_ro("name", &LibraryVersion::name)
            .def_ro("major", &LibraryVersion::major)
            .def_ro("minor", &LibraryVersion::minor)
            .def_ro("micro", &LibraryVersion::micro)
            .def_ro("compiled_major", &LibraryVersion::compiled_major)
            .def_ro("compiled_minor", &LibraryVersion::compiled_minor)
            .def_ro("compiled_micro", &LibraryVersion::compiled_micro)
            .def("to_string", &LibraryVersion::to_string)
            .def("matches_compiled", &LibraryVersion::matches_compiled)
            .def("to_dict", [](const LibraryVersion& self) { return to_python(self.to_json()); });

        nb::class_<BuildInfo>(m, "BuildInfo")
            .def_ro("version", &BuildInfo::version)
            .def_ro("configuration", &BuildInfo::configuration)
            .def_ro("license", &BuildInfo::license)
            .def_ro("libraries", &BuildInfo::libraries)
            .def_ro("external_libraries", &BuildInfo::external_libraries)
            .def_ro("build_id", &BuildInfo::build_id)
            .def("is_consistent", &BuildInfo::is_consistent)
            .def("has_external_library", &BuildInfo::has_external_library, "name"_a)
            .def("to_dict", [](const BuildInfo& self) { return to_python(self.to_json()); });

        m.def("build_info", &build_info, nb::rv_policy::reference);

        // ---- capabilities.hpp --------------------------------------------------
        nb::class_<OptionSchema>(m, "OptionSchema")
            .def_ro("name", &OptionSchema::name)
            .def_ro("help", &OptionSchema::help)
            .def_ro("type", &OptionSchema::type)
            .def_ro("min_value", &OptionSchema::min_value)
            .def_ro("max_value", &OptionSchema::max_value)
            .def_ro("default_value", &OptionSchema::default_value)
            .def_ro("choices", &OptionSchema::choices)
            .def("to_dict", [](const OptionSchema& self) { return to_python(self.to_json()); });

        nb::class_<CodecInfo>(m, "CodecInfo")
            .def_ro("name", &CodecInfo::name)
            .def_ro("long_name", &CodecInfo::long_name)
            .def_ro("codec_name", &CodecInfo::codec_name)
            .def_ro("is_encoder", &CodecInfo::is_encoder)
            .def_ro("is_hardware", &CodecInfo::is_hardware)
            .def_ro("experimental", &CodecInfo::experimental)
            .def_ro("pixel_formats", &CodecInfo::pixel_formats)
            .def_ro("options", &CodecInfo::options)
            .def("accepts", &CodecInfo::accepts, "pixel_format"_a)
            .def("find_option", &CodecInfo::find_option, "name"_a, nb::rv_policy::reference_internal)
            .def("to_dict", [](const CodecInfo& self) { return to_python(self.to_json()); });

        nb::class_<FilterInfo>(m, "FilterInfo")
            .def_ro("name", &FilterInfo::name)
            .def_ro("description", &FilterInfo::description)
            .def_ro("input_count", &FilterInfo::input_count)
            .def_ro("output_count", &FilterInfo::output_count)
            .def_ro("dynamic_inputs", &FilterInfo::dynamic_inputs)
            .def_ro("dynamic_outputs", &FilterInfo::dynamic_outputs)
            .def_ro("supports_slice_threads", &FilterInfo::supports_slice_threads)
            .def_ro("options", &FilterInfo::options)
            .def("to_dict", [](const FilterInfo& self) { return to_python(self.to_json()); });

        nb::class_<HardwareDeviceInfo>(m, "HardwareDeviceInfo").def_ro("name", &HardwareDeviceInfo::name);

        m.def("hardware_device_usable", &hardware_device_usable, "name"_a,
              "Opens the device and may load vendor drivers. Call in the parent process, before "
              "forking DataLoader workers, never inside one.");

        nb::class_<Capabilities>(m, "Capabilities")
            .def("encoders", &Capabilities::encoders)
            .def("decoders", &Capabilities::decoders)
            .def("filters", &Capabilities::filters)
            .def("hardware_devices", &Capabilities::hardware_devices)
            .def("find_encoder", &Capabilities::find_encoder, "name"_a, nb::rv_policy::reference_internal)
            .def("find_decoder", &Capabilities::find_decoder, "name"_a, nb::rv_policy::reference_internal)
            .def("find_filter", &Capabilities::find_filter, "name"_a, nb::rv_policy::reference_internal)
            .def("has_encoder", &Capabilities::has_encoder, "name"_a)
            .def("has_decoder", &Capabilities::has_decoder, "name"_a)
            .def("has_filter", &Capabilities::has_filter, "name"_a)
            .def("has_hardware_device", &Capabilities::has_hardware_device, "name"_a)
            .def("select_encoder",
                 nb::overload_cast<ImageCodec>(&Capabilities::select_encoder, nb::const_), "codec"_a,
                 nb::rv_policy::reference_internal)
            .def("select_encoder",
                 nb::overload_cast<VideoCodec, EncoderBackend>(&Capabilities::select_encoder, nb::const_),
                 "codec"_a, "backend"_a = EncoderBackend::Software, nb::rv_policy::reference_internal)
            .def("select_decoder",
                 nb::overload_cast<ImageCodec>(&Capabilities::select_decoder, nb::const_), "codec"_a,
                 nb::rv_policy::reference_internal)
            .def("select_decoder", nb::overload_cast<VideoCodec>(&Capabilities::select_decoder, nb::const_),
                 "codec"_a, nb::rv_policy::reference_internal)
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

        nb::class_<LogMessage>(m, "LogMessage")
            .def_ro("level", &LogMessage::level)
            .def_ro("component", &LogMessage::component)
            .def_ro("text", &LogMessage::text);

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
