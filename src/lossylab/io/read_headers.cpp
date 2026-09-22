#include "lossylab/io/read_headers.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/core/json_io.hpp"
#include "lossylab/core/schema_version.hpp"

namespace lossylab
{
    json::Value ParameterSet::to_json() const
    {
        return json::object({{"kind", kind}, {"id", id}, {"fields", fields}});
    }

    json::Value SliceInfo::to_json() const
    {
        return json::object({
            {"index", index},
            {"slice_type", slice_type},
            {"qp", json::optional_or_null(qp)},
            {"size_bytes", json::optional_or_null(size_bytes)},
        });
    }

    json::Value HeaderInfo::to_json() const
    {
        return json::object({
            {"schema_version", schema_version},
            {"codec", codec_name},
            {"embedded_encoder_settings", json::optional_or_null(embedded_encoder_settings)},
            {"embedded_encoder_settings_availability",
             to_string(embedded_encoder_settings_availability)},
            {"encoder_settings", json::to_object(encoder_settings)},
            {"parameter_sets", json::to_array(parameter_sets)},
            {"slices", json::to_array(slices)},
            {"quantizer_indices", json::to_array(quantizer_indices)},
            {"bitstream_color", bitstream_color},
        });
    }

    HeaderInfo read_headers(const Source& source, const ReadHeadersOptions& options)
    {
        static_cast<void>(source);
        if (options.max_slices < 0)
        {
            throw ConfigError("read_headers() max_slices cannot be negative");
        }

        LL_NOT_IMPLEMENTED();
    }
}
