#include "../demo/cli.hpp"
#include "audio.hpp"
#include "support/loopback.hpp"
#include "test_harness.hpp"
#include "unlimited/audio_io.hpp"
#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"
#include "unlimited/wav_codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using unlimited::ConfigError;
using unlimited::Decoder;
using unlimited::DecoderConfig;
using unlimited::DecoderSink;
using unlimited::DecisionMode;
using unlimited::Encoder;
using unlimited::EncoderConfig;
using unlimited::EncoderSource;
using unlimited::Event;
using unlimited::EventType;
using unlimited::Passband;
using unlimited::WavOutput;
using unlimited::WavReader;
using unlimited::pc::MemoryInput;
using unlimited::pc::ResamplingSink;
using unlimited::cli::UsageError;

namespace {

using std::int16_t;
using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint8_t;
using test::count_of;

const uint32_t k_decoder_rate = unlimited::k_decoder_rate_hz;
const uint32_t k_rates[] = {8000, 11025, 22050, 44100, 48000};
const size_t k_bytes = 40;                    // fits the encoder queue, so EncoderSource alone sends it all
const size_t k_odd_chunk = 37;                // device chunk that never lines up with a block or a slot
const size_t k_reader_chunk = 1000;           // samples per WavReader::read
const double k_lead_slots = 15.0;             // silence before a file, as unlimited_decode hears it (spec V6)
const double k_drain_windows = 2.0;           // silence after the input, besides the look-ahead
const double k_slot_tolerance = 0.005;        // measured T within 0.5 % (L1)
const double k_us_per_ms = 1e3;
const double k_ms_per_s = 1e3;
const uint16_t k_bytes_per_sample = 2;
const uint32_t k_seed = 13;

Passband passband(uint16_t low_hz, uint16_t high_hz) {
    Passband band;
    band.low_hz = low_hz;
    band.high_hz = high_hz;
    return band;
}

class EventLog {
public:
    static void on_event(const Event& event, void* context) {
        static_cast<EventLog*>(context)->events.push_back(event);
    }

    size_t count(EventType type) const {
        size_t n = 0;
        for (size_t i = 0; i < events.size(); ++i) n += events[i].type == type ? 1 : 0;
        return n;
    }

    std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> values;
        for (size_t i = 0; i < events.size(); ++i)
            if (events[i].type == EventType::byte) values.push_back(events[i].value);
        return values;
    }

    std::vector<Event> events;
};

bool same_bits(float a, float b) {
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}

bool same_event(const Event& a, const Event& b) {
    return a.type == b.type && a.reason == b.reason && a.state == b.state && a.flags == b.flags &&
           a.value == b.value && a.slot == b.slot && a.level_pct == b.level_pct &&
           a.threshold_pct == b.threshold_pct && a.start_pct == b.start_pct && a.stop_pct == b.stop_pct &&
           std::memcmp(a.soft, b.soft, sizeof(a.soft)) == 0 && a.byte_index == b.byte_index &&
           same_bits(a.tone_hz, b.tone_hz) && same_bits(a.slot_ms, b.slot_ms) && same_bits(a.snr_db, b.snr_db);
}

void check_same_events(const EventLog& a, const EventLog& b) {
    REQUIRE(a.events.size() == b.events.size());
    size_t differences = 0;
    for (size_t i = 0; i < a.events.size(); ++i) differences += same_event(a.events[i], b.events[i]) ? 0 : 1;
    CHECK_EQ(differences, size_t(0));
}

class MemoryByteSink final : public unlimited::ByteSink {
public:
    bool write(const uint8_t* data, size_t size) override {
        if (position_ + size > bytes.size()) bytes.resize(position_ + size);
        std::copy(data, data + size, bytes.begin() + static_cast<std::ptrdiff_t>(position_));
        position_ += size;
        return true;
    }

    bool seek(uint32_t position) override {
        if (position > bytes.size()) return false;
        position_ = position;
        return true;
    }

    std::vector<uint8_t> bytes;

private:
    size_t position_ = 0;
};

class MemoryByteSource final : public unlimited::ByteSource {
public:
    explicit MemoryByteSource(const std::vector<uint8_t>& bytes) : bytes_(bytes), position_(0) {}

    size_t read(uint8_t* data, size_t size) override {
        const size_t n = std::min(size, bytes_.size() - position_);
        std::copy(bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
                  bytes_.begin() + static_cast<std::ptrdiff_t>(position_ + n), data);
        position_ += n;
        return n;
    }

    bool seek(uint32_t position) override {
        if (position > bytes_.size()) return false;
        position_ = position;
        return true;
    }

private:
    const std::vector<uint8_t>& bytes_;
    size_t position_;
};

double slot_samples(const DecoderConfig& config) {
    return config.slot_us / k_us_per_ms * k_decoder_rate / k_ms_per_s;
}

void silence(unlimited::SampleSink& sink, size_t count) {
    const std::vector<int16_t> zeros(count, 0);
    sink.write(zeros.data(), zeros.size());
}

// A sender at `speed` bytes/s and `rate` Hz in the AM passband (every speed fits it).
EncoderConfig at_speed(float speed, uint32_t rate) {
    EncoderConfig config;
    config.sample_rate_hz = rate;
    config.slot_us = unlimited::slot_us_for_speed(speed);
    config.passband = passband(unlimited::k_am_passband_low_hz, unlimited::k_am_passband_high_hz);
    return config;
}

// Encoder at `rate` -> EncoderSource -> WavOutput -> memory -> WavReader -> ResamplingSink -> DecoderSink: the demos'
// file path, with the silence unlimited_decode hears around a file. The receiver is told the speed and the passband.
void round_trip(float speed, uint32_t rate) {
    const std::vector<uint8_t> data = unlimited::loopback::random_bytes(k_bytes, k_seed + rate);
    const EncoderConfig config = at_speed(speed, rate);
    Encoder encoder(config);
    REQUIRE(encoder.write(data.data(), data.size()) == data.size());
    REQUIRE(encoder.start());
    const uint32_t duration = encoder.duration_samples(data.size());

    MemoryByteSink file;
    WavOutput output(file, duration);
    EncoderSource source(encoder);
    REQUIRE(output.start(source, rate));
    REQUIRE(output.wait());

    MemoryByteSource input(file.bytes);
    WavReader reader;
    REQUIRE(reader.open(input));
    CHECK_EQ(reader.format().sample_rate_hz, rate);
    CHECK_EQ(reader.format().channels, 1);
    CHECK_EQ(reader.format().data_bytes, duration * k_bytes_per_sample);

    DecoderConfig decoder_config;
    decoder_config.slot_us = config.slot_us;
    decoder_config.passband = config.passband;
    EventLog log;
    Decoder decoder(decoder_config, &EventLog::on_event, &log);
    DecoderSink decoder_sink(decoder);
    silence(decoder_sink, static_cast<size_t>(std::ceil(k_lead_slots * slot_samples(decoder_config))));
    ResamplingSink resampling(decoder_sink, rate);
    std::vector<int16_t> chunk(k_reader_chunk);
    size_t samples = 0;
    for (size_t n = 0; (n = reader.read(chunk.data(), chunk.size())) > 0; samples += n)
        resampling.write(chunk.data(), n);
    resampling.flush();
    silence(decoder_sink, decoder.lookahead_samples() +
                              static_cast<size_t>(std::ceil(k_drain_windows * unlimited::k_window_slots *
                                                            slot_samples(decoder_config))));

    CHECK_EQ(samples, size_t(duration));
    CHECK_EQ(log.count(EventType::locked), size_t(1));
    CHECK_EQ(log.count(EventType::end), size_t(1));
    CHECK_EQ(log.count(EventType::lost), size_t(0));
    const std::vector<uint8_t> received = log.bytes();
    CHECK(received == data);
    const double slot_ms = config.slot_us / k_us_per_ms;
    double worst = 0.0;
    for (size_t i = 0; i < log.events.size(); ++i) {
        if (log.events[i].type != EventType::byte) continue;
        worst = std::max(worst, std::fabs(log.events[i].slot_ms / slot_ms - 1.0));
    }
    CHECK(worst <= k_slot_tolerance);
    NOTE("%.0f bytes/s at %u Hz: %zu of %zu bytes, worst T error %.3f %%", static_cast<double>(speed),
         static_cast<unsigned>(rate), received.size(), data.size(), 100.0 * worst);
}

}  // namespace

// U17: the DecoderSink adapter, fed by a driver in odd chunks, gives the events of one direct process() call, slot
// events included.
TEST(demo_io_decoder_sink_equals_direct_process) {
    const std::vector<uint8_t> data = unlimited::loopback::random_bytes(k_bytes, k_seed);
    const EncoderConfig sender = unlimited::loopback::speed_config(12.0f);
    const unlimited::loopback::Recording recording = unlimited::loopback::single(data, sender);
    const std::vector<int16_t>& samples = recording.samples;
    const DecoderConfig config = unlimited::loopback::receiver_for(sender);

    EventLog direct;
    Decoder direct_decoder(config, &EventLog::on_event, &direct);
    direct_decoder.process(samples.data(), samples.size());
    CHECK(direct.bytes() == data);
    CHECK_EQ(direct.count(EventType::slot), data.size() * unlimited::k_bits_per_byte);

    EventLog adapted;
    Decoder adapted_decoder(config, &EventLog::on_event, &adapted);
    DecoderSink sink(adapted_decoder);
    MemoryInput input(samples, k_decoder_rate, k_odd_chunk);
    REQUIRE(input.start(sink, k_decoder_rate));
    REQUIRE(input.wait());
    check_same_events(direct, adapted);

    // At 8 kHz the ResamplingSink is a plain copy, so the demo's decode path is bit-exact as well.
    EventLog resampled;
    Decoder resampled_decoder(config, &EventLog::on_event, &resampled);
    DecoderSink resampled_sink(resampled_decoder);
    ResamplingSink resampling(resampled_sink, k_decoder_rate);
    MemoryInput resampled_input(samples, k_decoder_rate, k_odd_chunk);
    REQUIRE(resampled_input.start(resampling, k_decoder_rate));
    resampling.flush();
    check_same_events(direct, resampled);
    NOTE("%zu events, %zu bytes", direct.events.size(), data.size());
}

// L13: sample-rate independence, 0 errors clean, T as sent, at each speed.
TEST(demo_io_l13_rate_independence_6) {
    for (size_t i = 0; i < count_of(k_rates); ++i) round_trip(6.0f, k_rates[i]);
}

TEST(demo_io_l13_rate_independence_1) {
    for (size_t i = 0; i < count_of(k_rates); ++i) round_trip(1.0f, k_rates[i]);
}

TEST(demo_io_l13_rate_independence_12) {
    for (size_t i = 0; i < count_of(k_rates); ++i) round_trip(12.0f, k_rates[i]);
}

TEST(demo_io_l13_rate_independence_25) {
    for (size_t i = 0; i < count_of(k_rates); ++i) round_trip(25.0f, k_rates[i]);
}

// demo/cli.hpp: the command-line helpers shared by the demos.
namespace {

const size_t k_file_bytes = 330;

template <typename F>
bool throws_usage(F f) {
    try {
        f();
    } catch (const UsageError&) {
        return true;
    }
    return false;
}

std::string temp_path(const std::string& name) {
    const char* dir = std::getenv("TMPDIR");
    std::string base = (dir != nullptr && *dir != '\0') ? dir : "/tmp";
    if (base[base.size() - 1] != '/') base += '/';
    return base + "unlimited_demo_io_" + name;
}

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST(demo_cli_numbers_and_names) {
    using namespace unlimited::cli;
    CHECK_EQ(to_number("--x", "12.5"), 12.5);
    CHECK_EQ(to_number("--x", "-3"), -3.0);
    const char* bad[] = {"", "abc", "12x", "1e999", "nan"};
    for (size_t i = 0; i < count_of(bad); ++i) CHECK(throws_usage([&] { to_number("--x", bad[i]); }));

    CHECK_EQ(to_integer<uint8_t>("--x", 254.6), 255);
    CHECK_EQ(to_integer<uint16_t>("--x", 2.5), 3);
    CHECK(throws_usage([] { to_integer<uint8_t>("--x", 255.6); }));
    CHECK(throws_usage([] { to_integer<uint32_t>("--x", -1.0); }));
    CHECK_EQ(to_clamped<uint8_t>(300.0), 255);  // far out of range still reaches check()
    CHECK_EQ(to_clamped<uint16_t>(-5.0), 0);
    CHECK_EQ(to_clamped<uint32_t>(16000.4), 16000u);

    const std::vector<double> pair = to_fields("--x", "300:2700", 2, 2);
    REQUIRE(pair.size() == 2u);
    CHECK_EQ(pair[0], 300.0);
    CHECK_EQ(pair[1], 2700.0);
    CHECK_EQ(to_fields("--x", "5", 1, 2).size(), 1u);
    CHECK(throws_usage([] { to_fields("--x", "5", 2, 2); }));
    CHECK(throws_usage([] { to_fields("--x", "1:2:3", 1, 2); }));
    CHECK(throws_usage([] { to_fields("--x", "1:", 2, 2); }));
    const Passband narrow = to_passband("--passband", "300:2100");
    CHECK_EQ(narrow.low_hz, 300);
    CHECK_EQ(narrow.high_hz, 2100);
    CHECK(throws_usage([] { to_passband("--passband", "300"); }));
    CHECK(throws_usage([] { to_passband("--passband", "-1:2700"); }));

    // --bps: rounded to 0.01 bytes/s; a speed outside 1..25 reaches check() as a slot out of range.
    CHECK_EQ(to_slot_us("--bps", "6"), 16667u);
    CHECK_EQ(to_slot_us("--bps", "3.33"), 30030u);
    CHECK_EQ(to_slot_us("--bps", "6.004"), 16667u);
    CHECK_EQ(to_slot_us("--bps", "25"), 4000u);
    CHECK(!unlimited::slot_valid(to_slot_us("--bps", "26")));
    CHECK(throws_usage([] { to_slot_us("--bps", "0"); }));
    CHECK(throws_usage([] { to_slot_us("--bps", "-6"); }));
    CHECK(throws_usage([] { to_slot_us("--bps", "fast"); }));
    // --threshold PCT|auto
    DecoderConfig receiver;
    to_threshold("--threshold", "auto", receiver);
    CHECK(receiver.decision_mode == DecisionMode::adaptive);
    to_threshold("--threshold", "55", receiver);
    CHECK(receiver.decision_mode == DecisionMode::fixed);
    CHECK_EQ(unsigned(receiver.threshold_percent), 55u);
    to_threshold("--threshold", "95", receiver);
    CHECK(receiver.check() == ConfigError::threshold);
    CHECK(throws_usage([&] { to_threshold("--threshold", "high", receiver); }));

    char program[] = "demo";
    char option[] = "--tone";
    char value[] = "1500";
    char* argv[] = {program, option, value, option};
    Arguments args(static_cast<int>(count_of(argv)), argv);
    std::string next;
    REQUIRE(args.next(next));
    CHECK_EQ(next, std::string("--tone"));
    CHECK_EQ(args.value(next), std::string("1500"));
    REQUIRE(args.next(next));
    CHECK(throws_usage([&] { args.value(next); }));
    CHECK(!args.next(next));
}

TEST(demo_cli_speed_and_numbers_in_words) {
    using namespace unlimited::cli;
    CHECK_EQ(speed_text(16667), std::string("6.00 bytes/s = 48 bit/s, slot T 16.667 ms"));
    CHECK_EQ(speed_text(4000), std::string("25.00 bytes/s = 200 bit/s, slot T 4 ms"));
    CHECK_EQ(speed_text(30030), std::string("3.33 bytes/s = 26.64 bit/s, slot T 30.03 ms"));
    CHECK_EQ(speed_number(100000), std::string("1.00"));
    CHECK_EQ(ms_text(16.0), std::string("16"));
    CHECK_EQ(ms_text(12.5), std::string("12.5"));
    CHECK_EQ(ms_text(16.667), std::string("16.667"));
    CHECK_EQ(trimmed(31.25, 2), std::string("31.25"));
    CHECK_EQ(trimmed(250.0, 2), std::string("250"));
    CHECK_EQ(threshold_text(DecoderConfig()), std::string("decision line at 70 % of the reference"));
    DecoderConfig adaptive;
    adaptive.decision_mode = DecisionMode::adaptive;
    CHECK(has(threshold_text(adaptive), "adaptive decision line"));
}

// The bandwidth line (spec 1.3), from the core's occupied_band() and passband_fit(): the shift tolerance stops where the
// band leaves the passband or the pitch leaves the receiver's search.
TEST(demo_cli_bandwidth_line) {
    using unlimited::cli::bandwidth_line;
    const EncoderConfig hf;  // 6 bytes/s
    CHECK_EQ(bandwidth_line(hf), std::string("occupied bandwidth 264 Hz (1368-1632 Hz); passband 300-2700 Hz: fits; "
                                             "shift tolerance -1068/+1068 Hz"));
    EncoderConfig narrow = hf;  // a 1.8 kHz filter
    narrow.passband = passband(300, 2100);
    CHECK_EQ(bandwidth_line(narrow), std::string("occupied bandwidth 264 Hz (1368-1632 Hz); passband 300-2100 Hz: "
                                                 "fits; shift tolerance -1068/+468 Hz"));
    EncoderConfig fm = at_speed(25.0f, k_decoder_rate);
    fm.passband = passband(300, 3000);
    CHECK(has(bandwidth_line(fm), "1100 Hz (950-2050 Hz); passband 300-3000 Hz: fits; shift tolerance -650/+950 Hz"));
    EncoderConfig wide = hf;  // a 3.0 kHz filter: the search ends at 300 and 2700 Hz, before the filter does
    wide.passband = passband(100, 3000);
    CHECK(has(bandwidth_line(wide), "passband 100-3000 Hz: fits; shift tolerance -1200/+1200 Hz"));
    EncoderConfig low = hf;  // 400 Hz at 6 bytes/s: 268-532 Hz
    low.tone_hz = 400;
    CHECK(has(bandwidth_line(low), "passband 300-2700 Hz: does not fit (32 Hz below the passband)"));
    EncoderConfig squeezed = at_speed(12.0f, k_decoder_rate);
    squeezed.passband = passband(1250, 1750);  // 530 Hz into 500 Hz
    CHECK(has(bandwidth_line(squeezed), "does not fit (15 Hz below and 15 Hz above the passband)"));
    EncoderConfig inverted = hf;
    inverted.passband = passband(2700, 300);
    CHECK(has(bandwidth_line(inverted), "passband 2700-300 Hz: not a valid passband"));
    // The decoder's line: the measured pitch and T of the received signal against its own passband and search.
    const unlimited::Band received = unlimited::occupied_band(unlimited::cli::received_tone_hz(1580.2f),
                                                              unlimited::cli::received_slot_us(16.667f));
    CHECK_EQ(received.low_hz, 1448);
    CHECK_EQ(received.high_hz, 1712);
    CHECK(has(bandwidth_line(DecoderConfig(), 1580.2f, 16.667f),
              "(1448-1712 Hz); passband 300-2700 Hz: fits; shift tolerance -1148/+988 Hz"));
    DecoderConfig fm_receiver;
    fm_receiver.slot_us = fm.slot_us;
    fm_receiver.passband = fm.passband;
    CHECK(has(bandwidth_line(fm_receiver, 1100.0f, 4.0f),
              "occupied bandwidth 1100 Hz (550-1650 Hz); passband 300-3000 Hz: fits; shift tolerance -250/+1350 Hz"));
}

// Every EncoderConfig::check() and DecoderConfig::check() refusal is explained in words, naming the option to
// change (U22: the demos map every ConfigError); no rule falls through to the generic text.
TEST(demo_cli_every_config_error_is_explained) {
    using unlimited::cli::decoder_problem;
    using unlimited::cli::encoder_problem;
    const EncoderConfig hf;
    CHECK_EQ(encoder_problem(hf), std::string());
    struct EncoderCase {
        ConfigError error;
        const char* expected;
    };
    const EncoderCase encoder_cases[] = {
        {ConfigError::sample_rate, "the sample rate 7999 Hz is outside 8000..192000 Hz (--rate)"},
        {ConfigError::tone, "the pitch 299 Hz is outside 300..2700 Hz (--tone)"},
        {ConfigError::slot, "the speed 25.01 bytes/s is outside 1.00..25.00 bytes/s (--bps)"},
        {ConfigError::passband, "the passband 2700-300 Hz is not valid"},
        {ConfigError::outside_passband, "move --tone to 432..2568 Hz"},
        {ConfigError::amplitude, "the level is too low"}};
    EncoderConfig broken[count_of(encoder_cases)];
    for (size_t i = 0; i < count_of(broken); ++i) broken[i] = hf;
    broken[0].sample_rate_hz = 7999;
    broken[1].tone_hz = 299;
    broken[2].slot_us = 3999;
    broken[3].passband = passband(2700, 300);
    broken[4].tone_hz = 400;
    broken[5].amplitude = 0;
    for (size_t i = 0; i < count_of(encoder_cases); ++i) {
        const std::string text = encoder_problem(broken[i]);
        CHECK(broken[i].check() == encoder_cases[i].error);
        if (!CHECK(has(text, encoder_cases[i].expected))) NOTE("case %zu: %s", i, text.c_str());
    }
    EncoderConfig wide = at_speed(25.0f, k_decoder_rate);
    wide.passband = passband(1250, 1750);
    CHECK(has(encoder_problem(wide), "only 500 Hz wide; use a slower speed (--bps) or a wider --passband"));
    EncoderConfig slow = hf;
    slow.slot_us = 100001;
    CHECK(has(encoder_problem(slow), "the speed 1.00 bytes/s is outside"));

    const DecoderConfig ssb;
    CHECK_EQ(decoder_problem(ssb), std::string());
    DecoderConfig decoders[] = {ssb, ssb, ssb, ssb, ssb};
    decoders[0].slot_us = 0;
    decoders[1].passband = passband(2700, 300);
    decoders[2].passband = passband(300, 360);  // no room left for a pitch
    decoders[3].threshold_percent = 49;
    decoders[4].decision_mode = static_cast<DecisionMode>(7);
    const char* decoder_expected[] = {"the speed 0.00 bytes/s is outside 1.00..25.00 bytes/s (--bps)",
                                      "the passband 2700-300 Hz is not valid",
                                      "the passband 300-360 Hz leaves no pitch to search for at 6.00 bytes/s",
                                      "the decision threshold 49 % is outside 50..90 % of the reference",
                                      "the decision rule must be fixed or adaptive"};
    for (size_t i = 0; i < count_of(decoders); ++i) {
        const std::string text = decoder_problem(decoders[i]);
        if (!CHECK(has(text, decoder_expected[i]))) NOTE("decoder case %zu: %s", i, text.c_str());
    }
}

TEST(demo_cli_read_file) {
    const std::string path = temp_path("read_file.bin");
    const std::vector<uint8_t> data = unlimited::loopback::random_bytes(k_file_bytes, k_seed);
    {
        std::ofstream file(path.c_str(), std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    std::vector<uint8_t> back;
    CHECK(unlimited::cli::read_file(path, back));
    CHECK(back == data);
    std::remove(path.c_str());
    CHECK(!unlimited::cli::read_file(temp_path("missing.bin"), back));
}
