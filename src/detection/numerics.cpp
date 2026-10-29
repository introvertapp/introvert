#include "numerics.h"
#define POCKETFFT_NO_MULTITHREADING
#include "pocketfft_hdronly.h"
#include <cmath>
#include <stdexcept>

namespace introvert::detection {
void ScalarSum::add(double value)
{
    const double next = m_total + value;
    m_correction += std::abs(m_total) >= std::abs(value) ? (m_total - next) + value : (value - next) + m_total;
    m_total = next;
}
double ScalarSum::value() const { return m_correction != 0 && std::isfinite(m_correction) ? m_total + m_correction : m_total; }
struct RealFft::Impl {
    size_t size;
    pocketfft::detail::pocketfft_r<double> plan;
    explicit Impl(size_t length) : size(length), plan(length) {}
};
RealFft::RealFft(size_t size)
{
    if (!size) throw std::invalid_argument("FFT length must be positive");
    m_impl = std::make_unique<Impl>(size);
}
RealFft::~RealFft() = default;
void RealFft::forward(std::vector<double> &scratch, std::vector<std::complex<double>> &output) const
{
    const auto n = m_impl->size;
    if (scratch.size() != n) throw std::invalid_argument("FFT input length differs from its plan");
    m_impl->plan.exec(scratch.data(), 1., true);
    output.resize(n / 2 + 1); output[0] = {scratch[0], 0};
    for (size_t i = 1; i <= (n - 1) / 2; ++i) output[i] = {scratch[i * 2 - 1], scratch[i * 2]};
    if (!(n & 1)) output[n / 2] = {scratch[n - 1], 0};
}
} // namespace introvert::detection
