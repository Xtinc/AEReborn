#include "wav_file.h"
#include "audio_types.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

namespace aereborn
{
namespace
{

constexpr std::uint16_t pcm_tag = 1;
constexpr std::uint16_t float_tag = 3;

void append_u16(unsigned char *&out, std::uint16_t value) noexcept
{
    *out++ = static_cast<unsigned char>(value);
    *out++ = static_cast<unsigned char>(value >> 8);
}

void append_u32(unsigned char *&out, std::uint32_t value) noexcept
{
    append_u16(out, static_cast<std::uint16_t>(value));
    append_u16(out, static_cast<std::uint16_t>(value >> 16));
}

std::uint32_t wav_header_size(SampleFormat format) noexcept
{
    return format == SampleFormat::F32LE ? 58 : 44;
}

bool write_header(std::FILE *file, PortSpec spec, std::uint32_t data_size) noexcept
{
    const auto width = static_cast<std::uint16_t>(spec.channels * sample_bytes(spec.format));
    const auto rate = static_cast<std::uint32_t>(spec.bandwidth);
    const auto floating = spec.format == SampleFormat::F32LE;
    std::array<unsigned char, 58> header;
    auto *out = header.data();
    const auto append_tag = [&out](const char *tag) {
        std::memcpy(out, tag, 4);
        out += 4;
    };
    append_tag("RIFF");
    append_u32(out, wav_header_size(spec.format) - 8 + data_size + (data_size & 1));
    append_tag("WAVE");
    append_tag("fmt ");
    append_u32(out, floating ? 18 : 16);
    append_u16(out, floating ? float_tag : pcm_tag);
    append_u16(out, static_cast<std::uint16_t>(spec.channels));
    append_u32(out, rate);
    append_u32(out, rate * width);
    append_u16(out, width);
    append_u16(out, static_cast<std::uint16_t>(sample_bits(spec.format)));
    if (floating)
    {
        append_u16(out, 0);
        append_tag("fact");
        append_u32(out, 4);
        append_u32(out, data_size / width);
    }
    append_tag("data");
    append_u32(out, data_size);
    const auto size = static_cast<std::size_t>(out - header.data());
    return std::fwrite(header.data(), 1, size, file) == size && std::ferror(file) == 0;
}

std::FILE *open_file(const char *path, const char *mode) noexcept
{
#ifdef _MSC_VER
    std::FILE *file = nullptr;
    const auto result = ::fopen_s(&file, path, mode);
    if (result)
        errno = result;
    return file;
#else
    return std::fopen(path, mode);
#endif
}

bool seek_bytes(std::FILE *file, std::uint64_t position, int origin) noexcept
{
#if WINDOWS_OS_ENVIRONMENT
    return ::_fseeki64(file, static_cast<std::int64_t>(position), origin) == 0;
#else
    if (position > static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max()))
        return false;
    return ::fseeko(file, static_cast<off_t>(position), origin) == 0;
#endif
}

std::int64_t tell_bytes(std::FILE *file) noexcept
{
#if WINDOWS_OS_ENVIRONMENT
    return ::_ftelli64(file);
#else
    return ::ftello(file);
#endif
}

struct WaveData
{
    PortSpec spec;
    std::uint64_t offset = 0;
    std::size_t size = 0;
    std::size_t frame_bytes = 0;
};

std::uint16_t read_u16(const unsigned char *p) noexcept
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t read_u32(const unsigned char *p) noexcept
{
    return read_u16(p) | (static_cast<std::uint32_t>(read_u16(p + 2)) << 16);
}

bool read_bytes(std::FILE *file, unsigned char *bytes, std::size_t size) noexcept
{
    return std::fread(bytes, 1, size, file) == size;
}

SampleFormat wav_format(std::uint16_t tag, std::uint16_t bits) noexcept
{
    if (tag == float_tag && (bits == 32 || bits == 64))
        return SampleFormat::F32LE;
    if (tag == pcm_tag)
    {
        switch (bits)
        {
        case 8: return SampleFormat::F32LE;
        case 16: return SampleFormat::S16LE;
        case 24: return SampleFormat::S24_3LE;
        case 32: return SampleFormat::S32LE;
        }
    }
    return {};
}

bool parse_format(std::FILE *file, std::uint32_t size, WaveData &data)
{
    std::array<unsigned char, 40> fmt{};
    const auto read_size = std::min<std::uint32_t>(size, fmt.size());
    if (read_size < 16 || !read_bytes(file, fmt.data(), read_size))
        return false;
    auto tag = read_u16(fmt.data());
    if (tag == 0xfffe) // WAVE_FORMAT_EXTENSIBLE
    {
        constexpr unsigned char subtype_suffix[] = {0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
        const auto extension_size = read_u16(fmt.data() + 16);
        if (read_size < fmt.size() || extension_size < 22 || extension_size > size - 18 ||
            std::memcmp(fmt.data() + 26, subtype_suffix, sizeof(subtype_suffix)))
            return false;
        tag = read_u16(fmt.data() + 24);
    }
    const auto bits = read_u16(fmt.data() + 14);
    data.spec = {static_cast<BandWidth>(read_u32(fmt.data() + 4)), read_u16(fmt.data() + 2), wav_format(tag, bits)};
    data.frame_bytes = read_u16(fmt.data() + 12);
    return valid_pcm_spec(data.spec) && data.frame_bytes == data.spec.channels * (bits / 8);
}

RetCode parse_wav(std::FILE *file, WaveData &data)
{
    unsigned char header[12];
    if (!read_bytes(file, header, sizeof(header)) || std::memcmp(header, "RIFF", 4) ||
        std::memcmp(header + 8, "WAVE", 4))
        return RetCode::INVFMT;
    const auto limit = std::int64_t{8} + read_u32(header + 4);
    if (limit < 12 || !seek_bytes(file, 0, SEEK_END) || tell_bytes(file) < limit)
        return RetCode::IOFAILED;

    bool have_format = false;
    for (std::int64_t pos = 12; pos + 8 <= limit; )
    {
        unsigned char chunk[8];
        if (!seek_bytes(file, pos, SEEK_SET) || !read_bytes(file, chunk, sizeof(chunk)))
            return RetCode::IOFAILED;
        const auto size = read_u32(chunk + 4);
        const auto payload = pos + 8;
        pos = payload + size + (size & 1);
        if (pos > limit)
            return RetCode::IOFAILED;
        if (!have_format && !std::memcmp(chunk, "fmt ", 4))
        {
            if (!parse_format(file, size, data))
                return RetCode::INVFMT;
            have_format = true;
        }
        else if (!data.offset && !std::memcmp(chunk, "data", 4))
        {
            data.offset = payload;
            data.size = size;
        }
        if (have_format && data.offset)
        {
            if (data.size % data.frame_bytes)
                return RetCode::INVFMT;
            return seek_bytes(file, data.offset, SEEK_SET) ? RetCode::OK : RetCode::IOFAILED;
        }
    }
    return RetCode::INVFMT;
}

float read_f64(const unsigned char *bytes) noexcept
{
    static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559, "WAV requires IEEE binary64");
    const auto bits = std::uint64_t{read_u32(bytes)} | (std::uint64_t{read_u32(bytes + 4)} << 32);
    double sample;
    std::memcpy(&sample, &bits, sizeof(sample));
    if (std::abs(sample) > std::numeric_limits<float>::max())
        return std::signbit(sample) ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
    return static_cast<float>(sample);
}

void convert_to_f32le(const unsigned char *src, std::size_t sample_bytes, std::size_t samples,
                      unsigned char *dst) noexcept
{
    for (std::size_t i = 0; i < samples; ++i, src += sample_bytes)
    {
        const auto sample = sample_bytes == 1 ? (static_cast<int>(*src) - 128) / 128.0f : read_f64(src);
        std::uint32_t bits;
        std::memcpy(&bits, &sample, sizeof(bits));
        for (unsigned shift = 0; shift < 32; shift += 8)
            *dst++ = static_cast<unsigned char>(bits >> shift);
    }
}

} // namespace

WavFile::~WavFile()
{
    close();
}

RetCode WavFile::open_read(const std::string &path) noexcept
{
    if (file_ && writing_)
        return RetCode::BUSY;
    const auto close_input = [](std::FILE *file) noexcept { std::fclose(file); };
    std::unique_ptr<std::FILE, decltype(close_input)> file(open_file(path.c_str(), "rb"), close_input);
    if (!file)
        return RetCode::IOFAILED;
    WaveData data;
    const auto parsed = parse_wav(file.get(), data);
    if (!parsed)
        return parsed;
    const auto closed = close();
    if (!closed)
        return closed;

    file_ = file.release();
    spec_ = data.spec;
    data_offset_ = data.offset;
    file_frame_bytes_ = data.frame_bytes;
    frame_count_ = data.size / data.frame_bytes;
    return {};
}

bool WavFile::supports_write_format(SampleFormat format) noexcept
{
    return format == SampleFormat::S16LE || format == SampleFormat::S24_3LE ||
           format == SampleFormat::S32LE || format == SampleFormat::F32LE;
}

Result<std::size_t> WavFile::max_write_frames(PortSpec spec) noexcept
{
    const auto format = spec.format;
    if (!supports_write_format(format))
        return RetCode::INVFMT;
    if (!spec.valid())
        return RetCode::EPARAM;
    const auto sample_width = sample_bytes(format);
    const auto rate = static_cast<std::uint32_t>(spec.bandwidth);
    constexpr auto max_bytes = std::numeric_limits<std::uint32_t>::max();
    if (spec.channels > std::numeric_limits<std::uint16_t>::max() / sample_width ||
        spec.channels > max_bytes / sample_width / rate)
        return RetCode::TOOLARGE;

    // Leave room for the RIFF header and the pad byte of an odd data chunk.
    const auto limit = (max_bytes - (wav_header_size(format) - 8)) & ~std::uint32_t{1};
    return limit / (spec.channels * sample_width);
}

RetCode WavFile::open_write(const std::string &path, PortSpec spec, WriteMode mode) noexcept
{
    if (file_)
        return RetCode::BUSY;
    const auto limit = max_write_frames(spec);
    if (!limit)
        return limit.ret_code();
    const auto exclusive = mode == WriteMode::Exclusive;
    auto *file = open_file(path.c_str(), exclusive ? "wbx" : "wb");
    if (!file)
        return exclusive && errno == EEXIST ? RetCode::BUSY : RetCode::IOFAILED;

    if (!write_header(file, spec, 0) || std::fflush(file) != 0)
    {
        std::fclose(file);
        return RetCode::IOFAILED;
    }

    file_ = file;
    writing_ = true;
    spec_ = spec;
    data_offset_ = wav_header_size(spec.format);
    file_frame_bytes_ = spec.channels * sample_bytes(spec.format);
    write_limit_ = limit.value();
    return {};
}

RetCode WavFile::close() noexcept
{
    if (!file_)
        return {};
    const auto prior_ok = std::ferror(file_) == 0;
    bool finalized = true;
    if (writing_ && frame_count_)
    {
        const auto size = static_cast<std::uint32_t>(frame_count_ * file_frame_bytes_);
        std::clearerr(file_);
        if (size & 1)
            finalized = seek_bytes(file_, data_offset_ + size, SEEK_SET) && std::fputc(0, file_) != EOF;
        finalized = finalized && seek_bytes(file_, 0, SEEK_SET) && write_header(file_, spec_, size);
    }
    const auto closed = std::fclose(file_);
    file_ = nullptr;
    writing_ = false;
    spec_ = {};
    frame_count_ = position_ = 0;
    return prior_ok && finalized && closed == 0 ? RetCode::OK : RetCode::IOFAILED;
}

RetCode WavFile::flush() noexcept
{
    if (!writing_)
        return RetCode::ESTATE;
    return std::fflush(file_) != 0 || std::ferror(file_) ? RetCode::IOFAILED : RetCode::OK;
}

RetCode WavFile::status() const noexcept
{
    if (!file_)
        return RetCode::ESTATE;
    return std::ferror(file_) ? RetCode::IOFAILED : RetCode::OK;
}

RetCode WavFile::seek(std::size_t frame) noexcept
{
    if (!file_)
        return RetCode::ESTATE;
    if (frame > frame_count_)
        return RetCode::EPARAM;
    // Allow readers to retry while preserving any write error for close().
    if (!writing_)
        std::clearerr(file_);
    if (!seek_bytes(file_, data_offset_ + frame * file_frame_bytes_, SEEK_SET))
        return RetCode::IOFAILED;
    position_ = frame;
    return {};
}

Result<std::size_t> WavFile::read(OBBlock output) noexcept
{
    if (!file_ || writing_)
        return RetCode::ESTATE;
    if (output.spec() != spec_)
        return RetCode::INVFMT;
    const auto frames = std::min(output.frames(), frame_count_ - position_);
    if (!frames)
        return frames;

    auto *dst = output.byte_view().data();
    if (file_frame_bytes_ == spec_.channels * sample_bytes(spec_.format))
    {
        if (!read_bytes(file_, dst, frames * file_frame_bytes_))
            return RetCode::IOFAILED;
    }
    else
    {
        // Convert extra on-disk formats in bounded groups of interleaved samples.
        std::array<unsigned char, 4096> scratch;
        const auto sample_width = file_frame_bytes_ / spec_.channels;
        auto samples = frames * spec_.channels;
        while (samples)
        {
            const auto count = std::min(samples, scratch.size() / sample_width);
            if (!read_bytes(file_, scratch.data(), count * sample_width))
                return RetCode::IOFAILED;
            convert_to_f32le(scratch.data(), sample_width, count, dst);
            samples -= count;
            dst += count * sizeof(float);
        }
    }
    position_ += frames;
    return frames;
}

RetCode WavFile::write(IBBlock input) noexcept
{
    if (!writing_)
        return RetCode::ESTATE;
    if (input.spec() != spec_)
        return RetCode::INVFMT;
    if (std::ferror(file_))
        return RetCode::IOFAILED;
    if (input.frames() > write_limit_ - position_)
        return RetCode::TOOLARGE;
    if (!input.frames())
        return {};

    if (std::fwrite(input.byte_view().data(), file_frame_bytes_, input.frames(), file_) != input.frames() ||
        std::ferror(file_))
        return RetCode::IOFAILED;
    position_ += input.frames();
    frame_count_ = std::max(frame_count_, position_);
    return {};
}

} // namespace aereborn
