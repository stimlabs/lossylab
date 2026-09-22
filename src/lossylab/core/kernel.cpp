#include "lossylab/core/kernel.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/core/json_io.hpp"

namespace lossylab
{
    std::string to_string(const Kernel kernel)
    {
        switch (kernel)
        {
        case Kernel::Nearest: return "nearest";
        case Kernel::Bilinear: return "bilinear";
        case Kernel::Bicubic: return "bicubic";
        case Kernel::Lanczos: return "lanczos";
        case Kernel::Area: return "area";
        case Kernel::Gaussian: return "gaussian";
        case Kernel::Sinc: return "sinc";
        case Kernel::Spline: return "spline";
        case Kernel::Bicublin: return "bicublin";
        }
        return "unknown";
    }

    Kernel kernel_from_string(const std::string_view name)
    {
        if (name == "nearest" || name == "point") { return Kernel::Nearest; }
        if (name == "bilinear" || name == "linear") { return Kernel::Bilinear; }
        if (name == "bicubic" || name == "cubic") { return Kernel::Bicubic; }
        if (name == "lanczos") { return Kernel::Lanczos; }
        if (name == "area" || name == "box") { return Kernel::Area; }
        if (name == "gaussian" || name == "gauss") { return Kernel::Gaussian; }
        if (name == "sinc") { return Kernel::Sinc; }
        if (name == "spline") { return Kernel::Spline; }
        if (name == "bicublin") { return Kernel::Bicublin; }
        throw ConfigError("unknown kernel '" + std::string(name) + "'");
    }

    std::vector<Kernel> all_kernels()
    {
        return {Kernel::Nearest, Kernel::Bilinear, Kernel::Bicubic, Kernel::Lanczos,
                Kernel::Area,    Kernel::Gaussian, Kernel::Sinc,    Kernel::Spline,
                Kernel::Bicublin};
    }

    json::Value KernelParams::to_json() const
    {
        return json::object({
            {"param_a", json::optional_or_null(param_a)},
            {"param_b", json::optional_or_null(param_b)},
        });
    }

    KernelParams KernelParams::from_json(const json::Value& value)
    {
        KernelParams params;
        params.param_a = json::optional_double(json::member(value, "param_a"));
        params.param_b = json::optional_double(json::member(value, "param_b"));
        return params;
    }

    bool operator==(const KernelParams& left, const KernelParams& right) noexcept
    {
        return left.param_a == right.param_a && left.param_b == right.param_b;
    }

    std::string KernelSpec::describe() const
    {
        std::string text = to_string(kernel);
        if (params.param_a.has_value())
        {
            text += '(' + std::to_string(*params.param_a);
            if (params.param_b.has_value())
            {
                text += ", " + std::to_string(*params.param_b);
            }
            text += ')';
        }
        return text;
    }

    json::Value KernelSpec::to_json() const
    {
        return json::object({
            {"kernel", to_string(kernel)},
            {"params", params.to_json()},
        });
    }

    KernelSpec KernelSpec::from_json(const json::Value& value)
    {
        KernelSpec spec;
        spec.kernel = kernel_from_string(value.at("kernel").get<std::string>());
        spec.params = KernelParams::from_json(json::member(value, "params"));
        return spec;
    }
}
