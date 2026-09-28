// unlimited_decode itself (spec 7, 12.6): its options, its --help and --list-devices, its file path, and its live path
// through a stand-in sound card whose device thread feeds the program's decoder chain, stopped by a signal. The
// program is compiled here with its main() renamed, so these tests run the code that ships.
#define main unlimited_decode_main
#include "../demo/unlimited_decode.cpp"
#undef main

#include "audio_live.hpp"
#include "output_capture.hpp"
#include "support/loopback.hpp"
#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <thread>
#include <unistd.h>

namespace decode_test {

using unlimited::Encoder;
using unlimited::EncoderConfig;

const uint32_t k_card_rate = 48000;
const size_t k_card_block = 480;     // frames per device callback: 10 ms at 48 kHz
const size_t k_card_channels = 2;    // the first carries the radio's audio
const std::chrono::microseconds k_card_pace(1000);  // a block every 1 ms: 10 times real time
const double k_lead_silence_s = 3.5;  // digital silence before the transmission: the warning comes at 3 s
const std::chrono::milliseconds k_patience(20000);  // the stand-in's work takes a second; this only stops a hung test
const char* const k_text = "LIVE CQ DE UNLIMITED";
const float k_speed = 12.0f;

// Delivers `samples` from a thread of its own in blocks, like a sound card's input callback (the first of `channels`
// channels carries them), paced by its own clock; then digital silence until stopped, or a device error.
class StandInCard final : public pc::CaptureBackend {
public:
    explicit StandInCard(const std::vector<int16_t>& samples) : samples_(samples), stopping_(false) {}
    ~StandInCard() { stop(); }

    bool start(pc::InputFeed& feed, std::string&) override {
        feed_ = &feed;
        thread_ = std::thread(&StandInCard::run, this);
        return true;
    }

    void stop() override {
        stopping_.store(true);
        if (thread_.joinable()) thread_.join();
    }

    uint32_t sample_rate_hz() const override { return k_card_rate; }
    std::string description() const override { return "stand-in card"; }

    std::string fail_after;  // non-empty: after the samples, this device error instead of silence

private:
    void run() {
        std::vector<int16_t> frames(k_card_block * k_card_channels, 0);
        for (size_t at = 0; !stopping_.load(); at += k_card_block) {
            if (at >= samples_.size() && !fail_after.empty()) {
                feed_->fail(fail_after);
                return;
            }
            for (size_t i = 0; i < k_card_block; ++i)
                frames[i * k_card_channels] = at + i < samples_.size() ? samples_[at + i] : 0;
            feed_->deliver(&frames[0], k_card_block, k_card_channels);
            std::this_thread::sleep_for(k_card_pace);
        }
    }

    std::vector<int16_t> samples_;
    pc::InputFeed* feed_ = nullptr;
    std::atomic<bool> stopping_;
    std::thread thread_;
};

std::vector<uint8_t> text_bytes(const std::string& text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

// The radio's audio at the card's rate: digital silence, one transmission at k_speed bytes/s, then 1 s of silence.
std::vector<int16_t> card_audio(const std::vector<uint8_t>& data) {
    EncoderConfig sender;
    sender.sample_rate_hz = k_card_rate;
    sender.slot_us = unlimited::slot_us_for_speed(k_speed);
    Encoder encoder(sender);
    REQUIRE(encoder.write(data.data(), data.size()) == data.size());
    REQUIRE(encoder.start());
    std::vector<int16_t> audio(static_cast<size_t>(k_lead_silence_s * k_card_rate), 0);
    const size_t lead = audio.size();
    audio.resize(lead + encoder.duration_samples(data.size()) + k_card_rate, 0);
    REQUIRE(encoder.render(&audio[lead], audio.size() - lead) == encoder.duration_samples(data.size()));
    return audio;
}

Options parse(const std::vector<std::string>& words) {
    std::vector<std::string> args(1, "unlimited_decode");
    args.insert(args.end(), words.begin(), words.end());
    std::vector<char*> argv;
    for (size_t i = 0; i < args.size(); ++i) argv.push_back(&args[i][0]);
    return parse_options(static_cast<int>(argv.size()), &argv[0]);
}

std::string refusal(const std::vector<std::string>& words) {
    try {
        parse(words);
    } catch (const UsageError& error) {
        return error.what();
    }
    return std::string();
}

// The program's main() on these words: its exit code, what it printed and what it warned.
int run_main(const std::vector<std::string>& words, std::string& printed, std::string& warned) {
    std::vector<std::string> args(1, "unlimited_decode");
    args.insert(args.end(), words.begin(), words.end());
    std::vector<char*> argv;
    for (size_t i = 0; i < args.size(); ++i) argv.push_back(&args[i][0]);
    test::OutputCapture out(STDOUT_FILENO);
    test::OutputCapture err(STDERR_FILENO);
    const int status = unlimited_decode_main(static_cast<int>(argv.size()), &argv[0]);
    printed = out.finish();
    warned = err.finish();
    return status;
}

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::string temp_path(const std::string& name) {
    const char* dir = std::getenv("TMPDIR");
    std::string base = (dir != nullptr && *dir != '\0') ? dir : "/tmp";
    if (base[base.size() - 1] != '/') base += '/';
    return base + "unlimited_decode_program_" + name;
}

}  // namespace decode_test

// The decoder takes its own options and the shared input options (k_radio_input), never the PTT or output ones;
// --threshold defaults to auto.
TEST(decode_program_options) {
    using namespace decode_test;
    const Options defaults = parse(std::vector<std::string>());
    CHECK_EQ(defaults.in_spec(), std::string("rx.wav"));
    CHECK(!defaults.live());
    CHECK(defaults.config.decision_mode == DecisionMode::adaptive);  // --threshold auto
    CHECK(!unlimited::cli::fade_bridge_of(defaults.config));
    CHECK(defaults.config.impulse_blanker);

    CHECK_EQ(parse({"--in", "a.wav"}).in_spec(), std::string("a.wav"));
    CHECK_EQ(parse({"--input", "wav:b"}).in_spec(), std::string("wav:b"));
    CHECK_EQ(parse({"-d", "null"}).in_spec(), std::string("null"));
    CHECK_EQ(parse({"-d", "null", "--in", "c.wav"}).in_spec(), std::string("c.wav"));  // the later one
    CHECK_EQ(parse({"--in", "c.wav", "-d", "coreaudio:5"}).in_spec(), std::string("coreaudio:5"));
    CHECK(parse({"--input", "coreaudio:Microsoft Teams Audio"}).live());
    CHECK(parse({"-d", "default"}).live());
    CHECK_EQ(parse({"-r", "44100", "--in", "coreaudio:5"}).radio.rate_hz, 44100u);
    CHECK(parse({"--list-devices", "--bps", "99"}).radio.list_devices);  // listing needs nothing else

    const Options fixed_line = parse({"--threshold", "65"});
    CHECK(fixed_line.config.decision_mode == DecisionMode::fixed);
    CHECK_EQ(unsigned(fixed_line.config.threshold_percent), 65u);
    CHECK(parse({"--threshold", "65", "--threshold", "auto"}).config.decision_mode == DecisionMode::adaptive);
    CHECK(unlimited::cli::fade_bridge_of(parse({"--fade-bridge"}).config));

    CHECK(has(refusal({"--in", "CODEC"}), "--in: 'CODEC' is not a device"));
    CHECK(has(refusal({"--input", "3"}), "--input: '3' is not a device"));
    CHECK(has(refusal({"--ptt", "rts"}), "unknown option '--ptt'"));  // the PTT is the sender's
    CHECK(has(refusal({"--output", "x.wav"}), "unknown option '--output'"));
    CHECK(has(refusal({"-r", "48000"}), "-r opens a sound card at a rate; rx.wav is not one"));
    CHECK(has(refusal({"-r", "1000", "--in", "coreaudio:5"}), "-r: '1000' is not a sample rate"));
    CHECK(has(refusal({"--realtime", "-d", "coreaudio:5"}), "--realtime paces a file"));
    CHECK(refusal({"--realtime", "--in", "x.wav"}).empty());
}

// --help names the device options and the default auto line; --list-devices prints this system's table and nothing
// else; a usage error exits 2.
TEST(decode_program_help_and_list_devices) {
    using namespace decode_test;
    std::string printed;
    std::string warned;
    CHECK_EQ(run_main({"--help"}, printed, warned), k_exit_ok);
    const char* const expected[] = {"--threshold PCT|auto", "auto (the default) is the adaptive line", "--fade-bridge",
                                    "Sound devices:", "--list-devices", "--input SPEC", "-r HZ",
                                    "A sound card is listened to until Ctrl-C"};
    for (size_t i = 0; i < test::count_of(expected); ++i)
        if (!CHECK(has(printed, expected[i]))) NOTE("missing: %s", expected[i]);
    CHECK(!has(printed, "--ptt"));
    CHECK(warned.empty());

    CHECK_EQ(run_main({"--list-devices"}, printed, warned), k_exit_ok);
    CHECK_EQ(printed, pc::device_table(pc::list_devices()));
    CHECK(warned.empty());

    CHECK_EQ(run_main({"--ptt", "rts"}, printed, warned), k_exit_usage);
    CHECK(has(warned, "unlimited_decode: unknown option '--ptt' (see --help)"));
}

// A file is read as before: no level, no listening lines (those are a sound card's).
TEST(decode_program_file_path) {
    using namespace decode_test;
    const std::vector<uint8_t> data = text_bytes(k_text);
    const std::string path = temp_path("file.wav");
    {
        EncoderConfig sender;
        sender.slot_us = unlimited::slot_us_for_speed(k_speed);
        Encoder encoder(sender);
        REQUIRE(encoder.write(data.data(), data.size()) == data.size());
        REQUIRE(encoder.start());
        std::string error;
        std::unique_ptr<pc::OutputDevice> file = pc::open_output(path, error);
        REQUIRE(file != nullptr);
        unlimited::EncoderSource source(encoder);
        REQUIRE(file->start(source, sender.sample_rate_hz) && file->wait());
    }
    std::string printed;
    std::string warned;
    CHECK_EQ(run_main({"--in", path, "--bps", "12"}, printed, warned), k_exit_ok);
    // --events: each state change with DCD as the decoder has it (spec 3.10: on while tracking, off while a candidate
    // is checked).
    std::string events;
    std::string events_warned;
    CHECK_EQ(run_main({"--in", path, "--bps", "12", "--events"}, events, events_warned), k_exit_ok);
    CHECK(has(events, "state acquire (DCD off)"));
    CHECK(has(events, "state track (DCD on)"));
    CHECK(has(events, "state search (DCD off)"));
    CHECK(!has(events, "state acquire (DCD on)"));
    std::remove(path.c_str());
    CHECK(has(printed, "receiver   passband 300-2700 Hz, pitch search 565-2435 Hz, adaptive decision line (auto: "));
    CHECK(has(printed, "input      " + path + ": 8000 Hz\n"));
    CHECK(has(printed, "text       \"" + std::string(k_text) + "\""));
    CHECK(!has(printed, "level"));
    CHECK(!has(printed, "listened"));
    CHECK(!has(printed, "received"));
    CHECK(warned.empty());
}

// The live path (spec 12.6): the stand-in card's thread delivers, the device's worker thread runs the program's
// level meter and decoder chain, the text comes out, the silence before it is reported once, and Ctrl-C (SIGINT, from
// a signal handler) ends the listening with the summary; --expect compares what was kept.
TEST(decode_program_listens_until_ctrl_c) {
    using namespace decode_test;
    const std::vector<uint8_t> data = text_bytes(k_text);
    pc::LiveInput input(std::unique_ptr<pc::CaptureBackend>(new StandInCard(card_audio(data))));
    // receive() compares with the data it is handed; run() reads the --expect file.
    const Options options = parse({"--in", "coreaudio:stand-in", "--bps", "12", "--expect", "expect.txt"});
    REQUIRE(options.live());
    const std::string text_line = "text       \"" + std::string(k_text) + "\"";

    test::DefaultSignal interrupt(SIGINT);
    std::atomic<bool> seen(false);
    int status = -1;
    std::string printed;
    std::string warned;
    {
        test::OutputCapture out(STDOUT_FILENO);
        test::OutputCapture err(STDERR_FILENO);
        std::thread operator_thread([&] {  // presses Ctrl-C once the text is printed
            seen.store(out.wait_for(text_line, k_patience));
            std::raise(SIGINT);
        });
        status = receive(options, input, data);
        operator_thread.join();
        printed = out.finish();
        warned = err.finish();
    }
    CHECK(seen.load());
    CHECK_EQ(status, k_exit_ok);
    CHECK(has(printed, "input      stand-in card, resampled to 8000 Hz; listening until Ctrl-C\n"));
    CHECK(has(printed, "locked     t "));
    CHECK(has(printed, "level      input peak -3.0 dBFS, RMS "));
    CHECK(has(printed, ", 0 clips\n"));
    CHECK(has(printed, text_line));
    CHECK(has(printed, "s, stopped by Ctrl-C (SIGINT): peak -3.0 dBFS, RMS "));
    CHECK(has(printed, "; 0 xruns\n"));
    CHECK(has(printed, "received   20 bytes, 1 lock\n"));
    CHECK(has(printed, "expect     20 bytes x 1 transmission: received 20, lost 0, wrong 0, extra 0, bit errors 0/160"));
    CHECK(has(printed, "result     match\n"));
    CHECK_EQ(test::occurrences(warned, "the input has been digital silence (every sample 0) for 3.0 s"), 1u);
    if (!CHECK(test::occurrences(warned, "unlimited_decode:") == 1)) NOTE("%s", warned.c_str());
}

// A device error ends the listening on its own: what came before it is decoded, then exit 3 with the device's reason.
TEST(decode_program_live_device_error) {
    using namespace decode_test;
    const std::vector<uint8_t> data = text_bytes(k_text);
    StandInCard* card = new StandInCard(card_audio(data));
    card->fail_after = "the device was disconnected";
    pc::LiveInput input((std::unique_ptr<pc::CaptureBackend>(card)));
    const Options options = parse({"-d", "coreaudio:stand-in", "--bps", "12"});
    std::string printed;
    std::string warned;
    int status = -1;
    {
        test::OutputCapture out(STDOUT_FILENO);
        test::OutputCapture err(STDERR_FILENO);
        status = receive(options, input, std::vector<uint8_t>());
        printed = out.finish();
        warned = err.finish();
    }
    CHECK_EQ(status, k_exit_io);
    CHECK(has(printed, "text       \"" + std::string(k_text) + "\""));
    CHECK(has(printed, "input      listened "));
    CHECK(!has(printed, "stopped by"));
    CHECK(has(printed, "received   20 bytes, 1 lock\n"));
    CHECK(has(warned, "unlimited_decode: reading coreaudio:stand-in failed: the device was disconnected"));
}
