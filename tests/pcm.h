#ifndef AEREBORN_TESTS_PCM_H
#define AEREBORN_TESTS_PCM_H

#include <cstdint>
#include <cstring>

namespace aereborn
{
namespace test
{
inline void write_f32le(std::uint8_t *output, float sample)
{
    std::uint32_t word;
    std::memcpy(&word, &sample, sizeof(word));
    for (unsigned int byte = 0; byte < 4; ++byte)
    {
        output[byte] = static_cast<std::uint8_t>(word >> (8 * byte));
    }
}

inline float read_f32le(const std::uint8_t *input)
{
    const auto word = std::uint32_t{input[0]} | (std::uint32_t{input[1]} << 8) |
                      (std::uint32_t{input[2]} << 16) | (std::uint32_t{input[3]} << 24);
    float sample;
    std::memcpy(&sample, &word, sizeof(sample));
    return sample;
}
} // namespace test
} // namespace aereborn

#endif // AEREBORN_TESTS_PCM_H
