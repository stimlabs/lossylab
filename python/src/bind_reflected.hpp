#pragma once

#include "bindings.hpp"
#include "json_convert.hpp"

#include "lossylab/core/reflect.hpp"

namespace lossylab::pybind
{
    namespace detail
    {
        /// json::Value has no nanobind type caster, so a reflected field of
        /// that type needs to convert through to_python instead of the plain
        /// def_ro every other field gets.
        template <typename T, typename M>
        void bind_reflected_field(nb::class_<T>& cls, const reflect::Member<T, M>& field)
        {
            if constexpr (std::is_same_v<M, json::Value>)
            {
                M T::*pointer = field.pointer;
                cls.def_prop_ro(field.name.data(), [pointer](const T& self) { return to_python(self.*pointer); });
            }
            else
            {
                cls.def_ro(field.name.data(), field.pointer);
            }
        }

        /// As above, but read-write: for the LOSSYLAB_REFLECT'd *Options
        /// structs a caller constructs and then sets fields on from Python.
        template <typename T, typename M>
        void bind_reflected_field_rw(nb::class_<T>& cls, const reflect::Member<T, M>& field)
        {
            if constexpr (std::is_same_v<M, json::Value>)
            {
                M T::*pointer = field.pointer;
                cls.def_prop_rw(
                    field.name.data(), [pointer](const T& self) { return to_python(self.*pointer); },
                    [pointer](T& self, nb::handle value) { self.*pointer = to_json(value); });
            }
            else
            {
                cls.def_rw(field.name.data(), field.pointer);
            }
        }
    }

    /// Binds every field in T's LOSSYLAB_REFLECT list as a read-only Python
    /// attribute of a new class. Struct changes only need to touch
    /// LOSSYLAB_REFLECT; this stays generic across every reflected type. When
    /// T has a to_json(), to_dict() is added automatically; a caller whose
    /// to_json() adds extra keys (e.g. schema_version) can still bind its own
    /// to_dict afterward, overriding this one.
    ///
    /// `scope` is a module for a top-level type, or an outer nb::class_ for a
    /// type nested inside another bound class (e.g. IccProfileInfo::Colorants
    /// under IccProfileInfo), since nanobind's own class_ constructor accepts
    /// either as its scope.
    template <typename T>
    nb::class_<T> bind_reflected(nb::handle scope, const char* name)
    {
        nb::class_<T> cls(scope, name);
        std::apply([&](const auto&... fields) { (detail::bind_reflected_field(cls, fields), ...); },
                   reflect::Fields<T>::members);
        if constexpr (json::Writable<T>)
        {
            cls.def("to_dict", [](const T& self) { return to_python(self.to_json()); });
        }
        return cls;
    }

    /// As above, but for a default-constructible struct a caller builds with
    /// no arguments and then sets fields on, such as an *Options struct: adds
    /// nb::init<>() and binds every reflected field read-write (def_rw)
    /// instead of read-only.
    template <typename T>
    nb::class_<T> bind_reflected_rw(nb::module_& m, const char* name)
    {
        static_assert(std::is_default_constructible_v<T>,
                      "bind_reflected_rw: T must be default-constructible");
        nb::class_<T> cls(m, name);
        cls.def(nb::init<>());
        std::apply([&](const auto&... fields) { (detail::bind_reflected_field_rw(cls, fields), ...); },
                   reflect::Fields<T>::members);
        if constexpr (json::Writable<T>)
        {
            cls.def("to_dict", [](const T& self) { return to_python(self.to_json()); });
        }
        return cls;
    }
}
