#pragma once

#include "resampler.hpp"
#include "unlimited/audio_io.hpp"
#include "unlimited/dsp.hpp"
#include "wav.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace unlimited {
namespace pc {

static const std::size_t k_device_chunk_samples = 256;  // samples per read()/write() call of the file drivers below

// Owned devices. The core interfaces have protected non-virtual destructors (the core never deletes through
// them), so PC drivers derive from these, which add the virtual destructor.
//
// Live devices (spec 12.5: CoreAudio on macOS, ALSA on Linux) run in real time: start() returns at once; wait()
// blocks until stop() or a device error and is true when the device stopped without an error; stop() may be called
// from any thread, a sink's write() or a signal handler (it only raises a flag and wakes the device's worker). A live
// output pulls its source in the device's own context (a real-time callback: no lock, no allocation) and plays
// silence where the source returns less than asked; the source must stay valid until wait() has returned.
class OutputDevice : public AudioOutput {
public:
    virtual ~OutputDevice() {}

    // The rate start() must be given. A live device plays at the rate fixed when it was opened; files and memory take
    // any rate: 0 until start(), then the rate in use.
    virtual std::uint32_t sample_rate_hz() const { return 0; }

    // How long a sample takes from the source's read() to the device's output: the PTT release waits this long after
    // the last sample (spec 12.4). 0 for files.
    virtual std::uint32_t latency_ms() const { return 0; }

    // Blocks until the source has run out (a read() returned less than asked) and latency_ms() has passed since: the
    // audio has left the device. True then; false when stop() or a device error came first. Files: wait().
    virtual bool drain() { return wait(); }

    virtual std::uint32_t xruns() const { return 0; }  // gaps played: device underruns the backend recovered from
    virtual std::string error() const { return std::string(); }  // why the device failed; empty when it did not
    virtual std::string description() const { return std::string(); }  // what was opened, for the programs' lines
};

class InputDevice : public AudioInput {
public:
    virtual ~InputDevice() {}
    virtual std::uint32_t sample_rate_hz() const = 0;  // the rate the device delivers; pass it to start()

    // A live input calls its sink's write() from a worker thread of its own, never from the device's real-time
    // context; xruns() counts the gaps: device overruns, and audio dropped because the sink fell behind.
    virtual std::uint32_t xruns() const { return 0; }
    virtual std::string error() const { return std::string(); }
    virtual std::string description() const { return std::string(); }
};

// Device specs (spec 12.5):
//   coreaudio:<number|name part|UID>  a live CoreAudio device (macOS)
//   alsa:<number|name part|UID|PCM>   a live ALSA device (Linux); any other ALSA PCM name (hw:1,0, pulse) is opened as is
//   default                            the system's default input or output device
//   wav:<path>, <path>.wav             a WAV file
//   null                               output: discard; input: no audio, nominally at 8 kHz
// Anything else gives nullptr and an error. The file and null drivers run synchronously inside start(): an output
// pulls until read() returns 0, an input pushes its audio in chunks, and stop() called from inside those calls ends
// the transfer after the current chunk. A WAV output is 16-bit PCM mono at the rate given to start(); a WAV input
// delivers the file's rate, downmixed to mono.
//
// rate_hz (-r) is the rate a live device is opened at: 0 takes the device's own (CoreAudio: its nominal rate; ALSA:
// 48000 Hz asked, the rate the device accepted used). CoreAudio runs at the nominal rate only, so another rate is
// refused; files ignore it.
std::unique_ptr<OutputDevice> open_output(const std::string& spec, std::string& error, std::uint32_t rate_hz = 0);
std::unique_ptr<InputDevice> open_input(const std::string& spec, std::string& error, std::uint32_t rate_hz = 0);

enum class DeviceKind { null, wav, live, unknown };

struct DeviceSpec {
    DeviceKind kind;
    std::string backend;  // live: "coreaudio" or "alsa" ("default": this system's backend, empty when it has none)
    std::string target;   // live: what follows the backend ("default" for the bare word); wav: the path
};

DeviceSpec parse_device_spec(const std::string& spec);

// ---------------------------------------------------------------------------
// Device list (--list-devices)
// ---------------------------------------------------------------------------

// The direction exists but its channel count is not known: an ALSA device busy in another program, or an ALSA
// plug-in PCM that converts to any count.
const unsigned k_channels_unknown = ~0u;

struct DeviceInfo {
    DeviceInfo();

    std::string backend;              // "coreaudio" or "alsa": the prefix of its spec
    unsigned number;                  // its position in the list: <backend>:<number>
    std::string name;
    std::string uid;                  // stays the same across runs: <backend>:<uid>
    unsigned input_channels;          // 0: no input
    unsigned output_channels;         // 0: no output
    bool default_input;
    bool default_output;
    std::uint32_t rate_hz;            // the current nominal rate; 0 when the device takes the rate it is asked (ALSA)
    std::vector<std::uint32_t> rates; // the nominal rates it offers; empty: any rate (converted) or not known
};

enum class Direction { input, output };

std::vector<DeviceInfo> list_devices();  // this system's live devices; empty without a live backend or devices

// The --list-devices text of every program: a table with the number, name, channels, default marks, rates and UID of
// each device, then how to choose one.
std::string device_table(const std::vector<DeviceInfo>& devices);
void print_devices(std::FILE* out);

// Picks the device a live spec's target names for one direction, among devices that have it: "default", a number, a
// UID, a whole name or a part of one (case-insensitive). Returns its index in `devices`; k_device_unmatched when
// nothing matches (ALSA then opens the target as a PCM name); k_device_refused when the target is ambiguous (a part
// of the name matching several devices: the two Icoms both appear as "USB Audio CODEC"), a number is out of range or
// the device lacks the direction. error says why and, when ambiguous, lists the matches.
const int k_device_unmatched = -1;
const int k_device_refused = -2;
int select_device(const std::vector<DeviceInfo>& devices, const std::string& target, Direction direction,
                  std::string& error);

// ---------------------------------------------------------------------------
// Memory devices and adapters
// ---------------------------------------------------------------------------

class MemoryOutput final : public OutputDevice {  // collects the audio
public:
    MemoryOutput();
    bool start(SampleSource& source, std::uint32_t sample_rate_hz) override;
    bool wait() override;
    void stop() override;
    const std::vector<std::int16_t>& samples() const;
    std::uint32_t sample_rate_hz() const override;

private:
    std::vector<std::int16_t> samples_;
    std::uint32_t sample_rate_hz_;
    bool stop_requested_;
    bool delivered_;
};

class MemoryInput final : public InputDevice {  // plays a buffer at its rate, chunk_samples per write()
public:
    MemoryInput(const std::vector<std::int16_t>& samples, std::uint32_t sample_rate_hz,
                std::size_t chunk_samples = k_device_chunk_samples);
    bool start(SampleSink& sink, std::uint32_t sample_rate_hz) override;
    bool wait() override;
    void stop() override;
    std::uint32_t sample_rate_hz() const override;

private:
    std::vector<std::int16_t> samples_;
    std::uint32_t sample_rate_hz_;
    std::size_t chunk_samples_;
    bool stop_requested_;
    bool delivered_;
};

class MemorySource final : public SampleSource {  // plays a buffer (which must outlive it) into an AudioOutput
public:
    explicit MemorySource(const std::vector<std::int16_t>& samples);
    std::size_t read(std::int16_t* out, std::size_t count) override;

private:
    const std::vector<std::int16_t>& samples_;
    std::size_t position_;
};

// Resamples audio from from_hz to to_hz (default: the decoder's 8 kHz) and passes it on; a copy when the
// rates are equal. Chunking-invariant. flush() at the end of the input passes on what the resampler still holds.
class ResamplingSink final : public SampleSink {
public:
    ResamplingSink(SampleSink& sink, double from_hz, double to_hz = k_decoder_rate_hz);
    void write(const std::int16_t* in, std::size_t count) override;
    void flush();

private:
    void deliver();

    SampleSink& sink_;
    bool copy_;
    Resampler resampler_;
    std::vector<float> input_;
    std::vector<float> output_;
    std::vector<std::int16_t> converted_;
};

}  // namespace pc
}  // namespace unlimited
