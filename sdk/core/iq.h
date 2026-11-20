#pragma once

#include <complex>
#include <cstddef>
#include <functional>

namespace skim {
using Samples = std::function<void(const std::complex<float>*, std::size_t)>;
using IqStream =
    std::function<void(const Samples&, const std::function<void()>& gap)>;
}  // namespace skim
