#include "bindings.hpp"
#include "json_convert.hpp"

#include "lossylab/core/codec_id.hpp"
#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/geometry.hpp"
#include "lossylab/core/kernel.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/rational.hpp"
#include "lossylab/core/strict.hpp"

#include <nanobind/operators.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

namespace lossylab::pybind
{
    using namespace nb::literals;

    namespace
    {
        template <typename T>
        void bind_json_methods(nb::class_<T>& cls)
        {
            cls.def("to_dict", [](const T& self) { return to_python(self.to_json()); });
            cls.def_static("from_dict", [](nb::dict value) { return T::from_json(to_json(value)); });
        }
    }

    void bind_core_types(nb::module_& m)
    {
        // ---- color_spec.hpp -------------------------------------------------
        nb::enum_<ColorMatrix>(m, "ColorMatrix")
            .value("Rgb", ColorMatrix::Rgb)
            .value("Bt709", ColorMatrix::Bt709)
            .value("Unspecified", ColorMatrix::Unspecified)
            .value("Fcc", ColorMatrix::Fcc)
            .value("Bt470bg", ColorMatrix::Bt470bg)
            .value("Smpte170m", ColorMatrix::Smpte170m)
            .value("Smpte240m", ColorMatrix::Smpte240m)
            .value("Ycgco", ColorMatrix::Ycgco)
            .value("Bt2020Ncl", ColorMatrix::Bt2020Ncl)
            .value("Bt2020Cl", ColorMatrix::Bt2020Cl)
            .value("Smpte2085", ColorMatrix::Smpte2085)
            .value("ChromaDerivedNcl", ColorMatrix::ChromaDerivedNcl)
            .value("ChromaDerivedCl", ColorMatrix::ChromaDerivedCl)
            .value("Ictcp", ColorMatrix::Ictcp)
            .value("IptC2", ColorMatrix::IptC2)
            .value("YcgcoRe", ColorMatrix::YcgcoRe)
            .value("YcgcoRo", ColorMatrix::YcgcoRo);

        nb::enum_<ColorRange>(m, "ColorRange")
            .value("Unspecified", ColorRange::Unspecified)
            .value("Limited", ColorRange::Limited)
            .value("Full", ColorRange::Full);

        nb::enum_<ColorPrimaries>(m, "ColorPrimaries")
            .value("Bt709", ColorPrimaries::Bt709)
            .value("Unspecified", ColorPrimaries::Unspecified)
            .value("Bt470m", ColorPrimaries::Bt470m)
            .value("Bt470bg", ColorPrimaries::Bt470bg)
            .value("Smpte170m", ColorPrimaries::Smpte170m)
            .value("Smpte240m", ColorPrimaries::Smpte240m)
            .value("Film", ColorPrimaries::Film)
            .value("Bt2020", ColorPrimaries::Bt2020)
            .value("Smpte428", ColorPrimaries::Smpte428)
            .value("Smpte431", ColorPrimaries::Smpte431)
            .value("Smpte432", ColorPrimaries::Smpte432)
            .value("Ebu3213", ColorPrimaries::Ebu3213);

        nb::enum_<TransferCharacteristic>(m, "TransferCharacteristic")
            .value("Bt709", TransferCharacteristic::Bt709)
            .value("Unspecified", TransferCharacteristic::Unspecified)
            .value("Gamma22", TransferCharacteristic::Gamma22)
            .value("Gamma28", TransferCharacteristic::Gamma28)
            .value("Smpte170m", TransferCharacteristic::Smpte170m)
            .value("Smpte240m", TransferCharacteristic::Smpte240m)
            .value("Linear", TransferCharacteristic::Linear)
            .value("Log", TransferCharacteristic::Log)
            .value("LogSqrt", TransferCharacteristic::LogSqrt)
            .value("Iec61966_2_4", TransferCharacteristic::Iec61966_2_4)
            .value("Bt1361Ecg", TransferCharacteristic::Bt1361Ecg)
            .value("Srgb", TransferCharacteristic::Srgb)
            .value("Bt2020_10", TransferCharacteristic::Bt2020_10)
            .value("Bt2020_12", TransferCharacteristic::Bt2020_12)
            .value("Smpte2084", TransferCharacteristic::Smpte2084)
            .value("Smpte428", TransferCharacteristic::Smpte428)
            .value("AribStdB67", TransferCharacteristic::AribStdB67);

        nb::enum_<ChromaLocation>(m, "ChromaLocation")
            .value("Unspecified", ChromaLocation::Unspecified)
            .value("Left", ChromaLocation::Left)
            .value("Center", ChromaLocation::Center)
            .value("TopLeft", ChromaLocation::TopLeft)
            .value("Top", ChromaLocation::Top)
            .value("BottomLeft", ChromaLocation::BottomLeft)
            .value("Bottom", ChromaLocation::Bottom);

        auto color_spec = nb::class_<ColorSpec>(m, "ColorSpec")
            .def(nb::init<>())
            .def_rw("matrix", &ColorSpec::matrix)
            .def_rw("range", &ColorSpec::range)
            .def_rw("primaries", &ColorSpec::primaries)
            .def_rw("transfer", &ColorSpec::transfer)
            .def_rw("chroma_location", &ColorSpec::chroma_location)
            .def_static("bt709_limited", &ColorSpec::bt709_limited)
            .def_static("bt709_full", &ColorSpec::bt709_full)
            .def_static("bt601_limited", &ColorSpec::bt601_limited)
            .def_static("bt601_full", &ColorSpec::bt601_full)
            .def_static("smpte170m_limited", &ColorSpec::smpte170m_limited)
            .def_static("bt2020_ncl_limited", &ColorSpec::bt2020_ncl_limited)
            .def_static("pq_bt2020", &ColorSpec::pq_bt2020)
            .def_static("hlg_bt2020", &ColorSpec::hlg_bt2020)
            .def_static("srgb", &ColorSpec::srgb)
            .def_static("jpeg", &ColorSpec::jpeg)
            .def("is_fully_specified", &ColorSpec::is_fully_specified)
            .def("require_fully_specified", &ColorSpec::require_fully_specified, "context"_a)
            .def("with_defaults_from", &ColorSpec::with_defaults_from, "fallback"_a)
            .def("is_rgb", &ColorSpec::is_rgb)
            .def("describe", &ColorSpec::describe)
            .def(nb::self == nb::self)
            .def(nb::self != nb::self);
        bind_json_methods(color_spec);

        // ---- pixel_format.hpp ------------------------------------------------
        nb::enum_<Subsampling>(m, "Subsampling")
            .value("Rgb", Subsampling::Rgb)
            .value("Gray", Subsampling::Gray)
            .value("Yuv444", Subsampling::Yuv444)
            .value("Yuv440", Subsampling::Yuv440)
            .value("Yuv422", Subsampling::Yuv422)
            .value("Yuv420", Subsampling::Yuv420)
            .value("Yuv411", Subsampling::Yuv411)
            .value("Yuv410", Subsampling::Yuv410);

        auto pixel_format = nb::class_<PixelFormat>(m, "PixelFormat")
            .def(nb::init<>())
            .def_static("from_name", &PixelFormat::from_name, "name"_a)
            .def_static("find", &PixelFormat::find, "name"_a)
            .def_static("planar_yuv", &PixelFormat::planar_yuv, "subsampling"_a, "bit_depth"_a,
                        "with_alpha"_a = false)
            .def_static("all", &PixelFormat::all)
            .def("raw", &PixelFormat::raw)
            .def("is_valid", &PixelFormat::is_valid)
            .def("name", &PixelFormat::name)
            .def("bit_depth", &PixelFormat::bit_depth)
            .def("component_count", &PixelFormat::component_count)
            .def("plane_count", &PixelFormat::plane_count)
            .def("log2_chroma_width", &PixelFormat::log2_chroma_width)
            .def("log2_chroma_height", &PixelFormat::log2_chroma_height)
            .def("subsampling", &PixelFormat::subsampling)
            .def("is_rgb", &PixelFormat::is_rgb)
            .def("is_gray", &PixelFormat::is_gray)
            .def("is_planar", &PixelFormat::is_planar)
            .def("has_alpha", &PixelFormat::has_alpha)
            .def("is_big_endian", &PixelFormat::is_big_endian)
            .def(nb::self == nb::self)
            .def(nb::self != nb::self);
        bind_json_methods(pixel_format);

        // ---- strict.hpp --------------------------------------------------
        nb::enum_<Strict>(m, "Strict").value("Refuse", Strict::Refuse).value("AllowRecorded", Strict::AllowRecorded);

        nb::enum_<ConversionCause>(m, "ConversionCause")
            .value("Requested", ConversionCause::Requested)
            .value("CodecConstraint", ConversionCause::CodecConstraint)
            .value("GraphNegotiation", ConversionCause::GraphNegotiation);

        auto conversion_event = nb::class_<ConversionEvent>(m, "ConversionEvent")
            .def(nb::init<>())
            .def_rw("property", &ConversionEvent::property)
            .def_rw("from_", &ConversionEvent::from)
            .def_rw("to", &ConversionEvent::to)
            .def_rw("cause", &ConversionEvent::cause)
            .def_rw("performed_by", &ConversionEvent::performed_by)
            .def(nb::self == nb::self)
            .def(nb::self != nb::self);
        bind_json_methods(conversion_event);

        // ---- kernel.hpp --------------------------------------------------
        nb::enum_<Kernel>(m, "Kernel")
            .value("Nearest", Kernel::Nearest)
            .value("Bilinear", Kernel::Bilinear)
            .value("Bicubic", Kernel::Bicubic)
            .value("Lanczos", Kernel::Lanczos)
            .value("Area", Kernel::Area)
            .value("Gaussian", Kernel::Gaussian)
            .value("Sinc", Kernel::Sinc)
            .value("Spline", Kernel::Spline)
            .value("Bicublin", Kernel::Bicublin);

        auto kernel_params = nb::class_<KernelParams>(m, "KernelParams")
            .def(nb::init<>())
            .def_rw("param_a", &KernelParams::param_a)
            .def_rw("param_b", &KernelParams::param_b)
            .def(nb::self == nb::self)
            .def(nb::self != nb::self);
        bind_json_methods(kernel_params);

        auto kernel_spec = nb::class_<KernelSpec>(m, "KernelSpec")
            .def(nb::init<>())
            .def(nb::init<Kernel, KernelParams>(), "kernel"_a, "params"_a = KernelParams{})
            .def_rw("kernel", &KernelSpec::kernel)
            .def_rw("params", &KernelSpec::params)
            .def("describe", &KernelSpec::describe);
        bind_json_methods(kernel_spec);

        // ---- rational.hpp --------------------------------------------------
        auto rational = nb::class_<Rational>(m, "Rational")
            .def(nb::init<>())
            .def(nb::init<int, int>(), "num"_a, "den"_a)
            .def_rw("num", &Rational::num)
            .def_rw("den", &Rational::den)
            .def("to_double", &Rational::to_double)
            .def("is_valid", &Rational::is_valid)
            .def("reduced", &Rational::reduced)
            .def("inverse", &Rational::inverse)
            .def("to_string", &Rational::to_string)
            .def_static("parse", &Rational::parse, "text"_a)
            .def(nb::self == nb::self)
            .def(nb::self != nb::self);
        bind_json_methods(rational);

        // ---- geometry.hpp --------------------------------------------------
        nb::class_<Point>(m, "Point")
            .def(nb::init<>())
            .def(nb::init<double, double>(), "x"_a, "y"_a)
            .def_rw("x", &Point::x)
            .def_rw("y", &Point::y);

        auto rect = nb::class_<Rect>(m, "Rect")
            .def(nb::init<>())
            .def(nb::init<double, double, double, double>(), "x"_a, "y"_a, "width"_a, "height"_a)
            .def_rw("x", &Rect::x)
            .def_rw("y", &Rect::y)
            .def_rw("width", &Rect::width)
            .def_rw("height", &Rect::height);
        bind_json_methods(rect);

        auto coordinate_transform = nb::class_<CoordinateTransform>(m, "CoordinateTransform")
            .def(nb::init<>())
            .def(nb::init<double, double, double, double, double, double>(), "scale_x"_a, "shear_x"_a,
                "translate_x"_a, "shear_y"_a, "scale_y"_a, "translate_y"_a)
            .def_rw("scale_x", &CoordinateTransform::scale_x)
            .def_rw("shear_x", &CoordinateTransform::shear_x)
            .def_rw("translate_x", &CoordinateTransform::translate_x)
            .def_rw("shear_y", &CoordinateTransform::shear_y)
            .def_rw("scale_y", &CoordinateTransform::scale_y)
            .def_rw("translate_y", &CoordinateTransform::translate_y)
            .def_static("identity", &CoordinateTransform::identity)
            .def_static("scaling", &CoordinateTransform::scaling, "scale_x"_a, "scale_y"_a)
            .def_static("translation", &CoordinateTransform::translation, "translate_x"_a, "translate_y"_a)
            .def_static("rotation_degrees", &CoordinateTransform::rotation_degrees, "degrees"_a)
            .def_static("resize", &CoordinateTransform::resize, "input_width"_a, "input_height"_a,
                        "output_width"_a, "output_height"_a)
            .def_static("crop", &CoordinateTransform::crop, "x"_a, "y"_a)
            .def("then", &CoordinateTransform::then, "after"_a)
            .def("map_forward", &CoordinateTransform::map_forward, "point"_a)
            .def("map_inverse", &CoordinateTransform::map_inverse, "point"_a)
            .def("map_bounds", &CoordinateTransform::map_bounds, "rect"_a)
            .def("inverse", &CoordinateTransform::inverse)
            .def("determinant", &CoordinateTransform::determinant)
            .def("is_invertible", &CoordinateTransform::is_invertible)
            .def("is_integer_translation", &CoordinateTransform::is_integer_translation)
            .def("is_identity", &CoordinateTransform::is_identity)
            .def(nb::self == nb::self)
            .def(nb::self != nb::self);
        bind_json_methods(coordinate_transform);

        nb::enum_<BlockGridKind>(m, "BlockGridKind")
            .value("Dct8", BlockGridKind::Dct8)
            .value("Macroblock16", BlockGridKind::Macroblock16)
            .value("Ctu32", BlockGridKind::Ctu32)
            .value("Ctu64", BlockGridKind::Ctu64);

        auto block_grid = nb::class_<BlockGrid>(m, "BlockGrid")
            .def(nb::init<>())
            .def_rw("kind", &BlockGrid::kind)
            .def_rw("block_width", &BlockGrid::block_width)
            .def_rw("block_height", &BlockGrid::block_height)
            .def_rw("phase_x", &BlockGrid::phase_x)
            .def_rw("phase_y", &BlockGrid::phase_y)
            .def_rw("valid", &BlockGrid::valid)
            .def_static("for_kind", &BlockGrid::for_kind, "kind"_a)
            .def("apply_transform", &BlockGrid::apply_transform, "transform"_a)
            .def("is_block_origin", &BlockGrid::is_block_origin, "x"_a, "y"_a)
            .def(nb::self == nb::self)
            .def(nb::self != nb::self);
        bind_json_methods(block_grid);

        // ---- codec_id.hpp --------------------------------------------------
        // Bound because capabilities()'s select_encoder/select_decoder/require_*
        // take these as arguments, even though encoding itself isn't
        // implemented yet.
        nb::enum_<ImageCodec>(m, "ImageCodec")
            .value("Png", ImageCodec::Png)
            .value("Mjpeg", ImageCodec::Mjpeg)
            .value("WebP", ImageCodec::WebP)
            .value("Avif", ImageCodec::Avif)
            .value("Jxl", ImageCodec::Jxl)
            .value("Heif", ImageCodec::Heif);

        nb::enum_<VideoCodec>(m, "VideoCodec")
            .value("H264", VideoCodec::H264)
            .value("Hevc", VideoCodec::Hevc)
            .value("Vp9", VideoCodec::Vp9)
            .value("Av1", VideoCodec::Av1);

        nb::enum_<EncoderBackend>(m, "EncoderBackend")
            .value("Software", EncoderBackend::Software)
            .value("Vaapi", EncoderBackend::Vaapi)
            .value("Nvenc", EncoderBackend::Nvenc)
            .value("Qsv", EncoderBackend::Qsv)
            .value("VideoToolbox", EncoderBackend::VideoToolbox);

        nb::enum_<ResizeBackend>(m, "ResizeBackend")
            .value("Swscale", ResizeBackend::Swscale)
            .value("Zscale", ResizeBackend::Zscale);

        nb::enum_<Metric>(m, "Metric")
            .value("Psnr", Metric::Psnr)
            .value("Ssim", Metric::Ssim)
            .value("Vmaf", Metric::Vmaf);
    }
}
