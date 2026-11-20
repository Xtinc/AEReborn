#ifndef AEREBORN_PLUGINS_MATRIX_H
#define AEREBORN_PLUGINS_MATRIX_H
#include "core/graph.h"
#include <atomic>
#include <initializer_list>
#include <vector>
namespace aereborn
{
// Rows select output channels; columns select input channels. F32P only.
class Matrix final : public NodeT<1, 1>
{
  public:
    Matrix(SampleRate rate, std::size_t channels, float gain = 1.0f);
    Matrix(SampleRate rate, std::size_t input_channels, std::size_t output_channels, View<const float> coefficients,
           float gain = 1.0f);
    Matrix(SampleRate rate, std::size_t input_channels, std::size_t output_channels,
           std::initializer_list<float> coefficients, float gain = 1.0f);
    // Thread-safe with process().
    RetCode set_gain(float gain) noexcept;
    /// @brief Read the requested gain.
    /// @return The most recently set gain target.
    /// @note Safe to call concurrently with set_gain() and process().
    float gain() const noexcept;
    RetCode prepare(BlockPeriod period) override;
    void process(const ProcessContext &context) noexcept override;
    void reset() noexcept override;

  private:
    std::vector<float> coefficients_;
    std::atomic<float> target_;
    float gain_;
};
} // namespace aereborn
#endif
