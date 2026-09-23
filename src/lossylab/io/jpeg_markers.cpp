#include "lossylab/io/jpeg_markers.hpp"

#include "lossylab/io/source_reader.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace lossylab::detail
{
    std::array<int, 64> ijg_scaled_table(const std::array<int, 64>& base, const int quality)
    {
        const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
        std::array<int, 64> scaled{};
        for (std::size_t i = 0; i < scaled.size(); ++i)
        {
            scaled[i] = std::clamp((base[i] * scale + 50) / 100, 1, 255);
        }
        return scaled;
    }

    namespace
    {
        // Huffman tables from the JPEG standard (ITU-T T.81, Annex K), as
        // libjpeg and FFmpeg (libavcodec/jpegtabs.h) carry them.

        /// Natural-order position of each zigzag-order coefficient.
        constexpr std::array<int, 64> zigzag_to_natural = {
            0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
            41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
            30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
        };

        /// An example Huffman table: code counts per length 1-16, then values.
        struct HuffmanTable
        {
            std::vector<std::uint8_t> counts;
            std::vector<std::uint8_t> values;

            bool operator==(const HuffmanTable&) const = default;
        };

        const HuffmanTable& standard_dc_luminance()
        {
            static const HuffmanTable table{
                {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0},
                {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
            };
            return table;
        }

        const HuffmanTable& standard_dc_chrominance()
        {
            static const HuffmanTable table{
                {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0},
                {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
            };
            return table;
        }

        const HuffmanTable& standard_ac_luminance()
        {
            static const HuffmanTable table{
                {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d},
                {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
                 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
                 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
                 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
                 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
                 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
                 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
                 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
                 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
                 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
                 0xf9, 0xfa},
            };
            return table;
        }

        const HuffmanTable& standard_ac_chrominance()
        {
            static const HuffmanTable table{
                {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77},
                {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
                 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
                 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
                 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
                 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
                 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
                 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
                 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
                 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
                 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
                 0xf9, 0xfa},
            };
            return table;
        }

        // Marker codes, the byte after 0xFF.
        constexpr std::uint8_t start_of_image = 0xd8;
        constexpr std::uint8_t end_of_image = 0xd9;
        constexpr std::uint8_t start_of_scan = 0xda;
        constexpr std::uint8_t define_quantization_table = 0xdb;
        constexpr std::uint8_t define_huffman_table = 0xc4;
        constexpr std::uint8_t define_restart_interval = 0xdd;
        constexpr std::uint8_t comment_marker = 0xfe;
        constexpr std::uint8_t temporary_marker = 0x01;

        /// How much of an APPn payload is read to find its signature, and of
        /// a COM payload to report as text. The rest is skipped.
        constexpr std::size_t signature_prefix_size = 64;
        constexpr std::size_t comment_size_limit = 4096;

        bool is_restart_marker(const std::uint8_t marker) { return marker >= 0xd0 && marker <= 0xd7; }

        bool is_start_of_frame(const std::uint8_t marker)
        {
            return marker >= 0xc0 && marker <= 0xcf && marker != define_huffman_table && marker != 0xc8 &&
                   marker != 0xcc;
        }

        /// SOF0-3 and SOF9-11 differ in their low two bits; SOF5-7 and
        /// SOF13-15 are the hierarchical (differential) processes.
        std::string process_of(const std::uint8_t marker)
        {
            if ((marker & 0x07) >= 5)
            {
                return "hierarchical";
            }
            switch (marker & 0x03)
            {
            case 0: return "baseline";
            case 1: return "extended";
            case 2: return "progressive";
            default: return "lossless";
            }
        }

        bool is_printable_text(const std::vector<std::uint8_t>& bytes)
        {
            return std::all_of(bytes.begin(), bytes.end(),
                               [](const std::uint8_t byte)
                               { return (byte >= 0x20 && byte < 0x7f) || byte == '\t' || byte == '\n' || byte == '\r'; });
        }

        /// The NUL-terminated signature an APPn payload starts with, when it
        /// is printable.
        std::string signature_of(const std::vector<std::uint8_t>& prefix)
        {
            const auto terminator = std::find(prefix.begin(), prefix.end(), std::uint8_t{0});
            const std::vector<std::uint8_t> signature(prefix.begin(), terminator);
            if (signature.empty() || !is_printable_text(signature))
            {
                return {};
            }
            return {signature.begin(), signature.end()};
        }

        /// Fills `ijg_quality` and `ijg_quality_exact` from the tables in use
        /// when the frame was coded.
        void estimate_ijg_quality(JpegInfo& info, const std::map<int, JpegInfo::QuantizationTable>& tables)
        {
            // libjpeg puts luminance in table 0 and chrominance in table 1.
            std::vector<std::pair<const JpegInfo::QuantizationTable*, const std::array<int, 64>*>> comparisons;
            for (const int id : {0, 1})
            {
                const bool used = std::any_of(info.components.begin(), info.components.end(),
                                              [id](const JpegInfo::Component& component)
                                              { return component.quantization_table == id; });
                const auto it = tables.find(id);
                if (used && it != tables.end() && it->second.precision == 8)
                {
                    comparisons.emplace_back(&it->second,
                                             id == 0 ? &standard_luminance_table : &standard_chrominance_table);
                }
            }
            if (comparisons.empty())
            {
                return;
            }
            const bool only_ijg_tables_used = std::all_of(info.components.begin(), info.components.end(),
                                                          [](const JpegInfo::Component& component)
                                                          { return component.quantization_table <= 1; });

            int best_quality = 0;
            long best_error = std::numeric_limits<long>::max();
            for (int quality = 1; quality <= 100; ++quality)
            {
                long error = 0;
                for (const auto& [table, base] : comparisons)
                {
                    const std::array<int, 64> scaled = ijg_scaled_table(*base, quality);
                    for (std::size_t i = 0; i < scaled.size(); ++i)
                    {
                        error += std::abs(table->values[i] - scaled[i]);
                    }
                }
                if (error < best_error)
                {
                    best_error = error;
                    best_quality = quality;
                }
            }
            info.ijg_quality = best_quality;
            info.ijg_quality_exact = best_error == 0 && only_ijg_tables_used;
        }

        /// Walks the file's markers into a JpegInfo, stopping at the
        /// end-of-image marker, the end of the input, or a segment whose
        /// length cannot be right.
        class MarkerWalk
        {
        public:
            MarkerWalk(SequentialReader& input, JpegInfo& info) : m_input(input), m_info(info) {}

            void run()
            {
                std::optional<std::uint8_t> marker = next_marker();
                while (marker.has_value())
                {
                    if (*marker == end_of_image)
                    {
                        m_info.has_end_of_image = true;
                        return;
                    }
                    if (*marker == start_of_image || *marker == temporary_marker || is_restart_marker(*marker))
                    {
                        marker = next_marker();
                        continue;
                    }

                    const std::vector<std::uint8_t> length_bytes = m_input.take(2);
                    if (length_bytes.size() < 2)
                    {
                        return;
                    }
                    const int length = (length_bytes[0] << 8) | length_bytes[1];
                    if (length < 2)
                    {
                        return;
                    }
                    const std::int64_t payload_end = m_input.position() + length - 2;
                    read_segment(*marker, length - 2);
                    m_input.skip(payload_end - m_input.position());

                    marker = (*marker == start_of_scan) ? marker_after_scan() : next_marker();
                }
            }

            [[nodiscard]] const std::map<int, JpegInfo::QuantizationTable>& tables_at_frame() const noexcept
            {
                return m_tables_at_frame;
            }

            [[nodiscard]] std::string huffman_summary() const
            {
                if (m_standard_huffman_tables == 0 && m_custom_huffman_tables == 0)
                {
                    return "none";
                }
                if (m_custom_huffman_tables == 0)
                {
                    return "standard";
                }
                return m_standard_huffman_tables == 0 ? "custom" : "mixed";
            }

            /// The ICC chunks in sequence order, with any missing one reported.
            [[nodiscard]] std::vector<std::uint8_t> assembled_icc_profile()
            {
                std::vector<std::uint8_t> profile;
                if (m_icc_chunks.empty())
                {
                    return profile;
                }
                for (int sequence = 1; sequence <= m_icc_chunk_count; ++sequence)
                {
                    const auto it = m_icc_chunks.find(sequence);
                    if (it == m_icc_chunks.end())
                    {
                        m_icc_problems.push_back("ICC_PROFILE chunk " + std::to_string(sequence) + " of " +
                                                 std::to_string(m_icc_chunk_count) + " is missing");
                        continue;
                    }
                    profile.insert(profile.end(), it->second.begin(), it->second.end());
                }
                return profile;
            }

            [[nodiscard]] std::vector<std::string>& icc_problems() noexcept { return m_icc_problems; }
            [[nodiscard]] std::optional<std::vector<std::uint8_t>>& exif() noexcept { return m_exif; }

        private:
            /// The next marker code, skipping the 0xFF fill bytes that may
            /// precede one and any stray bytes before it.
            std::optional<std::uint8_t> next_marker()
            {
                std::optional<std::uint8_t> byte = m_input.next();
                while (byte.has_value() && *byte != 0xff)
                {
                    byte = m_input.next();
                }
                while (byte.has_value() && *byte == 0xff)
                {
                    byte = m_input.next();
                }
                return byte;
            }

            /// Scans entropy-coded data for the marker that ends it. Inside
            /// it, 0xFF 0x00 is a stuffed data byte and restart markers are
            /// part of the scan.
            std::optional<std::uint8_t> marker_after_scan()
            {
                while (true)
                {
                    std::optional<std::uint8_t> byte = m_input.next();
                    if (!byte.has_value())
                    {
                        return std::nullopt;
                    }
                    if (*byte != 0xff)
                    {
                        continue;
                    }
                    do
                    {
                        byte = m_input.next();
                    } while (byte.has_value() && *byte == 0xff);
                    if (!byte.has_value())
                    {
                        return std::nullopt;
                    }
                    if (*byte != 0x00 && !is_restart_marker(*byte))
                    {
                        return byte;
                    }
                }
            }

            void read_segment(const std::uint8_t marker, const int payload_size)
            {
                if (is_start_of_frame(marker))
                {
                    read_frame_header(marker, m_input.take(static_cast<std::size_t>(payload_size)));
                }
                else if (marker == define_quantization_table)
                {
                    read_quantization_tables(m_input.take(static_cast<std::size_t>(payload_size)));
                }
                else if (marker == define_huffman_table)
                {
                    read_huffman_tables(m_input.take(static_cast<std::size_t>(payload_size)));
                }
                else if (marker == define_restart_interval && payload_size >= 2)
                {
                    const std::vector<std::uint8_t> interval = m_input.take(2);
                    if (interval.size() == 2)
                    {
                        m_info.restart_interval = (interval[0] << 8) | interval[1];
                    }
                }
                else if (marker == start_of_scan)
                {
                    ++m_info.scan_count;
                }
                else if (marker >= 0xe0 && marker <= 0xef)
                {
                    read_application_segment(marker, payload_size);
                }
                else if (marker == comment_marker)
                {
                    read_comment(payload_size);
                }
            }

            void read_frame_header(const std::uint8_t marker, const std::vector<std::uint8_t>& header)
            {
                // Only the first frame header describes the image; a
                // hierarchical file carries several.
                if (!m_info.process.empty() || header.size() < 6)
                {
                    return;
                }
                m_info.process = process_of(marker);
                m_info.arithmetic_coding = marker >= 0xc9;
                m_info.precision = header[0];

                const std::size_t component_count = header[5];
                for (std::size_t i = 0; i < component_count && 6 + i * 3 + 2 < header.size(); ++i)
                {
                    const std::size_t offset = 6 + i * 3;
                    m_info.components.push_back(JpegInfo::Component{header[offset], header[offset + 1] >> 4,
                                                                    header[offset + 1] & 0x0f, header[offset + 2]});
                }
                m_tables_at_frame = m_current_tables;
            }

            void read_quantization_tables(const std::vector<std::uint8_t>& payload)
            {
                std::size_t offset = 0;
                while (offset < payload.size())
                {
                    const int precision_flag = payload[offset] >> 4;
                    const int id = payload[offset] & 0x0f;
                    const std::size_t entry_size = precision_flag == 0 ? 1 : 2;
                    ++offset;
                    if (offset + 64 * entry_size > payload.size())
                    {
                        return;
                    }

                    JpegInfo::QuantizationTable table;
                    table.id = id;
                    table.precision = precision_flag == 0 ? 8 : 16;
                    for (std::size_t i = 0; i < 64; ++i)
                    {
                        const std::size_t at = offset + i * entry_size;
                        const int value = entry_size == 1 ? payload[at] : (payload[at] << 8) | payload[at + 1];
                        table.values[static_cast<std::size_t>(zigzag_to_natural[i])] = value;
                    }
                    offset += 64 * entry_size;

                    m_current_tables[id] = table;
                    m_info.quantization_tables.push_back(table);
                }
            }

            void read_huffman_tables(const std::vector<std::uint8_t>& payload)
            {
                std::size_t offset = 0;
                while (offset + 17 <= payload.size())
                {
                    const bool is_ac = (payload[offset] >> 4) != 0;
                    HuffmanTable table;
                    table.counts.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset + 1),
                                        payload.begin() + static_cast<std::ptrdiff_t>(offset + 17));
                    std::size_t value_count = 0;
                    for (const std::uint8_t count : table.counts)
                    {
                        value_count += count;
                    }
                    offset += 17;
                    if (offset + value_count > payload.size())
                    {
                        return;
                    }
                    table.values.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                        payload.begin() + static_cast<std::ptrdiff_t>(offset + value_count));
                    offset += value_count;

                    const bool standard = is_ac ? (table == standard_ac_luminance() || table == standard_ac_chrominance())
                                                : (table == standard_dc_luminance() || table == standard_dc_chrominance());
                    ++(standard ? m_standard_huffman_tables : m_custom_huffman_tables);
                }
            }

            void read_application_segment(const std::uint8_t marker, const int payload_size)
            {
                std::vector<std::uint8_t> prefix =
                    m_input.take(std::min(static_cast<std::size_t>(payload_size), signature_prefix_size));
                JpegInfo::Segment segment;
                segment.marker = "APP" + std::to_string(marker - 0xe0);
                segment.identifier = signature_of(prefix);
                segment.size_bytes = payload_size;
                m_info.segments.push_back(segment);

                // "Adobe", then version, two flag words and the transform.
                constexpr std::size_t adobe_transform_offset = 11;
                if (segment.identifier.starts_with("Adobe") && prefix.size() > adobe_transform_offset &&
                    !m_info.adobe_transform.has_value())
                {
                    m_info.adobe_transform = prefix[adobe_transform_offset];
                }

                const bool is_icc_chunk = marker == 0xe2 && segment.identifier == "ICC_PROFILE";
                const bool is_first_exif = marker == 0xe1 && segment.identifier == "Exif" && !m_exif.has_value();
                if (!is_icc_chunk && !is_first_exif)
                {
                    return;
                }
                const std::vector<std::uint8_t> rest =
                    m_input.take(static_cast<std::size_t>(payload_size) - prefix.size());
                prefix.insert(prefix.end(), rest.begin(), rest.end());
                if (is_first_exif)
                {
                    m_exif = std::move(prefix);
                }
                else
                {
                    read_icc_chunk(prefix);
                }
            }

            /// "ICC_PROFILE\0", the chunk's 1-based sequence number, the
            /// number of chunks, then that chunk's part of the profile.
            void read_icc_chunk(const std::vector<std::uint8_t>& payload)
            {
                constexpr std::size_t chunk_header_size = 14;
                if (payload.size() < chunk_header_size)
                {
                    m_icc_problems.emplace_back("an ICC_PROFILE segment is too short to hold a chunk");
                    return;
                }
                const int sequence = payload[12];
                const int count = payload[13];
                if (m_icc_chunk_count != 0 && count != m_icc_chunk_count)
                {
                    m_icc_problems.push_back("ICC_PROFILE segments disagree on the chunk count (" +
                                             std::to_string(m_icc_chunk_count) + " and " + std::to_string(count) +
                                             ")");
                }
                m_icc_chunk_count = std::max(m_icc_chunk_count, count);
                if (!m_icc_chunks.try_emplace(sequence, payload.begin() + chunk_header_size, payload.end()).second)
                {
                    m_icc_problems.push_back("ICC_PROFILE chunk " + std::to_string(sequence) + " appears twice");
                }
            }

            void read_comment(const int payload_size)
            {
                std::vector<std::uint8_t> text =
                    m_input.take(std::min(static_cast<std::size_t>(payload_size), comment_size_limit));
                m_info.segments.push_back(JpegInfo::Segment{"COM", "", payload_size});
                while (!text.empty() && text.back() == 0)
                {
                    text.pop_back();
                }
                if (!m_info.comment.has_value() && is_printable_text(text))
                {
                    m_info.comment = std::string(text.begin(), text.end());
                }
            }

            SequentialReader& m_input;
            JpegInfo& m_info;
            std::map<int, JpegInfo::QuantizationTable> m_current_tables;
            std::map<int, JpegInfo::QuantizationTable> m_tables_at_frame;
            int m_standard_huffman_tables = 0;
            int m_custom_huffman_tables = 0;
            std::map<int, std::vector<std::uint8_t>> m_icc_chunks;
            int m_icc_chunk_count = 0;
            std::vector<std::string> m_icc_problems;
            std::optional<std::vector<std::uint8_t>> m_exif;
        };
    }

    std::optional<JpegMarkers> read_jpeg_markers(const Source& source)
    {
        SourceReader reader(source);
        const std::vector<std::uint8_t> start = reader.read(0, 2);
        if (start.size() < 2 || start[0] != 0xff || start[1] != start_of_image)
        {
            return std::nullopt;
        }

        SequentialReader input(reader);
        input.skip(2);

        JpegMarkers markers;
        JpegInfo& info = markers.info;
        MarkerWalk walk(input, info);
        walk.run();
        info.huffman_tables = walk.huffman_summary();
        estimate_ijg_quality(info, walk.tables_at_frame());

        if (info.has_end_of_image)
        {
            if (const std::optional<std::int64_t> size = reader.size())
            {
                info.trailing_bytes = std::max<std::int64_t>(0, *size - input.position());
            }
        }

        markers.icc_profile = walk.assembled_icc_profile();
        markers.icc_problems = std::move(walk.icc_problems());
        markers.exif = std::move(walk.exif());
        return markers;
    }
}
