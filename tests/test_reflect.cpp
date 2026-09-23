#include "lossylab/core/reflect.hpp"
#include "lossylab/io/probe.hpp"
#include "lossylab/io/read_headers.hpp"

#include <cassert>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

// count_aggregate_fields and LOSSYLAB_REFLECT's own static_assert only prove
// anything at compile time, so most of what this file checks is expressed as
// static_assert rather than runtime assert(). A build failure here is the
// test failing.

namespace
{
    struct Empty
    {
    };

    struct OneField
    {
        int a = 0;
    };

    enum class ReflectTestColor
    {
        Red,
        Green,
        Blue
    };

    std::string to_string(ReflectTestColor color)
    {
        switch (color)
        {
        case ReflectTestColor::Red: return "Red";
        case ReflectTestColor::Green: return "Green";
        case ReflectTestColor::Blue: return "Blue";
        }
        return "Unknown";
    }

    // One of every field shape LOSSYLAB_REFLECT's field-to-JSON dispatch
    // handles: a nested Writable type, an enum, optional, vector, map, and a
    // plain scalar.
    struct ReflectTestInner
    {
        std::string label;
        int id = 0;

        [[nodiscard]] lossylab::json::Value to_json() const
        {
            return lossylab::json::object({{"label", label}, {"id", id}});
        }
    };

    struct ReflectTestSample
    {
        std::string name;
        int count = 0;
        std::optional<double> ratio;
        std::vector<ReflectTestInner> inner;
        std::vector<int> values;
        std::map<std::string, std::string> tags;
        ReflectTestColor color = ReflectTestColor::Red;
    };
}

// Explicit specializations of lossylab::reflect::Fields must be declared in a
// namespace enclosing lossylab::reflect, so this cannot be inside the
// anonymous namespace above (a sibling of lossylab::reflect, not an
// ancestor of it) — the same rule that keeps VideoFramesResult unreflected
// in python/src/bind_file_result.cpp.
LOSSYLAB_REFLECT(ReflectTestSample, name, count, ratio, inner, values, tags, color);

namespace
{
    struct FortyEightFields
    {
        int f1 = 0, f2 = 0, f3 = 0, f4 = 0, f5 = 0, f6 = 0, f7 = 0, f8 = 0, f9 = 0, f10 = 0, f11 = 0, f12 = 0,
            f13 = 0, f14 = 0, f15 = 0, f16 = 0, f17 = 0, f18 = 0, f19 = 0, f20 = 0, f21 = 0, f22 = 0, f23 = 0,
            f24 = 0, f25 = 0, f26 = 0, f27 = 0, f28 = 0, f29 = 0, f30 = 0, f31 = 0, f32 = 0, f33 = 0, f34 = 0,
            f35 = 0, f36 = 0, f37 = 0, f38 = 0, f39 = 0, f40 = 0, f41 = 0, f42 = 0, f43 = 0, f44 = 0, f45 = 0,
            f46 = 0, f47 = 0, f48 = 0;
    };

    using lossylab::reflect::detail::count_aggregate_fields;

    static_assert(count_aggregate_fields<Empty>() == 0);
    static_assert(count_aggregate_fields<OneField>() == 1);
    static_assert(count_aggregate_fields<ReflectTestSample>() == 7);
    static_assert(count_aggregate_fields<FortyEightFields>() == 48);

    // The count LOSSYLAB_REFLECT(ReflectTestSample, ...) checked itself
    // against, pinned down again here so a change to the macro's counting
    // logic that agrees with itself but is wrong is still caught.
    static_assert(std::tuple_size_v<decltype(lossylab::reflect::Fields<ReflectTestSample>::members)> == 7);

    // Two real, already-migrated structs: HeaderInfo (under the old 32-field
    // ceiling) and StreamInfo (33 fields, needs the extended macro chain).
    static_assert(count_aggregate_fields<lossylab::HeaderInfo>() == 8);
    static_assert(count_aggregate_fields<lossylab::StreamInfo>() == 33);

    void test_reflect_to_json_covers_every_field_shape()
    {
        ReflectTestSample sample;
        sample.name = "widget";
        sample.count = 3;
        sample.ratio = 0.5;
        sample.inner = {ReflectTestInner{"a", 1}, ReflectTestInner{"b", 2}};
        sample.values = {1, 2, 3};
        sample.tags = {{"k", "v"}};
        sample.color = ReflectTestColor::Green;

        const lossylab::json::Value value = lossylab::reflect::to_json(sample);

        assert(value.at("name") == "widget");
        assert(value.at("count") == 3);
        assert(value.at("ratio") == 0.5);
        assert(value.at("inner").size() == 2);
        assert(value.at("inner")[0].at("label") == "a");
        assert(value.at("inner")[1].at("id") == 2);
        assert(value.at("values") == lossylab::json::array({1, 2, 3}));
        assert(value.at("tags").at("k") == "v");
        assert(value.at("color") == "Green");
    }

    void test_reflect_to_json_writes_null_for_an_empty_optional()
    {
        const ReflectTestSample sample;
        const lossylab::json::Value value = lossylab::reflect::to_json(sample);
        assert(value.at("ratio").is_null());
    }
}

int main()
{
    test_reflect_to_json_covers_every_field_shape();
    test_reflect_to_json_writes_null_for_an_empty_optional();
}
