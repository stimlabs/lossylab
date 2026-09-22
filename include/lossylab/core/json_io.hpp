#pragma once

#include "lossylab/core/json.hpp"

#include <concepts>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lossylab::json
{
    /// A type that can write itself to JSON.
    template <typename T>
    concept Writable = requires(const T& value) {
        { value.to_json() } -> std::same_as<Value>;
    };

    /// A type that can be rebuilt from JSON.
    ///
    /// The read half is a static factory rather than a member function, which
    /// is also why this contract is a concept rather than an abstract base
    /// class: a static function cannot be virtual, so a vtable could only ever
    /// carry the write half, and the round trip is the part worth enforcing.
    /// The types involved are mostly aggregates held by value inside
    /// StageRecord, which is built once per stage per frame; a vtable would
    /// cost a pointer in each of them and take brace initialization away.
    template <typename T>
    concept Readable = requires(const Value& value) {
        { T::from_json(value) } -> std::same_as<T>;
    };

    /// Both halves, for the types that survive a round trip.
    ///
    /// Not everything serializable is Serializable. A capability listing or a
    /// probe result describes a machine or a file, so it is written for a
    /// reader and never read back; those satisfy Writable alone. Every type
    /// that claims the round trip is checked against this concept in
    /// json_io.cpp, so dropping half of a pair is a build failure.
    template <typename T>
    concept Serializable = Writable<T> && Readable<T>;

    /// Serializes a sequence as a JSON array.
    template <Writable T>
    [[nodiscard]] Value to_array(const std::vector<T>& items)
    {
        Array result;
        result.reserve(items.size());
        for (const T& item : items)
        {
            result.push_back(item.to_json());
        }
        return Value(std::move(result));
    }

    /// As above for elements JSON represents directly, such as int or string.
    template <typename T>
        requires(!Writable<T> && std::constructible_from<Value, const T&>)
    [[nodiscard]] Value to_array(const std::vector<T>& items)
    {
        Array result;
        result.reserve(items.size());
        for (const T& item : items)
        {
            result.emplace_back(item);
        }
        return Value(std::move(result));
    }

    /// As above, mapping each element first. Covers enums, whose JSON form is
    /// the name their own to_string gives them.
    template <typename T, typename Projection>
    [[nodiscard]] Value to_array(const std::vector<T>& items, Projection project)
    {
        Array result;
        result.reserve(items.size());
        for (const T& item : items)
        {
            result.emplace_back(project(item));
        }
        return Value(std::move(result));
    }

    /// Rebuilds a sequence from a JSON array. Throws ConfigError when the value
    /// is not an array, or when any element fails to parse.
    template <Readable T>
    [[nodiscard]] std::vector<T> from_array(const Value& value)
    {
        const Array& items = value.get_ref<const Array&>();
        std::vector<T> result;
        result.reserve(items.size());
        for (const Value& item : items)
        {
            result.push_back(T::from_json(item));
        }
        return result;
    }

    /// As above, applying `parse` to each element. For enums and anything else
    /// whose reader is a free function rather than a static member.
    template <typename T, typename Parse>
    [[nodiscard]] std::vector<T> from_array(const Value& value, Parse parse)
    {
        const Array& items = value.get_ref<const Array&>();
        std::vector<T> result;
        result.reserve(items.size());
        for (const Value& item : items)
        {
            result.push_back(parse(item));
        }
        return result;
    }

    /// A map keyed by string as a JSON object, in the map's own key order.
    template <typename T>
        requires std::constructible_from<Value, const T&>
    [[nodiscard]] Value to_object(const std::map<std::string, T>& entries)
    {
        Object members;
        members.reserve(entries.size());
        for (const auto& [key, value] : entries)
        {
            members.emplace_back(key, Value(value));
        }
        return object(std::move(members));
    }

    template <Writable T>
    [[nodiscard]] Value to_object(const std::map<std::string, T>& entries)
    {
        Object members;
        members.reserve(entries.size());
        for (const auto& [key, value] : entries)
        {
            members.emplace_back(key, value.to_json());
        }
        return object(std::move(members));
    }

    /// The inverse. Every member must be a string.
    [[nodiscard]] std::map<std::string, std::string> to_string_map(const Value& value);

    /// The inverse for numeric maps, as the measurement results use.
    [[nodiscard]] std::map<std::string, double> to_double_map(const Value& value);

    /// An engaged optional as its value, an empty one as JSON null.
    ///
    /// Written rather than omitted, because a reader has to be able to tell a
    /// quantizer the codec did not report from one it reported as zero, and a
    /// missing key cannot make that distinction.
    template <typename T>
        requires std::constructible_from<Value, const T&>
    [[nodiscard]] Value optional_or_null(const std::optional<T>& value)
    {
        return value.has_value() ? Value(*value) : Value();
    }

    template <Writable T>
    [[nodiscard]] Value optional_or_null(const std::optional<T>& value)
    {
        return value.has_value() ? value->to_json() : Value();
    }

    /// The inverses. A null or absent value reads back as nullopt; anything
    /// else has to be of the right type, or ConfigError is thrown.
    [[nodiscard]] std::optional<double> optional_double(const Value& value);
    [[nodiscard]] std::optional<std::int64_t> optional_int(const Value& value);
    [[nodiscard]] std::optional<int> optional_int32(const Value& value);
    [[nodiscard]] std::optional<bool> optional_bool(const Value& value);
    [[nodiscard]] std::optional<std::string> optional_string(const Value& value);

    template <Readable T>
    [[nodiscard]] std::optional<T> optional_object(const Value& value)
    {
        return value.is_null() ? std::nullopt : std::optional<T>(T::from_json(value));
    }
}
