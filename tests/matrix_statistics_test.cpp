#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

#include "Matrix.hpp"
#include "Statistics.hpp"

// Explicit instantiation compiles every member, print() included, so the strict warnings see
// all of Matrix's code and not only what the tests call.
template class Matrix<int>;
template class Matrix<double>;

namespace {

// One initializer list per row, e.g. makeMatrix<int>({{1, 2}, {3, 4}}).
template <typename T>
Matrix<T> makeMatrix(std::initializer_list<std::initializer_list<T>> rows) {
    Matrix<T> m(rows.size(), rows.begin()->size());
    size_t r = 0;
    for (const auto& row : rows) {
        size_t c = 0;
        for (const T& value : row) m(r, c++) = value;
        ++r;
    }
    return m;
}

// Exact comparison: the double cases use values that binary floating point represents exactly.
template <typename T>
void expectMatrix(const Matrix<T>& m, std::initializer_list<std::initializer_list<T>> expected) {
    ASSERT_EQ(m.getRows(), expected.size());
    size_t r = 0;
    for (const auto& row : expected) {
        ASSERT_EQ(m.getCols(), row.size()) << "row " << r;
        size_t c = 0;
        for (const T& value : row) {
            EXPECT_EQ(m(r, c), value) << "at (" << r << ", " << c << ")";
            ++c;
        }
        ++r;
    }
}

// Integer data would be averaged with integer division (the mean of {1, 2} would be 1), so the
// statistics only accept floating-point types.
template <typename T>
constexpr bool kHasMean = requires(const std::vector<T>& v) { Statistics::mean(v); };
template <typename T>
constexpr bool kHasMovingAverage = requires(const std::vector<T>& v) { Statistics::movingAverage(v, 2); };

} // namespace

static_assert(kHasMean<double> && kHasMean<float>);
static_assert(!kHasMean<int> && !kHasMean<long long>);
static_assert(kHasMovingAverage<double> && !kHasMovingAverage<long long>);

TEST(Matrix, StartsFilledWithTheInitialValue) {
    expectMatrix(Matrix<int>(2, 3, 7), {{7, 7, 7}, {7, 7, 7}});
    expectMatrix(Matrix<double>(1, 2), {{0.0, 0.0}});
}

TEST(Matrix, AddsAndSubtractsElementWise) {
    const auto a = makeMatrix<int>({{1, 2, 3}, {4, 5, 6}});
    const auto b = makeMatrix<int>({{10, 20, 30}, {40, 50, 60}});
    expectMatrix(a + b, {{11, 22, 33}, {44, 55, 66}});
    expectMatrix(b - a, {{9, 18, 27}, {36, 45, 54}});
    expectMatrix(a - b, {{-9, -18, -27}, {-36, -45, -54}});
}

TEST(Matrix, MultipliesRowsByColumns) {
    const auto a = makeMatrix<int>({{1, 2, 3}, {4, 5, 6}});
    const auto b = makeMatrix<int>({{7, 8}, {9, 10}, {11, 12}});
    expectMatrix(a * b, {{58, 64}, {139, 154}});
    expectMatrix(b * a, {{39, 54, 69}, {49, 68, 87}, {59, 82, 105}});

    const auto identity = makeMatrix<int>({{1, 0, 0}, {0, 1, 0}, {0, 0, 1}});
    expectMatrix(a * identity, {{1, 2, 3}, {4, 5, 6}});

    const auto m = makeMatrix<double>({{1.5, -2.0}, {0.25, 4.0}});
    const auto v = makeMatrix<double>({{2.0}, {0.5}});
    expectMatrix(m * v, {{2.0}, {2.5}});
}

TEST(Matrix, TransposeSwapsRowsAndColumns) {
    const auto a = makeMatrix<int>({{1, 2, 3}, {4, 5, 6}});
    expectMatrix(a.transpose(), {{1, 4}, {2, 5}, {3, 6}});
    expectMatrix(a.transpose().transpose(), {{1, 2, 3}, {4, 5, 6}});
    expectMatrix(makeMatrix<double>({{0.5, 1.5}}).transpose(), {{0.5}, {1.5}});
}

TEST(Matrix, MismatchedDimensionsThrow) {
    const Matrix<int> a(2, 3);
    const Matrix<int> b(3, 2);
    EXPECT_THROW(static_cast<void>(a + b), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(a - b), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(a * a), std::invalid_argument); // 3 columns times 2 rows
    EXPECT_NO_THROW(static_cast<void>(a * b));
}

TEST(Matrix, IndexOutsideTheMatrixThrows) {
    Matrix<int> m(2, 3);
    EXPECT_THROW(static_cast<void>(m(2, 0)), std::out_of_range);
    EXPECT_THROW(static_cast<void>(m(0, 3)), std::out_of_range);
    const Matrix<int>& constView = m;
    EXPECT_THROW(static_cast<void>(constView(2, 0)), std::out_of_range);
    m(1, 2) = 5;
    EXPECT_EQ(constView(1, 2), 5);
}

// rows * cols used to wrap around: a 2^63 x 2 matrix allocated no storage at all, yet element
// (0, 0) passed the bounds check and was read through a null pointer.
TEST(Matrix, DimensionsWhoseProductOverflowsAreRefused) {
    constexpr size_t kHalf = size_t{1} << (std::numeric_limits<size_t>::digits - 1);
    EXPECT_THROW(static_cast<void>(Matrix<int>(kHalf, 2)), std::length_error);
    EXPECT_THROW(static_cast<void>(Matrix<int>(2, kHalf)), std::length_error);
}

// The textbook sample: mean 5 and squared deviations summing to 32, so the sample variance
// (divided by n - 1 = 7) is 32/7.
TEST(Statistics, MeanVarianceAndStdDevOfKnownValues) {
    const std::vector<double> sample{2, 4, 4, 4, 5, 5, 7, 9};
    EXPECT_DOUBLE_EQ(Statistics::mean(sample), 5.0);
    EXPECT_DOUBLE_EQ(Statistics::variance(sample), 32.0 / 7.0);
    EXPECT_DOUBLE_EQ(Statistics::stdDev(sample), std::sqrt(32.0 / 7.0));
}

TEST(Statistics, CovarianceOfKnownValues) {
    const std::vector<double> x{1, 2, 3, 4, 5};
    const std::vector<double> rising{2, 4, 6, 8, 10};
    const std::vector<double> falling{5, 4, 3, 2, 1};
    EXPECT_DOUBLE_EQ(Statistics::covariance(x, rising), 5.0);   // 20 / 4
    EXPECT_DOUBLE_EQ(Statistics::covariance(x, falling), -2.5); // -10 / 4
    EXPECT_DOUBLE_EQ(Statistics::covariance(x, x), Statistics::variance(x));
}

TEST(Statistics, CovarianceNeedsTwoSeriesOfEqualLengthAtLeastTwo) {
    const std::vector<double> three{1, 2, 3};
    const std::vector<double> two{1, 2};
    const std::vector<double> one{1};
    EXPECT_THROW(static_cast<void>(Statistics::covariance(three, two)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(Statistics::covariance(one, one)), std::invalid_argument);
}

// The current contract: too few values give 0 rather than an error (covariance throws instead).
TEST(Statistics, MeanOfNothingAndVarianceOfOneValueAreZero) {
    EXPECT_EQ(Statistics::mean(std::vector<double>{}), 0.0);
    EXPECT_EQ(Statistics::variance(std::vector<double>{42.0}), 0.0);
    EXPECT_EQ(Statistics::stdDev(std::vector<double>{42.0}), 0.0);
}

TEST(Statistics, MovingAverageOfKnownValues) {
    const std::vector<double> prices{1, 2, 3, 4, 5, 6};
    EXPECT_EQ(Statistics::movingAverage(prices, 3), (std::vector<double>{2, 3, 4, 5}));
    EXPECT_EQ(Statistics::movingAverage(prices, 6), std::vector<double>{3.5});
    EXPECT_EQ(Statistics::movingAverage(prices, 1), prices);
    EXPECT_TRUE(Statistics::movingAverage(prices, 0).empty());
    EXPECT_TRUE(Statistics::movingAverage(prices, 7).empty());
}

// The window's sum is updated incrementally, one value in and one out; each average must
// still match the mean of its window computed from scratch.
TEST(Statistics, MovingAverageMatchesTheMeanOfEachWindow) {
    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> price(99.0, 101.0);
    std::vector<double> prices(1000);
    for (double& p : prices) p = price(rng);

    constexpr size_t kWindow = 20;
    const std::vector<double> averages = Statistics::movingAverage(prices, kWindow);
    ASSERT_EQ(averages.size(), prices.size() - kWindow + 1);
    for (size_t first = 0; first < averages.size(); ++first) {
        const std::vector<double> window(prices.begin() + static_cast<std::ptrdiff_t>(first),
                                         prices.begin() + static_cast<std::ptrdiff_t>(first + kWindow));
        EXPECT_NEAR(averages[first], Statistics::mean(window), 1e-9) << "window at " << first;
    }
}

// float must compile under the same warnings: no silent promotions to double.
TEST(Statistics, WorksForFloat) {
    const std::vector<float> v{1.0f, 2.0f, 3.0f, 4.0f};
    EXPECT_FLOAT_EQ(Statistics::mean(v), 2.5f);
    EXPECT_FLOAT_EQ(Statistics::variance(v), 5.0f / 3.0f);
    EXPECT_FLOAT_EQ(Statistics::stdDev(v), std::sqrt(5.0f / 3.0f));
    EXPECT_FLOAT_EQ(Statistics::covariance(v, v), 5.0f / 3.0f);
    EXPECT_EQ(Statistics::movingAverage(v, 2), (std::vector<float>{1.5f, 2.5f, 3.5f}));
}
