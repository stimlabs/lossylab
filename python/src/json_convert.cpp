#include "json_convert.hpp"

#include <nanobind/stl/string.h>

namespace lossylab::pybind
{
    nb::object to_python(const json::Value& value)
    {
        switch (value.type())
        {
            case json::Value::value_t::null:
                return nb::none();
            case json::Value::value_t::boolean:
                return nb::bool_(value.get<bool>());
            case json::Value::value_t::number_integer:
                return nb::int_(value.get<std::int64_t>());
            case json::Value::value_t::number_unsigned:
                return nb::int_(value.get<std::uint64_t>());
            case json::Value::value_t::number_float:
                return nb::float_(value.get<double>());
            case json::Value::value_t::string:
                return nb::cast(value.get<std::string>());
            case json::Value::value_t::array:
            {
                nb::list result;
                for (const auto& item : value)
                {
                    result.append(to_python(item));
                }
                return result;
            }
            case json::Value::value_t::object:
            {
                nb::dict result;
                for (auto it = value.begin(); it != value.end(); ++it)
                {
                    result[nb::cast(it.key())] = to_python(it.value());
                }
                return result;
            }
            case json::Value::value_t::binary:
            case json::Value::value_t::discarded:
                return nb::none();
        }
        return nb::none();
    }

    json::Value to_json(nb::handle value)
    {
        if (value.is_none())
        {
            return json::Value(nullptr);
        }
        if (nb::isinstance<nb::bool_>(value))
        {
            return json::Value(nb::cast<bool>(value));
        }
        if (nb::isinstance<nb::int_>(value))
        {
            return json::Value(nb::cast<std::int64_t>(value));
        }
        if (nb::isinstance<nb::float_>(value))
        {
            return json::Value(nb::cast<double>(value));
        }
        if (nb::isinstance<nb::str>(value))
        {
            return json::Value(nb::cast<std::string>(value));
        }
        if (nb::isinstance<nb::list>(value) || nb::isinstance<nb::tuple>(value))
        {
            json::Value result = json::Value::array();
            for (auto item : nb::borrow<nb::iterable>(value))
            {
                result.push_back(to_json(item));
            }
            return result;
        }
        if (nb::isinstance<nb::dict>(value))
        {
            json::Value result = json::Value::object();
            for (auto [key, item] : nb::cast<nb::dict>(value))
            {
                result[nb::cast<std::string>(key)] = to_json(item);
            }
            return result;
        }
        throw std::invalid_argument("lossylab: cannot convert this Python value to JSON");
    }
}
