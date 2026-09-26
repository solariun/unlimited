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

using unlimited::Decoder;
using unlimited::DecoderConfig;
using unlimited::DecoderSink;
using unlimited::Encoder;
using unlimited::EncoderConfig;
using unlimited::EncoderSource;
using unlimited::Event;
using unlimited::EventType;
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
const size_t k_bytes = 48;                    // fits the encoder queue, so EncoderSource alone sends it all
const size_t k_odd_chunk = 37;                // device chunk that never lines up with a block or a slot
const size_t k_reader_chunk = 1000;           // samples per WavReader::read
const double k_drain_slots = 4.0;             // silence after the input, in the longest slots
const double k_slot_tolerance = 0.005;        // measured T within 0.5 % (L1)
const double k_us_per_ms = 1e3;
const double k_ms_per_s = 1e3;
const uint16_t k_bytes_per_sample = 2;
const uint32_t k_seed = 13;

class EventLog {
public:
    static void on_event(const Event& event, void* context) { static_cast<EventLog*>(context)->events.push_back(event); }

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
           a.value == b.value && a.index == b.index && a.tone == b.tone && a.level_pct == b.level_pct &&
           a.confidence == b.confidence && std::memcmp(a.soft, b.soft, sizeof(a.soft)) == 0 &&
           a.bits_per_peak == b.bits_per_peak && a.data_slots == b.data_slots && a.spacing == b.spacing &&
           a.side == b.side && a.frame_index == b.frame_index && same_bits(a.tone_hz, b.tone_hz) &&
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

// Encoder at `rate` -> EncoderSource -> WavOutput -> memory -> WavReader -> ResamplingSink -> DecoderSink.
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
    for (size_t n = 0; (n = reader.read(chunk.data(), chunk.size())) > 0; samples += n) resampling.write(chunk.data(), n);
    resampling.flush();
    drain(decoder_sink, decoder_config);

    CHECK_EQ(samples, size_t(duration));
    CHECK_EQ(log.count(EventType::locked), size_t(1));
    CHECK_EQ(log.count(EventType::end), size_t(1));
    CHECK_EQ(log.count(EventType::lost), size_t(0));
    for (size_t i = 0; i < log.events.size(); ++i) {
        if (log.events[i].type != EventType::locked) continue;
        CHECK_EQ(log.events[i].bits_per_peak, config.bits_per_peak);
        CHECK_EQ(log.events[i].data_slots, config.data_slots);
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

// U17: the DecoderSink adapter, fed by a driver in odd chunks, gives the events of one direct process() call.
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

// L13: sample-rate independence, 0 errors clean.
TEST(demo_io_l13_rate_independence_hf) {
    for (size_t i = 0; i < count_of(k_rates); ++i) round_trip(Preset::hf, Profile::ssb, k_rates[i]);
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

    const std::vector<double> pair = to_fields("--x", "300:2700", 2, 2);
    REQUIRE(pair.size() == 2u);
    CHECK_EQ(pair[0], 300.0);
    CHECK_EQ(pair[1], 2700.0);
    CHECK_EQ(to_fields("--x", "5", 1, 2).size(), 1u);
    CHECK(throws_usage([] { to_fields("--x", "5", 2, 2); }));
    CHECK(throws_usage([] { to_fields("--x", "1:2:3", 1, 2); }));
    CHECK(throws_usage([] { to_fields("--x", "1:", 2, 2); }));

    CHECK(find_name(k_profiles, "--profile", "fm").profile == Profile::fm);
    CHECK(find_name(k_profiles, "--profile", "fm").preset == Preset::fm);
    CHECK(find_name(k_profiles, "--profile", "am").preset == Preset::hf);
    CHECK(find_name(k_presets, "--preset", "hf_weak").preset == Preset::hf_weak);
    CHECK(find_name(k_spacings, "--spacing", "dense").spacing == unlimited::Spacing::dense);
    CHECK(find_name(k_sides, "--side", "above").side == unlimited::GridSide::above);
    CHECK(throws_usage([] { find_name(k_sides, "--side", "usb"); }));
    std::string message;
    try {
        find_name(k_profiles, "--profile", "usb");
    } catch (const UsageError& error) {
        message = error.what();
    }
    CHECK_EQ(message, std::string("--profile: 'usb' is not one of ssb|am|fm"));

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

// The demos' mode arithmetic (spec 1.3, 1.4): spans, rates and the HF placement rule of the presets.
TEST(demo_cli_mode_helpers) {
    using namespace unlimited::cli;
    const double k_hz_tolerance = 0.01;
    CHECK_NEAR(tone_offset_hz(0, unlimited::Spacing::standard, 32.0), 156.25, k_hz_tolerance);
    CHECK_NEAR(tone_offset_hz(31, unlimited::Spacing::standard, 32.0), 1263.39, k_hz_tolerance);
    CHECK_NEAR(tone_offset_hz(31, unlimited::Spacing::dense, 32.0), 1125.0, k_hz_tolerance);
    CHECK_NEAR(span_hz(3, unlimited::Spacing::standard, 6.0), 2166.67, k_hz_tolerance);   // fm_fast
    CHECK_NEAR(span_hz(1, unlimited::Spacing::dense, 8.0), 1625.0, k_hz_tolerance);       // header tones
    CHECK_NEAR(span_hz(8, unlimited::Spacing::dense, 128.0), 2031.25, k_hz_tolerance);    // T128 k8 dense
    CHECK_NEAR(net_bit_rate(5, 8, 32.0), 138.89, k_hz_tolerance);
    CHECK_NEAR(net_bit_rate(7, 8, 128.0), 48.61, k_hz_tolerance);
    CHECK_EQ(mode_text(32.0, 5, 8, unlimited::Spacing::standard, side_name(unlimited::GridSide::below)),
             std::string("T 32 ms  k 5  N 8  standard  grid below  138.9 bit/s"));
    CHECK_EQ(std::string(side_name(static_cast<int8_t>(1))), std::string("above"));

    // HF presets: f_ref = ceil(1500 + W / 2), the grid below (the encoder demo places custom modes the same way).
    const Preset hf_presets[] = {Preset::hf_fast, Preset::hf, Preset::hf_robust, Preset::hf_weak};
    for (size_t i = 0; i < count_of(hf_presets); ++i) {
        const EncoderConfig config = EncoderConfig::from_preset(hf_presets[i], k_decoder_rate);
        const double span = span_hz(config.bits_per_peak, config.spacing, config.slot_us / unlimited::cli::k_us_per_ms);
        CHECK_EQ(config.tone_hz, static_cast<uint16_t>(std::ceil(unlimited::k_band_centre_hz + span / 2.0)));
        CHECK(config.side == unlimited::GridSide::below);
    }
}

// Every EncoderConfig::check() refusal is explained by its rule (spec 7), the G2 dense minimum included.
TEST(demo_cli_config_problem_names_the_rule) {
    using unlimited::ConfigError;
    using unlimited::cli::config_problem;
    const EncoderConfig hf;
    CHECK_EQ(config_problem(hf), std::string());
    EncoderConfig dense = hf;
    dense.slot_us = 16000;
    dense.bits_per_peak = 5;
    dense.spacing = unlimited::Spacing::dense;
    CHECK(dense.check() == ConfigError::dense_slot);
    CHECK(config_problem(dense).find("dense spacing needs T >= 32 ms") == 0u);
    EncoderConfig sync = hf;
    sync.sync_markers = 33;
    CHECK_EQ(config_problem(sync), std::string("--sync must be 8..32"));
    EncoderConfig slots = hf;
    slots.data_slots = 12;
    CHECK_EQ(config_problem(slots), std::string("data slots must be 8, 16 or 32"));
    EncoderConfig rate = hf;
    rate.sample_rate_hz = 7999;
    CHECK_EQ(config_problem(rate), std::string("--rate must be 8000..192000 Hz"));
    EncoderConfig band = hf;
    band.tone_hz = 1200;
    CHECK(config_problem(band).find("the band ") == 0u);
    // No rule falls through to the generic text.
    EncoderConfig broken[] = {hf, hf, hf, hf, hf, hf, hf};
    broken[0].tone_hz = 100;
    broken[1].slot_us = 32500;
    broken[2].bits_per_peak = 9;
    broken[3].data_slots = 32;
    broken[3].slot_us = 64000;
    broken[4].spacing = static_cast<unlimited::Spacing>(2);
    broken[5].side = static_cast<unlimited::GridSide>(2);
    broken[6].amplitude = 0;
    for (size_t i = 0; i < count_of(broken); ++i) {
        CHECK(!broken[i].valid());
        const std::string text = config_problem(broken[i]);
        if (!CHECK(!text.empty() && text != "the encoder refuses this combination")) NOTE("case %zu", i);
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
