#pragma once

#include <nanobind/nanobind.h>

namespace lossylab::pybind
{
    namespace nb = nanobind;

    void bind_errors(nb::module_& m);
    void bind_core_types(nb::module_& m);
    void bind_record(nb::module_& m);
    void bind_frame(nb::module_& m);
    void bind_env(nb::module_& m);
    void bind_io(nb::module_& m);
    void bind_file_result(nb::module_& m);
    void bind_convert(nb::module_& m);
}
