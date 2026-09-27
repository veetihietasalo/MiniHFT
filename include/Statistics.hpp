#pragma once

#include <vector>
#include <numeric>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <stdexcept>

// Floating-point data only: with integers, means and variances would use integer division.
namespace Statistics {

    template <std::floating_point T>
    T mean(const std::vector<T>& data) {
        if (data.empty()) return T{};
        T sum = std::accumulate(data.begin(), data.end(), T{});
        return sum / static_cast<T>(data.size());
    }

    template <std::floating_point T>
    T variance(const std::vector<T>& data) {
        if (data.size() < 2) return T{};
        T m = mean(data);
        T accum{};
        for (const auto& d : data) {
            accum += (d - m) * (d - m);
        }
        return accum / static_cast<T>(data.size() - 1); // Sample variance
    }

    template <std::floating_point T>
    T stdDev(const std::vector<T>& data) {
        return std::sqrt(variance(data));
    }

    template <std::floating_point T>
    T covariance(const std::vector<T>& x, const std::vector<T>& y) {
        if (x.size() != y.size() || x.size() < 2) {
            throw std::invalid_argument("Vectors must be same size and > 1 for covariance");
        }
        T meanX = mean(x);
        T meanY = mean(y);
        T accum{};
        for (size_t i = 0; i < x.size(); ++i) {
            accum += (x[i] - meanX) * (y[i] - meanY);
        }
        return accum / static_cast<T>(x.size() - 1);
    }

    // Moving Average
    template <std::floating_point T>
    std::vector<T> movingAverage(const std::vector<T>& data, size_t windowSize) {
        if (windowSize == 0 || windowSize > data.size()) return {};
        const T window = static_cast<T>(windowSize);
        std::vector<T> result;
        T sum{};
        for (size_t i = 0; i < windowSize; ++i) sum += data[i];
        result.push_back(sum / window);

        for (size_t i = windowSize; i < data.size(); ++i) {
            sum += data[i] - data[i - windowSize];
            result.push_back(sum / window);
        }
        return result;
    }
}
