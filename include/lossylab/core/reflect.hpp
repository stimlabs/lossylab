#pragma once

#include "lossylab/core/error.hpp"
#include "lossylab/core/json_io.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace lossylab::reflect
{
    /// One field of a reflected aggregate: its JSON/Python-visible name and a
    /// pointer to the member it reads. `name` always comes from stringifying
    /// the member name in LOSSYLAB_REFLECT, so it is guaranteed
    /// null-terminated even though its static type is string_view.
    template <typename T, typename M>
    struct Member
    {
        std::string_view name;
        M T::*pointer;
    };

    template <typename T, typename M>
    constexpr Member<T, M> member(std::string_view name, M T::*pointer)
    {
        return Member<T, M>{name, pointer};
    }

    /// Specialized once per reflected type by LOSSYLAB_REFLECT, with a
    /// `members` tuple of Member<T, ...>. Every consumer (to_json, nanobind
    /// binding, future ones) walks that one tuple instead of each keeping its
    /// own field list.
    template <typename T>
    struct Fields;

    namespace detail
    {
        /// Converts to anything, so it can stand in for any aggregate member
        /// when probing how many braced initializers a type accepts.
        struct AnyType
        {
            template <typename T>
            constexpr operator T() const noexcept;
        };

        template <typename T, std::size_t... I>
        constexpr bool brace_constructible_impl(std::index_sequence<I...>)
        {
            return requires { T{(static_cast<void>(I), AnyType{})...}; };
        }

        template <typename T, std::size_t N>
        constexpr bool brace_constructible_with()
        {
            return brace_constructible_impl<T>(std::make_index_sequence<N>{});
        }

        template <typename T, std::size_t N>
        consteval std::size_t count_impl()
        {
            if constexpr (N == 0)
            {
                return 0;
            }
            else if constexpr (brace_constructible_with<T, N>())
            {
                return N;
            }
            else
            {
                return count_impl<T, N - 1>();
            }
        }

        /// The number of members in aggregate T, found by the largest N for
        /// which `T{N brace-init args}` compiles. LOSSYLAB_REFLECT compares
        /// this against its own argument count, so a member added to T
        /// without being added to its reflection list is a build failure
        /// instead of a silently incomplete JSON/Python view.
        template <typename T, std::size_t Max = 48>
        consteval std::size_t count_aggregate_fields()
        {
            static_assert(std::is_aggregate_v<T>, "reflect: T must be a plain aggregate (no base class, "
                                                   "no private members, no user-declared constructors)");
            return count_impl<T, Max>();
        }

        template <typename>
        struct IsOptional : std::false_type
        {
        };

        template <typename U>
        struct IsOptional<std::optional<U>> : std::true_type
        {
        };

        template <typename>
        struct IsVector : std::false_type
        {
        };

        template <typename U>
        struct IsVector<std::vector<U>> : std::true_type
        {
        };

        template <typename>
        struct IsStringMap : std::false_type
        {
        };

        template <typename U>
        struct IsStringMap<std::map<std::string, U>> : std::true_type
        {
        };

        template <typename>
        struct IsArray : std::false_type
        {
        };

        template <typename U, std::size_t N>
        struct IsArray<std::array<U, N>> : std::true_type
        {
        };

        /// True for a type LOSSYLAB_REFLECT has declared the fields of.
        template <typename T>
        concept Reflected = requires { Fields<T>::members; };
    }

    template <typename T>
    [[nodiscard]] json::Value to_json(const T& value);

    template <typename T>
    [[nodiscard]] T from_json(const json::Value& value);

    namespace detail
    {
        /// Converts one reflected field to JSON, dispatching on its C++ type
        /// so LOSSYLAB_REFLECT'd structs never repeat this per field:
        /// Writable types (to_json()), enums (ADL to_string()), optional,
        /// vector, std::array, string-keyed map and reflected structs all
        /// resolve automatically; anything else must convert directly to
        /// json::Value.
        template <typename T>
        [[nodiscard]] json::Value to_json_value(const T& value)
        {
            if constexpr (json::Writable<T>)
            {
                return value.to_json();
            }
            else if constexpr (std::is_enum_v<T>)
            {
                return json::Value(to_string(value));
            }
            else if constexpr (IsOptional<T>::value)
            {
                return value.has_value() ? to_json_value(*value) : json::Value();
            }
            else if constexpr (IsVector<T>::value || IsArray<T>::value)
            {
                json::Array items;
                items.reserve(value.size());
                for (const auto& item : value)
                {
                    items.push_back(to_json_value(item));
                }
                return json::array(std::move(items));
            }
            else if constexpr (IsStringMap<T>::value)
            {
                json::Object entries;
                for (const auto& [key, item] : value)
                {
                    entries.emplace_back(key, to_json_value(item));
                }
                return json::object(std::move(entries));
            }
            else if constexpr (Reflected<T>)
            {
                return reflect::to_json(value);
            }
            else
            {
                static_assert(std::constructible_from<json::Value, const T&>,
                               "reflect: no JSON conversion known for this field's type");
                return json::Value(value);
            }
        }

        /// The reverse of to_json_value(): types with a static from_json(),
        /// enums (through an ADL `from_string(name, value&)` next to the
        /// enum's own `*_from_string`), optional (null is empty), vector,
        /// std::array, string-keyed map, reflected structs, and whatever
        /// json::Value converts to directly.
        template <typename T>
        [[nodiscard]] T from_json_value(const json::Value& value)
        {
            if constexpr (json::Readable<T>)
            {
                return T::from_json(value);
            }
            else if constexpr (std::is_enum_v<T>)
            {
                T parsed{};
                from_string(value.get<std::string>(), parsed);
                return parsed;
            }
            else if constexpr (IsOptional<T>::value)
            {
                return value.is_null() ? T() : T(from_json_value<typename T::value_type>(value));
            }
            else if constexpr (IsVector<T>::value)
            {
                T items;
                items.reserve(value.size());
                for (const json::Value& item : value)
                {
                    items.push_back(from_json_value<typename T::value_type>(item));
                }
                return items;
            }
            else if constexpr (IsArray<T>::value)
            {
                T items{};
                if (!value.is_array() || value.size() != items.size())
                {
                    throw ConfigError("expected an array of " + std::to_string(items.size()) + " values");
                }
                for (std::size_t index = 0; index < items.size(); ++index)
                {
                    items[index] = from_json_value<typename T::value_type>(value[index]);
                }
                return items;
            }
            else if constexpr (IsStringMap<T>::value)
            {
                T entries;
                for (const auto& [key, item] : value.items())
                {
                    entries.emplace(key, from_json_value<typename T::mapped_type>(item));
                }
                return entries;
            }
            else if constexpr (Reflected<T>)
            {
                return reflect::from_json<T>(value);
            }
            else
            {
                return value.get<T>();
            }
        }
    }

    /// Every reflected field of `value` as a JSON object, in declaration
    /// order. Callers that need extra top-level keys (e.g. schema_version)
    /// build those into the surrounding object themselves.
    template <typename T>
    json::Value to_json(const T& value)
    {
        json::Object fields;
        std::apply(
            [&](const auto&... members)
            { (fields.emplace_back(std::string(members.name), detail::to_json_value(value.*(members.pointer))), ...); },
            Fields<T>::members);
        return json::object(std::move(fields));
    }

    /// Reads what to_json() wrote. Every reflected field must be present;
    /// keys that are not fields are ignored.
    template <typename T>
    T from_json(const json::Value& value)
    {
        T result{};
        std::apply(
            [&](const auto&... members)
            {
                ((result.*(members.pointer) =
                      detail::from_json_value<std::remove_cvref_t<decltype(result.*(members.pointer))>>(
                          value.at(std::string(members.name)))),
                 ...);
            },
            Fields<T>::members);
        return result;
    }
}

#define LOSSYLAB_REFLECT_MEMBER(Type, field) ::lossylab::reflect::member(#field, &Type::field)

#define LOSSYLAB_REFLECT_EXPAND(x) x

#define LOSSYLAB_REFLECT_FE_1(Type, a) LOSSYLAB_REFLECT_MEMBER(Type, a)
#define LOSSYLAB_REFLECT_FE_2(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_1(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_3(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_2(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_4(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_3(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_5(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_4(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_6(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_5(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_7(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_6(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_8(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_7(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_9(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_8(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_10(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_9(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_11(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_10(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_12(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_11(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_13(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_12(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_14(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_13(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_15(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_14(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_16(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_15(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_17(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_16(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_18(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_17(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_19(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_18(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_20(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_19(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_21(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_20(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_22(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_21(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_23(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_22(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_24(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_23(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_25(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_24(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_26(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_25(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_27(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_26(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_28(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_27(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_29(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_28(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_30(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_29(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_31(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_30(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_32(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_31(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_33(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_32(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_34(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_33(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_35(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_34(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_36(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_35(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_37(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_36(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_38(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_37(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_39(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_38(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_40(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_39(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_41(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_40(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_42(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_41(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_43(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_42(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_44(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_43(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_45(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_44(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_46(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_45(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_47(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_46(Type, __VA_ARGS__))
#define LOSSYLAB_REFLECT_FE_48(Type, a, ...) \
    LOSSYLAB_REFLECT_MEMBER(Type, a), LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_FE_47(Type, __VA_ARGS__))

#define LOSSYLAB_REFLECT_GET_FE(Type, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17,  \
                                _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, _33,    \
                                _34, _35, _36, _37, _38, _39, _40, _41, _42, _43, _44, _45, _46, _47, _48, NAME,   \
                                ...)                                                                                \
    NAME

#define LOSSYLAB_REFLECT_FOR_EACH(Type, ...)                                                                       \
    LOSSYLAB_REFLECT_EXPAND(LOSSYLAB_REFLECT_GET_FE(                                                               \
        Type, __VA_ARGS__, LOSSYLAB_REFLECT_FE_48, LOSSYLAB_REFLECT_FE_47, LOSSYLAB_REFLECT_FE_46,                 \
        LOSSYLAB_REFLECT_FE_45, LOSSYLAB_REFLECT_FE_44, LOSSYLAB_REFLECT_FE_43, LOSSYLAB_REFLECT_FE_42,            \
        LOSSYLAB_REFLECT_FE_41, LOSSYLAB_REFLECT_FE_40, LOSSYLAB_REFLECT_FE_39, LOSSYLAB_REFLECT_FE_38,            \
        LOSSYLAB_REFLECT_FE_37, LOSSYLAB_REFLECT_FE_36, LOSSYLAB_REFLECT_FE_35, LOSSYLAB_REFLECT_FE_34,            \
        LOSSYLAB_REFLECT_FE_33, LOSSYLAB_REFLECT_FE_32, LOSSYLAB_REFLECT_FE_31, LOSSYLAB_REFLECT_FE_30,            \
        LOSSYLAB_REFLECT_FE_29, LOSSYLAB_REFLECT_FE_28, LOSSYLAB_REFLECT_FE_27, LOSSYLAB_REFLECT_FE_26,            \
        LOSSYLAB_REFLECT_FE_25, LOSSYLAB_REFLECT_FE_24, LOSSYLAB_REFLECT_FE_23, LOSSYLAB_REFLECT_FE_22,            \
        LOSSYLAB_REFLECT_FE_21, LOSSYLAB_REFLECT_FE_20, LOSSYLAB_REFLECT_FE_19, LOSSYLAB_REFLECT_FE_18,            \
        LOSSYLAB_REFLECT_FE_17, LOSSYLAB_REFLECT_FE_16, LOSSYLAB_REFLECT_FE_15, LOSSYLAB_REFLECT_FE_14,            \
        LOSSYLAB_REFLECT_FE_13, LOSSYLAB_REFLECT_FE_12, LOSSYLAB_REFLECT_FE_11, LOSSYLAB_REFLECT_FE_10,            \
        LOSSYLAB_REFLECT_FE_9, LOSSYLAB_REFLECT_FE_8, LOSSYLAB_REFLECT_FE_7, LOSSYLAB_REFLECT_FE_6,                \
        LOSSYLAB_REFLECT_FE_5, LOSSYLAB_REFLECT_FE_4, LOSSYLAB_REFLECT_FE_3, LOSSYLAB_REFLECT_FE_2,                \
        LOSSYLAB_REFLECT_FE_1)(Type, __VA_ARGS__))

/// Declares Type's JSON/Python-visible data model as the given list of member
/// names, e.g. `LOSSYLAB_REFLECT(HeaderInfo, codec_name, parameter_sets)`.
/// Type must be a plain aggregate. A member added to Type without being
/// added here fails to compile (LOSSYLAB_REFLECT_MEMBER for the missing name
/// still compiles fine on its own; it's the count static_assert below that
/// catches an incomplete list).
#define LOSSYLAB_REFLECT(Type, ...)                                                                                \
    template <>                                                                                                   \
    struct lossylab::reflect::Fields<Type>                                                                        \
    {                                                                                                              \
        static constexpr auto members = std::tuple{LOSSYLAB_REFLECT_FOR_EACH(Type, __VA_ARGS__)};                 \
        static_assert(std::tuple_size_v<decltype(members)> ==                                                     \
                          lossylab::reflect::detail::count_aggregate_fields<Type>(),                               \
                      #Type " reflection list is out of sync with its members: add or remove a name in "          \
                            "LOSSYLAB_REFLECT(" #Type ", ...) to match");                                          \
    }
