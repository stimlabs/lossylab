#include "bindings.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/core/frame.hpp"

#include <nanobind/ndarray.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

namespace lossylab::pybind
{
    using namespace nb::literals;

    namespace
    {
        /// dtype for a plane's samples: FFmpeg only ever hands out 1 (8-bit)
        /// or 2 (9- through 16-bit) bytes per sample.
        nb::dlpack::dtype plane_dtype(int bytes_per_sample)
        {
            if (bytes_per_sample == 1)
            {
                return nb::dtype<std::uint8_t>();
            }
            if (bytes_per_sample == 2)
            {
                return nb::dtype<std::uint16_t>();
            }
            throw std::runtime_error("lossylab: unexpected plane sample width");
        }

        /// Shape/strides for a plane view. Components (for packed formats)
        /// become a trailing axis; planar formats (components_per_pixel == 1)
        /// stay 2-D. PlaneView::stride is in bytes; nanobind wants strides in
        /// elements.
        template <typename View>
        void plane_shape(const View& view, std::vector<std::size_t>& shape, std::vector<std::int64_t>& strides)
        {
            const std::int64_t row_stride_elems = view.stride / view.bytes_per_sample;
            if (view.components_per_pixel > 1)
            {
                shape = {static_cast<std::size_t>(view.height), static_cast<std::size_t>(view.width),
                         static_cast<std::size_t>(view.components_per_pixel)};
                strides = {row_stride_elems, view.components_per_pixel, 1};
            }
            else
            {
                shape = {static_cast<std::size_t>(view.height), static_cast<std::size_t>(view.width)};
                strides = {row_stride_elems, 1};
            }
        }

        nb::ndarray<nb::numpy, nb::ro> read_only_plane(const Frame& self, int index)
        {
            ConstPlaneView view = self.plane(index);
            std::vector<std::size_t> shape;
            std::vector<std::int64_t> strides;
            plane_shape(view, shape, strides);
            return nb::ndarray<nb::numpy, nb::ro>(view.data, shape.size(), shape.data(), nb::find(&self),
                                                   strides.data(), plane_dtype(view.bytes_per_sample));
        }

        /// Bytes of one row without padding.
        std::size_t packed_row_bytes(const ConstPlaneView& view)
        {
            return static_cast<std::size_t>(view.width) *
                   static_cast<std::size_t>(view.components_per_pixel * view.bytes_per_sample);
        }

        /// The plane of a frame that is one array: a single plane whose
        /// samples fill whole bytes. `method` names the caller in the error.
        ConstPlaneView single_array_plane(const Frame& self, const std::string& method)
        {
            if (self.plane_count() != 1)
            {
                throw ConfigError(method + "() takes a frame with one plane, not " + self.pixel_format().name() +
                                  "; use plane()");
            }
            const ConstPlaneView view = self.plane(0);
            if (static_cast<std::size_t>(view.row_bytes()) != packed_row_bytes(view))
            {
                throw ConfigError(method + "() needs whole-byte samples, which " + self.pixel_format().name() +
                                  " does not have");
            }
            return view;
        }

        nb::ndarray<nb::numpy> writable_plane(Frame& self, int index)
        {
            self.make_writable();
            PlaneView view = self.plane(index);
            std::vector<std::size_t> shape;
            std::vector<std::int64_t> strides;
            plane_shape(view, shape, strides);
            return nb::ndarray<nb::numpy>(view.data, shape.size(), shape.data(), nb::find(&self), strides.data(),
                                           plane_dtype(view.bytes_per_sample));
        }

        /// The samples of a single-plane format as one C-contiguous array:
        /// a view when the rows carry no padding, a copy otherwise. A
        /// writable view first detaches a shared buffer.
        nb::object to_numpy(Frame& self, const bool writable)
        {
            if (writable)
            {
                self.make_writable();
            }
            const ConstPlaneView view = single_array_plane(self, "to_numpy");
            const std::size_t row_bytes = packed_row_bytes(view);
            if (view.stride == static_cast<std::ptrdiff_t>(row_bytes))
            {
                return writable ? nb::cast(writable_plane(self, 0)) : nb::cast(read_only_plane(self, 0));
            }

            auto* copy = new std::uint8_t[row_bytes * static_cast<std::size_t>(view.height)];
            for (int row = 0; row < view.height; ++row)
            {
                std::memcpy(copy + row_bytes * static_cast<std::size_t>(row), view.row(row), row_bytes);
            }
            nb::capsule owner(copy, [](void* data) noexcept { delete[] static_cast<std::uint8_t*>(data); });
            std::vector<std::size_t> shape;
            std::vector<std::int64_t> strides;
            ConstPlaneView contiguous = view;
            contiguous.stride = static_cast<std::ptrdiff_t>(row_bytes);
            plane_shape(contiguous, shape, strides);
            if (writable)
            {
                return nb::cast(nb::ndarray<nb::numpy>(copy, shape.size(), shape.data(), owner, strides.data(),
                                                       plane_dtype(view.bytes_per_sample)));
            }
            return nb::cast(nb::ndarray<nb::numpy, nb::ro>(copy, shape.size(), shape.data(), owner, strides.data(),
                                                           plane_dtype(view.bytes_per_sample)));
        }

        /// The samples of a single-plane format through DLPack, as a writable
        /// view with the plane's own strides. A shared buffer is detached
        /// first.
        nb::object to_dlpack(Frame& self, const nb::kwargs& arguments)
        {
            self.make_writable();
            const ConstPlaneView view = single_array_plane(self, "__dlpack__");
            std::vector<std::size_t> shape;
            std::vector<std::int64_t> strides;
            plane_shape(view, shape, strides);
            const nb::ndarray<nb::array_api> array(const_cast<std::uint8_t*>(view.data), shape.size(), shape.data(),
                                                   nb::find(&self), strides.data(), plane_dtype(view.bytes_per_sample),
                                                   nb::device::cpu::value);
            return nb::cast(array).attr("__dlpack__")(**arguments);
        }

        /// "uint8", "float32", ... for an error message.
        std::string dtype_name(const nb::dlpack::dtype dtype)
        {
            std::string name;
            switch (static_cast<nb::dlpack::dtype_code>(dtype.code))
            {
            case nb::dlpack::dtype_code::Int:
                name = "int";
                break;
            case nb::dlpack::dtype_code::UInt:
                name = "uint";
                break;
            case nb::dlpack::dtype_code::Float:
                name = "float";
                break;
            case nb::dlpack::dtype_code::Complex:
                name = "complex";
                break;
            case nb::dlpack::dtype_code::Bool:
                return "bool";
            default:
                name = "dtype code " + std::to_string(dtype.code) + " of ";
                break;
            }
            return name + std::to_string(dtype.bits);
        }

        /// "(480, 640, 4)" for an error message.
        std::string shape_text(const nb::ndarray<nb::ro, nb::device::cpu>& array)
        {
            std::string text = "(";
            for (std::size_t axis = 0; axis < array.ndim(); ++axis)
            {
                text += (axis == 0 ? "" : ", ") + std::to_string(array.shape(axis));
            }
            return text + (array.ndim() == 1 ? ",)" : ")");
        }

        /// A copy of a uint8 (height, width, 3) array as an sRGB rgb24 frame.
        Frame from_numpy(const nb::ndarray<nb::ro, nb::device::cpu>& array)
        {
            if (array.dtype() != nb::dtype<std::uint8_t>())
            {
                throw ConfigError("Frame.from_numpy() takes a uint8 array, not " + dtype_name(array.dtype()));
            }
            if (array.ndim() != 3 || array.shape(2) != 3)
            {
                throw ConfigError("Frame.from_numpy() takes an array of shape (height, width, 3), not " +
                                  shape_text(array));
            }
            const auto height = static_cast<int>(array.shape(0));
            const auto width = static_cast<int>(array.shape(1));
            Frame frame = Frame::allocate(width, height, PixelFormat::from_name("rgb24"), ColorSpec::srgb());

            const auto* samples = static_cast<const std::uint8_t*>(array.data());
            const std::int64_t row_stride = array.stride(0);
            const std::int64_t column_stride = array.stride(1);
            const std::int64_t channel_stride = array.stride(2);
            const PlaneView plane = frame.plane(0);
            nb::gil_scoped_release release;
            for (int row = 0; row < height; ++row)
            {
                const std::uint8_t* source_row = samples + row * row_stride;
                std::uint8_t* target_row = plane.row(row);
                if (column_stride == 3 && channel_stride == 1)
                {
                    std::memcpy(target_row, source_row, static_cast<std::size_t>(width) * 3);
                    continue;
                }
                for (int column = 0; column < width; ++column)
                {
                    for (int channel = 0; channel < 3; ++channel)
                    {
                        target_row[column * 3 + channel] =
                            source_row[column * column_stride + channel * channel_stride];
                    }
                }
            }
            return frame;
        }
    }

    void bind_frame(nb::module_& m)
    {
        nb::class_<QpMap>(m, "QpMap")
            .def(nb::init<>())
            .def_rw("width", &QpMap::width)
            .def_rw("height", &QpMap::height)
            .def_rw("block_width", &QpMap::block_width)
            .def_rw("block_height", &QpMap::block_height)
            .def_rw("values", &QpMap::values)
            .def("at", &QpMap::at, "block_x"_a, "block_y"_a)
            .def("mean_over", &QpMap::mean_over, "pixels"_a);

        nb::class_<Frame>(m, "Frame")
            .def(nb::init<>())
            .def_static("allocate", &Frame::allocate, "width"_a, "height"_a, "pixel_format"_a, "color"_a,
                        "align"_a = 0)
            .def("empty", &Frame::empty)
            .def("__bool__", [](const Frame& self) { return !self.empty(); })
            .def("width", &Frame::width)
            .def("height", &Frame::height)
            .def("pixel_format", &Frame::pixel_format)
            .def("color", &Frame::color, nb::rv_policy::reference_internal)
            .def("set_color", &Frame::set_color, "color"_a)
            .def(
                "icc_profile",
                [](const Frame& self) -> std::optional<IccProfileInfo>
                {
                    const IccProfile* profile = self.icc_profile();
                    return profile != nullptr ? std::optional(profile->info) : std::nullopt;
                },
                "What the ICC profile the samples are to be read with says, or None.")
            .def(
                "icc_profile_bytes",
                [](const Frame& self) -> std::optional<nb::bytes>
                {
                    const IccProfile* profile = self.icc_profile();
                    if (profile == nullptr)
                    {
                        return std::nullopt;
                    }
                    return nb::bytes(reinterpret_cast<const char*>(profile->bytes.data()), profile->bytes.size());
                },
                "The ICC profile's bytes, or None.")
            .def(
                "set_icc_profile",
                [](Frame& self, const std::optional<nb::bytes>& bytes)
                {
                    if (!bytes.has_value())
                    {
                        self.clear_icc_profile();
                        return;
                    }
                    const auto* data = static_cast<const std::uint8_t*>(bytes->data());
                    self.set_icc_profile(std::span<const std::uint8_t>(data, bytes->size()));
                },
                "bytes"_a.none(), "Sets the ICC profile from its bytes, or removes it with None.")
            .def("orientation", &Frame::orientation)
            .def("set_orientation", &Frame::set_orientation, "orientation"_a.none())
            .def("sample_aspect_ratio", &Frame::sample_aspect_ratio)
            .def("set_sample_aspect_ratio", &Frame::set_sample_aspect_ratio, "sample_aspect_ratio"_a)
            .def("copy_embedded_from", &Frame::copy_embedded_from, "other"_a)
            .def("clear_embedded", &Frame::clear_embedded)
            .def("pts", &Frame::pts)
            .def("set_pts", &Frame::set_pts, "pts"_a)
            .def("time_base", &Frame::time_base)
            .def("set_time_base", &Frame::set_time_base, "time_base"_a)
            .def("timestamp_seconds", &Frame::timestamp_seconds)
            .def("picture_type", &Frame::picture_type)
            .def("is_key_frame", &Frame::is_key_frame)
            .def("plane_count", &Frame::plane_count)
            .def("plane", &read_only_plane, "index"_a,
                 "Read-only view of one plane's samples. Call writable_plane() to mutate.")
            .def("to_numpy", &to_numpy, "writable"_a = false,
                 nb::sig("def to_numpy(self, writable: bool = False) -> NDArray"),
                 "The samples as one C-contiguous array, (height, width, components) for a packed format such as "
                 "rgb24. A view when the rows carry no padding, a copy otherwise. Read-only unless `writable`; a "
                 "writable view first detaches a buffer the frame shares. Whether writes reach the frame depends "
                 "on that padding: use writable_plane() to change the frame. Raises ConfigError for a format with "
                 "several planes or with samples smaller than a byte.")
            .def_static("from_numpy", &from_numpy, "array"_a,
                        "A copy of a uint8 array of shape (height, width, 3) as an sRGB rgb24 frame, with no ICC "
                        "profile, no orientation and square pixels. Any strides are accepted. Raises ConfigError for "
                        "another dtype or shape.")
            .def("__dlpack__", &to_dlpack,
                 "The samples of a single-plane format through DLPack, as a writable view with the frame's own "
                 "row stride; a buffer the frame shares is detached first. Raises ConfigError like to_numpy().")
            .def("__dlpack_device__", [](const Frame&) { return nb::make_tuple(1, 0); },
                 "(1, 0): the samples are in CPU memory.")
            .def("samples_sha256", &Frame::samples_sha256)
            .def("writable_plane", &writable_plane, "index"_a,
                 "Detaches this Frame's buffer if shared, then returns a writable view of one plane.")
            .def("is_writable", &Frame::is_writable)
            .def("make_writable", &Frame::make_writable)
            .def("clone", &Frame::clone)
            .def("describe", &Frame::describe)
            .def("qp_map", &Frame::qp_map)
            .def("sync_color_to_av_frame", &Frame::sync_color_to_av_frame);
    }
}
