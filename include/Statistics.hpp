#pragma once

#include <vector>
#include <numeric>
#include <cmath>
#include <stdexcept>
#include "Matrix.hpp"

namespace Statistics {

    template <typename T>
    T mean(const std::vector<T>& data) {
        if (data.empty()) return 0.0;
        T sum = std::accumulate(data.begin(), data.end(), T{});
        return sum / static_cast<T>(data.size());
    }

    template <typename T>
    T variance(const std::vector<T>& data) {
        if (data.size() < 2) return 0.0;
        T m = mean(data);
        T accum = 0.0;
        for (const auto& d : data) {
            accum += (d - m) * (d - m);
        }
        return accum / static_cast<T>(data.size() - 1); // Sample variance
    }

    template <typename T>
    T stdDev(const std::vector<T>& data) {
        return std::sqrt(variance(data));
    }

    template <typename T>
    T covariance(const std::vector<T>& x, const std::vector<T>& y) {
        if (x.size() != y.size() || x.size() < 2) {
            throw std::invalid_argument("Vectors must be same size and > 1 for covariance");
        }
        T meanX = mean(x);
        T meanY = mean(y);
        T accum = 0.0;
        for (size_t i = 0; i < x.size(); ++i) {
            accum += (x[i] - meanX) * (y[i] - meanY);
        }
        return accum / static_cast<T>(x.size() - 1);
    }

    // Moving Average
    template <typename T>
    std::vector<T> movingAverage(const std::vector<T>& data, size_t windowSize) {
        if (windowSize == 0 || windowSize > data.size()) return {};
        std::vector<T> result;
        T sum = 0;
        for (size_t i = 0; i < windowSize; ++i) sum += data[i];
        result.push_back(sum / windowSize);

        for (size_t i = windowSize; i < data.size(); ++i) {
            sum += data[i] - data[i - windowSize];
            result.push_back(sum / windowSize);
        }
        return result;
    }
}
