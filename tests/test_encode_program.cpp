// unlimited_encode itself (spec 7, 12.4 to 12.6): its options, its --help and --list-devices, its file path, and its
// live path through a stand-in sound card whose device thread pulls the program's encoder, with a PTT that records
// when it was keyed; the view's status hand-off; Ctrl-C from a signal handler. The program is compiled here with its
// main() renamed, so these tests run the code that ships.
#define main unlimited_encode_main
#include "../demo/unlimited_encode.cpp"
#undef main

#include "audio_live.hpp"
#include "output_capture.hpp"
#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace encode_test {

using unlimited::Decoder;
using unlimited::Event;
using unlimited::EventType;

typedef std::chrono::steady_clock Clock;

const uint32_t k_card_rate = 48000;
const size_t k_card_block = 512;    // frames per render callback
const size_t k_card_channels = 2;   // every channel gets the sample
const uint32_t k_card_latency_ms = 40;
const size_t k_tail_blocks = 8;     // blocks of silence pulled after the source ran out
const std::chrono::microseconds k_card_pace(1000);  // a block every 1 ms: about 10 times real time
const std::chrono::milliseconds k_patience(20000);   // the stand-in's work takes a second; this only stops a hung test
const std::chrono::milliseconds k_end_slack(1);      // between the card seeing the end and drain() timing it
const double k_lead_slots = 15.0;   // silence a decoder hears before a recording (spec V6)
const double k_drain_windows = 2.0;
const char* const k_text = "CQ CQ DE UNLIMITED LIVE";
const int k_columns = 120;
const int k_rows = 30;

// Pulls blocks from a thread of its own, like a sound card's render callback (float frames, every channel the same
// sample), paced by its own clock; records the first channel and when the source ran out (the feed's first short
// read), pulls k_tail_blocks more, then waits to be stopped. hold_after: after that many blocks it holds (waits to be
// stopped) and says so to wait_held().
class StandInSpeaker final : public pc::PlaybackBackend {
public:
    StandInSpeaker() : pulled_(0), stopping_(false), held_(false) {}
    ~StandInSpeaker() { stop(); }

    bool start(pc::OutputFeed& feed, std::string&) override {
        feed_ = &feed;
        thread_ = std::thread(&StandInSpeaker::run, this);
        return true;
    }

    void stop() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    uint32_t sample_rate_hz() const override { return k_card_rate; }
    uint32_t latency_ms() const override { return k_card_latency_ms; }
    std::string description() const override { return "stand-in speaker"; }

    size_t pulled() const { return pulled_.load(); }  // frames pulled so far

    bool wait_held() {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, k_patience, [this] { return held_; });
    }

    size_t hold_after = 0;        // 0: never holds
    std::vector<int16_t> played;  // the first channel; read after stop()
    bool ended = false;           // the source ran out; read after stop()
    Clock::time_point ended_at;

private:
    void run() {
        std::vector<float> frames(k_card_block * k_card_channels);
        size_t blocks = 0;
        size_t after_end = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                const bool holding = (hold_after != 0 && blocks == hold_after) || after_end >= k_tail_blocks;
                if (holding) {
                    held_ = true;
                    changed_.notify_all();
                    changed_.wait(lock, [this] { return stopping_; });
                }
                if (stopping_) return;
            }
            feed_->fill(&frames[0], k_card_block, k_card_channels);
            ++blocks;
            pulled_.fetch_add(k_card_block);
            for (size_t i = 0; i < k_card_block; ++i) played.push_back(pc::to_sample(frames[i * k_card_channels]));
            if (!ended && feed_->ended()) {
                ended = true;
                ended_at = Clock::now();
            }
            if (ended) ++after_end;
            std::this_thread::sleep_for(k_card_pace);
        }
    }

    pc::OutputFeed* feed_ = nullptr;
    std::atomic<size_t> pulled_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool stopping_;
    bool held_;
    std::thread thread_;
};

// Keys nothing; records each key(): on or off, the frames the speaker had pulled, when. refuse: key(true) fails.
class RecordingPtt final : public pc::Ptt {
public:
    explicit RecordingPtt(const StandInSpeaker& speaker) : speaker_(speaker) {}

    bool key(bool on) override {
        const Keying keying = {on, speaker_.pulled(), Clock::now()};
        keyings.push_back(keying);
        if (on && refuse) return false;
        return pc::Ptt::key(on);
    }

    std::string description() const override { return "recording PTT"; }

    struct Keying {
        bool on;
        size_t pulled;
        Clock::time_point at;
    };
    std::vector<Keying> keyings;
    bool refuse = false;

private:
    const StandInSpeaker& speaker_;
};

std::vector<uint8_t> text_bytes(const std::string& text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

std::vector<char*> argv_of(std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (size_t i = 0; i < args.size(); ++i) argv.push_back(&args[i][0]);
    return argv;
}

Options parse(const std::vector<std::string>& words) {
    std::vector<std::string> args(1, "unlimited_encode");
    args.insert(args.end(), words.begin(), words.end());
    std::vector<char*> argv = argv_of(args);
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

int run_main(const std::vector<std::string>& words, std::string& printed, std::string& warned) {
    std::vector<std::string> args(1, "unlimited_encode");
    args.insert(args.end(), words.begin(), words.end());
    std::vector<char*> argv = argv_of(args);
    test::OutputCapture out(STDOUT_FILENO);
    test::OutputCapture err(STDERR_FILENO);
    const int status = unlimited_encode_main(static_cast<int>(argv.size()), &argv[0]);
    printed = out.finish();
    warned = err.finish();
    return status;
}

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

class ByteLog {
public:
    static void on_event(const Event& event, void* context) {
        if (event.type == EventType::byte) static_cast<ByteLog*>(context)->bytes.push_back(event.value);
    }
    std::vector<uint8_t> bytes;
};

// What a receiver told the speed decodes from audio at `rate_hz`, heard as a recording (silence before and after).
std::vector<uint8_t> decode(const std::vector<int16_t>& audio, uint32_t rate_hz, uint32_t slot_us) {
    unlimited::DecoderConfig config = unlimited::cli::receiver_config();
    config.slot_us = slot_us;
    ByteLog log;
    Decoder decoder(config, &ByteLog::on_event, &log);
    unlimited::DecoderSink sink(decoder);
    const double slot_samples =
        unlimited::cli::slot_ms_of(slot_us) * unlimited::k_decoder_rate_hz / unlimited::cli::k_ms_per_s;
    const std::vector<int16_t> lead(static_cast<size_t>(std::ceil(k_lead_slots * slot_samples)), 0);
    sink.write(lead.data(), lead.size());
    pc::ResamplingSink resampling(sink, rate_hz);
    resampling.write(audio.data(), audio.size());
    resampling.flush();
    const std::vector<int16_t> drain(
        decoder.lookahead_samples() +
            static_cast<size_t>(std::ceil(k_drain_windows * unlimited::k_window_slots * slot_samples)),
        0);
    sink.write(drain.data(), drain.size());
    return log.bytes;
}

// What send() needs, as run() opens it, with the stand-in speaker as the sound card and the recording PTT.
void open_stand_ins(const Options& o, Opened& opened, StandInSpeaker*& speaker, RecordingPtt*& ptt) {
    opened.config = encoder_config(o, k_card_rate);
    opened.data = o.has_text ? text_bytes(o.text) : std::vector<uint8_t>();
    opened.channel_config = o.channel_config;
    speaker = new StandInSpeaker();
    opened.output.reset(new pc::LiveOutput(std::unique_ptr<pc::PlaybackBackend>(speaker)));
    ptt = new RecordingPtt(*speaker);
    opened.ptt.reset(ptt);
}

std::string render(const pc::Tui& tui) {
    return tui.render(k_columns, k_rows);
}

}  // namespace encode_test

// The encoder takes its own options and the shared output and PTT options (k_radio_output | k_radio_ptt), never the
// input ones; the VOX lead is on by default where a radio is keyed by VOX; --fade-bridge leaves 300 ms before the
// first START.
TEST(encode_program_options) {
    using namespace encode_test;
    const Options defaults = parse({"--text", "x"});
    CHECK_EQ(defaults.out_spec(), std::string("tx.wav"));
    CHECK(!defaults.live());
    CHECK(defaults.radio.ptt.method == pc::PttMethod::vox);
    CHECK_EQ(unsigned(vox_lead_ms(defaults)), 0u);  // a file: no VOX lead, as before
    CHECK_EQ(unsigned(encoder_config(defaults, k_default_rate_hz).lead_in_ms), 0u);

    CHECK_EQ(parse({"--text", "x", "--out", "a.wav"}).out_spec(), std::string("a.wav"));
    CHECK_EQ(parse({"--text", "x", "--output", "null"}).out_spec(), std::string("null"));
    CHECK_EQ(parse({"--text", "x", "-d", "coreaudio:5"}).out_spec(), std::string("coreaudio:5"));
    CHECK_EQ(parse({"--text", "x", "-d", "coreaudio:5", "--out", "b.wav"}).out_spec(), std::string("b.wav"));
    CHECK(parse({"--text", "x", "--output", "default"}).live());
    CHECK(parse({"--list-devices"}).radio.list_devices);  // no --text needed to list

    // The VOX lead (spec 2.1, V8): 150 ms where VOX keys the radio, --vox-lead-ms always wins.
    CHECK_EQ(unsigned(vox_lead_ms(parse({"--text", "x", "--output", "coreaudio:5"}))), 150u);
    CHECK_EQ(unsigned(vox_lead_ms(parse({"--text", "x", "--ptt", "vox"}))), 150u);  // named: a file for a VOX radio
    CHECK_EQ(unsigned(vox_lead_ms(parse({"--text", "x", "-d", "coreaudio:5", "--ptt", "rts", "--ptt-device",
                                         "/dev/cu.x"}))),
             0u);
    CHECK_EQ(unsigned(vox_lead_ms(parse({"--text", "x", "-d", "coreaudio:5", "--vox-lead-ms", "0"}))), 0u);
    CHECK_EQ(unsigned(vox_lead_ms(parse({"--text", "x", "--vox-lead-ms", "300"}))), 300u);

    // The TX delay (Gustavo, 2026-09-28): 100 ms of lead-in when the PTT keys the radio by RTS, DTR or CAT, no VOX
    // lead; --lead-in-ms wins; VOX keeps its lead tone and no lead-in.
    const char* const keyed_methods[] = {"rts", "-dtr", "icom", "kenwood"};
    for (size_t i = 0; i < test::count_of(keyed_methods); ++i) {
        const EncoderConfig keyed = encoder_config(
            parse({"--text", "x", "-d", "coreaudio:5", "--ptt", keyed_methods[i], "--ptt-device", "/dev/cu.x"}),
            k_card_rate);
        CHECK_EQ(unsigned(keyed.lead_in_ms), 100u);
        CHECK_EQ(unsigned(keyed.vox_lead_ms), 0u);
    }
    const Options keyed_given = parse({"--text", "x", "-d", "coreaudio:5", "--ptt", "rts", "--ptt-device", "/dev/cu.x",
                                       "--lead-in-ms", "30"});
    CHECK_EQ(unsigned(encoder_config(keyed_given, k_card_rate).lead_in_ms), 30u);
    const EncoderConfig vox_card = encoder_config(parse({"--text", "x", "-d", "coreaudio:5"}), k_card_rate);
    CHECK_EQ(unsigned(vox_card.lead_in_ms), 0u);
    CHECK_EQ(unsigned(vox_card.vox_lead_ms), 150u);
    const Options keyed_bridged =
        parse({"--text", "x", "-d", "coreaudio:5", "--ptt", "yaesu", "--ptt-device", "/dev/cu.x", "--fade-bridge"});
    CHECK_EQ(unsigned(encoder_config(keyed_bridged, k_card_rate).lead_in_ms), 300u);  // max(100, 300)

    // --fade-bridge: a lead-in of at least 300 ms, unless a VOX lead and its gap come first.
    CHECK_EQ(unsigned(encoder_config(parse({"--text", "x", "--fade-bridge"}), k_default_rate_hz).lead_in_ms), 300u);
    CHECK_EQ(unsigned(encoder_config(parse({"--text", "x", "--fade-bridge", "--lead-in-ms", "500"}), k_default_rate_hz)
                          .lead_in_ms),
             500u);
    const EncoderConfig vox_bridged = encoder_config(parse({"--text", "x", "--fade-bridge", "-d", "coreaudio:5"}),
                                                     k_card_rate);
    CHECK_EQ(unsigned(vox_bridged.vox_lead_ms), 150u);
    CHECK_EQ(unsigned(vox_bridged.lead_in_ms), 0u);

    CHECK(has(refusal({"--text", "x", "--input", "a.wav"}), "unknown option '--input'"));  // the receiver's
    CHECK(has(refusal({"--text", "x", "--out", "CODEC"}), "--out: 'CODEC' is not a device"));
    CHECK(has(refusal({"--text", "x", "--output", "3"}), "--output: '3' is not a device"));
    CHECK(has(refusal({"--text", "x", "-r", "48000"}), "-r opens a sound card at a rate; tx.wav is not one"));
    CHECK(has(refusal({"--text", "x", "--rate", "48000", "-d", "coreaudio:5"}), "--rate sets a file's sample rate"));
    CHECK(has(refusal({"--text", "x", "--realtime", "-d", "coreaudio:5"}), "--realtime paces a file"));
    CHECK(has(refusal({"--text", "x", "--ptt", "rts", "--ptt-device", "/dev/cu.x"}),
              "--ptt rts keys a radio: it needs a sound card"));
    CHECK(has(refusal({"--text", "x", "-d", "coreaudio:5", "--ptt", "icom"}), "--ptt icom needs --ptt-device"));
    CHECK(has(refusal({"--text", "x", "-d", "coreaudio:5", "--ptt", "-rts", "--ptt-device", "/dev/cu.x", "--channel",
                       "usb"}),
              "--channel plays what a receiver would hear"));
    CHECK(refusal({"--text", "x", "-d", "coreaudio:5", "--ptt", "icom", "--ptt-device", "/dev/cu.x", "--cat-addr",
                   "0xA4"})
              .empty());
    CHECK(has(refusal({"--output", "coreaudio:5"}), "give exactly one of --text and --in"));
}

TEST(encode_program_help_and_list_devices) {
    using namespace encode_test;
    std::string printed;
    std::string warned;
    CHECK_EQ(run_main({"--help"}, printed, warned), k_exit_ok);
    const char* const expected[] = {"--output SPEC", "--ptt METHOD", "--list-devices", "-r HZ", "--fade-bridge",
                                    "default 150 on a sound card keyed by VOX",
                                    "(default 100\n                          when --ptt keys the radio by RTS, DTR or CAT",
                                    "Ctrl-C stops the transmission and releases the PTT",
                                    "1 stopped by Ctrl-C before the end"};
    for (size_t i = 0; i < test::count_of(expected); ++i)
        if (!CHECK(has(printed, expected[i]))) NOTE("missing: %s", expected[i]);
    CHECK(!has(printed, "--input SPEC"));

    CHECK_EQ(run_main({"--list-devices"}, printed, warned), k_exit_ok);
    CHECK_EQ(printed, pc::device_table(pc::list_devices()));

    CHECK_EQ(run_main({"--text", "x", "--input", "a.wav"}, printed, warned), k_exit_usage);
    CHECK(has(warned, "unlimited_encode: unknown option '--input' (see --help)"));
}

// A file is written as before: no device or PTT lines, no VOX lead unless asked.
TEST(encode_program_file_path) {
    using namespace encode_test;
    const char* dir = std::getenv("TMPDIR");
    std::string path = (dir != nullptr && *dir != '\0') ? dir : "/tmp";
    if (path[path.size() - 1] != '/') path += '/';
    path += "unlimited_encode_program.wav";
    std::string printed;
    std::string warned;
    CHECK_EQ(run_main({"--text", "Hi", "--out", path}, printed, warned), k_exit_ok);
    std::remove(path.c_str());
    CHECK(has(printed, "airtime    0.433 s: lead-in 0 ms, 2 windows of 10 slots, tail 100 ms\n"));
    CHECK(has(printed, "audio      3467 samples at 8000 Hz -> " + path + "\n"));
    CHECK(!has(printed, "output     "));
    CHECK(!has(printed, "ptt        "));
    CHECK(warned.empty());
}

// The live path (spec 12.4 to 12.6): the PTT keyed before the first sample, the stand-in card's thread pulling the
// encoder, the PTT released once the audio has left the device (drain: the source ran out and the latency passed),
// the output level; what the card played decodes back to the text.
TEST(encode_program_plays_through_a_sound_card) {
    using namespace encode_test;
    const Options o = parse({"--text", k_text, "--bps", "12", "--output", "coreaudio:stand-in"});
    Opened opened;
    StandInSpeaker* speaker = nullptr;
    RecordingPtt* ptt = nullptr;
    open_stand_ins(o, opened, speaker, ptt);
    std::string printed;
    std::string warned;
    int status = -1;
    {
        test::OutputCapture out(STDOUT_FILENO);
        test::OutputCapture err(STDERR_FILENO);
        status = send(o, opened);
        printed = out.finish();
        warned = err.finish();
    }
    CHECK_EQ(status, k_exit_ok);
    REQUIRE(ptt->keyings.size() == 2u);
    CHECK(ptt->keyings[0].on);
    CHECK_EQ(ptt->keyings[0].pulled, 0u);  // keyed before the audio
    CHECK(!ptt->keyings[1].on);
    CHECK(!ptt->keyed());
    REQUIRE(speaker->ended);
    // drain() times the end when its service thread wakes, a few microseconds from when the card saw it.
    const std::chrono::microseconds released =
        std::chrono::duration_cast<std::chrono::microseconds>(ptt->keyings[1].at - speaker->ended_at);
    NOTE("PTT released %lld us after the source ran out (latency %u ms)", static_cast<long long>(released.count()),
         static_cast<unsigned>(k_card_latency_ms));
    CHECK(released + k_end_slack >= std::chrono::milliseconds(k_card_latency_ms));

    const uint32_t duration = Encoder(opened.config).duration_samples(opened.data.size());
    CHECK(has(printed, "VOX lead "));  // VOX keys the radio by default
    CHECK(has(printed, "output     stand-in speaker\nptt        recording PTT\n"));
    CHECK(has(printed, "audio      " + std::to_string(duration) + " samples at 48000 Hz -> stand-in speaker\n"));
    CHECK(has(printed, "output     peak -3.0 dBFS, RMS "));
    CHECK(has(printed, "; 0 xruns\n"));
    CHECK(!has(printed, "stopped"));
    CHECK(warned.empty());
    CHECK(decode(speaker->played, k_card_rate, opened.config.slot_us) == opened.data);
}

// A radio keyed by RTS, DTR or CAT gets its TX delay: the card plays 100 ms of silence after the key and before the
// first START (4800 samples at 48 kHz), no VOX lead; the text still comes back.
TEST(encode_program_keyed_ptt_waits_the_tx_delay) {
    using namespace encode_test;
    const Options o = parse({"--text", k_text, "--bps", "12", "-d", "coreaudio:stand-in", "--ptt", "rts",
                             "--ptt-device", "/dev/cu.stand-in"});  // the port is never opened: the PTT is recorded
    Opened opened;
    StandInSpeaker* speaker = nullptr;
    RecordingPtt* ptt = nullptr;
    open_stand_ins(o, opened, speaker, ptt);
    std::string printed;
    std::string warned;
    int status = -1;
    {
        test::OutputCapture out(STDOUT_FILENO);
        test::OutputCapture err(STDERR_FILENO);
        status = send(o, opened);
        printed = out.finish();
        warned = err.finish();
    }
    CHECK_EQ(status, k_exit_ok);
    CHECK(warned.empty());
    REQUIRE(ptt->keyings.size() == 2u);
    CHECK_EQ(ptt->keyings[0].pulled, 0u);
    CHECK(has(printed, " s: lead-in 100 ms, " + std::to_string(opened.data.size()) + " windows of 10 slots, tail 100 ms\n"));
    const size_t delay_samples = k_card_rate / 10;  // 100 ms
    const size_t slot_samples = k_card_rate / 120;  // T = 8.333 ms at 12 bytes/s
    size_t first_sound = 0;
    while (first_sound < speaker->played.size() && speaker->played[first_sound] == 0) ++first_sound;
    NOTE("the first non-zero sample is sample %zu (the START's ramp begins at %zu)", first_sound, delay_samples);
    CHECK(first_sound >= delay_samples);
    CHECK(first_sound < delay_samples + slot_samples / 4);  // within the START's rising quarter
    CHECK(decode(speaker->played, k_card_rate, opened.config.slot_us) == opened.data);
}

// The view on a sound card (spec 12.6): the card's thread publishes each slot's status and the audio; the main thread
// takes them at each frame. Every STOP arrives (the sent text is whole), none is dropped, the view ends idle with the
// output level; the channel simulator's first-pass marks are handed over as the played audio passes them.
TEST(encode_program_view_takes_every_slot) {
    using namespace encode_test;
    const Options o = parse({"--text", k_text, "--bps", "25", "--output", "coreaudio:stand-in"});
    const EncoderConfig config = encoder_config(o, k_card_rate);
    const std::vector<uint8_t> data = text_bytes(k_text);
    const std::string sent_title = "sent  " + std::to_string(data.size()) + " bytes";
    {
        pc::Tui tui(pc::TuiMode::encoder);  // not open: drawn to a string
        tui.set_color(false);
        LiveView view(tui, k_card_rate);
        Encoder encoder(config);
        TransmitSource transmit(encoder, data, &view.publisher());
        REQUIRE(transmit.start());
        LiveSource source(transmit, k_card_rate, &view, nullptr);
        pc::LiveOutput output(std::unique_ptr<pc::PlaybackBackend>(new StandInSpeaker()));
        pc::Ptt vox;
        const LiveOutcome outcome = transmit_live(output, vox, source, k_card_rate, &view);
        CHECK(outcome.keyed && outcome.started && outcome.drained && outcome.unkeyed && outcome.device_ok);
        CHECK_EQ(outcome.signal_number, 0);
        CHECK_EQ(view.statuses().dropped(), 0u);
        const std::string frame = render(tui);
        CHECK(has(frame, sent_title));
        CHECK(has(frame, k_text));
        CHECK(has(frame, "idle  the transmission has ended"));
        CHECK(has(frame, "out peak -3.0 dBFS, RMS "));
    }
    {
        MarkRecorder recorder;
        Encoder encoder(config);
        TransmitSource transmit(encoder, data, &recorder);
        REQUIRE(transmit.start());
        pc::MemoryOutput capture;
        REQUIRE(capture.start(transmit, k_card_rate));
        pc::MemorySource memory(capture.samples());
        pc::Tui tui(pc::TuiMode::encoder);
        tui.set_color(false);
        LiveView view(tui, k_card_rate);
        LiveSource source(memory, k_card_rate, &view, &recorder.marks);
        pc::LiveOutput output(std::unique_ptr<pc::PlaybackBackend>(new StandInSpeaker()));
        pc::Ptt vox;
        CHECK(transmit_live(output, vox, source, k_card_rate, &view).drained);
        const std::string frame = render(tui);
        CHECK(has(frame, sent_title));
        CHECK(has(frame, "idle  the transmission has ended"));
    }
}

// Ctrl-C (SIGINT) during a transmission stops the sound card, never the program: the PTT is released, the summary
// says how far it got, exit 1.
TEST(encode_program_ctrl_c_stops_and_releases_the_ptt) {
    using namespace encode_test;
    const std::string long_text(200, 'U');
    const Options o = parse({"--text", long_text, "--bps", "25", "-d", "coreaudio:stand-in"});
    Opened opened;
    StandInSpeaker* speaker = nullptr;
    RecordingPtt* ptt = nullptr;
    open_stand_ins(o, opened, speaker, ptt);
    speaker->hold_after = 20;  // then it waits: Ctrl-C comes in the middle of the transmission
    test::DefaultSignal interrupt(SIGINT);
    std::atomic<bool> held(false);
    std::string printed;
    std::string warned;
    int status = -1;
    {
        test::OutputCapture out(STDOUT_FILENO);
        test::OutputCapture err(STDERR_FILENO);
        std::thread operator_thread([&] {
            held.store(speaker->wait_held());
            std::raise(SIGINT);
        });
        status = send(o, opened);
        operator_thread.join();
        printed = out.finish();
        warned = err.finish();
    }
    CHECK(held.load());
    CHECK_EQ(status, k_exit_stopped);
    REQUIRE(ptt->keyings.size() == 2u);
    CHECK(ptt->keyings[0].on && !ptt->keyings[1].on);
    CHECK(!ptt->keyed());
    CHECK(has(printed, "stopped    by Ctrl-C (SIGINT) after "));
    CHECK(has(printed, " of 200 bytes; the PTT released\n"));
    CHECK(has(warned, "unlimited_encode: the transmission was stopped before its end"));
}

// A PTT that cannot be keyed: nothing is played, exit 3 with the PTT named.
TEST(encode_program_ptt_failure_sends_nothing) {
    using namespace encode_test;
    const Options o = parse({"--text", k_text, "-d", "coreaudio:stand-in"});
    Opened opened;
    StandInSpeaker* speaker = nullptr;
    RecordingPtt* ptt = nullptr;
    open_stand_ins(o, opened, speaker, ptt);
    ptt->refuse = true;
    std::string printed;
    std::string warned;
    int status = -1;
    {
        test::OutputCapture out(STDOUT_FILENO);
        test::OutputCapture err(STDERR_FILENO);
        status = send(o, opened);
        printed = out.finish();
        warned = err.finish();
    }
    CHECK_EQ(status, k_exit_io);
    CHECK_EQ(speaker->pulled(), 0u);
    CHECK(has(warned, "unlimited_encode: PTT: keying failed (recording PTT)"));
}
