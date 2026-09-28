// unlimited_modem's command line (spec 12.3): kiss_modem's defaults, the lead each PTT method gets, the refusals, the
// help text, the monitor's words for a frame and --test-tx's AX.25 UI frame.
#include "../demo/modem_cli.hpp"
#include "test_harness.hpp"

#include <string>
#include <vector>

namespace mc = unlimited::modem_cli;

using unlimited::ModemConfig;
using unlimited::cli::UsageError;

namespace {

using std::size_t;
using std::uint8_t;

typedef std::vector<uint8_t> Bytes;

mc::Options parse(const std::vector<std::string>& words) {
    std::vector<std::string> args(1, "unlimited_modem");
    args.insert(args.end(), words.begin(), words.end());
    std::vector<char*> argv;
    for (size_t i = 0; i < args.size(); ++i) argv.push_back(&args[i][0]);
    return mc::parse_options(static_cast<int>(argv.size()), &argv[0]);
}

std::vector<std::string> words(const char* a, const char* b = nullptr, const char* c = nullptr,
                               const char* d = nullptr, const char* e = nullptr, const char* f = nullptr) {
    std::vector<std::string> list;
    const char* all[] = {a, b, c, d, e, f};
    for (size_t i = 0; i < test::count_of(all); ++i)
        if (all[i] != nullptr) list.push_back(all[i]);
    return list;
}

bool refused(const std::vector<std::string>& list) {
    try {
        parse(list);
    } catch (const UsageError&) {
        return true;
    }
    return false;
}

}  // namespace

// kiss_modem's defaults, but the TX delay: 100 ms (Gustavo, 2026-09-28: radios switch to transmit in 20..100 ms).
TEST(modem_cli_defaults) {
    const mc::Options o = parse(std::vector<std::string>());
    CHECK_EQ(o.txdelay, 10u);
    CHECK_EQ(o.txtail, 10u);
    CHECK_EQ(o.persist, 63u);
    CHECK_EQ(o.slottime, 10u);
    CHECK_EQ(o.dwait_ms, 1500u);
    CHECK_EQ(o.link, std::string("/tmp/unlimited"));
    CHECK_EQ(o.serial_baud, 115200u);
    CHECK(o.vox());
    CHECK(o.decision_mode == unlimited::DecisionMode::adaptive);
    CHECK_EQ(o.fade_bridge, unlimited::k_default_fade_bridge);
    CHECK_EQ(o.min_frame, 15u);  // V23: the shortest AX.25 frame
    const ModemConfig config = mc::modem_config(o);
    CHECK_EQ(static_cast<unsigned>(config.min_frame_bytes), 15u);
    CHECK(config.valid());
    CHECK_EQ(config.signal.slot_us, unlimited::slot_us_for_speed(6.0f));
    CHECK_EQ(config.signal.lead_in_ms, 0u);  // VOX: the lead tone instead of the TX delay
    CHECK_EQ(config.signal.vox_lead_ms, unlimited::k_default_vox_lead_ms);
    CHECK_EQ(config.signal.tail_ms, 100u);
    CHECK_EQ(config.access.slot_time_ms, 100u);
    CHECK_EQ(config.access.dwait_ms, 1500u);
    CHECK_EQ(config.signal.amplitude, 23197);  // -3 dBFS, as unlimited_encode
    CHECK(!config.access.full_duplex);
    CHECK(mc::config_problem(config).empty());
    CHECK(mc::min_frame_text(o).find("15 bytes (--min-frame 15)") == 0);
    CHECK(mc::min_frame_text(o).find("14 windows (2.33 s)") != std::string::npos);
    const mc::Options off = parse(words("--min-frame", "0"));
    CHECK_EQ(static_cast<unsigned>(mc::modem_config(off).min_frame_bytes), 0u);
    CHECK(mc::min_frame_text(off).find("off (--min-frame 0)") == 0);
    CHECK_EQ(static_cast<unsigned>(mc::modem_config(parse(words("--min-frame", "64"))).min_frame_bytes), 64u);
}

// The TX delay goes with a line or CAT PTT, the lead tone with VOX; each option refuses the other kind.
TEST(modem_cli_each_ptt_gets_its_lead) {
    const mc::Options rts = parse(words("--ptt", "rts", "--ptt-device", "/dev/cu.usbserial-110", "--txdelay", "25"));
    const ModemConfig line = mc::modem_config(rts, 14);
    CHECK_EQ(line.signal.lead_in_ms, 250u);
    CHECK_EQ(line.signal.vox_lead_ms, 0u);
    CHECK_EQ(line.access.output_latency_ms, 14u);
    const mc::Options icom =
        parse(words("--ptt", "icom", "--ptt-device", "/dev/ttyUSB0", "--cat-addr", "0xA4"));
    CHECK_EQ(mc::modem_config(icom).signal.lead_in_ms, unlimited::k_default_txdelay_ms);
    CHECK_EQ(static_cast<unsigned>(icom.radio.ptt.cat_address), 0xA4u);
    const mc::Options vox = parse(words("--vox-lead-ms", "300"));
    CHECK_EQ(mc::modem_config(vox).signal.vox_lead_ms, 300u);
    CHECK(refused(words("--txdelay", "20")));                                          // with vox
    CHECK(refused(words("--ptt", "dtr", "--ptt-device", "/dev/x", "--vox-lead-ms", "100")));
    // The fade bridge: a TX delay of at least 300 ms of silence with a line or CAT, the VOX lead as it is.
    const mc::Options bridged =
        parse({"--ptt", "rts", "--ptt-device", "/dev/x", "--txdelay", "10", "--fade-bridge"});
    CHECK(bridged.fade_bridge);
    CHECK_EQ(mc::modem_config(bridged).sent().lead_in_ms, unlimited::k_fade_bridge_silence_ms);
    CHECK(!parse(words("--fade-bridge", "--no-fade-bridge")).fade_bridge);
}

TEST(modem_cli_refusals) {
    CHECK(refused(words("--bogus")));
    CHECK(refused(words("--bps")));                                    // a value missing
    CHECK(refused(words("--persist", "256")));
    CHECK(refused(words("--slottime", "0")));
    CHECK(refused(words("--txtail", "2.5")));
    CHECK(refused(words("--volume", "0")));
    CHECK(refused(words("--level-dbfs", "3")));
    CHECK(refused(words("--volume", "50", "--level-dbfs", "-6")));
    CHECK(refused(words("--serial-baud", "9600")));                    // without --serial
    CHECK(refused(words("--serial", "/dev/x", "--serial-baud", "12345")));
    CHECK(refused(words("--serial", "/dev/x", "--link", "/tmp/k")));
    CHECK(refused(words("--ptt", "rts")));                             // no --ptt-device (RadioOptions::check)
    CHECK(refused(words("--loopback", "--test-ptt")));
    CHECK(refused(words("-c", "TOOLONGCALL")));
    CHECK(refused(words("-c", "N0CALL-16")));
    CHECK(refused(words("--debug", "4")));
    CHECK(refused(words("--min-frame", "65")));                       // at most k_max_min_frame_bytes
    CHECK(refused(words("--min-frame", "-1")));
    CHECK(refused(words("--min-frame", "2.5")));
    CHECK(refused(words("--test-tx", "")));
    CHECK(refused(words("--tui", "--input", "null")));                 // nothing heard to show
    CHECK(refused(words("--tui", "--loopback")));
    CHECK(parse(words("--tui", "-d", "coreaudio:3")).tui);
    CHECK_EQ(parse(words("-c", "pu2uit-7")).callsign, std::string("PU2UIT-7"));
    // The configuration's own rules: refused afterwards (exit 1), with the reason.
    const std::string misfit = mc::config_problem(mc::modem_config(parse(words("--bps", "25", "--passband", "1250:1750"))));
    CHECK(misfit.find("does not fit") != std::string::npos);
    const std::string threshold = mc::config_problem(mc::modem_config(parse(words("--threshold", "95"))));
    CHECK(threshold.find("threshold") != std::string::npos);
    CHECK(!mc::config_problem(mc::modem_config(parse(words("--bps", "0.5")))).empty());
}

TEST(modem_cli_loopback_takes_an_optional_snr) {
    const mc::Options with = parse(words("--loopback", "7.5"));
    CHECK(with.loopback && with.has_loopback_snr);
    CHECK_NEAR(with.loopback_snr_db, 7.5, 1e-9);
    const mc::Options negative = parse(words("--loopback", "-3"));
    CHECK(negative.has_loopback_snr);
    CHECK_NEAR(negative.loopback_snr_db, -3.0, 1e-9);
    const mc::Options without = parse(words("--loopback", "--monitor"));
    CHECK(without.loopback && !without.has_loopback_snr && without.monitor);
}

// Every option the help names is accepted, and every option accepted is named in the help.
TEST(modem_cli_help_lists_each_option) {
    const std::string help = mc::help_text(mc::Options());
    const char* const options[] = {"--bps",        "--tone",       "--passband",   "--level-dbfs",  "--volume",
                                   "--threshold",  "--fade-bridge", "--no-fade-bridge", "--link",   "--serial",
                                   "--min-frame",
                                   "--serial-baud", "--list-devices", "-d SPEC",   "--input",       "--output",
                                   "-r HZ",        "--ptt ",       "--ptt-device", "--ptt-invert",  "--cat-rate",
                                   "--cat-addr",   "--cat-tx-on",  "--cat-tx-off", "--txdelay",     "--vox-lead-ms",
                                   "--txtail",     "--persist",    "--slottime",   "--dwait",       "--full-duplex",
                                   "-c CALL",      "--monitor",    "--debug",      "--loopback",    "--test-ptt",
                                   "--test-tx",    "--tui",        "-h, --help"};
    for (size_t i = 0; i < test::count_of(options); ++i) {
        if (help.find(options[i]) == std::string::npos) NOTE("the help does not name %s", options[i]);
        CHECK(help.find(options[i]) != std::string::npos);
    }
    const mc::Options all = parse(
        {"--bps", "12", "--tone", "1200", "--passband", "200:2800", "--threshold", "70", "--volume", "50", "--link",
         "/tmp/k", "-d", "null", "--input", "null", "--output", "null", "-r", "48000", "--ptt", "icom", "--ptt-device",
         "/dev/x", "--cat-rate", "9600", "--cat-addr", "94h", "--txdelay", "30", "--txtail", "20", "--persist", "128",
         "--slottime", "5", "--dwait", "800", "--full-duplex", "--fade-bridge", "-c", "N0CALL", "--monitor",
         "--debug", "2"});
    const ModemConfig config = mc::modem_config(all);
    CHECK(config.valid());
    CHECK_EQ(config.signal.tone_hz, 1200u);
    CHECK_EQ(config.signal.lead_in_ms, 300u);
    CHECK_EQ(config.signal.tail_ms, 200u);
    CHECK_EQ(config.access.persist, 128u);
    CHECK_EQ(config.access.slot_time_ms, 50u);
    CHECK_EQ(config.access.dwait_ms, 800u);
    CHECK(config.access.full_duplex);
    CHECK(config.receiver.decision_mode == unlimited::DecisionMode::fixed);
    CHECK_EQ(static_cast<unsigned>(config.receiver.threshold_percent), 70u);
    CHECK_EQ(all.radio.rate_hz, 48000u);
    CHECK_EQ(all.debug, 2u);
}

// --test-tx with -c: an AX.25 UI frame CALL>CQ, byte for byte; the monitor reads it back.
TEST(modem_cli_ui_frame_and_monitor_words) {
    const Bytes frame = mc::ui_frame("PU2UIT-7", "hi");
    const uint8_t expected[] = {'C' << 1, 'Q' << 1, ' ' << 1, ' ' << 1, ' ' << 1, ' ' << 1, 0xE0,  // CQ, command
                                'P' << 1, 'U' << 1, '2' << 1, 'U' << 1, 'I' << 1, 'T' << 1, 0x6F,  // PU2UIT-7, last
                                0x03,     0xF0,     'h',      'i'};
    CHECK(frame == Bytes(expected, expected + test::count_of(expected)));
    const std::vector<std::string> ax25 = mc::frame_lines(frame);
    REQUIRE(ax25.size() == 2);
    CHECK_EQ(ax25[0], std::string("AX.25 PU2UIT-7>CQ [UI pid F0] \"hi\""));
    CHECK(ax25[1].compare(0, 6, "hex 86") == 0);

    const std::string hello = "hello, world\r\n";
    const std::vector<std::string> text = mc::frame_lines(Bytes(hello.begin(), hello.end()));
    REQUIRE(text.size() == 1);
    CHECK_EQ(text[0], std::string("\"hello, world\\r\\n\""));

    const uint8_t binary[] = {0xC0, 0xDB, 0x00, 'A'};
    const std::vector<std::string> raw = mc::frame_lines(Bytes(binary, binary + test::count_of(binary)));
    REQUIRE(raw.size() == 2);
    CHECK_EQ(raw[0], std::string("\"\\xC0\\xDB\\x00A\""));
    CHECK_EQ(raw[1], std::string("hex C0 DB 00 41"));
    // Callsign bytes that are not letters or digits: not AX.25, only text and hex.
    CHECK(mc::ax25_text(Bytes(20, 0x41)).empty());
}

TEST(modem_cli_airtime_is_the_encoders) {
    const mc::Options o = parse(words("--ptt", "rts", "--ptt-device", "/dev/x"));
    const unlimited::EncoderConfig signal = mc::modem_config(o).sent();
    const unlimited::Encoder encoder(signal);
    const std::string text = mc::airtime_text(signal);
    CHECK(text.find("1 byte " + mc::seconds_text(encoder.duration_samples(1))) == 0);
    CHECK(text.find("64 bytes " + mc::seconds_text(encoder.duration_samples(64))) != std::string::npos);
    CHECK(text.find("144 bytes " + mc::seconds_text(encoder.duration_samples(144))) != std::string::npos);
    NOTE("6 bytes/s, TX delay %u ms: %s", static_cast<unsigned>(signal.lead_in_ms), text.c_str());
}

namespace {

const std::size_t k_samples_per_ms = unlimited::k_modem_rate_hz / 1000;
const std::uint32_t k_max_render_ms = 60000;

struct Keying {
    bool keyed = false;
    static void on_ptt(bool on, void* context) { static_cast<Keying*>(context)->keyed = on; }
};

// Renders one frame through the modem the options describe, keyed at once: the samples from the key to the release.
std::vector<std::int16_t> keyed_audio(const mc::Options& o, const Bytes& frame) {
    ModemConfig config = mc::modem_config(o);
    config.access.full_duplex = true;  // no channel check: the key comes at the first tick
    Keying keying;
    unlimited::Modem modem(config, nullptr, &Keying::on_ptt, &keying);
    const Bytes kiss = mc::kiss_frame(frame);
    modem.host_input(kiss.data(), kiss.size());
    std::vector<std::int16_t> audio;
    std::int16_t step[k_samples_per_ms];
    bool was_keyed = false;
    for (std::uint32_t ms = 0; ms < k_max_render_ms && (!was_keyed || keying.keyed); ++ms) {
        modem.tick(ms);
        modem.audio_output(step, k_samples_per_ms);
        if (keying.keyed) audio.insert(audio.end(), step, step + k_samples_per_ms);
        was_keyed = was_keyed || keying.keyed;
    }
    return audio;
}

std::size_t first_sound(const std::vector<std::int16_t>& audio, std::size_t from = 0) {
    for (std::size_t i = from; i < audio.size(); ++i)
        if (audio[i] != 0) return i;
    return audio.size();
}

std::size_t first_silence(const std::vector<std::int16_t>& audio, std::size_t from, std::size_t run) {
    for (std::size_t i = from; i + run <= audio.size(); ++i) {
        std::size_t zeros = 0;
        while (zeros < run && audio[i + zeros] == 0) ++zeros;
        if (zeros == run) return i;
    }
    return audio.size();
}

std::size_t last_sound(const std::vector<std::int16_t>& audio) {
    for (std::size_t i = audio.size(); i > 0; --i)
        if (audio[i - 1] != 0) return i - 1;
    return 0;
}

}  // namespace

// The lead options reach the audio the modem plays (Gustavo, 2026-09-28): --txdelay N is N x 10 ms of silence from
// the key to the first START (100 ms by default, 300 ms at least with --fade-bridge), --vox-lead-ms the lead tone and
// its 2 silent slots, --txtail the silence after the last STOP. Measured on the rendered samples.
TEST(modem_cli_lead_options_reach_the_audio) {
    const Bytes frame(3, 0xA5);  // START, bits, STOP: every window begins and ends with a beep
    struct Case {
        std::vector<std::string> options;
        unsigned lead_ms;
    };
    const Case cases[] = {{{"--ptt", "rts", "--ptt-device", "/dev/x"}, unlimited::k_default_txdelay_ms},
                          {{"--ptt", "rts", "--ptt-device", "/dev/x", "--txdelay", "25"}, 250},
                          {{"--ptt", "icom", "--ptt-device", "/dev/x", "--txdelay", "0"}, 0},
                          {{"--ptt", "rts", "--ptt-device", "/dev/x", "--txdelay", "10", "--fade-bridge"}, 300},
                          {{"--ptt", "rts", "--ptt-device", "/dev/x", "--txdelay", "45", "--fade-bridge"}, 450}};
    for (std::size_t c = 0; c < test::count_of(cases); ++c) {
        const std::vector<std::int16_t> audio = keyed_audio(parse(cases[c].options), frame);
        const std::size_t start = first_sound(audio);
        const std::size_t expected = cases[c].lead_ms * k_samples_per_ms;
        CHECK(start >= expected && start <= expected + 2);
        NOTE("%s: the first START sounds %.3f ms after the key (asked %u ms)", cases[c].options.back().c_str(),
             start / static_cast<double>(k_samples_per_ms), cases[c].lead_ms);
    }
    // VOX: the lead tone from the key, 12 slots for 200 ms at 6 bytes/s, then 2 silent slots, then the START.
    const std::vector<std::int16_t> vox = keyed_audio(parse({"--vox-lead-ms", "200"}), frame);
    const double slot = unlimited::slot_us_for_speed(6.0f) / 1000.0 * k_samples_per_ms;
    const std::size_t lead_slots = 12;
    const std::size_t start_slot = lead_slots + unlimited::k_vox_gap_slots;
    CHECK(first_sound(vox) <= 1);
    const std::size_t gap = first_silence(vox, 0, static_cast<std::size_t>(slot));
    CHECK(gap + 2 >= static_cast<std::size_t>(lead_slots * slot) && gap <= static_cast<std::size_t>(lead_slots * slot) + 1);
    const std::size_t start = first_sound(vox, gap);
    CHECK(start + 1 >= static_cast<std::size_t>(start_slot * slot) &&
          start <= static_cast<std::size_t>(start_slot * slot) + 2);
    NOTE("--vox-lead-ms 200: lead tone %.2f ms, gap %.2f ms, the first START at %.2f ms", gap / 8.0,
         (start - gap) / 8.0, start / 8.0);
    // --txtail 30: 300 ms of silence after the last STOP, before the release (no output latency here).
    const std::vector<std::int16_t> tail =
        keyed_audio(parse({"--ptt", "rts", "--ptt-device", "/dev/x", "--txtail", "30"}), frame);
    const std::size_t after = tail.size() - 1 - last_sound(tail);
    CHECK(after >= 300 * k_samples_per_ms && after <= 300 * k_samples_per_ms + 2 * k_samples_per_ms);
    NOTE("--txtail 30: %.2f ms of silence after the last STOP, until the release", after / 8.0);
}

namespace {

std::vector<std::string> render_lines(const unlimited::pc::Tui& tui, int columns, int rows) {
    const std::string text = tui.render(columns, rows);
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t end = text.find('\n'); end != std::string::npos; end = text.find('\n', start)) {
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

const int k_view_columns = 160;
const int k_view_rows = 48;

}  // namespace

// The modem's --tui items for each channel state (spec 12.6): what is on the air, "sending n of m bytes" (m unknown
// while a frame longer than the queue still arrives), the frames waiting behind it, the frames each way.
TEST(modem_cli_tui_fields_name_each_state) {
    unlimited::ModemCounters c = unlimited::ModemCounters();
    c.queued_frames = 3;
    c.transmissions = 7;
    c.frames_received = 5;
    const unlimited::ChannelState states[] = {unlimited::ChannelState::idle, unlimited::ChannelState::waiting,
                                              unlimited::ChannelState::keyed, unlimited::ChannelState::releasing};
    const char* const expected[] = {"idle", "waiting for the channel", "sending 12 of 40 bytes", "sent, PTT releasing"};
    c.on_air_sent = 12;
    c.on_air_size = 40;
    for (std::size_t i = 0; i < test::count_of(states); ++i) {
        const std::vector<std::pair<std::string, std::string> > fields =
            mc::modem_fields(c, i == 2, i >= 2, states[i], "rts");
        REQUIRE(fields.size() == 5);
        CHECK_EQ(fields[0].first + " " + fields[0].second,
                 std::string(i == 2 ? "channel busy (DCD on)" : "channel clear (DCD off)"));
        CHECK_EQ(fields[1].first + " " + fields[1].second, std::string(i >= 2 ? "PTT on (rts)" : "PTT off (rts)"));
        CHECK_EQ(fields[2].first + " " + fields[2].second, "TX " + std::string(expected[i]));
        // The frame on the air is not waiting.
        CHECK_EQ(fields[3].second, std::string(i == 2 ? "2 frames waiting" : "3 frames waiting"));
        CHECK_EQ(fields[4].first + " " + fields[4].second, std::string("frames tx 7, rx 5"));
    }
    c.short_frames = 3;  // receptions shorter than --min-frame, dropped (V23)
    CHECK_EQ(mc::modem_fields(c, false, false, unlimited::ChannelState::idle, "vox")[4].second,
             std::string("tx 7, rx 5, 3 short dropped"));
    c.short_frames = 0;
    c.on_air_size = 0;  // a frame longer than the send queue: its size is not known yet
    CHECK_EQ(mc::modem_fields(c, false, true, unlimited::ChannelState::keyed, "vox")[2].second,
             std::string("sending 12 bytes"));
}

// The --tui view of a running modem, rendered as the TUI tests do: the decoder's view of what the radio hears (a
// reception's text) with the modem's items (DCD, PTT, what is being sent, the queue, the frames each way), the speed
// and the band.
TEST(modem_cli_tui_shows_the_modem) {
    const mc::Options o = parse({"--ptt", "icom", "--ptt-device", "/dev/x", "--bps", "25"});
    ModemConfig config = mc::modem_config(o);
    config.access.full_duplex = true;
    Keying keying;
    unlimited::Modem modem(config, nullptr, &Keying::on_ptt, &keying);
    const Bytes kiss = mc::kiss_frame(Bytes(40, 'k'));
    const Bytes more = mc::kiss_frame(Bytes(3, 'm'));
    modem.host_input(kiss.data(), kiss.size());
    modem.host_input(more.data(), more.size());
    std::int16_t step[k_samples_per_ms];
    const std::uint32_t window_ms = unlimited::k_window_slots * config.signal.slot_us / 1000u;
    const std::uint32_t until_ms = config.signal.lead_in_ms + 12 * window_ms + 5;
    for (std::uint32_t ms = 0; ms < until_ms; ++ms) {
        modem.tick(ms);
        modem.audio_output(step, k_samples_per_ms);
    }
    unlimited::pc::Tui tui(unlimited::pc::TuiMode::decoder);
    tui.set_color(false);
    tui.set_label("coreaudio:3");
    tui.set_speed(unlimited::bytes_per_second(config.receiver.slot_us));
    tui.set_tone_hz(config.signal.tone_hz);
    tui.set_slot_ms(static_cast<float>(unlimited::cli::slot_ms_of(config.receiver.slot_us)));
    tui.set_passband(config.receiver.passband);
    mc::show_modem(tui, modem, unlimited::pc::ptt_method_name(o.radio.ptt.method));
    unlimited::Event locked = unlimited::Event();
    locked.type = unlimited::EventType::locked;
    locked.state = unlimited::DecoderState::track;
    locked.tone_hz = 1500.0f;
    locked.slot_ms = 4.0f;
    locked.snr_db = 20.0f;
    tui.on_event(locked);
    const char text[] = "Hi";
    for (std::uint32_t i = 0; i < 2; ++i) {
        unlimited::Event byte = locked;
        byte.type = unlimited::EventType::byte;
        byte.value = static_cast<std::uint8_t>(text[i]);
        byte.byte_index = i;
        tui.on_event(byte);
    }
    const std::vector<std::string> lines = render_lines(tui, k_view_columns, k_view_rows);
    REQUIRE(lines.size() == static_cast<std::size_t>(k_view_rows));
    const std::string status = lines[0] + lines[1] + lines[2];
    CHECK(contains(status, "RX coreaudio:3"));
    CHECK(contains(status, "25.00 bytes/s"));
    CHECK(contains(status, "channel clear (DCD off)"));
    CHECK(contains(status, "PTT on (icom)"));
    CHECK(contains(status, "TX sending 12 of 40 bytes"));
    CHECK(contains(status, "queue 1 frame waiting"));
    CHECK(contains(status, "frames tx 0, rx 0"));
    CHECK(contains(status, "band "));
    bool text_shown = false;
    for (std::size_t i = 0; i < lines.size(); ++i) text_shown = text_shown || contains(lines[i], "Hi");
    CHECK(text_shown);
}

// In an 80 x 24 terminal the modem's items stay on screen: they come right after the receiver's state
// (Tui::set_front_field), before the receiver's details, which the small terminal leaves out first.
TEST(modem_cli_tui_items_fit_a_small_terminal) {
    unlimited::pc::Tui tui(unlimited::pc::TuiMode::decoder);
    tui.set_color(false);
    tui.set_label("coreaudio:3");
    tui.set_speed(6.0f);
    tui.set_tone_hz(1500.0f);
    tui.set_slot_ms(16.667f);
    const unlimited::pc::Level level = {48000, -12.3, -28.4, 0};
    tui.set_level(level);
    tui.set_field("threshold", "auto");
    unlimited::ModemCounters c = unlimited::ModemCounters();
    c.on_air_sent = 3;
    c.on_air_size = 20;
    const std::vector<std::pair<std::string, std::string> > fields =
        mc::modem_fields(c, false, true, unlimited::ChannelState::keyed, "rts");
    for (std::size_t i = 0; i < fields.size(); ++i) tui.set_front_field(fields[i].first, fields[i].second);
    const int small_columns = 80;
    const int small_rows = 24;
    const std::vector<std::string> lines = render_lines(tui, small_columns, small_rows);
    std::string status;
    for (std::size_t i = 0; i < 3 && i < lines.size(); ++i) status += lines[i];
    CHECK(contains(status, "channel clear (DCD off)"));
    CHECK(contains(status, "PTT on (rts)"));
    CHECK(contains(status, "TX sending 3 of 20 bytes"));
    CHECK(contains(status, "queue 0 frames waiting"));
    CHECK(contains(status, "frames tx 0, rx 0"));
    // A removed front item is gone.
    tui.set_front_field("PTT", "");
    const std::vector<std::string> after = render_lines(tui, small_columns, small_rows);
    CHECK(!contains(after[0] + after[1] + after[2], "PTT"));
}
