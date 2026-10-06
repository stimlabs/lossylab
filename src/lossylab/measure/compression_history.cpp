#include "lossylab/measure/compression_history.hpp"

#include "lossylab/convert/convert.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/core/json_io.hpp"
#include "lossylab/core/schema_version.hpp"
#include "lossylab/core/statistics.hpp"
#include "lossylab/detail/ff_error.hpp"
#include "lossylab/detail/ff_ptr.hpp"
#include "lossylab/env/capabilities.hpp"
#include "lossylab/io/jpeg_markers.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <optional>
#include <utility>

#if defined(__GNUC__) && defined(__x86_64__)
#include <immintrin.h>
#define LOSSYLAB_LATTICE_AVX2 1
#endif

namespace lossylab
{
    namespace
    {
        // JPEG quantization: the grid search fits all 63 AC coefficients and
        // scores an offset by its best five, since which coefficients show
        // the lattice depends on the quality: at high qualities the lowest
        // ones have a step of 1 or 2.
        constexpr std::size_t probe_count = 63;
        constexpr std::size_t scored_probes = 5;
        constexpr std::size_t max_screening_blocks = 256;
        constexpr std::size_t rescored_offsets = 3;
        constexpr double min_decisive_screening_score = 0.3;
        constexpr std::size_t max_grid_blocks = 2048;
        constexpr std::size_t max_table_blocks = 16384;
        constexpr int min_lattice_samples = 50;
        constexpr double min_unit_share = 0.1;
        constexpr double min_step_score = 0.55;
        constexpr double min_grid_score = 0.5;
        constexpr double min_grid_margin = 0.2;
        constexpr int min_determined_steps = 3;
        constexpr double min_tie_breaking_match = 0.9;
        constexpr double min_quality_match = 0.8;

        // Chroma subsampling.
        constexpr double max_upsampled_residual = 0.25;
        constexpr double max_phase_ratio = 0.85;

        // A pairing this much better than the other is upsampling, whatever
        // the rounding floor; blockiness alone gets no lower than about 0.5.
        constexpr double max_phase_ratio_alone = 0.4;

        // Upsampled chroma fits its own upsampling much better than the
        // other, by 0.2 to 0.5; chroma that is only blocky fits both about
        // as well, by 0.55 or more.
        constexpr double max_upsampling_ratio = 0.45;
        constexpr int max_pairing_lines = 512;
        constexpr int pairing_margin = 4;
        constexpr long min_pairing_samples = 1000;
        constexpr double achromatic_limit = 0.5;

        // Recompression: a coarse curve this confident, and at least as
        // confident as a trace needs, gets a fine sweep.
        constexpr double min_refine_confidence = 0.3;

        // Recompression: a detected JPEG with a grid score above this is
        // swept with MJPEG alone.
        constexpr double min_jpeg_only_grid_score = 0.8;

        // Below this many pixels (128 x 128), a notch is as likely to be
        // noise as a trace: the curves are kept, but no trace is listed.
        constexpr long min_recompression_trace_pixels = 128 * 128;

        template <typename Enum>
        json::Value enum_or_null(const std::optional<Enum>& value)
        {
            return value.has_value() ? json::Value(to_string(*value)) : json::Value();
        }

        /// One plane in 8-bit code values, with the samples that were
        /// clipped to 0 or 255 (in any RGB channel they came from) marked.
        struct Plane
        {
            int width = 0;
            int height = 0;
            std::vector<float> samples;
            std::vector<std::uint8_t> clipped;

            Plane() = default;
            Plane(const int plane_width, const int plane_height)
                : width(plane_width), height(plane_height),
                  samples(static_cast<std::size_t>(plane_width) * static_cast<std::size_t>(plane_height)),
                  clipped(samples.size())
            {
            }

            [[nodiscard]] std::size_t index(const int x, const int y) const
            {
                return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            }

            [[nodiscard]] bool empty() const { return samples.empty(); }
        };

        /// The frame as JPEG's YCbCr: BT.601, full range, 8-bit code values.
        struct WorkingPlanes
        {
            Plane luma;
            Plane cb;
            Plane cr;

            /// Gray when there is no chroma; Yuv444 when the chroma is at full
            /// resolution, whatever it was before; the frame's own layout
            /// when it is still in the JPEG's YUV.
            Subsampling chroma_layout = Subsampling::Gray;

            bool achromatic = false;
            std::string analyzed_as;
        };

        Plane plane_from_8bit(const ConstPlaneView& view)
        {
            Plane plane(view.width, view.height);
            for (int y = 0; y < view.height; ++y)
            {
                const std::uint8_t* row = view.row(y);
                for (int x = 0; x < view.width; ++x)
                {
                    plane.samples[plane.index(x, y)] = row[x];
                    plane.clipped[plane.index(x, y)] = row[x] == 0 || row[x] == 255;
                }
            }
            return plane;
        }

        bool has_jpeg_color(const ColorSpec& color)
        {
            return color.range == ColorRange::Full &&
                   (color.matrix == ColorMatrix::Bt470bg || color.matrix == ColorMatrix::Smpte170m);
        }

        bool is_jpeg_layout(const Subsampling subsampling)
        {
            return subsampling == Subsampling::Yuv444 || subsampling == Subsampling::Yuv440 ||
                   subsampling == Subsampling::Yuv422 || subsampling == Subsampling::Yuv420;
        }

        /// The frame in `target_format`, converted with its unspecified color
        /// fields filled in and both steps recorded.
        Frame converted_for_analysis(const Frame& frame, const PixelFormat& target_format, ConversionList& conversions)
        {
            ColorSpec source_color = frame.color();
            if (!source_color.is_fully_specified())
            {
                ColorSpec fallback = ColorSpec::srgb();
                if (!frame.pixel_format().is_rgb())
                {
                    fallback.matrix = ColorMatrix::Bt470bg;
                    fallback.range = ColorRange::Limited;
                    fallback.chroma_location = ChromaLocation::Center;
                }
                source_color = source_color.with_defaults_from(fallback);
                conversions.push_back(ConversionEvent{"color_tags", frame.color().describe(), source_color.describe(),
                                                      ConversionCause::Requested, "assumed_color"});
            }
            Frame source = frame;
            source.set_color(source_color);
            source.sync_color_to_av_frame();

            ColorSpec target_color = source_color;
            if (target_format.is_rgb())
            {
                target_color.matrix = ColorMatrix::Rgb;
                target_color.range = ColorRange::Full;
            }
            FrameResult converted = convert(source, target_format, target_color, Strict::AllowRecorded);
            for (ConversionEvent& event : converted.record.conversions)
            {
                event.cause = ConversionCause::CodecConstraint;
                conversions.push_back(std::move(event));
            }
            return std::move(converted.frame);
        }

        /// JPEG's own RGB-to-YCbCr conversion (JFIF), unrounded.
        void add_ycbcr_from_rgb(const Frame& rgb, WorkingPlanes& planes)
        {
            const ConstPlaneView view = rgb.plane(0);
            planes.luma = Plane(view.width, view.height);
            planes.cb = Plane(view.width, view.height);
            planes.cr = Plane(view.width, view.height);
            for (int y = 0; y < view.height; ++y)
            {
                const std::uint8_t* row = view.row(y);
                for (int x = 0; x < view.width; ++x)
                {
                    const double red = row[3 * x];
                    const double green = row[3 * x + 1];
                    const double blue = row[3 * x + 2];
                    const std::size_t at = planes.luma.index(x, y);
                    planes.luma.samples[at] = static_cast<float>(0.299 * red + 0.587 * green + 0.114 * blue);
                    const double cb = -0.168735892 * red - 0.331264108 * green + 0.5 * blue + 128.0;
                    const double cr = 0.5 * red - 0.418687589 * green - 0.081312411 * blue + 128.0;
                    planes.cb.samples[at] = static_cast<float>(cb);
                    planes.cr.samples[at] = static_cast<float>(cr);

                    const bool clipped = red == 0 || red == 255 || green == 0 || green == 255 || blue == 0 ||
                                         blue == 255;
                    planes.luma.clipped[at] = clipped;
                    planes.cb.clipped[at] = clipped;
                    planes.cr.clipped[at] = clipped;
                }
            }
            planes.chroma_layout = Subsampling::Yuv444;
        }

        /// Whether every chroma sample is within `achromatic_limit` of
        /// neutral, as for a gray image stored in color.
        bool is_achromatic(const Plane& cb, const Plane& cr)
        {
            const auto neutral = [](const float sample) { return std::abs(sample - 128.0f) < achromatic_limit; };
            return std::all_of(cb.samples.begin(), cb.samples.end(), neutral) &&
                   std::all_of(cr.samples.begin(), cr.samples.end(), neutral);
        }

        WorkingPlanes working_planes(const Frame& frame, ConversionList& conversions)
        {
            const PixelFormat format = frame.pixel_format();
            const Subsampling subsampling = format.subsampling();
            const bool plain_eight_bit = format.bit_depth() == 8 && format.is_planar() && !format.is_big_endian();

            WorkingPlanes planes;
            if (plain_eight_bit && subsampling == Subsampling::Gray && !format.has_alpha())
            {
                planes.luma = plane_from_8bit(frame.plane(0));
                planes.analyzed_as = format.name();
                return planes;
            }
            if (plain_eight_bit && is_jpeg_layout(subsampling) && !format.is_rgb() && format.plane_count() >= 3 &&
                has_jpeg_color(frame.color()))
            {
                planes.luma = plane_from_8bit(frame.plane(0));
                planes.cb = plane_from_8bit(frame.plane(1));
                planes.cr = plane_from_8bit(frame.plane(2));
                planes.chroma_layout = subsampling;
                planes.achromatic = is_achromatic(planes.cb, planes.cr);
                planes.analyzed_as = format.name();
                return planes;
            }
            if (subsampling == Subsampling::Gray)
            {
                planes.luma = plane_from_8bit(
                    converted_for_analysis(frame, PixelFormat::from_name("gray"), conversions).plane(0));
                planes.analyzed_as = "gray";
                return planes;
            }
            add_ycbcr_from_rgb(converted_for_analysis(frame, PixelFormat::from_name("rgb24"), conversions), planes);
            planes.achromatic = is_achromatic(planes.cb, planes.cr);
            planes.analyzed_as = "rgb24";
            return planes;
        }

        /// Counts of clipped samples over any rectangle in constant time.
        class ClippedCounts
        {
        public:
            explicit ClippedCounts(const Plane& plane)
                : m_width(plane.width + 1),
                  m_sums(static_cast<std::size_t>(plane.width + 1) * static_cast<std::size_t>(plane.height + 1))
            {
                for (int y = 0; y < plane.height; ++y)
                {
                    for (int x = 0; x < plane.width; ++x)
                    {
                        m_sums[at(x + 1, y + 1)] =
                            plane.clipped[plane.index(x, y)] + m_sums[at(x, y + 1)] + m_sums[at(x + 1, y)] -
                            m_sums[at(x, y)];
                    }
                }
            }

            [[nodiscard]] bool any_in(const int x, const int y, const int width, const int height) const
            {
                return m_sums[at(x + width, y + height)] - m_sums[at(x, y + height)] - m_sums[at(x + width, y)] +
                           m_sums[at(x, y)] >
                       0;
            }

        private:
            [[nodiscard]] std::size_t at(const int x, const int y) const
            {
                return static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x);
            }

            int m_width;
            std::vector<std::int64_t> m_sums;
        };

        /// The orthonormal 8-point DCT-II basis: basis[k][n], which is JPEG's
        /// forward DCT.
        const std::array<std::array<double, 8>, 8>& dct_basis()
        {
            static const std::array<std::array<double, 8>, 8> basis = []
            {
                std::array<std::array<double, 8>, 8> values{};
                for (std::size_t k = 0; k < 8; ++k)
                {
                    const double scale = k == 0 ? std::sqrt(1.0 / 8.0) : std::sqrt(2.0 / 8.0);
                    for (std::size_t n = 0; n < 8; ++n)
                    {
                        values[k][n] = scale * std::cos(std::numbers::pi * static_cast<double>((2 * n + 1) * k) / 16.0);
                    }
                }
                return values;
            }();
            return basis;
        }

        /// A block's samples, level shifted by 128 as JPEG does.
        std::array<double, 64> block_samples(const Plane& plane, const int x, const int y)
        {
            std::array<double, 64> samples{};
            for (int row = 0; row < 8; ++row)
            {
                const float* source = &plane.samples[plane.index(x, y + row)];
                for (int column = 0; column < 8; ++column)
                {
                    samples[static_cast<std::size_t>(row * 8 + column)] = source[column] - 128.0;
                }
            }
            return samples;
        }

        /// All 64 DCT coefficients of a block, in natural order.
        std::array<double, 64> block_dct(const std::array<double, 64>& samples)
        {
            const auto& basis = dct_basis();
            std::array<double, 64> vertical{};
            for (std::size_t u = 0; u < 8; ++u)
            {
                for (std::size_t column = 0; column < 8; ++column)
                {
                    double sum = 0.0;
                    for (std::size_t row = 0; row < 8; ++row)
                    {
                        sum += basis[u][row] * samples[row * 8 + column];
                    }
                    vertical[u * 8 + column] = sum;
                }
            }
            std::array<double, 64> coefficients{};
            for (std::size_t u = 0; u < 8; ++u)
            {
                for (std::size_t v = 0; v < 8; ++v)
                {
                    double sum = 0.0;
                    for (std::size_t column = 0; column < 8; ++column)
                    {
                        sum += vertical[u * 8 + column] * basis[v][column];
                    }
                    coefficients[u * 8 + v] = sum;
                }
            }
            return coefficients;
        }


        struct BlockOrigin
        {
            const Plane* plane = nullptr;
            int x = 0;
            int y = 0;
        };

        /// The 8x8 blocks of the grid starting at (offset_x, offset_y), across
        /// `planes`, clipped or not.
        std::size_t grid_block_count(const std::vector<const Plane*>& planes, const int offset_x, const int offset_y)
        {
            std::size_t total = 0;
            for (const Plane* plane : planes)
            {
                const int columns = std::max(0, (plane->width - offset_x) / 8);
                const int rows = std::max(0, (plane->height - offset_y) / 8);
                total += static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows);
            }
            return total;
        }

        /// The unclipped 8x8 blocks of the grid starting at (offset_x,
        /// offset_y), across `planes`, thinned evenly to at most `max_blocks`.
        std::vector<BlockOrigin> block_origins(const std::vector<const Plane*>& planes,
                                               const std::vector<ClippedCounts>& clipped_counts, const int offset_x,
                                               const int offset_y, const std::size_t max_blocks)
        {
            std::vector<BlockOrigin> unclipped;
            for (std::size_t plane_index = 0; plane_index < planes.size(); ++plane_index)
            {
                const Plane& plane = *planes[plane_index];
                for (int y = offset_y; y + 8 <= plane.height; y += 8)
                {
                    for (int x = offset_x; x + 8 <= plane.width; x += 8)
                    {
                        if (!clipped_counts[plane_index].any_in(x, y, 8, 8))
                        {
                            unclipped.push_back(BlockOrigin{&plane, x, y});
                        }
                    }
                }
            }
            const std::size_t stride = std::max<std::size_t>(1, (unclipped.size() + max_blocks - 1) / max_blocks);
            if (stride == 1)
            {
                return unclipped;
            }
            std::vector<BlockOrigin> origins;
            for (std::size_t index = 0; index < unclipped.size(); index += stride)
            {
                origins.push_back(unclipped[index]);
            }
            return origins;
        }

        struct LatticeFit
        {
            /// 0 when no step had enough samples away from zero.
            int step = 0;
            double score = 0.0;

            /// The samples the step was judged on.
            std::size_t samples = 0;

            /// The score less two standard deviations of its value for
            /// samples unrelated to the lattice (about 0.9 per sample), so
            /// that a fit on a few samples cannot outrank one on many.
            [[nodiscard]] double lower_bound() const
            {
                return samples == 0 ? 0.0 : score - 2.0 * 0.9 / std::sqrt(static_cast<double>(samples));
            }
        };

#ifdef LOSSYLAB_LATTICE_AVX2
        __attribute__((target("avx2"))) detail::LatticeSums lattice_sums_avx2_kernel(const double* magnitudes,
                                                                                   const std::size_t count,
                                                                                   const double inverse_step)
        {
            const __m256d inverse = _mm256_set1_pd(inverse_step);
            const __m256d half = _mm256_set1_pd(0.5);
            const __m128i one = _mm_set1_epi32(1);
            __m256d squared_distances = _mm256_setzero_pd();
            __m128i unit_counts = _mm_setzero_si128();
            std::size_t index = 0;
            for (; index + 4 <= count; index += 4)
            {
                const __m256d position = _mm256_mul_pd(_mm256_loadu_pd(magnitudes + index), inverse);
                const __m128i lattice_index = _mm256_cvttpd_epi32(_mm256_add_pd(position, half));
                const __m256d distance = _mm256_sub_pd(position, _mm256_cvtepi32_pd(lattice_index));
                squared_distances = _mm256_add_pd(squared_distances, _mm256_mul_pd(distance, distance));

                // A match compares as -1 in every bit.
                unit_counts = _mm_sub_epi32(unit_counts, _mm_cmpeq_epi32(lattice_index, one));
            }
            std::array<double, 4> distance_lanes{};
            std::array<std::int32_t, 4> unit_lanes{};
            _mm256_storeu_pd(distance_lanes.data(), squared_distances);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(unit_lanes.data()), unit_counts);

            // The fewer than four left over, then the lanes, as
            // detail::lattice_sums_scalar() adds them.
            detail::LatticeSums sums = detail::lattice_sums_scalar(magnitudes + index, count - index, inverse_step);
            sums.squared_distance += (distance_lanes[0] + distance_lanes[1]) + (distance_lanes[2] + distance_lanes[3]);
            for (const std::int32_t lane : unit_lanes)
            {
                sums.unit_count += static_cast<std::size_t>(lane);
            }
            return sums;
        }
#endif

        detail::LatticeSums lattice_sums(const double* magnitudes, const std::size_t count, const double inverse_step)
        {
            if (const std::optional<detail::LatticeSums> sums =
                    detail::lattice_sums_avx2(magnitudes, count, inverse_step))
            {
                return *sums;
            }
            return detail::lattice_sums_scalar(magnitudes, count, inverse_step);
        }

        /// The quantization step, from 2 to `max_step`, whose lattice the
        /// samples fit best. A step is judged on the samples it would not
        /// quantize to zero (at least half a step from it), by 1 - 12 times
        /// their mean squared distance from the lattice in steps: 1 on it,
        /// about 0 for samples unrelated to it, and negative at a multiple
        /// of the true step. A divisor of the true step fits too, but less
        /// well, since the rounding noise is larger relative to it.
        ///
        /// For an AC coefficient, whose quantized values are mostly -1, 0
        /// and 1, a step also needs `min_unit_share` of its samples at one
        /// step from zero. Otherwise a few samples far out, all of them at a
        /// large true step, make any of its divisors fit, while too few of
        /// them reach the true step for it to be judged.
        LatticeFit fit_lattice(const std::vector<double>& values, const int max_step, const bool ac_coefficient)
        {
            // The lattice is symmetric about zero, so only magnitudes count. A
            // magnitude is at least half a step when its double, rounded down,
            // is at least the step; grouped by that, capped at `max_step`, and
            // largest group first, every step's samples are a prefix.
            const auto group_of = [max_step](const double magnitude)
            {
                const double doubled = 2.0 * magnitude;
                return doubled >= max_step ? max_step : static_cast<int>(doubled);
            };
            std::vector<std::size_t> at_least(static_cast<std::size_t>(max_step) + 2, 0);
            for (const double value : values)
            {
                at_least[static_cast<std::size_t>(group_of(std::abs(value)))] += 1;
            }
            for (int group = max_step - 1; group >= 0; --group)
            {
                at_least[static_cast<std::size_t>(group)] += at_least[static_cast<std::size_t>(group) + 1];
            }
            std::vector<double> magnitudes(at_least[2]);
            std::vector<std::size_t> next(at_least.begin() + 1, at_least.end());
            for (const double value : values)
            {
                const double magnitude = std::abs(value);
                const int group = group_of(magnitude);
                if (group >= 2)
                {
                    magnitudes[next[static_cast<std::size_t>(group)]++] = magnitude;
                }
            }

            LatticeFit best;
            std::vector<double> scores(static_cast<std::size_t>(max_step) + 1, -1.0);
            std::vector<std::size_t> counts(scores.size(), 0);
            for (int step = 2; step <= max_step; ++step)
            {
                const std::size_t count = at_least[static_cast<std::size_t>(step)];
                if (count < static_cast<std::size_t>(min_lattice_samples))
                {
                    break;
                }

                const detail::LatticeSums sums = lattice_sums(magnitudes.data(), count, 1.0 / step);
                const double squared_distance = sums.squared_distance;
                const std::size_t unit_count = sums.unit_count;
                if (ac_coefficient &&
                    static_cast<double>(unit_count) < min_unit_share * static_cast<double>(count))
                {
                    continue;
                }
                const double score = 1.0 - 12.0 * squared_distance / static_cast<double>(count);
                scores[static_cast<std::size_t>(step)] = score;
                counts[static_cast<std::size_t>(step)] = count;
                if (score > best.score)
                {
                    best = LatticeFit{step, score, count};
                }
            }

            // A best fit at a divisor of the true step happens when few
            // samples reach the true step; the true step still fits.
            if (best.step != 0)
            {
                for (int multiple = 2 * best.step; multiple <= max_step; multiple += best.step)
                {
                    if (scores[static_cast<std::size_t>(multiple)] >= min_step_score)
                    {
                        best = LatticeFit{multiple, scores[static_cast<std::size_t>(multiple)],
                                          counts[static_cast<std::size_t>(multiple)]};
                    }
                }
            }
            return best;
        }

        struct IjgMatch
        {
            std::optional<int> quality;
            std::optional<int> lowest;
            std::optional<int> highest;
            double match = 0.0;
        };

        /// The libjpeg qualities whose tables agree with the most determined
        /// values of the first of `estimates`, each against its standard
        /// table; of several, those agreeing with the most of the next, and
        /// so on, then those off by least in total. A later table only breaks
        /// ties, so a poorly estimated chroma table cannot outvote the luma.
        /// Qualities still tied cannot be told apart from these values: the
        /// match spans them, and `quality` is the middle one.
        IjgMatch match_ijg_quality(
            const std::vector<std::pair<const QuantizationEstimate*, const std::array<int, 64>*>>& estimates)
        {
            int determined = 0;
            for (const auto& [estimate, standard_table] : estimates)
            {
                determined += estimate->determined;
            }
            IjgMatch best;
            if (determined == 0)
            {
                return best;
            }
            std::vector<int> best_matches;
            long best_error = std::numeric_limits<long>::max();
            for (int quality = 1; quality <= 100; ++quality)
            {
                std::vector<int> matches;
                long error = 0;
                for (const auto& [estimate, standard_table] : estimates)
                {
                    const std::array<int, 64> table = detail::ijg_scaled_table(*standard_table, quality);
                    int table_matches = 0;
                    for (std::size_t i = 0; i < table.size(); ++i)
                    {
                        if (estimate->values[i] != 0)
                        {
                            table_matches += estimate->values[i] == table[i];
                            error += std::abs(estimate->values[i] - table[i]);
                        }
                    }
                    matches.push_back(table_matches);
                }
                if (best_matches.empty() || matches > best_matches ||
                    (matches == best_matches && error < best_error))
                {
                    best_matches = std::move(matches);
                    best_error = error;
                    best.lowest = quality;
                    best.highest = quality;
                }
                else if (matches == best_matches && error == best_error)
                {
                    best.highest = quality;
                }
            }
            best.quality = (*best.lowest + *best.highest + 1) / 2;
            int total_matches = 0;
            for (const int table_matches : best_matches)
            {
                total_matches += table_matches;
            }
            best.match = static_cast<double>(total_matches) / determined;
            return best;
        }

        void match_ijg_quality(QuantizationEstimate& estimate, const std::array<int, 64>& standard_table)
        {
            const IjgMatch match = match_ijg_quality({{&estimate, &standard_table}});
            estimate.ijg_quality = match.quality;
            estimate.ijg_match = match.match;
        }

        struct GridEstimate
        {
            int grid_x = 0;
            int grid_y = 0;
            double grid_score = 0.0;
            double runner_up_grid_score = 0.0;

            /// Every offset's score, row by row from (0, 0).
            std::vector<double> grid_scores;

            int blocks = 0;
            QuantizationEstimate table;
        };

        /// How well the blocks of the grid starting at (offset_x, offset_y)
        /// fit a lattice: the mean of the `scored_probes` best AC fits, each
        /// at its lower bound, from at most `max_blocks` blocks.
        double offset_grid_score(const std::vector<const Plane*>& planes,
                                 const std::vector<ClippedCounts>& clipped_counts, const int offset_x,
                                 const int offset_y, const std::size_t max_blocks)
        {
            const std::vector<BlockOrigin> origins =
                block_origins(planes, clipped_counts, offset_x, offset_y, max_blocks);
            std::array<std::vector<double>, probe_count> probes;
            for (std::vector<double>& values : probes)
            {
                values.reserve(origins.size());
            }
            for (const BlockOrigin& origin : origins)
            {
                const std::array<double, 64> dct = block_dct(block_samples(*origin.plane, origin.x, origin.y));
                for (std::size_t probe = 0; probe < probe_count; ++probe)
                {
                    probes[probe].push_back(dct[probe + 1]);
                }
            }
            std::vector<double> probe_scores;
            for (const std::vector<double>& values : probes)
            {
                const LatticeFit fit = fit_lattice(values, 64, true);
                probe_scores.push_back(std::max(0.0, fit.lower_bound()));
            }
            std::partial_sort(probe_scores.begin(), probe_scores.begin() + scored_probes, probe_scores.end(),
                              std::greater<>());
            return statistics::mean(std::ranges::subrange(probe_scores.begin(), probe_scores.begin() + scored_probes));
        }

        /// The grid offset whose blocks fit a lattice best, and the table
        /// estimated at it, from blocks pooled across `planes` (Cb and Cr
        /// share a table in every libjpeg-written file).
        ///
        /// Every offset is screened on at most `max_screening_blocks` blocks.
        /// A decisive screening, whose best offset scores at least
        /// `min_decisive_screening_score` and at least `min_grid_margin` above
        /// every offset sharing neither its row nor its column, has its
        /// `rescored_offsets` best offsets, offset (0, 0), every offset
        /// sharing a row or column with its best one, and as many of the best
        /// that share neither a row nor a column with the one chosen, scored
        /// again on at most `max_grid_blocks`; any other has every offset
        /// scored again.
        /// The grid and its runner-up are chosen from those scores.
        GridEstimate estimate_quantization(const std::vector<const Plane*>& planes,
                                           const std::array<int, 64>& standard_table)
        {
            std::vector<ClippedCounts> clipped_counts;
            clipped_counts.reserve(planes.size());
            for (const Plane* plane : planes)
            {
                clipped_counts.emplace_back(*plane);
            }

            GridEstimate estimate;
            std::vector<double> screening_scores;
            for (int offset_y = 0; offset_y < 8; ++offset_y)
            {
                for (int offset_x = 0; offset_x < 8; ++offset_x)
                {
                    screening_scores.push_back(
                        offset_grid_score(planes, clipped_counts, offset_x, offset_y, max_screening_blocks));
                }
            }
            estimate.grid_scores = screening_scores;

            // Offset (0, 0) has the most blocks of any offset.
            const bool screened_every_block = grid_block_count(planes, 0, 0) <= max_screening_blocks;
            std::array<std::optional<double>, 64> full_scores;
            const auto full_score = [&](const int offset)
            {
                std::optional<double>& score = full_scores[static_cast<std::size_t>(offset)];
                if (!score.has_value())
                {
                    score = screened_every_block ? screening_scores[static_cast<std::size_t>(offset)]
                                                 : offset_grid_score(planes, clipped_counts, offset % 8, offset / 8,
                                                                     max_grid_blocks);
                }
                return *score;
            };

            std::vector<int> ranked(64);
            std::iota(ranked.begin(), ranked.end(), 0);
            std::stable_sort(ranked.begin(), ranked.end(), [&](const int left, const int right)
                             { return screening_scores[static_cast<std::size_t>(left)] >
                                      screening_scores[static_cast<std::size_t>(right)]; });

            // An offset sharing a row or column with the true grid keeps that
            // direction's block edges, and with them part of the lattice.
            const auto shares_row_or_column = [](const int offset, const int other)
            { return offset % 8 == other % 8 || offset / 8 == other / 8; };

            const double screened_best = screening_scores[static_cast<std::size_t>(ranked.front())];
            double screened_runner_up = 0.0;
            for (int offset = 0; offset < 64; ++offset)
            {
                if (!shares_row_or_column(offset, ranked.front()))
                {
                    screened_runner_up =
                        std::max(screened_runner_up, screening_scores[static_cast<std::size_t>(offset)]);
                }
            }
            const bool decisive = screened_best >= min_decisive_screening_score &&
                                  screened_best - screened_runner_up >= min_grid_margin;
            const std::size_t rescored = decisive ? rescored_offsets : ranked.size();

            int best = ranked.front();
            const auto consider = [&](const int offset)
            {
                if (full_score(offset) > full_score(best))
                {
                    best = offset;
                }
            };
            for (std::size_t rank = 1; rank < rescored; ++rank)
            {
                consider(ranked[rank]);
            }

            // Offset (0, 0), and every offset sharing a row or column with the
            // best screened one, are candidates too.
            consider(0);
            if (ranked.front() != 0)
            {
                for (int offset = 0; offset < 64; ++offset)
                {
                    if (shares_row_or_column(offset, ranked.front()))
                    {
                        consider(offset);
                    }
                }
            }
            estimate.grid_x = best % 8;
            estimate.grid_y = best / 8;
            estimate.grid_score = full_score(best);

            std::size_t runner_up_candidates = 0;
            for (const int offset : ranked)
            {
                if (shares_row_or_column(offset, best))
                {
                    continue;
                }
                estimate.runner_up_grid_score = std::max(estimate.runner_up_grid_score, full_score(offset));
                if (++runner_up_candidates == rescored)
                {
                    break;
                }
            }

            const std::vector<BlockOrigin> origins =
                block_origins(planes, clipped_counts, estimate.grid_x, estimate.grid_y, max_table_blocks);
            estimate.blocks = static_cast<int>(origins.size());
            std::array<std::vector<double>, 64> coefficients;
            for (const BlockOrigin& origin : origins)
            {
                const std::array<double, 64> dct = block_dct(block_samples(*origin.plane, origin.x, origin.y));
                for (std::size_t i = 0; i < dct.size(); ++i)
                {
                    coefficients[i].push_back(dct[i]);
                }
            }

            QuantizationEstimate& table = estimate.table;
            double total_score = 0.0;
            for (std::size_t i = 0; i < coefficients.size(); ++i)
            {
                const LatticeFit fit = fit_lattice(coefficients[i], 255, i != 0);
                if (fit.step != 0 && fit.score >= min_step_score)
                {
                    table.values[i] = fit.step;
                    table.determined += 1;
                    total_score += fit.score;
                }
            }
            table.lattice_score = table.determined > 0 ? total_score / table.determined : 0.0;
            match_ijg_quality(table, standard_table);
            return estimate;
        }

        // -------------------------------------------------------------------
        // Chroma subsampling
        // -------------------------------------------------------------------

        /// Undoes the pair averages of triangle-upsampled samples: solves
        /// (c[i-1] + 6 c[i] + c[i+1]) / 8 = average[i], with libjpeg's edge
        /// rows (7 c[0] + c[1]) / 8 and (c[n-2] + 7 c[n-1]) / 8, by the
        /// Thomas algorithm.
        std::vector<double> undo_triangle(const std::vector<double>& averages)
        {
            const std::size_t count = averages.size();
            std::vector<double> solution(count);
            if (count == 1)
            {
                solution[0] = averages[0];
                return solution;
            }
            constexpr double off_diagonal = 1.0 / 8.0;
            std::vector<double> upper(count);
            std::vector<double> right(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                const double diagonal = (i == 0 || i + 1 == count) ? 7.0 / 8.0 : 6.0 / 8.0;
                const double lower = i == 0 ? 0.0 : off_diagonal;
                const double denominator = diagonal - lower * (i == 0 ? 0.0 : upper[i - 1]);
                upper[i] = off_diagonal / denominator;
                right[i] = (averages[i] - lower * (i == 0 ? 0.0 : right[i - 1])) / denominator;
            }
            solution[count - 1] = right[count - 1];
            for (std::size_t i = count - 1; i-- > 0;)
            {
                solution[i] = right[i] - upper[i] * solution[i + 1];
            }
            return solution;
        }

        /// The subsampled samples a line of `count` pairs from `first` came
        /// from, under `upsampling`.
        std::vector<double> undo_upsampling(const std::vector<double>& line, const int first, const int count,
                                            const ChromaUpsampling upsampling)
        {
            std::vector<double> averages(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i)
            {
                const auto left = static_cast<std::size_t>(first + 2 * i);
                averages[static_cast<std::size_t>(i)] = (line[left] + line[left + 1]) / 2.0;
            }
            return upsampling == ChromaUpsampling::Replicate ? averages : undo_triangle(averages);
        }

        double upsampled_at(const std::vector<double>& subsampled, const int index, const int half,
                            const ChromaUpsampling upsampling)
        {
            const double center = subsampled[static_cast<std::size_t>(index)];
            if (upsampling == ChromaUpsampling::Replicate)
            {
                return center;
            }
            const int neighbor = half == 0 ? index - 1 : index + 1;
            return 0.75 * center + 0.25 * subsampled[static_cast<std::size_t>(neighbor)];
        }

        struct PairingResidual
        {
            double squared_sum = 0.0;
            long count = 0;
        };

        /// Adds the change that undoing and redoing `upsampling` at pairs
        /// starting from `phase` makes to the lines of `plane` in one
        /// direction, leaving out clipped samples and each line's ends.
        void add_pairing_residual(const Plane& plane, const bool horizontal, const ChromaUpsampling upsampling,
                                  const int phase, PairingResidual& residual)
        {
            const int line_count = horizontal ? plane.height : plane.width;
            const int length = horizontal ? plane.width : plane.height;
            const int pair_count = (length - phase) / 2;
            if (pair_count < 2 * pairing_margin + 4)
            {
                return;
            }
            const int stride = std::max(1, line_count / max_pairing_lines);
            std::vector<double> line(static_cast<std::size_t>(length));
            for (int line_index = 0; line_index < line_count; line_index += stride)
            {
                for (int position = 0; position < length; ++position)
                {
                    line[static_cast<std::size_t>(position)] =
                        horizontal ? plane.samples[plane.index(position, line_index)]
                                   : plane.samples[plane.index(line_index, position)];
                }
                const std::vector<double> subsampled = undo_upsampling(line, phase, pair_count, upsampling);
                for (int pair = pairing_margin; pair < pair_count - pairing_margin; ++pair)
                {
                    for (int half = 0; half < 2; ++half)
                    {
                        const int position = phase + 2 * pair + half;
                        const std::size_t at =
                            horizontal ? plane.index(position, line_index) : plane.index(line_index, position);
                        if (plane.clipped[at] != 0)
                        {
                            continue;
                        }
                        const double difference =
                            line[static_cast<std::size_t>(position)] - upsampled_at(subsampled, pair, half, upsampling);
                        residual.squared_sum += difference * difference;
                        residual.count += 1;
                    }
                }
            }
        }

        /// What pairing the chroma up in one direction shows.
        struct AxisPairing
        {
            ChromaUpsampling upsampling = ChromaUpsampling::Triangle;
            int phase = 0;
            double residual = 0.0;
            double phase_ratio = 1.0;

            /// The residual over the other upsampling's, each at its better
            /// pairing.
            double upsampling_ratio = 1.0;

            bool enough_samples = false;

            /// How far past each rule's threshold the pairing is, from 0 at
            /// the threshold to 1; the strongest of the three.
            [[nodiscard]] double strength() const
            {
                const auto past = [](const double value, const double threshold)
                { return std::clamp((threshold - value) / threshold, 0.0, 1.0); };
                if (phase_ratio > max_phase_ratio)
                {
                    return past(phase_ratio, max_phase_ratio_alone);
                }
                return std::max({past(residual, max_upsampled_residual), past(phase_ratio, max_phase_ratio_alone),
                                 past(upsampling_ratio, max_upsampling_ratio)});
            }

            [[nodiscard]] bool subsampled() const { return enough_samples && strength() > 0.0; }
        };

        AxisPairing pair_axis(const Plane& cb, const Plane& cr, const bool horizontal)
        {
            AxisPairing best;
            bool first = true;
            std::array<double, 2> method_residuals{};
            std::size_t method_index = 0;
            for (const ChromaUpsampling upsampling : {ChromaUpsampling::Triangle, ChromaUpsampling::Replicate})
            {
                std::array<double, 2> residuals{};
                long count = 0;
                for (int phase = 0; phase < 2; ++phase)
                {
                    PairingResidual residual;
                    add_pairing_residual(cb, horizontal, upsampling, phase, residual);
                    add_pairing_residual(cr, horizontal, upsampling, phase, residual);
                    residuals[static_cast<std::size_t>(phase)] =
                        residual.count > 0 ? std::sqrt(residual.squared_sum / static_cast<double>(residual.count))
                                           : 0.0;
                    count = phase == 0 ? residual.count : std::min(count, residual.count);
                }
                const int phase = residuals[1] < residuals[0] ? 1 : 0;
                const double lower = residuals[static_cast<std::size_t>(phase)];
                const double higher = residuals[static_cast<std::size_t>(1 - phase)];
                method_residuals[method_index++] = lower;
                if (first || lower < best.residual)
                {
                    best.upsampling = upsampling;
                    best.phase = phase;
                    best.residual = lower;
                    best.phase_ratio = higher > 0.0 ? lower / higher : 1.0;
                    best.enough_samples = count >= min_pairing_samples;
                    first = false;
                }
            }
            const double other = std::max(method_residuals[0], method_residuals[1]);
            best.upsampling_ratio = other > 0.0 ? best.residual / other : 1.0;
            return best;
        }

        struct ChromaPairing
        {
            AxisPairing horizontal;
            AxisPairing vertical;
        };

        ChromaSubsamplingEvidence subsampling_evidence(const ChromaPairing& pairing)
        {
            ChromaSubsamplingEvidence evidence;
            evidence.horizontal_residual = pairing.horizontal.residual;
            evidence.horizontal_phase_ratio = pairing.horizontal.phase_ratio;
            evidence.horizontal_upsampling_ratio = pairing.horizontal.upsampling_ratio;
            evidence.vertical_residual = pairing.vertical.residual;
            evidence.vertical_phase_ratio = pairing.vertical.phase_ratio;
            evidence.vertical_upsampling_ratio = pairing.vertical.upsampling_ratio;
            if (!pairing.horizontal.enough_samples || !pairing.vertical.enough_samples)
            {
                return evidence;
            }

            const bool horizontal = pairing.horizontal.subsampled();
            const bool vertical = pairing.vertical.subsampled();
            evidence.subsampling = horizontal && vertical ? Subsampling::Yuv420
                                   : horizontal           ? Subsampling::Yuv422
                                   : vertical             ? Subsampling::Yuv440
                                                          : Subsampling::Yuv444;
            if (horizontal || vertical)
            {
                const AxisPairing& weaker =
                    !vertical || (horizontal && pairing.horizontal.strength() <= pairing.vertical.strength())
                        ? pairing.horizontal
                        : pairing.vertical;
                evidence.upsampling = weaker.upsampling;
                evidence.confidence = 0.5 + 0.5 * weaker.strength();
            }
            else
            {
                // How far the closer direction is from the residual and
                // upsampling-ratio thresholds, both of which it misses.
                const auto missed_by = [](const AxisPairing& axis)
                {
                    return std::min(std::clamp((axis.residual - max_upsampled_residual) / max_upsampled_residual,
                                               0.0, 1.0),
                                    std::clamp((axis.upsampling_ratio - max_upsampling_ratio) /
                                                   (1.0 - max_upsampling_ratio),
                                               0.0, 1.0));
                };
                evidence.confidence =
                    0.5 + 0.5 * std::min(missed_by(pairing.horizontal), missed_by(pairing.vertical));
            }
            return evidence;
        }

        /// The subsampled plane `plane` was upsampled from, when it was, in
        /// the given directions, with a sample clipped when any sample it
        /// was recovered from was.
        Plane undo_plane_upsampling(const Plane& plane, const AxisPairing* horizontal, const AxisPairing* vertical)
        {
            Plane result = plane;
            for (const bool along_rows : {true, false})
            {
                const AxisPairing* pairing = along_rows ? horizontal : vertical;
                if (pairing == nullptr)
                {
                    continue;
                }
                const Plane source = result;
                const int length = along_rows ? source.width : source.height;
                const int line_count = along_rows ? source.height : source.width;
                const int pair_count = (length - pairing->phase) / 2;
                result = along_rows ? Plane(pair_count, source.height) : Plane(source.width, pair_count);
                std::vector<double> line(static_cast<std::size_t>(length));
                for (int line_index = 0; line_index < line_count; ++line_index)
                {
                    for (int position = 0; position < length; ++position)
                    {
                        line[static_cast<std::size_t>(position)] =
                            along_rows ? source.samples[source.index(position, line_index)]
                                       : source.samples[source.index(line_index, position)];
                    }
                    const std::vector<double> subsampled =
                        undo_upsampling(line, pairing->phase, pair_count, pairing->upsampling);
                    for (int pair = 0; pair < pair_count; ++pair)
                    {
                        const int first = pairing->phase + 2 * pair;
                        const bool clipped =
                            along_rows ? source.clipped[source.index(first, line_index)] != 0 ||
                                             source.clipped[source.index(first + 1, line_index)] != 0
                                       : source.clipped[source.index(line_index, first)] != 0 ||
                                             source.clipped[source.index(line_index, first + 1)] != 0;
                        const std::size_t at =
                            along_rows ? result.index(pair, line_index) : result.index(line_index, pair);
                        result.samples[at] = static_cast<float>(subsampled[static_cast<std::size_t>(pair)]);
                        result.clipped[at] = clipped;
                    }
                }
            }
            return result;
        }

        /// The JPEG chroma table and layout. Chroma still in the JPEG's YUV
        /// has its layout; chroma at full resolution has the layout its
        /// pairing showed, and is reduced to it first, undoing the
        /// upsampling found. The pairing decides the layout, not the chroma
        /// lattice, which is weak wherever the chroma is: a poor table under
        /// a wrong layout can fit about as well as one under the right one.
        /// Without a pairing to go on, only a 4:4:4 lattice can be read.
        void add_chroma_quantization(const WorkingPlanes& planes, const std::optional<ChromaPairing>& pairing,
                                     const std::optional<ChromaSubsamplingEvidence>& subsampling,
                                     JpegQuantizationEvidence& evidence)
        {
            if (planes.chroma_layout != Subsampling::Yuv444)
            {
                const GridEstimate estimate =
                    estimate_quantization({&planes.cb, &planes.cr}, detail::standard_chrominance_table);
                if (estimate.grid_score >= min_grid_score)
                {
                    evidence.chroma = estimate.table;
                }
                evidence.chroma_subsampling = planes.chroma_layout;
                return;
            }

            const bool horizontal = pairing.has_value() && pairing->horizontal.subsampled();
            const bool vertical = pairing.has_value() && pairing->vertical.subsampled();
            const Plane cb =
                undo_plane_upsampling(planes.cb, horizontal ? &pairing->horizontal : nullptr,
                                      vertical ? &pairing->vertical : nullptr);
            const Plane cr =
                undo_plane_upsampling(planes.cr, horizontal ? &pairing->horizontal : nullptr,
                                      vertical ? &pairing->vertical : nullptr);
            const GridEstimate estimate = estimate_quantization({&cb, &cr}, detail::standard_chrominance_table);
            if (estimate.grid_score >= min_grid_score)
            {
                evidence.chroma = estimate.table;
            }
            if (pairing.has_value() && subsampling.has_value())
            {
                evidence.chroma_subsampling = subsampling->subsampling;
            }
            else if (evidence.chroma.has_value())
            {
                evidence.chroma_subsampling = Subsampling::Yuv444;
            }
        }

        // -------------------------------------------------------------------
        // Recompression
        // -------------------------------------------------------------------

        /// A codec's coarse sweep over its whole quality scale, and the step
        /// of the fine sweep between a notch's two coarse neighbors.
        struct SweepPlan
        {
            std::vector<double> coarse;
            double fine_step = 0.0;
            std::map<std::string, std::string> encoder_options;
        };

        std::vector<double> evenly_spaced(const double first, const double last, const double step)
        {
            std::vector<double> values;
            const auto count = static_cast<int>(std::floor((last - first) / step + 1e-9));
            for (int i = 0; i <= count; ++i)
            {
                values.push_back(first + i * step);
            }
            return values;
        }

        SweepPlan sweep_plan(const ImageCodec codec)
        {
            SweepPlan plan;
            switch (codec)
            {
            case ImageCodec::Mjpeg:
                plan.coarse = evenly_spaced(1, 31, 1);
                break;
            case ImageCodec::WebP:
                plan.coarse = evenly_spaced(5, 100, 5);
                plan.fine_step = 1;
                break;
            case ImageCodec::Avif:
            {
                const std::string encoder_name = capabilities().require_encoder(codec).name;
                if (encoder_name == "librav1e")
                {
                    plan.coarse = evenly_spaced(15, 255, 16);
                    plan.fine_step = 4;
                }
                else
                {
                    plan.coarse = evenly_spaced(3, 63, 4);
                    plan.fine_step = 1;
                }
                if (encoder_name == "libaom-av1")
                {
                    plan.encoder_options["cpu-used"] = "6";
                }
                break;
            }
            case ImageCodec::Jxl:
                plan.coarse = evenly_spaced(0.5, 6, 0.5);
                plan.fine_step = 0.1;
                break;
            case ImageCodec::Jpeg2000:
                // Compression ratios, which the encoder takes as integers,
                // spaced roughly evenly in their logarithm. FFmpeg's nominal
                // RGB ratio is about three times the true one, so the sweep
                // reaches well past the ratios files are written at, and
                // starts at 4: at 2 and 3 an RGB re-encode is all but
                // lossless, which leaves the same flat start as a prior does.
                plan.coarse = {4,  5,  6,  7,  8,  10, 12,  14,  16,  20,  24,  28,
                               32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 200};
                plan.fine_step = 1;
                break;
            case ImageCodec::Png:
            case ImageCodec::Heif:
                throw ConfigError("compression_history() cannot recompress with " + to_string(codec) +
                                  "; it has no lossy quality scale to sweep");
            }
            return plan;
        }

        /// Whether recompression_curve() re-encodes a frame of this layout
        /// with this codec in RGB by default, so that the error has to cover
        /// every plane; it covers luma alone otherwise.
        bool reencodes_in_rgb(const ImageCodec codec, const Subsampling subsampling)
        {
            switch (codec)
            {
            case ImageCodec::Jxl:
            case ImageCodec::Jpeg2000: return subsampling != Subsampling::Gray;
            case ImageCodec::Mjpeg:
            case ImageCodec::WebP:
            case ImageCodec::Avif:
            case ImageCodec::Png:
            case ImageCodec::Heif: return false;
            }
            return false;
        }

        /// The point of a curve at `parameter`, which must be one of its
        /// parameters.
        const RecompressionPoint& point_at(const RecompressionCurveEvidence& curve, const double parameter)
        {
            return *std::find_if(curve.points.begin(), curve.points.end(),
                                 [&](const RecompressionPoint& point) { return point.quality_parameter == parameter; });
        }

        /// The parameter at a curve's deepest interior notch, when its log
        /// error dips below its neighbors' anywhere.
        std::optional<double> deepest_notch(const RecompressionCurveEvidence& curve)
        {
            std::optional<double> parameter;
            double deepest = 0.0;
            for (std::size_t i = 1; i + 1 < curve.points.size(); ++i)
            {
                const double depth = curve.notch_depths.at(i);
                if (depth > deepest)
                {
                    deepest = depth;
                    parameter = curve.points[i].quality_parameter;
                }
            }
            return parameter;
        }

        /// A copy of the rectangle at (x, y) in the frame's own pixel format,
        /// which must lie on the 16-pixel grid so that every plane's corner
        /// is a whole sample and a whole byte, also for formats that pack
        /// several pixels into a byte.
        Frame crop_frame(const Frame& frame, const int x, const int y, const int width, const int height)
        {
            Frame cropped = Frame::allocate(width, height, frame.pixel_format(), frame.color());
            detail::copy_rectangle(*frame.raw(), x, y, *cropped.raw(), 0, 0, width, height);
            return cropped;
        }

        std::optional<Subsampling> known_subsampling(const CompressionHistoryEvidence& history)
        {
            if (history.jpeg.has_value() && history.jpeg->detected && history.jpeg->chroma_subsampling.has_value())
            {
                return history.jpeg->chroma_subsampling;
            }
            if (history.chroma.has_value())
            {
                return history.chroma->subsampling;
            }
            return std::nullopt;
        }

        /// The format MJPEG re-encodes in: the chroma layout found, which
        /// the default for an RGB frame would only guess at.
        std::optional<PixelFormat> mjpeg_format_for(const std::optional<Subsampling> subsampling)
        {
            if (!subsampling.has_value())
            {
                return std::nullopt;
            }
            switch (*subsampling)
            {
            case Subsampling::Yuv444: return PixelFormat::from_name("yuvj444p");
            case Subsampling::Yuv422: return PixelFormat::from_name("yuvj422p");
            case Subsampling::Yuv420: return PixelFormat::from_name("yuvj420p");
            default: return std::nullopt;
            }
        }

        void add_recompression(const Frame& frame, const CompressionHistoryOptions& options,
                               CompressionHistoryEvidence& history)
        {
            RecompressionOutcome& outcome = history.recompression;

            // Only a sweep whose confidence can list a trace is refined.
            const double refine_confidence = std::max(min_refine_confidence, options.min_confidence);

            // A header declares a JPEG outright and leaves the grid score absent.
            const bool jpeg_only =
                history.jpeg.has_value() && history.jpeg->detected &&
                (!history.jpeg->grid_score.has_value() || *history.jpeg->grid_score > min_jpeg_only_grid_score);

            // Lossy WebP codes 4:2:0 alone, so 4:4:4 chroma is not swept with it.
            const bool full_resolution_chroma =
                history.chroma.has_value() && history.chroma->subsampling == Subsampling::Yuv444;

            Frame analyzed = frame;
            if (options.recompression_crop > 0 &&
                (frame.width() > options.recompression_crop || frame.height() > options.recompression_crop))
            {
                const int side = std::max(16, options.recompression_crop / 16 * 16);
                const int width = std::min(frame.width(), side);
                const int height = std::min(frame.height(), side);
                const int x = (frame.width() - width) / 2 / 16 * 16;
                const int y = (frame.height() - height) / 2 / 16 * 16;
                analyzed = crop_frame(frame, x, y, width, height);
                outcome.crop = Rect{static_cast<double>(x), static_cast<double>(y), static_cast<double>(width),
                                    static_cast<double>(height)};
            }

            for (const ImageCodec codec : options.recompression_codecs)
            {
                if (!capabilities().supports(codec))
                {
                    outcome.skipped.push_back(codec);
                    continue;
                }
                if (jpeg_only && codec != ImageCodec::Mjpeg)
                {
                    outcome.skipped_after_jpeg.push_back(codec);
                    continue;
                }
                if (full_resolution_chroma && codec == ImageCodec::WebP)
                {
                    outcome.skipped_for_chroma.push_back(codec);
                    continue;
                }
                try
                {
                    const SweepPlan plan = sweep_plan(codec);
                    RecompressionOptions sweep;
                    sweep.codec = codec;
                    sweep.parameter_range = plan.coarse;
                    sweep.encoder_options = plan.encoder_options;
                    if (codec == ImageCodec::Mjpeg)
                    {
                        sweep.pixel_format = mjpeg_format_for(known_subsampling(history));
                    }
                    sweep.planes = reencodes_in_rgb(codec, analyzed.pixel_format().subsampling())
                                       ? RecompressionPlanes::All
                                       : RecompressionPlanes::Luma;

                    const RecompressionCurve coarse = recompression_curve(analyzed, sweep);
                    std::optional<double> quality = deepest_notch(coarse.evidence());
                    const double confidence = coarse.evidence().confidence;
                    const PixelFormat encoded_as = coarse.record.output.pixel_format;

                    // A notch is never at either end, so it has two coarse
                    // neighbors; the fine sweep runs between them.
                    std::vector<double> fine_parameters;
                    if (quality.has_value() && plan.fine_step > 0.0 && confidence >= refine_confidence)
                    {
                        const RecompressionPoint* notch = &point_at(coarse.evidence(), *quality);
                        const double previous = (notch - 1)->quality_parameter;
                        const double next = (notch + 1)->quality_parameter;
                        const auto steps = static_cast<int>(std::lround((next - previous) / plan.fine_step));
                        for (int i = 1; i < steps; ++i)
                        {
                            fine_parameters.push_back(previous + i * plan.fine_step);
                        }
                    }
                    history.recompression_curves.push_back(RecompressionSweep{coarse.configuration, coarse.evidence()});

                    if (fine_parameters.size() >= 3)
                    {
                        sweep.parameter_range = std::move(fine_parameters);
                        const RecompressionCurve fine = recompression_curve(analyzed, sweep);
                        if (const std::optional<double> refined = deepest_notch(fine.evidence()))
                        {
                            quality = refined;
                        }
                        history.recompression_curves.push_back(RecompressionSweep{fine.configuration, fine.evidence()});
                    }

                    const long analyzed_pixels = static_cast<long>(analyzed.width()) * analyzed.height();
                    if (quality.has_value() && confidence >= options.min_confidence &&
                        analyzed_pixels >= min_recompression_trace_pixels)
                    {
                        CompressionTrace trace;
                        trace.evidence = TraceEvidence::Recompression;
                        trace.codec = codec;
                        trace.quality = quality;
                        trace.subsampling = encoded_as.subsampling();
                        trace.confidence = confidence;
                        history.traces.push_back(std::move(trace));
                    }
                }
                catch (const Error& error)
                {
                    outcome.errors[to_string(codec)] = error.what();
                }
            }
        }

        /// The libjpeg quality `evidence`'s luma table agrees with best, the
        /// chroma table breaking ties when it matches a libjpeg table itself.
        void add_joint_ijg_quality(JpegQuantizationEvidence& evidence)
        {
            std::vector<std::pair<const QuantizationEstimate*, const std::array<int, 64>*>> tables = {
                {&evidence.luma, &detail::standard_luminance_table}};
            if (evidence.chroma.has_value() && evidence.chroma->ijg_match >= min_tie_breaking_match)
            {
                tables.emplace_back(&*evidence.chroma, &detail::standard_chrominance_table);
            }
            const IjgMatch joint = match_ijg_quality(tables);
            evidence.ijg_quality = joint.quality;
            evidence.ijg_quality_lowest = joint.lowest;
            evidence.ijg_quality_highest = joint.highest;
            evidence.ijg_match = joint.match;
        }

        CompressionTrace jpeg_trace(const JpegQuantizationEvidence& evidence, const TraceEvidence kind,
                                    const double confidence)
        {
            CompressionTrace trace;
            trace.evidence = kind;
            trace.codec = ImageCodec::Mjpeg;
            if (evidence.ijg_quality.has_value() && evidence.ijg_match >= min_quality_match)
            {
                trace.quality = *evidence.ijg_quality;
            }
            trace.subsampling = evidence.chroma_subsampling;
            trace.confidence = confidence;
            return trace;
        }

        /// The JPEG quantization the pixels show, with every luma grid
        /// offset's screening score put in `luma_grid_scores`; absent for
        /// planes too small for an 8x8 grid.
        std::optional<JpegQuantizationEvidence> pixel_jpeg_evidence(
            const WorkingPlanes& planes, const std::optional<ChromaPairing>& pairing,
            const std::optional<ChromaSubsamplingEvidence>& chroma, std::vector<double>& luma_grid_scores)
        {
            if (planes.luma.width < 16 || planes.luma.height < 16)
            {
                return std::nullopt;
            }
            const GridEstimate luma = estimate_quantization({&planes.luma}, detail::standard_luminance_table);
            luma_grid_scores = luma.grid_scores;
            JpegQuantizationEvidence evidence;
            evidence.grid_x = luma.grid_x;
            evidence.grid_y = luma.grid_y;
            evidence.grid_score = luma.grid_score;
            evidence.runner_up_grid_score = luma.runner_up_grid_score;
            evidence.blocks = luma.blocks;
            evidence.luma = luma.table;
            evidence.detected = luma.grid_score >= min_grid_score &&
                                luma.grid_score - luma.runner_up_grid_score >= min_grid_margin &&
                                luma.table.determined >= min_determined_steps;
            if (!evidence.detected)
            {
                return evidence;
            }
            if (planes.cb.empty())
            {
                evidence.chroma_subsampling = Subsampling::Gray;
            }
            else if (!planes.achromatic)
            {
                add_chroma_quantization(planes, pairing, chroma, evidence);
                if (!evidence.chroma_subsampling.has_value() && chroma.has_value())
                {
                    evidence.chroma_subsampling = chroma->subsampling;
                }
            }
            add_joint_ijg_quality(evidence);
            return evidence;
        }

        /// What a JPEG file's header declares about a decoded image, or why
        /// it does not describe the image's frame.
        struct HeaderReading
        {
            std::optional<JpegQuantizationEvidence> evidence;
            std::string unused_because;
        };

        /// The chroma layout of a gray or YCbCr JPEG's sampling factors;
        /// absent when a chroma component is itself subsampled or the factors
        /// match no layout.
        std::optional<Subsampling> jpeg_layout(const std::vector<JpegInfo::Component>& components)
        {
            if (components.size() == 1)
            {
                return Subsampling::Gray;
            }
            for (std::size_t index = 1; index < components.size(); ++index)
            {
                if (components[index].horizontal_sampling != 1 || components[index].vertical_sampling != 1)
                {
                    return std::nullopt;
                }
            }
            const std::pair<int, int> factors{components[0].horizontal_sampling, components[0].vertical_sampling};
            if (factors == std::pair{1, 1}) { return Subsampling::Yuv444; }
            if (factors == std::pair{2, 1}) { return Subsampling::Yuv422; }
            if (factors == std::pair{1, 2}) { return Subsampling::Yuv440; }
            if (factors == std::pair{2, 2}) { return Subsampling::Yuv420; }
            if (factors == std::pair{4, 1}) { return Subsampling::Yuv411; }
            return std::nullopt;
        }

        /// The table a JPEG defines under `id`; absent when it defines none,
        /// or different ones one after another.
        std::optional<std::array<int, 64>> jpeg_table(const JpegInfo& jpeg, const int id)
        {
            std::optional<std::array<int, 64>> found;
            for (const JpegInfo::QuantizationTable& table : jpeg.quantization_tables)
            {
                if (table.id != id)
                {
                    continue;
                }
                if (found.has_value() && *found != table.values)
                {
                    return std::nullopt;
                }
                found = table.values;
            }
            return found;
        }

        QuantizationEstimate declared_table(const std::array<int, 64>& values,
                                            const std::array<int, 64>& standard_table)
        {
            QuantizationEstimate table;
            table.values = values;
            table.determined = static_cast<int>(values.size());
            match_ijg_quality(table, standard_table);
            return table;
        }

        HeaderReading read_jpeg_header(const DecodedImage& image)
        {
            const auto unused = [](std::string reason) { return HeaderReading{std::nullopt, std::move(reason)}; };
            if (image.tile_grid() != nullptr)
            {
                return unused("a tile grid");
            }
            const StreamInfo& stream = image.stream();
            if (!stream.jpeg.has_value())
            {
                return unused("not a JPEG file");
            }
            if (image.evidence().orientation_handling != "reported")
            {
                return unused("its orientation was applied");
            }
            if (image.configuration.conversion.has_value())
            {
                return unused("it was converted on decode");
            }
            if (image.frame.describe() != image.record.output || image.frame.pixel_format() != stream.pixel_format)
            {
                return unused("the frame is not the file's decode as stored");
            }

            const JpegInfo& jpeg = *stream.jpeg;
            if (jpeg.precision != 8)
            {
                return unused("a " + std::to_string(jpeg.precision) + "-bit JPEG");
            }
            if (jpeg.process == "lossless" || jpeg.process == "hierarchical")
            {
                return unused("a " + jpeg.process + " JPEG");
            }
            if (jpeg.components.size() != 1 && jpeg.components.size() != 3)
            {
                return unused("a JPEG of " + std::to_string(jpeg.components.size()) + " components");
            }
            if (jpeg.adobe_transform == 0)
            {
                return unused("a JPEG coded in RGB");
            }
            const std::optional<Subsampling> layout = jpeg_layout(jpeg.components);
            if (!layout.has_value())
            {
                return unused("sampling factors of no known chroma layout");
            }
            const std::optional<std::array<int, 64>> luma = jpeg_table(jpeg, jpeg.components[0].quantization_table);
            if (!luma.has_value())
            {
                return unused("no single luma table");
            }

            JpegQuantizationEvidence evidence;
            evidence.detected = true;
            evidence.luma = declared_table(*luma, detail::standard_luminance_table);
            evidence.chroma_subsampling = layout;
            if (jpeg.components.size() == 3)
            {
                if (jpeg.components[1].quantization_table != jpeg.components[2].quantization_table)
                {
                    return unused("different Cb and Cr tables");
                }
                const std::optional<std::array<int, 64>> chroma =
                    jpeg_table(jpeg, jpeg.components[1].quantization_table);
                if (!chroma.has_value())
                {
                    return unused("no single chroma table");
                }
                evidence.chroma = declared_table(*chroma, detail::standard_chrominance_table);
            }
            add_joint_ijg_quality(evidence);
            return HeaderReading{std::move(evidence), ""};
        }

        /// How many of the steps the pixels determine equal the header's.
        JpegTableAgreement table_agreement(const QuantizationEstimate& pixels, const QuantizationEstimate& header)
        {
            int matching = 0;
            for (std::size_t index = 0; index < pixels.values.size(); ++index)
            {
                matching += pixels.values[index] != 0 && pixels.values[index] == header.values[index];
            }
            return JpegTableAgreement{pixels.determined, matching};
        }

        CompressionHistory analyze(const Frame& frame, const CompressionHistoryOptions& options,
                                   const HeaderReading& header)
        {
            const detail::StageClock clock;
            if (frame.empty())
            {
                throw ConfigError("compression_history() received an empty frame");
            }
            if (options.recompression_crop < 0)
            {
                throw ConfigError("compression_history() recompression_crop must not be negative");
            }

            CompressionHistory result;
            CompressionHistoryEvidence history;
            StageRecord& record = result.record;
            record.implementation = "lossylab";
            record.input = frame.describe();
            record.output = record.input;
            record.transform = CoordinateTransform::identity();

            const WorkingPlanes planes = working_planes(frame, record.conversions);
            const bool has_chroma = !planes.cb.empty() && !planes.achromatic;

            // A frame still in subsampled YUV says its layout itself; converting
            // it to RGB for analysis upsamples its chroma, which is not a trace.
            const Subsampling frame_layout = frame.pixel_format().subsampling();
            const bool full_resolution_chroma =
                frame_layout == Subsampling::Rgb || frame_layout == Subsampling::Yuv444;
            std::optional<ChromaPairing> pairing;
            if (has_chroma && planes.chroma_layout == Subsampling::Yuv444 && full_resolution_chroma)
            {
                pairing =
                    ChromaPairing{pair_axis(planes.cb, planes.cr, true), pair_axis(planes.cb, planes.cr, false)};
                history.chroma = subsampling_evidence(*pairing);
                if (!history.chroma->subsampling.has_value())
                {
                    pairing.reset();
                }
            }

            if (header.evidence.has_value())
            {
                history.jpeg = header.evidence;
                CompressionTrace trace = jpeg_trace(*header.evidence, TraceEvidence::JpegHeader, 1.0);
                if (trace.confidence >= options.min_confidence)
                {
                    history.traces.push_back(std::move(trace));
                }
                if (options.jpeg_pixel_check)
                {
                    history.jpeg_pixel_check =
                        pixel_jpeg_evidence(planes, pairing, history.chroma, history.luma_grid_scores);
                    if (history.jpeg_pixel_check.has_value())
                    {
                        const JpegQuantizationEvidence& pixels = *history.jpeg_pixel_check;
                        JpegPixelAgreement agreement;
                        agreement.detected = pixels.detected;
                        agreement.ijg_quality_equal = pixels.ijg_quality == header.evidence->ijg_quality;
                        agreement.luma = table_agreement(pixels.luma, header.evidence->luma);
                        if (pixels.chroma.has_value() && header.evidence->chroma.has_value())
                        {
                            agreement.chroma = table_agreement(*pixels.chroma, *header.evidence->chroma);
                        }
                        history.jpeg_pixel_agreement = agreement;
                    }
                }
            }
            else
            {
                history.jpeg = pixel_jpeg_evidence(planes, pairing, history.chroma, history.luma_grid_scores);
                if (history.jpeg.has_value() && history.jpeg->detected)
                {
                    CompressionTrace trace = jpeg_trace(*history.jpeg, TraceEvidence::JpegQuantization,
                                                        std::min(1.0, *history.jpeg->grid_score));
                    if (trace.confidence >= options.min_confidence)
                    {
                        history.traces.push_back(std::move(trace));
                    }
                }
            }

            if (history.chroma.has_value() && history.chroma->subsampling.has_value() &&
                *history.chroma->subsampling != Subsampling::Yuv444 &&
                history.chroma->confidence >= options.min_confidence)
            {
                CompressionTrace trace;
                trace.evidence = TraceEvidence::ChromaSubsampling;
                trace.subsampling = history.chroma->subsampling;
                trace.confidence = history.chroma->confidence;
                history.traces.push_back(std::move(trace));
            }

            history.analyzed_as = planes.analyzed_as;
            history.chroma_layout = planes.cb.empty()   ? "none"
                                    : planes.achromatic ? "achromatic"
                                                        : to_string(planes.chroma_layout);
            history.jpeg_tables = header.evidence.has_value() ? "header" : "pixels";
            if (!header.evidence.has_value())
            {
                history.jpeg_header_unused = header.unused_because;
            }
            add_recompression(frame, options, history);
            record.modifies_state = false;

            std::stable_sort(history.traces.begin(), history.traces.end(),
                             [](const CompressionTrace& left, const CompressionTrace& right)
                             { return left.confidence > right.confidence; });
            record.evidence = std::move(history);
            result.configuration = options;
            record.duration_ms = clock.duration_ms();
            record.ffmpeg_duration_ms = clock.ffmpeg_duration_ms();
            return result;
        }
    }

    std::string to_string(const ChromaUpsampling upsampling)
    {
        return upsampling == ChromaUpsampling::Replicate ? "replicate" : "triangle";
    }

    ChromaUpsampling chroma_upsampling_from_string(const std::string_view name)
    {
        if (name == "replicate") { return ChromaUpsampling::Replicate; }
        if (name == "triangle") { return ChromaUpsampling::Triangle; }
        throw ConfigError("unknown chroma upsampling '" + std::string(name) + "'");
    }

    std::string to_string(const TraceEvidence evidence)
    {
        switch (evidence)
        {
        case TraceEvidence::JpegHeader: return "jpeg_header";
        case TraceEvidence::JpegQuantization: return "jpeg_quantization";
        case TraceEvidence::ChromaSubsampling: return "chroma_subsampling";
        case TraceEvidence::Recompression: return "recompression";
        }
        return "unknown";
    }

    TraceEvidence trace_evidence_from_string(const std::string_view name)
    {
        if (name == "jpeg_header") { return TraceEvidence::JpegHeader; }
        if (name == "jpeg_quantization") { return TraceEvidence::JpegQuantization; }
        if (name == "chroma_subsampling") { return TraceEvidence::ChromaSubsampling; }
        if (name == "recompression") { return TraceEvidence::Recompression; }
        throw ConfigError("unknown trace evidence '" + std::string(name) + "'");
    }

    json::Value QuantizationEstimate::to_json() const
    {
        return json::object({
            {"values", json::Value(values)},
            {"determined", determined},
            {"ijg_quality", json::optional_or_null(ijg_quality)},
            {"ijg_match", ijg_match},
            {"lattice_score", json::optional_or_null(lattice_score)},
        });
    }

    json::Value JpegQuantizationEvidence::to_json() const
    {
        return json::object({
            {"detected", detected},
            {"grid_x", grid_x},
            {"grid_y", grid_y},
            {"grid_score", json::optional_or_null(grid_score)},
            {"runner_up_grid_score", json::optional_or_null(runner_up_grid_score)},
            {"blocks", json::optional_or_null(blocks)},
            {"luma", luma.to_json()},
            {"chroma", json::optional_or_null(chroma)},
            {"chroma_subsampling", enum_or_null(chroma_subsampling)},
            {"ijg_quality", json::optional_or_null(ijg_quality)},
            {"ijg_quality_lowest", json::optional_or_null(ijg_quality_lowest)},
            {"ijg_quality_highest", json::optional_or_null(ijg_quality_highest)},
            {"ijg_match", ijg_match},
        });
    }

    json::Value ChromaSubsamplingEvidence::to_json() const
    {
        return json::object({
            {"subsampling", enum_or_null(subsampling)},
            {"upsampling", enum_or_null(upsampling)},
            {"horizontal_residual", horizontal_residual},
            {"horizontal_phase_ratio", horizontal_phase_ratio},
            {"horizontal_upsampling_ratio", horizontal_upsampling_ratio},
            {"vertical_residual", vertical_residual},
            {"vertical_phase_ratio", vertical_phase_ratio},
            {"vertical_upsampling_ratio", vertical_upsampling_ratio},
            {"confidence", confidence},
        });
    }

    json::Value CompressionTrace::to_json() const
    {
        return json::object({
            {"evidence", to_string(evidence)},
            {"codec", enum_or_null(codec)},
            {"quality", json::optional_or_null(quality)},
            {"subsampling", enum_or_null(subsampling)},
            {"confidence", confidence},
        });
    }

    json::Value CompressionHistory::to_json() const
    {
        return json::object({
            {"schema_version", schema_version},
            {"record", record.to_json()},
            {"configuration", reflect::to_json(configuration)},
        });
    }

    CompressionHistory compression_history(const Frame& frame, const CompressionHistoryOptions& options)
    {
        return analyze(frame, options, HeaderReading{std::nullopt, "only a frame was given"});
    }

    CompressionHistory compression_history(const DecodedImage& image, const CompressionHistoryOptions& options)
    {
        if (image.frame.empty())
        {
            throw ConfigError("compression_history() received an empty frame");
        }
        return analyze(image.frame, options, read_jpeg_header(image));
    }

    detail::LatticeSums detail::lattice_sums_scalar(const double* magnitudes, const std::size_t count,
                                                    const double inverse_step)
    {
        const auto add = [inverse_step](const double magnitude, double& squared_distance, std::size_t& unit_count)
        {
            const double position = magnitude * inverse_step;

            // Truncating a non-negative position plus a half rounds it to the
            // nearest lattice index, as std::round does.
            const int lattice_index = static_cast<int>(position + 0.5);
            const double distance = position - lattice_index;
            squared_distance += distance * distance;
            unit_count += lattice_index == 1;
        };

        std::array<double, 4> lanes{};
        std::size_t unit_count = 0;
        std::size_t index = 0;
        for (; index + 4 <= count; index += 4)
        {
            for (std::size_t lane = 0; lane < lanes.size(); ++lane)
            {
                add(magnitudes[index + lane], lanes[lane], unit_count);
            }
        }
        double rest = 0.0;
        for (; index < count; ++index)
        {
            add(magnitudes[index], rest, unit_count);
        }
        return LatticeSums{rest + ((lanes[0] + lanes[1]) + (lanes[2] + lanes[3])), unit_count};
    }

    std::optional<detail::LatticeSums> detail::lattice_sums_avx2(const double* magnitudes, const std::size_t count,
                                                                 const double inverse_step)
    {
#ifdef LOSSYLAB_LATTICE_AVX2
        static const bool has_avx2 = __builtin_cpu_supports("avx2");
        if (has_avx2)
        {
            return lattice_sums_avx2_kernel(magnitudes, count, inverse_step);
        }
#else
        static_cast<void>(magnitudes);
        static_cast<void>(count);
        static_cast<void>(inverse_step);
#endif
        return std::nullopt;
    }
}
