#include "bindings.hpp"
#include "json_convert.hpp"

#include "lossylab/io/decode_image.hpp"
#include "lossylab/io/file_result.hpp"
#include "lossylab/io/probe.hpp"
#include "lossylab/io/read_headers.hpp"
#include "lossylab/io/video_reader.hpp"

#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

namespace lossylab::pybind
{
    using namespace nb::literals;

    namespace
    {
        /// What one VideoReader read produced: the selected frames and the
        /// record describing the read.
        struct VideoFramesResult
        {
            std::vector<VideoFrame> frames;
            StageRecord record;
        };

        template <typename T>
        nb::class_<FileResult<T>> bind_file_result(nb::module_& m, const char* name)
        {
            return nb::class_<FileResult<T>>(m, name)
                .def("ok", &FileResult<T>::ok)
                .def("__bool__", &FileResult<T>::ok)
                .def(
                    "value", [](const FileResult<T>& self) -> const T& { return self.value(); },
                    nb::rv_policy::reference_internal, "The result. Raises ConfigError when the operation failed.")
                .def("error", &FileResult<T>::error, nb::rv_policy::reference_internal,
                     "The error. Raises ConfigError when the operation succeeded.")
                .def("log", &FileResult<T>::log, nb::rv_policy::reference_internal,
                     "What FFmpeg logged on the calling thread while the operation ran, at Info and above.");
        }
    }

    void bind_file_result(nb::module_& m)
    {
        nb::enum_<FileErrorKind>(m, "FileErrorKind")
            .value("Config", FileErrorKind::Config)
            .value("UnsupportedCapability", FileErrorKind::UnsupportedCapability)
            .value("ConversionRefused", FileErrorKind::ConversionRefused)
            .value("FFmpeg", FileErrorKind::FFmpeg)
            .value("NotImplemented", FileErrorKind::NotImplemented)
            .value("Library", FileErrorKind::Library)
            .value("OutOfMemory", FileErrorKind::OutOfMemory)
            .value("Internal", FileErrorKind::Internal);

        nb::class_<FileError>(m, "FileError")
            .def_ro("kind", &FileError::kind)
            .def_ro("operation", &FileError::operation)
            .def_ro("source", &FileError::source)
            .def_ro("message", &FileError::message)
            .def_prop_ro("details", [](const FileError& self) { return to_python(self.details); })
            .def("__eq__", [](const FileError& self, const FileError& other) { return self == other; })
            .def("to_dict", [](const FileError& self) { return to_python(self.to_json()); })
            .def_static("from_dict", [](nb::dict value) { return FileError::from_json(to_json(value)); });

        bind_file_result<ProbeResult>(m, "ProbeFileResult")
            .def("to_dict", [](const FileResult<ProbeResult>& self) { return to_python(self.to_json()); });
        bind_file_result<FrameResult>(m, "DecodeImageFileResult");
        bind_file_result<HeaderInfo>(m, "ReadHeadersFileResult")
            .def("to_dict", [](const FileResult<HeaderInfo>& self) { return to_python(self.to_json()); });

        nb::class_<VideoFramesResult>(m, "VideoFramesResult")
            .def_ro("frames", &VideoFramesResult::frames)
            .def_ro("record", &VideoFramesResult::record);
        bind_file_result<VideoFramesResult>(m, "VideoFramesFileResult");

        m.def(
            "capture_probe",
            [](const Source& source) { return capture("probe", source, [&] { return probe(source); }); },
            "source"_a, nb::call_guard<nb::gil_scoped_release>(),
            "probe(), with any exception it raises returned as a FileError instead.");

        m.def(
            "capture_decode_image",
            [](const Source& source, const DecodeImageOptions& options)
            { return capture("decode_image", source, [&] { return decode_image(source, options); }); },
            "source"_a, "options"_a = DecodeImageOptions{}, nb::call_guard<nb::gil_scoped_release>(),
            "decode_image(), with any exception it raises returned as a FileError instead.");

        m.def(
            "capture_read_headers",
            [](const Source& source, const ReadHeadersOptions& options)
            { return capture("read_headers", source, [&] { return read_headers(source, options); }); },
            "source"_a, "options"_a = ReadHeadersOptions{}, nb::call_guard<nb::gil_scoped_release>(),
            "read_headers(), with any exception it raises returned as a FileError instead.");

        m.def(
            "capture_video_frames",
            [](const Source& source, const FrameSelector& select, const VideoReaderOptions& options)
            {
                return capture("VideoReader.frames", source,
                               [&]
                               {
                                   VideoReader reader(source, options);
                                   std::vector<VideoFrame> frames = reader.frames(select);
                                   return VideoFramesResult{std::move(frames), reader.record()};
                               });
            },
            "source"_a, "select"_a, "options"_a = VideoReaderOptions{}, nb::call_guard<nb::gil_scoped_release>(),
            "VideoReader(source, options).frames(select) together with the read's record, with any exception "
            "either raises returned as a FileError instead.");
    }
}
