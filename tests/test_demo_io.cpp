#include "../demo/cli.hpp"
#include "audio.hpp"
#include "support/loopback.hpp"
#include "test_harness.hpp"
#include "unlimited/audio_io.hpp"
#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"
#include "unlimited/packet.hpp"
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
using unlimited::Preset;
using unlimited::Profile;
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
const double k_drain_slots = 8.0;             // silence after the input, in the longest slots
const double k_slot_tolerance = 0.005;        // measured T within 0.5 % (L1)
const double k_us_per_ms = 1e3;
const double k_ms_per_s = 1e3;
const uint16_t k_bytes_per_sample = 2;
const uint32_t k_seed = 13;

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
           a.value == b.value && a.slot == b.slot && a.bits_per_package == b.bits_per_package &&
           a.level_pct == b.level_pct && a.threshold_pct == b.threshold_pct && a.start_pct == b.start_pct &&
           a.stop_pct == b.stop_pct && std::memcmp(a.soft, b.soft, sizeof(a.soft)) == 0 &&
           a.package_index == b.package_index && a.byte_index == b.byte_index && same_bits(a.tone_hz, b.tone_hz) &&
           same_bits(a.slot_ms, b.slot_ms) && same_bits(a.snr_db, b.snr_db);
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

void drain(unlimited::SampleSink& sink, const DecoderConfig& config) {
    const std::vector<int16_t> silence(
        static_cast<size_t>(k_drain_slots * config.max_slot_ms() * k_decoder_rate / k_ms_per_s), 0);
    sink.write(silence.data(), silence.size());
}

// Encoder at `rate` -> EncoderSource -> WavOutput -> memory -> WavReader -> ResamplingSink -> DecoderSink: the demos'
// file path. The receiver is told nothing about T or N.
void round_trip(Preset preset, Profile profile, uint32_t rate) {
    const std::vector<uint8_t> data = unlimited::loopback::random_bytes(k_bytes, k_seed + rate);
    const EncoderConfig config = EncoderConfig::from_preset(preset, rate);
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

    const DecoderConfig decoder_config = DecoderConfig::for_profile(profile);
    EventLog log;
    Decoder decoder(decoder_config, &EventLog::on_event, &log);
    DecoderSink decoder_sink(decoder);
    ResamplingSink resampling(decoder_sink, rate);
    std::vector<int16_t> chunk(k_reader_chunk);
    size_t samples = 0;
    for (size_t n = 0; (n = reader.read(chunk.data(), chunk.size())) > 0; samples += n)
        resampling.write(chunk.data(), n);
    resampling.flush();
    drain(decoder_sink, decoder_config);

    CHECK_EQ(samples, size_t(duration));
    CHECK_EQ(log.count(EventType::locked), size_t(1));
    CHECK_EQ(log.count(EventType::end), size_t(1));
    CHECK_EQ(log.count(EventType::lost), size_t(0));
    for (size_t i = 0; i < log.events.size(); ++i) {
        if (log.events[i].type != EventType::locked) continue;
        CHECK_EQ(log.events[i].bits_per_package, config.bits_per_package);  // N learnt from the signal
    }
    const std::vector<uint8_t> received = log.bytes();
    CHECK(received == data);
    const double slot_ms = config.slot_us / k_us_per_ms;
    double worst = 0.0;
    for (size_t i = 0; i < log.events.size(); ++i) {
        if (log.events[i].type != EventType::byte) continue;
        worst = std::max(worst, std::fabs(log.events[i].slot_ms / slot_ms - 1.0));
    }
    CHECK(worst <= k_slot_tolerance);
    NOTE("rate %u: %zu of %zu bytes, worst T error %.3f %%", static_cast<unsigned>(rate), received.size(),
         data.size(), 100.0 * worst);
}

}  // namespace

// U17: the DecoderSink adapter, fed by a driver in odd chunks, gives the events of one direct process() call, slot and
// package events included.
TEST(demo_io_decoder_sink_equals_direct_process) {
    const std::vector<uint8_t> data = unlimited::loopback::random_bytes(k_bytes, k_seed);
    const unlimited::loopback::Recording recording =
        unlimited::loopback::single(data, unlimited::loopback::preset_config(Preset::hf_fast));
    const std::vector<int16_t>& samples = recording.samples;
    const DecoderConfig config = DecoderConfig::for_profile(Profile::ssb);

    EventLog direct;
    Decoder direct_decoder(config, &EventLog::on_event, &direct);
    direct_decoder.process(samples.data(), samples.size());
    CHECK(direct.bytes() == data);
    CHECK(direct.count(EventType::slot) >= data.size() * unlimited::k_bits_per_byte);
    CHECK(direct.count(EventType::package) >= data.size());

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

// L13: sample-rate independence, 0 errors clean, the learnt T and N are the sent ones.
TEST(demo_io_l13_rate_independence_hf) {
    for (size_t i = 0; i < count_of(k_rates); ++i) round_trip(Preset::hf, Profile::ssb, k_rates[i]);
}

TEST(demo_io_l13_rate_independence_hf_slow) {
    for (size_t i = 0; i < count_of(k_rates); ++i) round_trip(Preset::hf_slow, Profile::ssb, k_rates[i]);
}

TEST(demo_io_l13_rate_independence_am) {
    for (size_t i = 0; i < count_of(k_rates); ++i) round_trip(Preset::am, Profile::am, k_rates[i]);
}

TEST(demo_io_l13_rate_independence_fm) {
    for (size_t i = 0; i < count_of(k_rates); ++i) round_trip(Preset::fm, Profile::fm, k_rates[i]);
}

// demo/cli.hpp: the command-line helpers shared by the demos.
namespace {

const size_t k_ax25_payload = 330;
const size_t k_no_packets = 0;

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

struct Packets {
    std::vector<uint8_t> payload;
    size_t count = 0;

    static void on_packet(const uint8_t* data, uint16_t size, uint8_t, void* context) {
        Packets* self = static_cast<Packets*>(context);
        self->payload.insert(self->payload.end(), data, data + size);
        ++self->count;
    }
};

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

Passband passband(uint16_t low_hz, uint16_t high_hz) {
    Passband band;
    band.low_hz = low_hz;
    band.high_hz = high_hz;
    return band;
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

    CHECK(find_name(k_presets, "--preset", "hf_slow").value == Preset::hf_slow);
    CHECK(find_name(k_presets, "--preset", "fm").value == Preset::fm);
    CHECK(find_name(k_profiles, "--profile", "am").value == Profile::am);
    CHECK(find_name(k_rules, "--rule", "fixed").value == DecisionMode::fixed_ratio);
    CHECK(throws_usage([] { find_name(k_presets, "--preset", "hf_robust"); }));  // a v0.2 name
    std::string message;
    try {
        find_name(k_profiles, "--profile", "usb");
    } catch (const UsageError& error) {
        message = error.what();
    }
    CHECK_EQ(message, std::string("--profile: 'usb' is not one of ssb|am|fm"));
    CHECK_EQ(std::string(name_of(k_presets, Preset::hf_fast)), std::string("hf_fast"));

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

TEST(demo_cli_rates_and_numbers_in_words) {
    using namespace unlimited::cli;
    const double k_rate_tolerance = 0.005;
    CHECK_NEAR(net_bit_rate(8, 32.0), 27.78, k_rate_tolerance);   // hf_slow (spec 1.7)
    CHECK_NEAR(net_bit_rate(8, 16.0), 55.56, k_rate_tolerance);   // hf
    CHECK_NEAR(net_bit_rate(8, 8.0), 111.11, k_rate_tolerance);   // hf_fast
    CHECK_NEAR(net_bit_rate(16, 8.0), 117.65, k_rate_tolerance);  // am
    CHECK_NEAR(net_bit_rate(16, 4.0), 235.29, k_rate_tolerance);  // fm
    CHECK_EQ(net_bit_rate(8, 0.0), 0.0);
    CHECK_EQ(ms_text(16.0), std::string("16"));
    CHECK_EQ(ms_text(12.5), std::string("12.5"));
    CHECK_EQ(ms_text(16.001), std::string("16.001"));
    CHECK_EQ(trimmed(31.25, k_baud_decimals), std::string("31.25"));
    CHECK_EQ(trimmed(250.0, k_baud_decimals), std::string("250"));
    CHECK_EQ(rule_text(DecoderConfig()), std::string("smart decision line"));
    DecoderConfig fixed_rule;
    fixed_rule.decision_mode = DecisionMode::fixed_ratio;
    CHECK_EQ(rule_text(fixed_rule), std::string("fixed decision line at 70 % of the reference"));
}

// The bandwidth line of spec 7, from the core's occupied_band() and passband_fit(): the shift tolerance stops where the
// band leaves the passband or the pitch leaves the receiver's search (spec 1.5).
TEST(demo_cli_bandwidth_line) {
    using unlimited::cli::bandwidth_line;
    const EncoderConfig hf = EncoderConfig::from_preset(Preset::hf, k_decoder_rate);
    CHECK_EQ(bandwidth_line(hf), std::string("occupied bandwidth 276 Hz (1362-1638 Hz); passband 300-2700 Hz: fits; "
                                             "shift tolerance -1062/+1062 Hz"));
    EncoderConfig narrow = hf;  // a 1.8 kHz filter (spec 1.5 table)
    narrow.passband = passband(300, 2100);
    CHECK_EQ(bandwidth_line(narrow), std::string("occupied bandwidth 276 Hz (1362-1638 Hz); passband 300-2100 Hz: "
                                                 "fits; shift tolerance -1062/+462 Hz"));
    const EncoderConfig fm = EncoderConfig::from_preset(Preset::fm, k_decoder_rate);
    // The fm preset's receiver (the fm profile) searches from 1000 Hz: 500 Hz below, not the filter's 650 Hz.
    CHECK(has(bandwidth_line(fm), "1100 Hz (950-2050 Hz); passband 300-3000 Hz: fits; shift tolerance -500/+950 Hz"));
    EncoderConfig wide = hf;  // a 3.0 kHz filter: the search ends at 300 and 2700 Hz, before the filter does
    wide.passband = passband(100, 3000);
    CHECK(has(bandwidth_line(wide), "passband 100-3000 Hz: fits; shift tolerance -1200/+1200 Hz"));
    EncoderConfig low = hf;  // 400 Hz at 16 ms: 262-538 Hz
    low.tone_hz = 400;
    CHECK(has(bandwidth_line(low), "passband 300-2700 Hz: does not fit (38 Hz below the passband)"));
    EncoderConfig squeezed = EncoderConfig::from_preset(Preset::hf_fast, k_decoder_rate);
    squeezed.passband = passband(1250, 1750);  // 550 Hz into 500 Hz
    CHECK(has(bandwidth_line(squeezed), "does not fit (25 Hz below and 25 Hz above the passband)"));
    EncoderConfig inverted = hf;
    inverted.passband = passband(2700, 300);
    CHECK(has(bandwidth_line(inverted), "passband 2700-300 Hz: not a valid passband"));
    // The decoder's line: the measured pitch and T of the received signal against its own passband and search.
    const unlimited::Band received = unlimited::cli::received_band(1580.2f, 15.9996f);
    CHECK_EQ(received.low_hz, 1442);
    CHECK_EQ(received.high_hz, 1718);
    CHECK(has(bandwidth_line(DecoderConfig(), 1580.2f, 15.9996f),
              "(1442-1718 Hz); passband 300-2700 Hz: fits; shift tolerance -1142/+982 Hz"));
    CHECK(has(bandwidth_line(DecoderConfig::for_profile(unlimited::Profile::fm), 1100.0f, 4.0f),
              "occupied bandwidth 1100 Hz (550-1650 Hz); passband 300-3000 Hz: fits; shift tolerance -100/+1350 Hz"));
}

// Every EncoderConfig::check() and DecoderConfig::check() refusal is explained in words, naming the option to
// change (spec 7, U22: the demos map every ConfigError); no rule falls through to the generic text.
TEST(demo_cli_every_config_error_is_explained) {
    using unlimited::cli::decoder_problem;
    using unlimited::cli::encoder_problem;
    const EncoderConfig hf = EncoderConfig::from_preset(Preset::hf, k_decoder_rate);
    CHECK_EQ(encoder_problem(hf), std::string());
    struct EncoderCase {
        ConfigError error;
        const char* expected;
    };
    const EncoderCase encoder_cases[] = {
        {ConfigError::sample_rate, "the sample rate 7999 Hz is outside 8000..192000 Hz (--rate)"},
        {ConfigError::tone, "the pitch 299 Hz is outside 300..2700 Hz (--tone)"},
        {ConfigError::slot, "the slot length T = 3.999 ms is outside 4..128 ms"},
        {ConfigError::fast_tone, "slots shorter than 8 ms need a pitch of at least 1000 Hz, and the pitch is 999 Hz"},
        {ConfigError::bits_per_package, "the bits per package N = 0 is outside 1.."},
        {ConfigError::package_length, "lasts (N + 1) x T = 1152.018 ms, more than the 1152 ms limit: use --bits 16"},
        {ConfigError::passband, "the passband 2700-300 Hz is not valid"},
        {ConfigError::outside_passband, "move --tone to 438..2562 Hz"},
        {ConfigError::sync_markers, "the sync train must have 8..32 markers, not 33 (--sync)"},
        {ConfigError::amplitude, "the level is too low"}};
    EncoderConfig broken[count_of(encoder_cases)];
    for (size_t i = 0; i < count_of(broken); ++i) broken[i] = hf;
    broken[0].sample_rate_hz = 7999;
    broken[1].tone_hz = 299;
    broken[2].slot_us = 3999;
    broken[3].slot_us = 4000;
    broken[3].tone_hz = 999;
    broken[4].bits_per_package = 0;
    broken[5].bits_per_package = 17;  // 18 x 64 ms = 1152 ms is the limit itself; 1 us more per slot breaks it
    broken[5].slot_us = 64000;
    CHECK(broken[5].check() == ConfigError::none);
    broken[5].slot_us = 64001;
    broken[6].passband = passband(2700, 300);
    broken[7].tone_hz = 400;
    broken[8].sync_markers = 33;
    broken[9].amplitude = 0;
    for (size_t i = 0; i < count_of(encoder_cases); ++i) {
        const std::string text = encoder_problem(broken[i]);
        CHECK(broken[i].check() == encoder_cases[i].error);
        if (!CHECK(has(text, encoder_cases[i].expected))) NOTE("case %zu: %s", i, text.c_str());
    }
    EncoderConfig wide = EncoderConfig::from_preset(Preset::hf_fast, k_decoder_rate);
    wide.passband = passband(1250, 1750);
    CHECK(has(encoder_problem(wide), "only 500 Hz wide; use longer slots"));
    EncoderConfig cap = hf;
    cap.bits_per_package = static_cast<uint8_t>(unlimited::k_max_bits_per_package + 1);
    CHECK(has(encoder_problem(cap), "1.." + std::to_string(unlimited::k_max_bits_per_package) + " (--bits)"));

    const DecoderConfig ssb = DecoderConfig::for_profile(Profile::ssb);
    CHECK_EQ(decoder_problem(ssb), std::string());
    DecoderConfig decoders[] = {ssb, ssb, ssb, ssb, ssb, ssb, ssb};
    decoders[0].min_slot_ms = 3;
    decoders[1].min_slot_ms = 33;
    decoders[2].passband = passband(2700, 300);
    decoders[3].passband = passband(300, 360);  // no room left for a pitch
    decoders[4].decision_mode = static_cast<DecisionMode>(2);
    decoders[5].fixed_ratio = 0.0f;
    decoders[6].fixed_ratio = 1.0f;
    const char* decoder_expected[] = {"the shortest slot must be 4..32 ms, not 3",
                                      "the shortest slot must be 4..32 ms, not 33",
                                      "the passband 2700-300 Hz is not valid",
                                      "the passband 300-360 Hz leaves no pitch to search for",
                                      "the decision rule must be adaptive or fixed (--rule)",
                                      "the fixed decision line must lie between 0 and 1",
                                      "the fixed decision line must lie between 0 and 1"};
    decoders[5].decision_mode = DecisionMode::fixed_ratio;
    decoders[6].decision_mode = DecisionMode::fixed_ratio;
    for (size_t i = 0; i < count_of(decoders); ++i) {
        const std::string text = decoder_problem(decoders[i]);
        if (!CHECK(has(text, decoder_expected[i]))) NOTE("decoder case %zu: %s", i, text.c_str());
    }
}

TEST(demo_cli_packetize_and_read_file) {
    using unlimited::k_packet_max_payload;
    using unlimited::k_packet_overhead;
    const size_t sizes[] = {1, k_ax25_payload, k_packet_max_payload, 2 * k_packet_max_payload + k_ax25_payload};
    for (size_t i = 0; i < count_of(sizes); ++i) {
        const std::vector<uint8_t> payload = unlimited::loopback::random_bytes(sizes[i], k_seed + i);
        std::vector<uint8_t> framed(1, 0xAA);  // replaced, not appended to
        const size_t packets = unlimited::cli::packetize(payload, framed);
        const size_t expected = (sizes[i] + k_packet_max_payload - 1) / k_packet_max_payload;
        CHECK_EQ(packets, expected);
        CHECK_EQ(framed.size(), sizes[i] + expected * k_packet_overhead);
        Packets received;
        unlimited::PacketReader reader(&Packets::on_packet, &received);
        for (size_t n = 0; n < framed.size(); ++n) reader.push(framed[n], 0);
        CHECK_EQ(received.count, expected);
        CHECK(received.payload == payload);
    }
    std::vector<uint8_t> framed(3, 0);
    CHECK_EQ(unlimited::cli::packetize(std::vector<uint8_t>(), framed), k_no_packets);
    CHECK(framed.empty());

    const std::string path = temp_path("read_file.bin");
    const std::vector<uint8_t> data = unlimited::loopback::random_bytes(k_ax25_payload, k_seed);
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
