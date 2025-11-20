#pragma once

#include <vector>
#include <iostream>
#include <stdexcept>
#include <iomanip>

template <typename T>
class Matrix {
private:
    std::vector<T> data;
    size_t rows;
    size_t cols;

public:
    // Constructors
    Matrix(size_t r, size_t c, T initialValue = T{}) : rows(r), cols(c), data(r * c, initialValue) {}

    // Accessors
    T& operator()(size_t r, size_t c) {
        if (r >= rows || c >= cols) throw std::out_of_range("Matrix index out of bounds");
        return data[r * cols + c];
    }

    const T& operator()(size_t r, size_t c) const {
        if (r >= rows || c >= cols) throw std::out_of_range("Matrix index out of bounds");
        return data[r * cols + c];
    }

    size_t getRows() const { return rows; }
    size_t getCols() const { return cols; }

    // Operations
    Matrix<T> operator+(const Matrix<T>& other) const {
        if (rows != other.rows || cols != other.cols) {
            throw std::invalid_argument("Matrix dimensions must match for addition");
        }
        Matrix<T> result(rows, cols);
        for (size_t i = 0; i < data.size(); ++i) {
            result.data[i] = data[i] + other.data[i];
        }
        return result;
    }

    Matrix<T> operator-(const Matrix<T>& other) const {
        if (rows != other.rows || cols != other.cols) {
            throw std::invalid_argument("Matrix dimensions must match for subtraction");
        }
        Matrix<T> result(rows, cols);
        for (size_t i = 0; i < data.size(); ++i) {
            result.data[i] = data[i] - other.data[i];
        }
        return result;
    }

    Matrix<T> operator*(const Matrix<T>& other) const {
        if (cols != other.rows) {
            throw std::invalid_argument("Matrix dimensions invalid for multiplication");
        }
        Matrix<T> result(rows, other.cols);
        for (size_t i = 0; i < rows; ++i) {
            for (size_t j = 0; j < other.cols; ++j) {
                T sum = 0;
                for (size_t k = 0; k < cols; ++k) {
                    sum += (*this)(i, k) * other(k, j);
                }
                result(i, j) = sum;
            }
        }
        return result;
    }

    Matrix<T> transpose() const {
        Matrix<T> result(cols, rows);
        for (size_t i = 0; i < rows; ++i) {
            for (size_t j = 0; j < cols; ++j) {
                result(j, i) = (*this)(i, j);
            }
        }
        return result;
    }

    // Print utility
    void print() const {
        for (size_t i = 0; i < rows; ++i) {
            for (size_t j = 0; j < cols; ++j) {
                std::cout << std::setw(10) << (*this)(i, j) << " ";
            }
            std::cout << "\n";
        }
    }
};
