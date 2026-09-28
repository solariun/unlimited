#include "audio_live.hpp"

#if defined(__APPLE__)

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cctype>
#include <cstring>

// CoreAudio backend (spec 12.5): one AUHAL unit per direction at the device's nominal rate, in 32-bit float with the
// device's channels. The input callback renders into a buffer allocated at open and hands the first channel to the
// InputFeed; the output render callback pulls the source through the OutputFeed. Device listeners report a
// disconnection, a change of the nominal rate and processor overloads.
namespace unlimited {
namespace pc {

using std::size_t;
using std::uint32_t;

namespace {

const char* const k_backend = "coreaudio";
const AudioObjectPropertyElement k_element_main = 0;  // kAudioObjectPropertyElementMain (Master before macOS 12)
const AudioUnitElement k_output_element = 0;          // AUHAL: element 0 plays, element 1 captures
const AudioUnitElement k_input_element = 1;
const AudioUnitElement k_global_element = 0;
const UInt32 k_float_bytes = sizeof(Float32);
const UInt32 k_bits_per_byte = 8;
const uint32_t k_byte_mask = 0xFF;
const double k_ms_per_s = 1000.0;
const size_t k_four_char_code = 4;

AudioObjectPropertyAddress address(AudioObjectPropertySelector selector,
                                   AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    const AudioObjectPropertyAddress result = {selector, scope, k_element_main};
    return result;
}

// "'what'" for a four-character code, the number otherwise.
std::string status_text(OSStatus status) {
    const uint32_t code = static_cast<uint32_t>(status);
    char text[k_four_char_code];
    bool printable = true;
    for (size_t i = 0; i < k_four_char_code; ++i) {
        text[i] = static_cast<char>((code >> (k_bits_per_byte * (k_four_char_code - 1 - i))) & k_byte_mask);
        printable = printable && std::isprint(static_cast<unsigned char>(text[i]));
    }
    if (printable) return "'" + std::string(text, k_four_char_code) + "'";
    return std::to_string(static_cast<long>(status));
}

std::string failure(const char* what, OSStatus status) {
    return std::string(what) + " failed (CoreAudio status " + status_text(status) + ")";
}

// Property notifications on the HAL's own thread: a command-line program runs no main run loop, and without this the
// device listeners (and the device list's updates) would never run.
void use_hal_thread() {
    static const bool done = [] {
        CFRunLoopRef none = nullptr;
        const AudioObjectPropertyAddress run_loop = address(kAudioHardwarePropertyRunLoop);
        AudioObjectSetPropertyData(kAudioObjectSystemObject, &run_loop, 0, nullptr, sizeof(none), &none);
        return true;
    }();
    (void)done;
}

template <typename T>
bool get(AudioObjectID object, const AudioObjectPropertyAddress& where, T& value) {
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(object, &where, 0, nullptr, &size, &value) == noErr && size == sizeof(T);
}

template <typename T>
std::vector<T> get_array(AudioObjectID object, const AudioObjectPropertyAddress& where) {
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(object, &where, 0, nullptr, &size) != noErr || size < sizeof(T))
        return std::vector<T>();
    std::vector<T> values(size / sizeof(T));
    size = static_cast<UInt32>(values.size() * sizeof(T));
    if (AudioObjectGetPropertyData(object, &where, 0, nullptr, &size, &values[0]) != noErr) return std::vector<T>();
    values.resize(size / sizeof(T));
    return values;
}

std::string to_utf8(CFStringRef text) {
    const CFIndex length = CFStringGetLength(text);
    const CFIndex most = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::vector<char> buffer(static_cast<size_t>(most));
    if (!CFStringGetCString(text, &buffer[0], most, kCFStringEncodingUTF8)) return std::string();
    return std::string(&buffer[0]);
}

std::string get_string(AudioObjectID object, AudioObjectPropertySelector selector) {
    CFStringRef text = nullptr;
    if (!get(object, address(selector), text) || text == nullptr) return std::string();
    const std::string result = to_utf8(text);
    CFRelease(text);
    return result;
}

unsigned channel_count(AudioObjectID device, AudioObjectPropertyScope scope) {
    const AudioObjectPropertyAddress where = address(kAudioDevicePropertyStreamConfiguration, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &where, 0, nullptr, &size) != noErr || size < sizeof(AudioBufferList))
        return 0;
    std::vector<AudioBufferList> storage(size / sizeof(AudioBufferList) + 1);  // aligned for AudioBufferList
    AudioBufferList* list = &storage[0];
    if (AudioObjectGetPropertyData(device, &where, 0, nullptr, &size, list) != noErr) return 0;
    unsigned channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) channels += list->mBuffers[i].mNumberChannels;
    return channels;
}

std::vector<uint32_t> available_rates(AudioObjectID device) {
    const std::vector<AudioValueRange> ranges =
        get_array<AudioValueRange>(device, address(kAudioDevicePropertyAvailableNominalSampleRates));
    std::vector<uint32_t> rates;
    for (size_t i = 0; i < ranges.size(); ++i) {
        if (ranges[i].mMinimum == ranges[i].mMaximum) {
            rates.push_back(static_cast<uint32_t>(std::lround(ranges[i].mMinimum)));
            continue;
        }
        const std::vector<uint32_t> within = standard_rates_between(ranges[i].mMinimum, ranges[i].mMaximum);
        rates.insert(rates.end(), within.begin(), within.end());
    }
    std::sort(rates.begin(), rates.end());
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
    return rates;
}

struct Device {
    AudioObjectID id;
    DeviceInfo info;
};

// Every device with an input or an output, in the HAL's order.
std::vector<Device> enumerate() {
    use_hal_thread();
    AudioObjectID default_input = kAudioObjectUnknown;
    AudioObjectID default_output = kAudioObjectUnknown;
    get(kAudioObjectSystemObject, address(kAudioHardwarePropertyDefaultInputDevice), default_input);
    get(kAudioObjectSystemObject, address(kAudioHardwarePropertyDefaultOutputDevice), default_output);
    const std::vector<AudioObjectID> ids =
        get_array<AudioObjectID>(kAudioObjectSystemObject, address(kAudioHardwarePropertyDevices));
    std::vector<Device> devices;
    for (size_t i = 0; i < ids.size(); ++i) {
        UInt32 hidden = 0;
        if (get(ids[i], address(kAudioDevicePropertyIsHidden), hidden) && hidden != 0) continue;
        Device device;
        device.id = ids[i];
        DeviceInfo& info = device.info;
        info.input_channels = channel_count(ids[i], kAudioObjectPropertyScopeInput);
        info.output_channels = channel_count(ids[i], kAudioObjectPropertyScopeOutput);
        if (info.input_channels == 0 && info.output_channels == 0) continue;
        info.backend = k_backend;
        info.number = static_cast<unsigned>(devices.size());
        info.name = get_string(ids[i], kAudioObjectPropertyName);
        info.uid = get_string(ids[i], kAudioDevicePropertyDeviceUID);
        info.default_input = ids[i] == default_input && info.input_channels > 0;
        info.default_output = ids[i] == default_output && info.output_channels > 0;
        Float64 rate = 0.0;
        if (get(ids[i], address(kAudioDevicePropertyNominalSampleRate), rate))
            info.rate_hz = static_cast<uint32_t>(std::lround(rate));
        info.rates = available_rates(ids[i]);
        devices.push_back(device);
    }
    return devices;
}

bool resolve(const std::string& target, Direction direction, Device& found, std::string& error) {
    const std::vector<Device> devices = enumerate();
    std::vector<DeviceInfo> infos;
    for (size_t i = 0; i < devices.size(); ++i) infos.push_back(devices[i].info);
    const int index = select_device(infos, target, direction, error);
    if (index < 0) {
        error = std::string(k_backend) + ":" + target + ": " + error;
        return false;
    }
    found = devices[static_cast<size_t>(index)];
    return true;
}

AudioStreamBasicDescription float_format(Float64 rate_hz, UInt32 channels) {
    AudioStreamBasicDescription format;
    std::memset(&format, 0, sizeof(format));
    format.mSampleRate = rate_hz;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    format.mChannelsPerFrame = channels;
    format.mBitsPerChannel = k_float_bytes * k_bits_per_byte;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = k_float_bytes * channels;
    format.mBytesPerPacket = format.mBytesPerFrame;
    return format;
}

// The time from the render callback to the device's output: its buffer, latency, safety offset and stream latency.
uint32_t output_latency_ms(AudioObjectID device, Float64 rate_hz) {
    UInt32 buffer = 0;
    UInt32 latency = 0;
    UInt32 safety = 0;
    UInt32 stream_latency = 0;
    get(device, address(kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeOutput), buffer);
    get(device, address(kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput), latency);
    get(device, address(kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput), safety);
    const std::vector<AudioStreamID> streams =
        get_array<AudioStreamID>(device, address(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput));
    if (!streams.empty()) get(streams[0], address(kAudioStreamPropertyLatency), stream_latency);
    const double frames = static_cast<double>(buffer) + latency + safety + stream_latency;
    return static_cast<uint32_t>(std::ceil(frames * k_ms_per_s / rate_hz));
}

std::string describe(const DeviceInfo& info, uint32_t rate_hz, UInt32 channels) {
    return std::string(k_backend) + ":" + std::to_string(info.number) + " " + info.name + " (" +
           std::to_string(rate_hz) + " Hz, " + std::to_string(channels) + (channels == 1 ? " channel)" : " channels)");
}

// Reports a disconnection, a change of the nominal rate (the programs' resamplers are set for the old one) and
// processor overloads (a gap in the audio) to the device's status. Listeners run on the HAL's thread.
class DeviceWatch {
public:
    DeviceWatch() : device_(kAudioObjectUnknown), status_(nullptr), rate_hz_(0), watching_(false) {}
    ~DeviceWatch() { stop(); }

    void start(AudioObjectID device, DeviceStatus& status, uint32_t rate_hz) {
        device_ = device;
        status_ = &status;
        rate_hz_ = rate_hz;
        for (size_t i = 0; i < k_count; ++i) {
            const AudioObjectPropertyAddress where = address(selectors()[i]);
            AudioObjectAddPropertyListener(device_, &where, &DeviceWatch::changed, this);
        }
        watching_ = true;
    }

    void stop() {
        if (!watching_) return;
        for (size_t i = 0; i < k_count; ++i) {
            const AudioObjectPropertyAddress where = address(selectors()[i]);
            AudioObjectRemovePropertyListener(device_, &where, &DeviceWatch::changed, this);
        }
        watching_ = false;
    }

private:
    static const size_t k_count = 3;

    static const AudioObjectPropertySelector* selectors() {
        static const AudioObjectPropertySelector watched[k_count] = {
            kAudioDevicePropertyDeviceIsAlive, kAudioDevicePropertyNominalSampleRate, kAudioDeviceProcessorOverload};
        return watched;
    }

    static OSStatus changed(AudioObjectID device, UInt32 count, const AudioObjectPropertyAddress* addresses,
                            void* context) {
        DeviceWatch& self = *static_cast<DeviceWatch*>(context);
        for (UInt32 i = 0; i < count; ++i) {
            const AudioObjectPropertySelector selector = addresses[i].mSelector;
            if (selector == kAudioDevicePropertyDeviceIsAlive) {
                UInt32 alive = 1;
                if (get(device, address(kAudioDevicePropertyDeviceIsAlive), alive) && alive == 0)
                    self.status_->fail("the device was disconnected");
            } else if (selector == kAudioDevicePropertyNominalSampleRate) {
                Float64 rate = 0.0;
                if (get(device, address(kAudioDevicePropertyNominalSampleRate), rate) &&
                    static_cast<uint32_t>(std::lround(rate)) != self.rate_hz_)
                    self.status_->fail("the device's sample rate changed from " + std::to_string(self.rate_hz_) +
                                       " to " + std::to_string(std::lround(rate)) + " Hz; start again");
            } else if (selector == kAudioDeviceProcessorOverload) {
                self.status_->xrun();
            }
        }
        return noErr;
    }

    AudioObjectID device_;
    DeviceStatus* status_;
    uint32_t rate_hz_;
    bool watching_;
};

// An AUHAL unit, uninitialized and disposed with its owner.
class HalUnit {
public:
    HalUnit() : unit_(nullptr) {}
    ~HalUnit() {
        if (unit_ == nullptr) return;
        AudioUnitUninitialize(unit_);
        AudioComponentInstanceDispose(unit_);
    }
    HalUnit(const HalUnit&) = delete;
    HalUnit& operator=(const HalUnit&) = delete;

    bool create(std::string& error) {
        AudioComponentDescription description;
        std::memset(&description, 0, sizeof(description));
        description.componentType = kAudioUnitType_Output;
        description.componentSubType = kAudioUnitSubType_HALOutput;
        description.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent component = AudioComponentFindNext(nullptr, &description);
        if (component == nullptr) {
            error = "the CoreAudio HAL output unit is missing";
            return false;
        }
        const OSStatus status = AudioComponentInstanceNew(component, &unit_);
        if (status != noErr) {
            unit_ = nullptr;
            error = failure("creating the audio unit", status);
            return false;
        }
        return true;
    }

    AudioUnit get() const { return unit_; }

private:
    AudioUnit unit_;
};

// Enables one direction of the unit, disables the other and attaches the device.
OSStatus attach(AudioUnit unit, AudioObjectID device, bool input) {
    const UInt32 on = 1;
    const UInt32 off = 0;
    OSStatus status = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input,
                                           k_input_element, input ? &on : &off, sizeof(UInt32));
    if (status == noErr)
        status = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output,
                                      k_output_element, input ? &off : &on, sizeof(UInt32));
    if (status == noErr)
        status = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global,
                                      k_global_element, &device, sizeof(device));
    return status;
}

class CoreAudioCapture final : public CaptureBackend {
public:
    explicit CoreAudioCapture(const Device& device)
        : device_(device), channels_(0), rate_hz_(0), feed_(nullptr), running_(false) {}

    ~CoreAudioCapture() { stop(); }

    bool open(std::string& error) {
        if (!unit_.create(error)) return false;
        const AudioUnit unit = unit_.get();
        OSStatus status = attach(unit, device_.id, true);
        AudioStreamBasicDescription hardware;
        std::memset(&hardware, 0, sizeof(hardware));
        UInt32 size = sizeof(hardware);
        if (status == noErr)
            status = AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                                          k_input_element, &hardware, &size);
        if (status != noErr) {
            error = failure("opening the input", status);
            return false;
        }
        channels_ = hardware.mChannelsPerFrame;
        rate_hz_ = static_cast<uint32_t>(std::lround(hardware.mSampleRate));
        if (channels_ == 0 || rate_hz_ == 0) {
            error = "the device reports no input format";
            return false;
        }
        const AudioStreamBasicDescription client = float_format(hardware.mSampleRate, channels_);
        status = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, k_input_element,
                                      &client, sizeof(client));
        AURenderCallbackStruct callback;
        callback.inputProc = &CoreAudioCapture::on_input;
        callback.inputProcRefCon = this;
        if (status == noErr)
            status = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global,
                                          k_global_element, &callback, sizeof(callback));
        // The input callback renders a whole device buffer at once: the unit's slice must hold the largest one.
        UInt32 max_frames = 0;
        size = sizeof(max_frames);
        if (status == noErr)
            status = AudioUnitGetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                                          k_global_element, &max_frames, &size);
        AudioValueRange buffer_range;
        if (status == noErr &&
            get(device_.id, address(kAudioDevicePropertyBufferFrameSizeRange, kAudioObjectPropertyScopeInput),
                buffer_range) &&
            buffer_range.mMaximum > max_frames) {
            max_frames = static_cast<UInt32>(buffer_range.mMaximum);
            status = AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                                          k_global_element, &max_frames, sizeof(max_frames));
        }
        if (status == noErr) status = AudioUnitInitialize(unit);
        if (status != noErr) {
            error = failure("setting up the input", status);
            return false;
        }
        buffer_.assign(static_cast<size_t>(max_frames) * channels_, 0.0f);
        return true;
    }

    bool start(InputFeed& feed, std::string& error) override {
        feed_ = &feed;
        watch_.start(device_.id, feed, rate_hz_);
        const OSStatus status = AudioOutputUnitStart(unit_.get());
        if (status != noErr) {
            watch_.stop();
            error = failure("starting the input", status);
            return false;
        }
        running_ = true;
        return true;
    }

    void stop() override {
        if (running_) AudioOutputUnitStop(unit_.get());
        running_ = false;
        watch_.stop();
    }

    uint32_t sample_rate_hz() const override { return rate_hz_; }
    std::string description() const override { return describe(device_.info, rate_hz_, channels_); }

private:
    // Real-time: renders the new frames into the buffer allocated at open, hands them to the feed. No lock, no
    // allocation, no work beyond the copy.
    static OSStatus on_input(void* context, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* time,
                             UInt32 bus, UInt32 frames, AudioBufferList*) {
        CoreAudioCapture& self = *static_cast<CoreAudioCapture*>(context);
        if (static_cast<size_t>(frames) * self.channels_ > self.buffer_.size()) {
            self.feed_->xrun();
            return noErr;
        }
        AudioBufferList list;
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = self.channels_;
        list.mBuffers[0].mDataByteSize = frames * self.channels_ * k_float_bytes;
        list.mBuffers[0].mData = &self.buffer_[0];
        const OSStatus status = AudioUnitRender(self.unit_.get(), flags, time, bus, frames, &list);
        if (status != noErr) {
            self.feed_->xrun();
            return status;
        }
        self.feed_->deliver(static_cast<const float*>(list.mBuffers[0].mData), frames, self.channels_);
        return noErr;
    }

    Device device_;
    HalUnit unit_;
    DeviceWatch watch_;
    UInt32 channels_;
    uint32_t rate_hz_;
    std::vector<float> buffer_;
    InputFeed* feed_;
    bool running_;
};

class CoreAudioPlayback final : public PlaybackBackend {
public:
    explicit CoreAudioPlayback(const Device& device)
        : device_(device), channels_(0), rate_hz_(0), latency_ms_(0), feed_(nullptr), running_(false) {}

    ~CoreAudioPlayback() { stop(); }

    bool open(std::string& error) {
        if (!unit_.create(error)) return false;
        const AudioUnit unit = unit_.get();
        OSStatus status = attach(unit, device_.id, false);
        AudioStreamBasicDescription hardware;
        std::memset(&hardware, 0, sizeof(hardware));
        UInt32 size = sizeof(hardware);
        if (status == noErr)
            status = AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
                                          k_output_element, &hardware, &size);
        if (status != noErr) {
            error = failure("opening the output", status);
            return false;
        }
        channels_ = hardware.mChannelsPerFrame;
        rate_hz_ = static_cast<uint32_t>(std::lround(hardware.mSampleRate));
        if (channels_ == 0 || rate_hz_ == 0) {
            error = "the device reports no output format";
            return false;
        }
        const AudioStreamBasicDescription client = float_format(hardware.mSampleRate, channels_);
        status = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, k_output_element,
                                      &client, sizeof(client));
        AURenderCallbackStruct callback;
        callback.inputProc = &CoreAudioPlayback::on_render;
        callback.inputProcRefCon = this;
        if (status == noErr)
            status = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input,
                                          k_output_element, &callback, sizeof(callback));
        if (status == noErr) status = AudioUnitInitialize(unit);
        if (status != noErr) {
            error = failure("setting up the output", status);
            return false;
        }
        latency_ms_ = output_latency_ms(device_.id, hardware.mSampleRate);
        return true;
    }

    bool start(OutputFeed& feed, std::string& error) override {
        feed_ = &feed;
        watch_.start(device_.id, feed, rate_hz_);
        const OSStatus status = AudioOutputUnitStart(unit_.get());
        if (status != noErr) {
            watch_.stop();
            error = failure("starting the output", status);
            return false;
        }
        running_ = true;
        return true;
    }

    void stop() override {
        if (running_) AudioOutputUnitStop(unit_.get());
        running_ = false;
        watch_.stop();
    }

    uint32_t sample_rate_hz() const override { return rate_hz_; }
    uint32_t latency_ms() const override { return latency_ms_; }
    std::string description() const override {
        return describe(device_.info, rate_hz_, channels_) + ", latency " + std::to_string(latency_ms_) + " ms";
    }

private:
    // Real-time: the source is read in place into the device's buffer (interleaved, every channel the same).
    static OSStatus on_render(void* context, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames,
                              AudioBufferList* data) {
        CoreAudioPlayback& self = *static_cast<CoreAudioPlayback*>(context);
        if (data == nullptr || data->mNumberBuffers == 0) return noErr;
        AudioBuffer& first = data->mBuffers[0];
        if (first.mNumberChannels == 0 || first.mData == nullptr) return noErr;
        const UInt32 room = first.mDataByteSize / (k_float_bytes * first.mNumberChannels);
        self.feed_->fill(static_cast<float*>(first.mData), std::min(frames, room), first.mNumberChannels);
        for (UInt32 i = 1; i < data->mNumberBuffers; ++i)
            if (data->mBuffers[i].mData != nullptr)
                std::memset(data->mBuffers[i].mData, 0, data->mBuffers[i].mDataByteSize);
        return noErr;
    }

    Device device_;
    HalUnit unit_;
    DeviceWatch watch_;
    UInt32 channels_;
    uint32_t rate_hz_;
    uint32_t latency_ms_;
    OutputFeed* feed_;
    bool running_;
};

// CoreAudio runs at the device's nominal rate (changing it would change it for every program until reset in Audio
// MIDI Setup), so -r must name that rate or be left out.
bool rate_accepted(const Device& device, uint32_t rate_hz, std::string& error) {
    if (rate_hz == 0 || rate_hz == device.info.rate_hz) return true;
    error = std::string(k_backend) + ":" + std::to_string(device.info.number) + " (" + device.info.name +
            ") runs at its nominal rate of " + std::to_string(device.info.rate_hz) + " Hz, not " +
            std::to_string(rate_hz) + " Hz: leave -r out, or set the rate in Audio MIDI Setup";
    return false;
}

}  // namespace

const char* live_backend_name() {
    return k_backend;
}

std::vector<DeviceInfo> live_devices() {
    const std::vector<Device> devices = enumerate();
    std::vector<DeviceInfo> infos;
    for (size_t i = 0; i < devices.size(); ++i) infos.push_back(devices[i].info);
    return infos;
}

std::unique_ptr<InputDevice> open_live_input(const std::string& target, uint32_t rate_hz, std::string& error) {
    Device device;
    if (!resolve(target, Direction::input, device, error) || !rate_accepted(device, rate_hz, error))
        return std::unique_ptr<InputDevice>();
    std::unique_ptr<CoreAudioCapture> backend(new CoreAudioCapture(device));
    if (!backend->open(error)) {
        error = std::string(k_backend) + ":" + target + ": " + error;
        return std::unique_ptr<InputDevice>();
    }
    return std::unique_ptr<InputDevice>(new LiveInput(std::move(backend)));
}

std::unique_ptr<OutputDevice> open_live_output(const std::string& target, uint32_t rate_hz, std::string& error) {
    Device device;
    if (!resolve(target, Direction::output, device, error) || !rate_accepted(device, rate_hz, error))
        return std::unique_ptr<OutputDevice>();
    std::unique_ptr<CoreAudioPlayback> backend(new CoreAudioPlayback(device));
    if (!backend->open(error)) {
        error = std::string(k_backend) + ":" + target + ": " + error;
        return std::unique_ptr<OutputDevice>();
    }
    return std::unique_ptr<OutputDevice>(new LiveOutput(std::move(backend)));
}

}  // namespace pc
}  // namespace unlimited

#endif  // __APPLE__
