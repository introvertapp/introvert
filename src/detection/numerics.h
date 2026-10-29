#pragma once

#include <complex>
#include <memory>
#include <vector>

namespace introvert::detection {

// Compensated scalar float reduction. Array reductions and fixed-unit
// integer sums deliberately use other paths.
class ScalarSum final {
public:
    void add(double value);
    double value() const;
private:
    double m_total = 0, m_correction = 0;
};

// Unnormalized forward real FFT. A plan owns its immutable decomposition;
// execution scratch belongs to the caller, so concurrent callers do not share it.
class RealFft final {
public:
    explicit RealFft(size_t size);
    ~RealFft();
    void forward(std::vector<double> &scratch, std::vector<std::complex<double>> &output) const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace introvert::detection
