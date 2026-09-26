#include "wav.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace unlimited {
namespace wav {

using std::int16_t;
using std::size_t;
using std::uint32_t;
using std::uint8_t;

namespace {

const size_t chunk_samples = 4096;
const uint32_t bytes_per_output_sample = 2;
const uint32_t riff_overhead = 36;
const double pcm16_full_scale = 32767.0;

int16_t to_pcm16(float sample) {
    const double clipped = std::max(-1.0, std::min(1.0, static_cast<double>(sample)));
    return static_cast<int16_t>(std::lround(clipped * pcm16_full_scale));
}

}  // namespace

FileByteSink::FileByteSink() : file_(nullptr), ok_(false) {}

FileByteSink::~FileByteSink() {
    close();
}

bool FileByteSink::open(const std::string& path) {
    close();
    file_ = std::fopen(path.c_str(), "wb");
    ok_ = file_ != nullptr;
    return ok_;
}

bool FileByteSink::close() {
    if (file_ == nullptr) return ok_;
    const bool closed = std::fclose(file_) == 0;
    file_ = nullptr;
    ok_ = ok_ && closed;
    return ok_;
}

bool FileByteSink::write(const uint8_t* data, size_t size) {
    if (file_ == nullptr) return false;
    const bool written = std::fwrite(data, 1, size, file_) == size;
    ok_ = ok_ && written;
    return written;
}

// A sink that cannot seek (a pipe) is not an error: the WAV codec then keeps streaming sizes.
bool FileByteSink::seek(uint32_t position) {
    return file_ != nullptr && std::fseek(file_, static_cast<long>(position), SEEK_SET) == 0;
}

FileByteSource::FileByteSource() : file_(nullptr), ok_(false) {}

FileByteSource::~FileByteSource() {
    close();
}

bool FileByteSource::open(const std::string& path) {
    close();
    file_ = std::fopen(path.c_str(), "rb");
    ok_ = file_ != nullptr;
    return ok_;
}

bool FileByteSource::close() {
    if (file_ != nullptr) std::fclose(file_);
    file_ = nullptr;
    return ok_;
}

size_t FileByteSource::read(uint8_t* data, size_t size) {
    if (file_ == nullptr) return 0;
    const size_t got = std::fread(data, 1, size, file_);
    if (got < size && std::ferror(file_) != 0) ok_ = false;
    return got;
}

bool FileByteSource::seek(uint32_t position) {
    return file_ != nullptr && std::fseek(file_, static_cast<long>(position), SEEK_SET) == 0;
}

bool read_wav(const std::string& path, std::vector<float>& samples, uint32_t& sample_rate, std::string& error) {
    FileByteSource source;
    if (!source.open(path)) {
        error = "cannot open " + path;
        return false;
    }
    WavReader reader;
    if (!reader.open(source)) {
        error = path + ": not a supported WAV file (RIFF/WAVE, PCM 8/16/24/32-bit or float 32-bit)";
        return false;
    }
    std::vector<float> decoded;
    std::vector<float> chunk(chunk_samples);
    for (;;) {
        const size_t got = reader.read(chunk.data(), chunk.size());
        if (got == 0) break;
        decoded.insert(decoded.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
    }
    if (!source.close()) {
        error = "read failed for " + path;
        return false;
    }
    samples.swap(decoded);
    sample_rate = reader.format().sample_rate_hz;
    return true;
}

bool write_wav(const std::string& path, const std::vector<float>& samples, uint32_t sample_rate, std::string& error) {
    const uint32_t max_samples = (std::numeric_limits<uint32_t>::max() - riff_overhead) / bytes_per_output_sample;
    if (sample_rate == 0 || sample_rate > std::numeric_limits<uint32_t>::max() / bytes_per_output_sample) {
        error = "invalid sample rate " + std::to_string(sample_rate);
        return false;
    }
    if (samples.size() > max_samples) {
        error = "too many samples for a WAV file";
        return false;
    }
    FileByteSink sink;
    if (!sink.open(path)) {
        error = "cannot create " + path;
        return false;
    }
    WavWriter writer;
    bool ok = writer.begin(sink, sample_rate, static_cast<uint32_t>(samples.size()));
    std::vector<int16_t> chunk(chunk_samples);
    for (size_t done = 0; ok && done < samples.size();) {
        const size_t count = std::min(chunk.size(), samples.size() - done);
        for (size_t i = 0; i < count; ++i) chunk[i] = to_pcm16(samples[done + i]);
        writer.write(chunk.data(), count);
        ok = writer.ok();
        done += count;
    }
    ok = writer.finish() && ok;
    ok = sink.close() && ok;
    if (!ok) {
        error = "write failed for " + path;
        return false;
    }
    return true;
}

}  // namespace wav
}  // namespace unlimited
