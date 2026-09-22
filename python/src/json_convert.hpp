#pragma once

#include "lossylab/core/json.hpp"

#include <nanobind/nanobind.h>

namespace lossylab::pybind
{
    namespace nb = nanobind;

    /// Converts a lossylab::json::Value into the equivalent Python object
    /// (None / bool / int / float / str / list / dict), recursively.
    [[nodiscard]] nb::object to_python(const json::Value& value);

    /// Converts a Python object into a lossylab::json::Value, recursively.
    /// Mirrors to_python's mapping in reverse.
    [[nodiscard]] json::Value to_json(nb::handle value);
}
