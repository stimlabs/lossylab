#include "lossylab/core/stage_evidence.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/core/reflect.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace lossylab
{
    namespace
    {
        template <typename Variant, std::size_t Index>
        constexpr StageKind kind_at = StageKindOf<std::variant_alternative_t<Index, Variant>>::value;

        template <std::size_t... Index>
        constexpr bool kinds_match_and_are_distinct(std::index_sequence<Index...>)
        {
            constexpr std::size_t count = sizeof...(Index);
            constexpr StageKind evidence_kinds[] = {kind_at<StageEvidence, Index>...};
            constexpr StageKind configuration_kinds[] = {kind_at<StageConfiguration, Index>...};
            for (std::size_t i = 0; i < count; ++i)
            {
                if (evidence_kinds[i] != configuration_kinds[i])
                {
                    return false;
                }
                for (std::size_t j = i + 1; j < count; ++j)
                {
                    if (evidence_kinds[i] == evidence_kinds[j])
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        static_assert(std::variant_size_v<StageEvidence> == std::variant_size_v<StageConfiguration>,
                      "StageEvidence and StageConfiguration need one alternative per stage kind each");
        static_assert(kinds_match_and_are_distinct(std::make_index_sequence<std::variant_size_v<StageEvidence>>{}),
                      "StageEvidence and StageConfiguration must list the same kinds in the same order, once each");

        template <typename Variant>
        StageKind kind_of(const Variant& value) noexcept
        {
            return std::visit([](const auto& alternative)
                              { return StageKindOf<std::remove_cvref_t<decltype(alternative)>>::value; },
                              value);
        }

        template <typename Variant>
        json::Value fields_to_json(const Variant& value)
        {
            return std::visit([](const auto& alternative) { return reflect::to_json(alternative); }, value);
        }

        template <typename Variant, std::size_t... Index>
        Variant fields_from_json(const StageKind kind, const json::Value& value, const char* what,
                          std::index_sequence<Index...>)
        {
            std::optional<Variant> result;
            const auto read_if_kind = [&]<std::size_t I>(std::integral_constant<std::size_t, I>)
            {
                if (kind_at<Variant, I> == kind)
                {
                    result.emplace(std::in_place_index<I>,
                                   reflect::from_json<std::variant_alternative_t<I, Variant>>(value));
                }
            };
            (read_if_kind(std::integral_constant<std::size_t, Index>{}), ...);
            if (!result.has_value())
            {
                throw ConfigError(std::string("no ") + what + " for stage kind '" + to_string(kind) + "'");
            }
            return std::move(*result);
        }
    }

    StageKind stage_kind(const StageEvidence& evidence) noexcept
    {
        return kind_of(evidence);
    }

    StageKind stage_kind(const StageConfiguration& configuration) noexcept
    {
        return kind_of(configuration);
    }

    json::Value evidence_to_json(const StageEvidence& evidence)
    {
        return fields_to_json(evidence);
    }

    json::Value configuration_to_json(const StageConfiguration& configuration)
    {
        return fields_to_json(configuration);
    }

    StageEvidence evidence_from_json(const StageKind kind, const json::Value& value)
    {
        return fields_from_json<StageEvidence>(kind, value, "evidence",
                                               std::make_index_sequence<std::variant_size_v<StageEvidence>>{});
    }

    StageConfiguration configuration_from_json(const StageKind kind, const json::Value& value)
    {
        return fields_from_json<StageConfiguration>(
            kind, value, "configuration", std::make_index_sequence<std::variant_size_v<StageConfiguration>>{});
    }
}
