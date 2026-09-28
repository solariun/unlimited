#include "audio.hpp"

#include "audio_live.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace unlimited {
namespace pc {

using std::int16_t;
using std::size_t;
using std::uint32_t;
using std::uint8_t;

namespace {

const std::string k_null_spec = "null";
const std::string k_default_spec = "default";
const std::string k_wav_prefix = "wav:";
const std::string k_wav_extension = ".wav";
const char* const k_live_backends[] = {"coreaudio", "alsa"};
const char* const k_backend_systems[] = {"macOS", "Linux"};  // where each of k_live_backends runs
const size_t k_max_number_digits = 9;                         // a device number fits an int
const int k_decimal = 10;
const double k_hz_per_khz = 1000.0;
const int k_khz_decimals = 3;
const size_t k_number_text = 32;
const unsigned char k_utf8_continuation_mask = 0xC0;
const unsigned char k_utf8_continuation = 0x80;
const std::string k_table_gap = "  ";  // before each column of the device table

bool has_wav_extension(const std::string& path) {
    if (path.size() <= k_wav_extension.size()) return false;
    const size_t offset = path.size() - k_wav_extension.size();
    for (size_t i = 0; i < k_wav_extension.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(path[offset + i])) != k_wav_extension[i]) return false;
    return true;
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

std::string lower(const std::string& text) {
    std::string result = text;
    for (size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(result[i])));
    return result;
}

bool all_digits(const std::string& text) {
    if (text.empty()) return false;
    for (size_t i = 0; i < text.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
    return true;
}

std::string live_forms() {
    const std::string backend = live_backend_name();
    if (backend.empty()) return "";
    if (backend == "alsa") return "alsa:<number|name part|UID|PCM>, default, ";
    return backend + ":<number|name part|UID>, default, ";
}

std::string unknown_spec(const std::string& spec) {
    return "unknown audio device '" + spec + "' (expected " + live_forms() +
           "wav:<path>, <path>.wav or null; --list-devices shows the devices)";
}

// A live spec this system cannot open: another system's backend, no backend, or no device named.
bool live_spec_problem(const DeviceSpec& spec, std::string& error) {
    const std::string here = live_backend_name();
    if (here.empty()) {
        error = "this system has no live audio devices (wav:<path> and null only)";
        return true;
    }
    if (spec.backend != here) {
        for (size_t i = 0; i < sizeof(k_live_backends) / sizeof(k_live_backends[0]); ++i)
            if (spec.backend == k_live_backends[i])
                error = spec.backend + ": devices exist on " + k_backend_systems[i] + " only; this system uses " +
                        here + ": (--list-devices)";
        return true;
    }
    if (spec.target.empty()) {
        error = spec.backend + ": needs a device: " + live_forms() + "see --list-devices";
        return true;
    }
    return false;
}

bool has_direction(const DeviceInfo& device, Direction direction) {
    return (direction == Direction::input ? device.input_channels : device.output_channels) != 0;
}

const char* direction_name(Direction direction) {
    return direction == Direction::input ? "input" : "output";
}

// Display width of UTF-8 text: its code points.
size_t text_width(const std::string& text) {
    size_t width = 0;
    for (size_t i = 0; i < text.size(); ++i)
        if ((static_cast<unsigned char>(text[i]) & k_utf8_continuation_mask) != k_utf8_continuation) ++width;
    return width;
}

std::string pad_right(const std::string& text, size_t width) {
    const size_t used = text_width(text);
    return used >= width ? text : text + std::string(width - used, ' ');
}

std::string pad_left(const std::string& text, size_t width) {
    const size_t used = text_width(text);
    return used >= width ? text : std::string(width - used, ' ') + text;
}

std::string channels_text(unsigned channels) {
    if (channels == 0) return "-";
    if (channels == k_channels_unknown) return "yes";
    return std::to_string(channels);
}

// 44100 -> "44.1", 8000 -> "8".
std::string khz_text(uint32_t rate_hz) {
    char text[k_number_text];
    std::snprintf(text, sizeof(text), "%.*f", k_khz_decimals, rate_hz / k_hz_per_khz);
    std::string result = text;
    while (result[result.size() - 1] == '0') result.erase(result.size() - 1);
    if (result[result.size() - 1] == '.') result.erase(result.size() - 1);
    return result;
}

std::string rates_text(const std::vector<uint32_t>& rates) {
    if (rates.empty()) return "any";
    std::string text;
    for (size_t i = 0; i < rates.size(); ++i) text += (i == 0 ? "" : " ") + khz_text(rates[i]);
    return text;
}

std::string defaults_text(const DeviceInfo& device) {
    if (device.default_input && device.default_output) return "in out";
    if (device.default_input) return "in";
    if (device.default_output) return "out";
    return "";
}

// "  3  USB Audio CODEC  AppleUSBAudioEngine:...": one line per device, for an ambiguous or misdirected target.
std::string device_lines(const std::vector<DeviceInfo>& devices, const std::vector<size_t>& indexes) {
    std::string lines;
    for (size_t i = 0; i < indexes.size(); ++i) {
        const DeviceInfo& device = devices[indexes[i]];
        lines += "\n  " + std::to_string(device.number) + "  " + device.name + "  " + device.uid;
    }
    return lines;
}

}  // namespace

DeviceInfo::DeviceInfo()
    : number(0),
      input_channels(0),
      output_channels(0),
      default_input(false),
      default_output(false),
      rate_hz(0) {}

DeviceSpec parse_device_spec(const std::string& spec) {
    DeviceSpec parsed;
    parsed.kind = DeviceKind::unknown;
    if (spec == k_null_spec) {
        parsed.kind = DeviceKind::null;
    } else if (spec == k_default_spec) {
        parsed.kind = DeviceKind::live;
        parsed.backend = live_backend_name();
        parsed.target = k_default_spec;
    } else if (starts_with(spec, k_wav_prefix)) {
        parsed.kind = DeviceKind::wav;
        parsed.target = spec.substr(k_wav_prefix.size());
    } else if (has_wav_extension(spec)) {
        parsed.kind = DeviceKind::wav;
        parsed.target = spec;
    } else {
        for (size_t i = 0; i < sizeof(k_live_backends) / sizeof(k_live_backends[0]); ++i) {
            const std::string prefix = std::string(k_live_backends[i]) + ":";
            if (!starts_with(spec, prefix)) continue;
            parsed.kind = DeviceKind::live;
            parsed.backend = k_live_backends[i];
            parsed.target = spec.substr(prefix.size());
        }
    }
    return parsed;
}

int select_device(const std::vector<DeviceInfo>& devices, const std::string& target, Direction direction,
                  std::string& error) {
    const std::string what = direction_name(direction);
    if (target == k_default_spec) {
        for (size_t i = 0; i < devices.size(); ++i)
            if (direction == Direction::input ? devices[i].default_input : devices[i].default_output)
                return static_cast<int>(i);
        error = "there is no default " + what + " device";
        return k_device_refused;
    }
    if (all_digits(target)) {
        const unsigned long number = target.size() <= k_max_number_digits
                                         ? std::strtoul(target.c_str(), nullptr, k_decimal)
                                         : std::numeric_limits<unsigned long>::max();
        for (size_t i = 0; i < devices.size(); ++i) {
            if (devices[i].number != number) continue;
            if (has_direction(devices[i], direction)) return static_cast<int>(i);
            error = "device " + target + " (" + devices[i].name + ") has no " + what;
            return k_device_refused;
        }
        error = "there is no device " + target + " (--list-devices shows " +
                (devices.empty() ? std::string("none") : "0.." + std::to_string(devices.size() - 1)) + ")";
        return k_device_refused;
    }
    for (size_t i = 0; i < devices.size(); ++i) {
        if (devices[i].uid != target) continue;
        if (has_direction(devices[i], direction)) return static_cast<int>(i);
        error = devices[i].name + " (" + target + ") has no " + what;
        return k_device_refused;
    }
    // A whole name first (a name may be part of another), then a part of one.
    const std::string wanted = lower(target);
    std::vector<size_t> whole;
    std::vector<size_t> part;
    std::vector<size_t> other_direction;
    for (size_t i = 0; i < devices.size(); ++i) {
        const std::string name = lower(devices[i].name);
        if (name.find(wanted) == std::string::npos) continue;
        if (!has_direction(devices[i], direction)) {
            other_direction.push_back(i);
            continue;
        }
        part.push_back(i);
        if (name == wanted) whole.push_back(i);
    }
    const std::vector<size_t>& matches = whole.empty() ? part : whole;
    if (matches.size() == 1) return static_cast<int>(matches[0]);
    if (matches.size() > 1) {
        const std::string backend = devices[matches[0]].backend;
        error = "'" + target + "' matches " + std::to_string(matches.size()) + " " + what +
                " devices; choose one by its number (" + backend + ":" + std::to_string(devices[matches[0]].number) +
                ") or its UID (" + backend + ":<UID>):" + device_lines(devices, matches);
        return k_device_refused;
    }
    error = "no " + what + " device matches '" + target + "'";
    if (!other_direction.empty())
        error += "; it names devices without " + what + ":" + device_lines(devices, other_direction);
    else
        error += " (--list-devices shows the devices)";
    return k_device_unmatched;
}

std::vector<DeviceInfo> list_devices() {
    return live_devices();
}

std::string device_table(const std::vector<DeviceInfo>& devices) {
    const std::string backend = devices.empty() ? std::string(live_backend_name()) : devices[0].backend;
    if (backend.empty()) return "This system has no live audio devices: only wav:<path> and null.\n";
    if (devices.empty()) return "No audio devices found (" + backend + ").\n";

    const char* const headings[] = {"#", "Name", "In", "Out", "Default", "Rate Hz", "Rates kHz", "UID"};
    const size_t columns = sizeof(headings) / sizeof(headings[0]);
    const bool right_aligned[] = {true, false, true, true, false, true, false, false};
    std::vector<std::vector<std::string> > rows(1, std::vector<std::string>(headings, headings + columns));
    for (size_t i = 0; i < devices.size(); ++i) {
        const DeviceInfo& device = devices[i];
        const std::string cells[] = {std::to_string(device.number),
                                     device.name,
                                     channels_text(device.input_channels),
                                     channels_text(device.output_channels),
                                     defaults_text(device),
                                     device.rate_hz != 0 ? std::to_string(device.rate_hz) : "-",
                                     rates_text(device.rates),
                                     device.uid};
        rows.push_back(std::vector<std::string>(cells, cells + columns));
    }
    std::vector<size_t> widths(columns, 0);
    for (size_t r = 0; r < rows.size(); ++r)
        for (size_t c = 0; c < columns; ++c) widths[c] = std::max(widths[c], text_width(rows[r][c]));

    std::string table = "Audio devices (" + backend + "):\n";
    for (size_t r = 0; r < rows.size(); ++r) {
        std::string line;
        for (size_t c = 0; c < columns; ++c) {
            const bool last = c + 1 == columns;
            line += k_table_gap + (right_aligned[c] ? pad_left(rows[r][c], widths[c])
                                                    : (last ? rows[r][c] : pad_right(rows[r][c], widths[c])));
        }
        table += line + "\n";
    }
    const std::string b = backend + ":";
    table += "\nChoose with --input and --output, or -d for both: " + b + "<#>, " + b + "<part of the name>, " + b +
             "<UID>, or default.\nA part of the name that matches several devices is refused: use the # or the UID.\n";
    std::string example;
    for (size_t i = 0; i < devices.size() && example.empty(); ++i)
        if (has_direction(devices[i], Direction::input)) example = "--input " + b + std::to_string(devices[i].number);
    for (size_t i = 0; i < devices.size(); ++i) {
        if (!has_direction(devices[i], Direction::output)) continue;
        example += (example.empty() ? "" : " ") + std::string("--output ") + b + std::to_string(devices[i].number);
        break;
    }
    table += "For example: " + example + "\n";
    if (backend == "alsa") table += "Any other ALSA PCM name works too: alsa:hw:1,0, alsa:plughw:CARD=CODEC,DEV=0.\n";
    return table;
}

void print_devices(std::FILE* out) {
    std::fputs(device_table(list_devices()).c_str(), out);
}

// ---------------------------------------------------------------------------
// File and null drivers
// ---------------------------------------------------------------------------

namespace {

int16_t to_int16(float x) {
    const float low = std::numeric_limits<int16_t>::min();
    const float high = std::numeric_limits<int16_t>::max();
    return static_cast<int16_t>(std::lround(std::max(low, std::min(high, x))));
}

// Ends the stream early once stop was requested.
class StoppableSource final : public SampleSource {
public:
    StoppableSource(SampleSource& source, const bool& stop_requested)
        : source_(source), stop_requested_(stop_requested) {}

    size_t read(int16_t* out, size_t count) override { return stop_requested_ ? 0 : source_.read(out, count); }

private:
    SampleSource& source_;
    const bool& stop_requested_;
};

class NullOutput final : public OutputDevice {
public:
    bool start(SampleSource& source, uint32_t sample_rate_hz) override {
        if (sample_rate_hz == 0) return false;
        int16_t buffer[k_device_chunk_samples];
        while (!stop_requested_ && source.read(buffer, k_device_chunk_samples) > 0) {
        }
        delivered_ = !stop_requested_;
        return true;
    }

    bool wait() override { return delivered_; }
    void stop() override { stop_requested_ = true; }
    std::string description() const override { return k_null_spec; }

private:
    bool stop_requested_ = false;
    bool delivered_ = false;
};

class NullInput final : public InputDevice {
public:
    bool start(SampleSink&, uint32_t) override { return true; }
    bool wait() override { return true; }
    void stop() override {}
    uint32_t sample_rate_hz() const override { return k_decoder_rate_hz; }
    std::string description() const override { return k_null_spec; }
};

class WavFileOutput final : public OutputDevice {
public:
    explicit WavFileOutput(const std::string& path) : path_(path), wav_(file_) {}

    bool open() { return file_.open(path_); }

    bool start(SampleSource& source, uint32_t sample_rate_hz) override {
        if (started_) return false;
        started_ = true;
        sample_rate_hz_ = sample_rate_hz;
        StoppableSource stoppable(source, stop_requested_);
        const bool started = wav_.start(stoppable, sample_rate_hz);
        const bool written = started && wav_.wait();
        const bool closed = file_.close();
        delivered_ = written && closed && !stop_requested_;
        return started;
    }

    bool wait() override { return delivered_; }
    void stop() override { stop_requested_ = true; }
    uint32_t sample_rate_hz() const override { return sample_rate_hz_; }
    std::string description() const override { return k_wav_prefix + path_; }

private:
    std::string path_;
    wav::FileByteSink file_;
    WavOutput wav_;
    uint32_t sample_rate_hz_ = 0;
    bool started_ = false;
    bool stop_requested_ = false;
    bool delivered_ = false;
};

class WavFileInput final : public InputDevice {
public:
    bool open(const std::string& path, std::string& error) {
        path_ = path;
        if (!file_.open(path)) {
            error = "cannot open " + path;
            return false;
        }
        if (!reader_.open(file_)) {
            error = path + ": not a supported WAV file";
            return false;
        }
        return true;
    }

    std::string description() const override { return k_wav_prefix + path_; }

    bool start(SampleSink& sink, uint32_t sample_rate_hz) override {
        if (started_ || sample_rate_hz != this->sample_rate_hz()) return false;
        started_ = true;
        int16_t buffer[k_device_chunk_samples];
        size_t count = 0;
        while (!stop_requested_ && (count = reader_.read(buffer, k_device_chunk_samples)) > 0)
            sink.write(buffer, count);
        file_.close();
        return true;
    }

    bool wait() override { return started_; }
    void stop() override { stop_requested_ = true; }
    uint32_t sample_rate_hz() const override { return reader_.format().sample_rate_hz; }

private:
    std::string path_;
    wav::FileByteSource file_;
    WavReader reader_;
    bool started_ = false;
    bool stop_requested_ = false;
};

}  // namespace

std::unique_ptr<OutputDevice> open_output(const std::string& spec, std::string& error, uint32_t rate_hz) {
    const DeviceSpec parsed = parse_device_spec(spec);
    switch (parsed.kind) {
    case DeviceKind::null:
        return std::unique_ptr<OutputDevice>(new NullOutput());
    case DeviceKind::live:
        if (live_spec_problem(parsed, error)) return std::unique_ptr<OutputDevice>();
        return open_live_output(parsed.target, rate_hz, error);
    case DeviceKind::wav: {
        if (parsed.target.empty()) {
            error = "wav: needs a file name";
            return std::unique_ptr<OutputDevice>();
        }
        std::unique_ptr<WavFileOutput> device(new WavFileOutput(parsed.target));
        if (!device->open()) {
            error = "cannot create " + parsed.target;
            return std::unique_ptr<OutputDevice>();
        }
        return std::unique_ptr<OutputDevice>(device.release());
    }
    case DeviceKind::unknown:
        break;
    }
    error = unknown_spec(spec);
    return std::unique_ptr<OutputDevice>();
}

std::unique_ptr<InputDevice> open_input(const std::string& spec, std::string& error, uint32_t rate_hz) {
    const DeviceSpec parsed = parse_device_spec(spec);
    switch (parsed.kind) {
    case DeviceKind::null:
        return std::unique_ptr<InputDevice>(new NullInput());
    case DeviceKind::live:
        if (live_spec_problem(parsed, error)) return std::unique_ptr<InputDevice>();
        return open_live_input(parsed.target, rate_hz, error);
    case DeviceKind::wav: {
        if (parsed.target.empty()) {
            error = "wav: needs a file name";
            return std::unique_ptr<InputDevice>();
        }
        std::unique_ptr<WavFileInput> device(new WavFileInput());
        if (!device->open(parsed.target, error)) return std::unique_ptr<InputDevice>();
        return std::unique_ptr<InputDevice>(device.release());
    }
    case DeviceKind::unknown:
        break;
    }
    error = unknown_spec(spec);
    return std::unique_ptr<InputDevice>();
}

// ---------------------------------------------------------------------------
// Memory devices
// ---------------------------------------------------------------------------

MemoryOutput::MemoryOutput() : sample_rate_hz_(0), stop_requested_(false), delivered_(false) {}

bool MemoryOutput::start(SampleSource& source, uint32_t sample_rate_hz) {
    if (sample_rate_hz == 0) return false;
    sample_rate_hz_ = sample_rate_hz;
    samples_.clear();
    int16_t buffer[k_device_chunk_samples];
    size_t count = 0;
    while (!stop_requested_ && (count = source.read(buffer, k_device_chunk_samples)) > 0)
        samples_.insert(samples_.end(), buffer, buffer + count);
    delivered_ = !stop_requested_;
    return true;
}

bool MemoryOutput::wait() {
    return delivered_;
}

void MemoryOutput::stop() {
    stop_requested_ = true;
}

const std::vector<int16_t>& MemoryOutput::samples() const {
    return samples_;
}

uint32_t MemoryOutput::sample_rate_hz() const {
    return sample_rate_hz_;
}

MemoryInput::MemoryInput(const std::vector<int16_t>& samples, uint32_t sample_rate_hz, size_t chunk_samples)
    : samples_(samples),
      sample_rate_hz_(sample_rate_hz),
      chunk_samples_(std::max<size_t>(1, chunk_samples)),
      stop_requested_(false),
      delivered_(false) {}

bool MemoryInput::start(SampleSink& sink, uint32_t sample_rate_hz) {
    if (sample_rate_hz != sample_rate_hz_) return false;
    for (size_t pos = 0; pos < samples_.size() && !stop_requested_; pos += chunk_samples_)
        sink.write(&samples_[pos], std::min(chunk_samples_, samples_.size() - pos));
    delivered_ = true;
    return true;
}

bool MemoryInput::wait() {
    return delivered_;
}

void MemoryInput::stop() {
    stop_requested_ = true;
}

uint32_t MemoryInput::sample_rate_hz() const {
    return sample_rate_hz_;
}

MemorySource::MemorySource(const std::vector<int16_t>& samples) : samples_(samples), position_(0) {}

size_t MemorySource::read(int16_t* out, size_t count) {
    const size_t n = std::min(count, samples_.size() - position_);
    std::copy(samples_.begin() + position_, samples_.begin() + position_ + n, out);
    position_ += n;
    return n;
}

// ---------------------------------------------------------------------------
// ResamplingSink
// ---------------------------------------------------------------------------

ResamplingSink::ResamplingSink(SampleSink& sink, double from_hz, double to_hz)
    : sink_(sink), copy_(from_hz == to_hz), resampler_(from_hz, to_hz) {}

void ResamplingSink::write(const int16_t* in, size_t count) {
    if (copy_) {
        sink_.write(in, count);
        return;
    }
    input_.assign(in, in + count);
    output_.clear();
    resampler_.process(input_.data(), input_.size(), output_);
    deliver();
}

void ResamplingSink::flush() {
    if (copy_) return;
    output_.clear();
    resampler_.flush(output_);
    deliver();
}

void ResamplingSink::deliver() {
    converted_.resize(output_.size());
    for (size_t i = 0; i < output_.size(); ++i) converted_[i] = to_int16(output_[i]);
    if (!converted_.empty()) sink_.write(&converted_[0], converted_.size());
}

}  // namespace pc
}  // namespace unlimited
