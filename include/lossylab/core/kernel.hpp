#pragma once

#include "lossylab/core/json.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lossylab
{
    /// A resampling kernel.
    ///
    /// Named explicitly everywhere rather than defaulted, because kernel choice
    /// is one of the strongest traces a resize leaves: a bicubic downscale and
    /// a Lanczos downscale of the same image differ in ways a detector learns
    /// long before it learns anything about content.
    enum class Kernel
    {
        Nearest,
        Bilinear,
        Bicubic,
        Lanczos,
        Area,      ///< Box average; what most "thumbnail" paths use
        Gaussian,
        Sinc,
        Spline,
        Bicublin   ///< swscale's bicubic luma with bilinear chroma
    };

    std::string to_string(Kernel kernel);
    Kernel kernel_from_string(std::string_view name);

    [[nodiscard]] std::vector<Kernel> all_kernels();

    /// Kernel shape parameters.
    ///
    /// `param_a` and `param_b` are the kernel's own coefficients: B and C for
    /// bicubic (Mitchell is 1/3, 1/3; Catmull-Rom is 0, 1/2), the tap count for
    /// Lanczos, sigma for Gaussian. Left unset, the backend's default applies
    /// and the resolved value is written to the record, so a replay does not
    /// depend on that default staying the same.
    struct KernelParams
    {
        std::optional<double> param_a;
        std::optional<double> param_b;

        [[nodiscard]] json::Value to_json() const;
        static KernelParams from_json(const json::Value& value);
    };

    bool operator==(const KernelParams& left, const KernelParams& right) noexcept;
    inline bool operator!=(const KernelParams& left, const KernelParams& right) noexcept
    {
        return !(left == right);
    }

    /// A kernel together with its parameters.
    struct KernelSpec
    {
        Kernel kernel = Kernel::Bicubic;
        KernelParams params;

        [[nodiscard]] std::string describe() const;
        [[nodiscard]] json::Value to_json() const;
        static KernelSpec from_json(const json::Value& value);
    };
}
