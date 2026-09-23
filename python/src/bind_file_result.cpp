#include "bind_reflected.hpp"
#include "bindings.hpp"
#include "json_convert.hpp"

#include "lossylab/io/decode_image.hpp"
#include "lossylab/io/file_result.hpp"
#include "lossylab/io/probe.hpp"
#include "lossylab/io/read_headers.hpp"
#include "lossylab/io/video_reader.hpp"
#include "lossylab/measure/compression_history.hpp"
#include "lossylab/measure/measure.hpp"

#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

namespace lossylab::pybind
{
    using namespace nb::literals;

    namespace
    {
        /// What one VideoReader read produced: the selected frames and the
        /// record describing the read.
        // Not reflected: LOSSYLAB_REFLECT's explicit specialization must be
        // declared inside a namespace enclosing lossylab::reflect, and this
        // anonymous namespace (nested in lossylab::pybind, a sibling of
        // lossylab::reflect) does not qualify.
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

        bind_reflected<FileError>(m, "FileError")
            .def("__eq__", [](const FileError& self, const FileError& other) { return self == other; })
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
        bind_file_result<MeasureResult>(m, "MeasureFileResult")
            .def("to_dict", [](const FileResult<MeasureResult>& self) { return to_python(self.to_json()); });

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

        m.def(
            "capture_measure",
            [](const Source& source, const std::vector<Frame>& frames, const std::vector<Analyzer>& analyzers,
               const MeasureOptions& options)
            { return capture("measure", source, [&] { return measure(frames, analyzers, options); }); },
            "source"_a, "frames"_a, "analyzers"_a, "options"_a = MeasureOptions{},
            nb::call_guard<nb::gil_scoped_release>(),
            "measure(frames, analyzers, options), with any exception it raises returned as a FileError instead. "
            "`source` names the file the frames came from.");

        bind_file_result<CompareResult>(m, "CompareFileResult")
            .def("to_dict", [](const FileResult<CompareResult>& self) { return to_python(self.to_json()); });
        m.def(
            "capture_compare",
            [](const Source& source, const std::vector<Frame>& reference, const std::vector<Frame>& distorted,
               const std::vector<Metric>& metrics, const CompareOptions& options)
            { return capture("compare", source, [&] { return compare(reference, distorted, metrics, options); }); },
            "source"_a, "reference"_a, "distorted"_a, "metrics"_a, "options"_a = CompareOptions{},
            nb::call_guard<nb::gil_scoped_release>(),
            "compare(reference, distorted, metrics, options), with any exception it raises returned as a FileError "
            "instead. `source` names the file the distorted frames came from.");

        bind_file_result<CompressionHistory>(m, "CompressionHistoryFileResult")
            .def("to_dict", [](const FileResult<CompressionHistory>& self) { return to_python(self.to_json()); });
        m.def(
            "capture_compression_history",
            [](const Source& source, const Frame& frame, const CompressionHistoryOptions& options)
            {
                return capture("compression_history", source,
                               [&] { return compression_history(frame, options); });
            },
            "source"_a, "frame"_a, "options"_a = CompressionHistoryOptions{},
            nb::call_guard<nb::gil_scoped_release>(),
            "compression_history(frame, options), with any exception it raises returned as a FileError instead. "
            "`source` names the file the frame came from.");
    }
}
