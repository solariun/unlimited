#pragma once

#include "unlimited/wav_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace unlimited {
namespace wav {

// std::FILE adapters for the core WAV codec (the one WAV parser of the project). Both can seek: WavWriter
// patches the sizes at the end, WavReader finds a data chunk placed before fmt. Offsets are `long`, so files
// stay under 2 GB on Windows.
class FileByteSink final : public ByteSink {
public:
    FileByteSink();
    ~FileByteSink();
    FileByteSink(const FileByteSink&) = delete;
    FileByteSink& operator=(const FileByteSink&) = delete;

    bool open(const std::string& path);  // creates or truncates
    bool close();                        // false when the open, a write or the close itself failed
    bool write(const std::uint8_t* data, std::size_t size) override;
    bool seek(std::uint32_t position) override;

private:
    std::FILE* file_;
    bool ok_;
};

class FileByteSource final : public ByteSource {
public:
    FileByteSource();
    ~FileByteSource();
    FileByteSource(const FileByteSource&) = delete;
    FileByteSource& operator=(const FileByteSource&) = delete;

    bool open(const std::string& path);
    bool close();                        // false when the open or a read failed
    std::size_t read(std::uint8_t* data, std::size_t size) override;
    bool seek(std::uint32_t position) override;

private:
    std::FILE* file_;
    bool ok_;
};

bool read_wav(const std::string& path, std::vector<float>& samples, std::uint32_t& sample_rate, std::string& error);

bool write_wav(const std::string& path, const std::vector<float>& samples, std::uint32_t sample_rate,
               std::string& error);

}  // namespace wav
}  // namespace unlimited
