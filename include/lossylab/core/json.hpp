#pragma once

#include "lossylab/core/error.hpp"

#include <nlohmann/json.hpp>

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lossylab::json
{
    /// A JSON value, backed by nlohmann::json (vendored in external/nlohmann).
    ///
    /// Objects keep insertion order rather than sorting keys. Record output is
    /// compared and diffed across runs and across classes, so a stable, chosen
    /// field order is worth more than lookup speed at these sizes, hence
    /// ordered_json rather than the library's default map-backed json.
    using Value = nlohmann::ordered_json;

    /// A plain accumulator for building an array value imperatively before
    /// handing it to `array()`. Matches nlohmann's own array storage exactly.
    using Array = Value::array_t;

    /// As above for an object, keeping insertion order until `object()` wraps
    /// it into a Value.
    using Object = std::vector<std::pair<std::string, Value>>;

    /// Convenience builders, so call sites read as data rather than as code.
    inline Value object(std::initializer_list<std::pair<std::string, Value>> fields)
    {
        Value result = Value::object();
        for (const auto& [key, value] : fields)
        {
            result.push_back({key, value});
        }
        return result;
    }

    inline Value object(Object fields)
    {
        Value result = Value::object();
        for (auto& [key, value] : fields)
        {
            result.push_back({std::move(key), std::move(value)});
        }
        return result;
    }

    inline Value array(std::initializer_list<Value> items)
    {
        Value result = Value::array();
        for (const Value& item : items)
        {
            result.push_back(item);
        }
        return result;
    }

    inline Value array(Array items)
    {
        Value result = Value::array();
        for (auto& item : items)
        {
            result.push_back(std::move(item));
        }
        return result;
    }

    /// Object member lookup that reads as data rather than as code: `member`
    /// returns a null Value for an absent key instead of throwing, and the
    /// `*_or` forms fall back on an absent key or a present value of the
    /// wrong type, rather than throwing either way.
    [[nodiscard]] inline Value member(const Value& object_value, std::string_view key)
    {
        const auto it = object_value.find(std::string(key));
        return it == object_value.end() ? Value() : *it;
    }

    [[nodiscard]] inline bool bool_or(const Value& object_value, std::string_view key, bool fallback)
    {
        const auto it = object_value.find(std::string(key));
        return (it != object_value.end() && it->is_boolean()) ? it->get<bool>() : fallback;
    }

    [[nodiscard]] inline std::int64_t int_or(const Value& object_value, std::string_view key,
                                             std::int64_t fallback)
    {
        const auto it = object_value.find(std::string(key));
        return (it != object_value.end() && it->is_number()) ? it->get<std::int64_t>() : fallback;
    }

    [[nodiscard]] inline double double_or(const Value& object_value, std::string_view key,
                                          double fallback)
    {
        const auto it = object_value.find(std::string(key));
        return (it != object_value.end() && it->is_number()) ? it->get<double>() : fallback;
    }

    [[nodiscard]] inline std::string string_or(const Value& object_value, std::string_view key,
                                               const std::string& fallback)
    {
        const auto it = object_value.find(std::string(key));
        return (it != object_value.end() && it->is_string()) ? it->get<std::string>() : fallback;
    }

    /// Parses JSON text. Throws ConfigError on malformed input, matching the
    /// contract that everything escaping the public API derives from
    /// lossylab::Error rather than a third-party exception type.
    [[nodiscard]] inline Value parse(std::string_view text)
    {
        try
        {
            return Value::parse(text);
        }
        catch (const Value::exception& error)
        {
            throw ConfigError(std::string("json: ") + error.what());
        }
    }
}
