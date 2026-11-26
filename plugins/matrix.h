#ifndef AEREBORN_PLUGINS_MATRIX_H
#define AEREBORN_PLUGINS_MATRIX_H
#include "core/graph.h"
#include <atomic>
#include <initializer_list>
#include <vector>
namespace aereborn
{
/// @brief Map F32P channels using a fixed matrix and a runtime-adjustable gain.
/// @note Matrix rows correspond to output channels and columns to input channels.
///       Both ports use the same sample rate.
class Matrix final : public NodeT<1, 1>
{
  public:
    /// @copydoc Node::type_name
    const char *type_name() const noexcept override { return "Matrix"; }

    /// @brief Construct an identity channel map with a gain applied to each channel.
    /// @param rate Sample rate of both ports.
    /// @param channels Channel count of both ports.
    /// @param gain Initial finite linear gain.
    /// @throws Error RetCode::EPARAM If the port specification is invalid or gain is not finite.
    Matrix(BandWidth rate, std::size_t channels, float gain = 1.0f);

    /// @brief Construct a channel map from a row-major matrix.
    /// @param rate Sample rate of both ports.
    /// @param input_channels Number of columns.
    /// @param output_channels Number of rows.
    /// @param coefficients Finite coefficients in output-channel order, one row per output.
    /// @param gain Initial finite linear gain.
    /// @note The constructor copies the coefficients. Keep the view valid until it returns.
    /// @throws Error RetCode::EPARAM For invalid port specs, mismatched dimensions, or nonfinite coefficients or gain.
    /// @throws Error RetCode::TOOLARGE If the matrix exceeds the supported storage size.
    Matrix(BandWidth rate, std::size_t input_channels, std::size_t output_channels, View<const float> coefficients,
           float gain = 1.0f);

    /// @brief Construct a row-major channel map from an initializer list.
    /// @note Uses the same dimensions, validation, and copying rules as the view overload.
    Matrix(BandWidth rate, std::size_t input_channels, std::size_t output_channels,
           std::initializer_list<float> coefficients, float gain = 1.0f);

    /// @brief Set the requested linear gain.
    /// @param gain New finite gain target.
    /// @throws Error RetCode::EPARAM if the gain is not finite.
    /// @note Safe to call concurrently with process(). A nonempty block reaches its sampled target on its final frame.
    void set_gain(float gain);

    /// @brief Read the requested gain.
    /// @return The most recently set gain target.
    /// @note Safe to call concurrently with set_gain() and process().
    float gain() const noexcept;

    /// @brief Initialize the applied gain to the current target on the control thread.
    /// @param period Graph block period. Gain ramps use actual block sizes during processing.
    void prepare(BlockPeriod period) override;

    /// @copydoc Node::process
    /// @note Takes one gain target per nonempty block and ramps from the previously applied gain.
    void process(const ProcessContext &context) noexcept override;

    /// @brief Set the applied gain to the current target for the next block.
    /// @pre Serialize with process().
    void reset() noexcept override;

  private:
    std::vector<float> coefficients_;
    std::atomic<float> target_;
    float gain_;
};
} // namespace aereborn
#endif
