#include "bindings.hpp"

#include "lossylab/core/error.hpp"

#include <nanobind/stl/string.h>

#include <exception>
#include <utility>
#include <vector>

namespace lossylab::pybind
{
    namespace
    {
        /// Constructs an instance of a Python exception type carrying extra
        /// attributes beyond the message, and raises it. Plain nb::exception<T>
        /// only forwards what(), which loses e.g. UnsupportedCapability's
        /// kind()/name()/identity_hash() — those fields are the point of catching
        /// the specific type rather than the base Error.
        template <typename ExceptionT>
        void raise_with_fields(const nb::handle& type, const ExceptionT& error,
                                const std::vector<std::pair<const char*, std::string>>& fields)
        {
            nb::object instance = nb::borrow<nb::object>(type)(error.what());
            for (const auto& [name, value] : fields)
            {
                nb::setattr(instance, name, nb::cast(value));
            }
            PyErr_SetObject(type.ptr(), instance.ptr());
        }
    }

    void bind_errors(nb::module_& m)
    {
        // Registration order matters: nanobind tries the most-recently
        // registered translator first, and catch(const Error&) also matches
        // every derived type by reference. Error must be registered first so
        // the more specific translators below (registered after) are checked
        // before it ever gets a chance to swallow a derived exception as a
        // plain Error.
        auto error_exc = nb::exception<lossylab::Error>(m, "Error");

        nb::exception<lossylab::ConfigError>(m, "ConfigError", error_exc);

        static nb::handle capability_type =
            nb::exception<lossylab::UnsupportedCapability>(m, "UnsupportedCapability", error_exc);
        nb::register_exception_translator(
            [](const std::exception_ptr& active, void*)
            {
                if (!active)
                {
                    return;
                }
                try
                {
                    std::rethrow_exception(active);
                }
                catch (const lossylab::UnsupportedCapability& e)
                {
                    raise_with_fields(capability_type, e,
                                       {{"kind", e.kind()}, {"name", e.name()}, {"identity_hash", e.identity_hash()}});
                }
            },
            nullptr);

        static nb::handle conversion_refused_type =
            nb::exception<lossylab::ConversionRefused>(m, "ConversionRefused", error_exc);
        nb::register_exception_translator(
            [](const std::exception_ptr& active, void*)
            {
                if (!active)
                {
                    return;
                }
                try
                {
                    std::rethrow_exception(active);
                }
                catch (const lossylab::ConversionRefused& e)
                {
                    raise_with_fields(conversion_refused_type, e,
                                       {{"from_", e.from()}, {"to", e.to()}, {"context", e.context()}});
                }
            },
            nullptr);

        static nb::handle ffmpeg_error_type =
            nb::exception<lossylab::FFmpegError>(m, "FFmpegError", error_exc);
        nb::register_exception_translator(
            [](const std::exception_ptr& active, void*)
            {
                if (!active)
                {
                    return;
                }
                try
                {
                    std::rethrow_exception(active);
                }
                catch (const lossylab::FFmpegError& e)
                {
                    nb::object instance = nb::borrow<nb::object>(ffmpeg_error_type)(e.what());
                    nb::setattr(instance, "averror", nb::cast(e.averror()));
                    nb::setattr(instance, "call", nb::cast(e.call()));
                    PyErr_SetObject(ffmpeg_error_type.ptr(), instance.ptr());
                }
            },
            nullptr);

        static nb::handle not_implemented_type =
            nb::exception<lossylab::NotImplemented>(m, "NotImplemented", error_exc);
        nb::register_exception_translator(
            [](const std::exception_ptr& active, void*)
            {
                if (!active)
                {
                    return;
                }
                try
                {
                    std::rethrow_exception(active);
                }
                catch (const lossylab::NotImplemented& e)
                {
                    raise_with_fields(not_implemented_type, e, {{"symbol", e.symbol()}});
                }
            },
            nullptr);
    }
}
