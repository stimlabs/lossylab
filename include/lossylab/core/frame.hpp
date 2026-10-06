#pragma once

#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/rational.hpp"
#include "lossylab/core/record.hpp"
#include "lossylab/io/icc_profile.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

struct AVFrame;

namespace lossylab
{
    /// A borrowed view of one plane's samples.
    ///
    /// Points into the frame's buffer; it does not own anything and is
    /// invalidated by anything that reallocates the frame. This is the surface
    /// the Python bindings will expose as a zero-copy NumPy array, which is why
    /// it carries a byte stride rather than assuming packed rows: FFmpeg pads
    /// rows for alignment, so stride is almost never width * bytes_per_sample.
    struct PlaneView
    {
        std::uint8_t* data = nullptr;

        /// Bytes between the start of one row and the next. May exceed the row
        /// length, and may be negative for a vertically flipped frame.
        std::ptrdiff_t stride = 0;

        /// Samples per row, for this plane. Chroma planes are smaller than
        /// luma by the format's subsampling factors.
        int width = 0;
        int height = 0;

        /// 1 for 8-bit formats, 2 for 9- through 16-bit.
        int bytes_per_sample = 1;

        /// For packed formats, the number of components interleaved per pixel;
        /// 1 for planar formats.
        int components_per_pixel = 1;

        [[nodiscard]] bool empty() const noexcept { return data == nullptr; }

        /// Bytes actually occupied by samples in a row, ignoring padding.
        [[nodiscard]] std::ptrdiff_t row_bytes() const noexcept;

        /// Pointer to the start of `row`. Bounds are not checked.
        [[nodiscard]] std::uint8_t* row(int index) const noexcept;
    };

    struct ConstPlaneView
    {
        ConstPlaneView() = default;

        /// A mutable view converts to a const one, so read-only helpers take
        /// ConstPlaneView and work on either.
        ConstPlaneView(const PlaneView& view) noexcept
            : data(view.data), stride(view.stride), width(view.width), height(view.height),
              bytes_per_sample(view.bytes_per_sample),
              components_per_pixel(view.components_per_pixel)
        {
        }

        const std::uint8_t* data = nullptr;
        std::ptrdiff_t stride = 0;
        int width = 0;
        int height = 0;
        int bytes_per_sample = 1;
        int components_per_pixel = 1;

        [[nodiscard]] bool empty() const noexcept { return data == nullptr; }
        [[nodiscard]] std::ptrdiff_t row_bytes() const noexcept;
        [[nodiscard]] const std::uint8_t* row(int index) const noexcept;
    };

    /// Per-block quantizer values a decoder exported for a frame.
    ///
    /// The strongest per-region signal of compression strength available
    /// without re-encoding, which is what makes crop-level severity estimates
    /// possible rather than only whole-frame ones.
    struct QpMap
    {
        int width = 0;   ///< In blocks, not pixels
        int height = 0;
        int block_width = 16;
        int block_height = 16;
        std::vector<int> values;  ///< Row-major, width * height entries

        [[nodiscard]] int at(int block_x, int block_y) const;

        /// Mean quantizer over a pixel rectangle, which is how a crop's
        /// severity is read off the map.
        [[nodiscard]] double mean_over(const Rect& pixels) const;
    };

    /// A decoded image or video frame.
    ///
    /// Holds its samples inside C++ between operations, so a chain of stages
    /// costs no conversions to and from NumPy. Copying a Frame is cheap: copies
    /// share the underlying buffers, like FFmpeg's own reference counting. Call
    /// `make_writable` before modifying samples in place, or `clone` for an
    /// independent deep copy.
    ///
    /// A Frame always carries its ColorSpec. Nothing in the library infers one.
    class Frame
    {
    public:
        Frame();
        ~Frame();

        Frame(const Frame& other);
        Frame(Frame&& other) noexcept;
        Frame& operator=(const Frame& other);
        Frame& operator=(Frame&& other) noexcept;

        /// Allocates an uninitialized frame. Sample contents are undefined;
        /// `align` is the row alignment in bytes, 0 meaning FFmpeg's default.
        static Frame allocate(int width, int height, PixelFormat pixel_format,
                              const ColorSpec& color, int align = 0);

        /// Adopts an existing AVFrame, taking a new reference to its buffers.
        /// The FFmpeg boundary; `raw` must outlive the call but not the Frame.
        ///
        /// The ICC profile and display matrix FFmpeg attached become the
        /// Frame's `icc_profile()` and `orientation()`, the orientation read
        /// from the EXIF block when no display matrix gives one. All three
        /// are then removed from the new reference, together with the rest of
        /// the EXIF block, so what an encoder later sees is what the Frame
        /// states and nothing else.
        static Frame from_av_frame(const AVFrame* raw, const ColorSpec& color);

        /// As above, reading the ColorSpec from the frame's own tags. Tags that
        /// are unspecified stay unspecified rather than being guessed at.
        static Frame from_av_frame(const AVFrame* raw);

        [[nodiscard]] bool empty() const noexcept;
        explicit operator bool() const noexcept { return !empty(); }

        [[nodiscard]] int width() const noexcept;
        [[nodiscard]] int height() const noexcept;
        [[nodiscard]] PixelFormat pixel_format() const noexcept;

        [[nodiscard]] const ColorSpec& color() const noexcept { return m_color; }

        /// Relabels the samples without touching them. This is what
        /// `reinterpret` does, and the reason it is separate from `convert`:
        /// one changes the numbers, the other changes what they mean.
        void set_color(const ColorSpec& color) noexcept { m_color = color; }

        /// The ICC profile the samples are to be read with, or nullptr.
        [[nodiscard]] const IccProfile* icc_profile() const noexcept { return m_icc_profile.get(); }
        void set_icc_profile(std::span<const std::uint8_t> bytes);
        void clear_icc_profile() noexcept { m_icc_profile.reset(); }

        /// The EXIF orientation (2 to 8) the samples are stored in and which
        /// has not been applied to them; nullopt when there is nothing to
        /// apply. Setting 1, upright, is the same as setting nullopt.
        [[nodiscard]] std::optional<int> orientation() const noexcept { return m_orientation; }
        void set_orientation(std::optional<int> orientation);

        /// The shape of one pixel, width to height; 1:1 when unknown.
        [[nodiscard]] Rational sample_aspect_ratio() const noexcept;
        void set_sample_aspect_ratio(Rational sample_aspect_ratio);

        /// Takes over `other`'s ICC profile, orientation and sample aspect
        /// ratio, for an operation that writes its output into a new Frame.
        void copy_embedded_from(const Frame& other);

        /// Removes the ICC profile and orientation and makes the pixels
        /// square, leaving samples that are read by their ColorSpec alone.
        void clear_embedded() noexcept;

        [[nodiscard]] std::int64_t pts() const noexcept;
        void set_pts(std::int64_t pts) noexcept;

        [[nodiscard]] Rational time_base() const noexcept { return m_time_base; }
        void set_time_base(Rational time_base) noexcept { m_time_base = time_base; }

        /// Presentation time in seconds, or nullopt when the frame has no
        /// timestamp or no time base to interpret it with.
        [[nodiscard]] std::optional<double> timestamp_seconds() const noexcept;

        [[nodiscard]] PictureType picture_type() const noexcept;
        [[nodiscard]] bool is_key_frame() const noexcept;

        [[nodiscard]] int plane_count() const noexcept;

        /// Views of one plane's samples. Calling the mutable form on a frame
        /// whose buffers are shared throws; call `make_writable` first, so that
        /// an accidental write through a shared buffer cannot corrupt another
        /// Frame that looks independent.
        [[nodiscard]] PlaneView plane(int index);
        [[nodiscard]] ConstPlaneView plane(int index) const;

        /// True when this Frame holds the only reference to its buffers.
        [[nodiscard]] bool is_writable() const noexcept;

        /// Ensures the buffers are not shared, copying them if they are.
        void make_writable();

        /// An independent deep copy, sharing nothing.
        [[nodiscard]] Frame clone() const;

        /// Format, color, ICC profile, orientation and pixel shape as a
        /// record entry.
        [[nodiscard]] FormatDescription describe() const;

        /// "sha256:" and the SHA-256 of `describe()` and the samples, row by
        /// row without padding, palette included. Equal for two frames that
        /// hold the same image, however their rows are aligned.
        [[nodiscard]] std::string samples_sha256() const;

        /// The decoder's per-block quantizer map, when it exported one.
        /// Requires the frame to have come from a VideoReader configured to
        /// export them.
        [[nodiscard]] std::optional<QpMap> qp_map() const;

        /// The FFmpeg handle. Internal boundary; not part of the stable API.
        [[nodiscard]] AVFrame* raw() noexcept { return m_frame; }
        [[nodiscard]] const AVFrame* raw() const noexcept { return m_frame; }

        /// Writes this Frame's ColorSpec onto its AVFrame tags. Called before
        /// handing the frame to FFmpeg, so a filter or encoder sees the same
        /// color interpretation the library recorded.
        void sync_color_to_av_frame();

    private:
        explicit Frame(AVFrame* owned, const ColorSpec& color);

        /// Moves the ICC profile, display matrix and EXIF side data off the
        /// AVFrame into the Frame's own fields.
        void take_embedded_side_data();

        AVFrame* m_frame = nullptr;
        ColorSpec m_color;
        Rational m_time_base{0, 1};
        std::shared_ptr<const IccProfile> m_icc_profile;
        std::optional<int> m_orientation;
    };
}
