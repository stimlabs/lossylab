#include "bindings.hpp"

#include "lossylab/core/frame.hpp"

#include <nanobind/ndarray.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/vector.h>

#include <cstdint>
#include <span>
#include <stdexcept>

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
