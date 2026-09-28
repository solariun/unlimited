#include "radio_options.hpp"
#include "test_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The shared sound-device and PTT options of the programs (spec 12.3 to 12.5).

using unlimited::pc::PttMethod;
using unlimited::pc::RadioOptions;
using unlimited::pc::k_radio_input;
using unlimited::pc::k_radio_output;
using unlimited::pc::k_radio_ptt;

namespace {

using std::size_t;
using std::uint32_t;
using std::uint8_t;
using test::count_of;

const unsigned k_decoder = k_radio_input;               // unlimited_decode
const unsigned k_encoder = k_radio_output | k_radio_ptt;  // unlimited_encode
const unsigned k_modem = k_radio_input | k_radio_output | k_radio_ptt;

// Feeds a command line the way a program does; false with the error of the first refused option.
bool parse(RadioOptions& options, const std::vector<std::string>& args, std::string& error) {
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string option = args[i];
        bool has_value = false;
        if (!options.takes(option, has_value)) {
            error = option + " is not taken";
            return false;
        }
        std::string value;
        if (has_value) {
            if (i + 1 >= args.size()) {
                error = option + " needs a value";
                return false;
            }
            value = args[++i];
        }
        if (!options.apply(option, value, error)) return false;
    }
    return options.check(error);
}

std::vector<std::string> words(const char* const* list, size_t count) {
    return std::vector<std::string>(list, list + count);
}

}  // namespace

TEST(radio_options_each_program_takes_its_own) {
    const char* const device_options[] = {"--list-devices", "-d", "-r"};
    const char* const ptt_options[] = {"--ptt",     "--ptt-device", "--ptt-invert", "--cat-rate",
                                       "--cat-addr", "--cat-tx-on",  "--cat-tx-off"};
    const RadioOptions decoder(k_decoder);
    const RadioOptions encoder(k_encoder);
    const RadioOptions modem(k_modem);
    bool has_value = false;
    for (size_t i = 0; i < count_of(device_options); ++i) {
        CHECK(decoder.takes(device_options[i], has_value));
        CHECK(encoder.takes(device_options[i], has_value));
        CHECK(modem.takes(device_options[i], has_value));
    }
    for (size_t i = 0; i < count_of(ptt_options); ++i) {
        CHECK(!decoder.takes(ptt_options[i], has_value));
        CHECK(encoder.takes(ptt_options[i], has_value));
        CHECK(modem.takes(ptt_options[i], has_value));
    }
    CHECK(decoder.takes("--input", has_value) && has_value);
    CHECK(!decoder.takes("--output", has_value));
    CHECK(encoder.takes("--output", has_value) && has_value);
    CHECK(!encoder.takes("--input", has_value));
    CHECK(modem.takes("--list-devices", has_value) && !has_value);
    CHECK(modem.takes("--ptt-invert", has_value) && !has_value);
    CHECK(!modem.takes("--bps", has_value));  // a program's own option
    CHECK(!modem.takes("--ptt=rts", has_value));
}

TEST(radio_options_devices_and_rate) {
    std::string error;
    RadioOptions modem(k_modem);
    const char* const both[] = {"-d", "coreaudio:3", "-r", "44100", "--list-devices"};
    REQUIRE(parse(modem, words(both, count_of(both)), error));
    CHECK_EQ(modem.input, std::string("coreaudio:3"));
    CHECK_EQ(modem.output, std::string("coreaudio:3"));
    CHECK_EQ(modem.rate_hz, 44100u);
    CHECK(modem.list_devices);

    RadioOptions split(k_modem);
    const char* const later_wins[] = {"-d", "alsa:plughw:CARD=CODEC,DEV=0", "--input", "wav:rx.wav", "--output",
                                      "null"};
    REQUIRE(parse(split, words(later_wins, count_of(later_wins)), error));
    CHECK_EQ(split.input, std::string("wav:rx.wav"));
    CHECK_EQ(split.output, std::string("null"));

    RadioOptions decoder(k_decoder);
    const char* const decoder_d[] = {"-d", "default"};
    REQUIRE(parse(decoder, words(decoder_d, count_of(decoder_d)), error));
    CHECK_EQ(decoder.input, std::string("default"));
    CHECK(decoder.output.empty());
    CHECK(!decoder.list_devices);
    CHECK_EQ(decoder.rate_hz, 0u);

    const char* const bad[][2] = {{"-d", "3"},          {"--input", "CODEC"}, {"-r", "7999"},
                                  {"-r", "384001"},     {"-r", "48k"},       {"-r", "99999999999"},
                                  {"--output", "nul"},  {"-r", ""}};
    for (size_t i = 0; i < count_of(bad); ++i) {
        RadioOptions options(k_modem);
        error.clear();
        if (!CHECK(!parse(options, words(bad[i], 2), error))) NOTE("%s %s accepted", bad[i][0], bad[i][1]);
        CHECK(error.find(bad[i][0]) == 0);  // the message starts with the option
    }
}

TEST(radio_options_ptt_methods) {
    struct Case {
        const char* name;
        PttMethod method;
        bool invert;
    };
    const Case cases[] = {{"vox", PttMethod::vox, false},         {"rts", PttMethod::rts, false},
                          {"+rts", PttMethod::rts, false},        {"-rts", PttMethod::rts, true},
                          {"dtr", PttMethod::dtr, false},         {"+dtr", PttMethod::dtr, false},
                          {"-dtr", PttMethod::dtr, true},         {"icom", PttMethod::icom, false},
                          {"yaesu", PttMethod::yaesu, false},     {"kenwood", PttMethod::kenwood, false},
                          {"cat", PttMethod::cat, false}};
    for (size_t i = 0; i < count_of(cases); ++i) {
        unlimited::pc::PttOptions ptt;
        std::string error;
        CHECK(unlimited::pc::parse_ptt_method(cases[i].name, ptt, error));
        CHECK(ptt.method == cases[i].method);
        CHECK_EQ(ptt.invert, cases[i].invert);
    }
    unlimited::pc::PttOptions ptt;
    std::string error;
    CHECK(!unlimited::pc::parse_ptt_method("RTS", ptt, error));
    CHECK(error.find("vox, rts, +rts, -rts, dtr, +dtr, -dtr, icom, yaesu, kenwood, cat") != std::string::npos);
    CHECK(!unlimited::pc::parse_ptt_method("hamlib", ptt, error));

    // --ptt-invert holds whatever the order; +rts and -rts say it themselves.
    RadioOptions inverted(k_encoder);
    const char* const invert_first[] = {"--ptt-invert", "--ptt", "rts", "--ptt-device", "/dev/ttyUSB0"};
    REQUIRE(parse(inverted, words(invert_first, count_of(invert_first)), error));
    CHECK(inverted.ptt.invert);
    CHECK_EQ(inverted.ptt.device, std::string("/dev/ttyUSB0"));
    RadioOptions plus(k_encoder);
    const char* const plus_after[] = {"--ptt-invert", "--ptt", "+dtr", "--ptt-device", "/dev/cu.usbserial-110"};
    REQUIRE(parse(plus, words(plus_after, count_of(plus_after)), error));
    CHECK(!plus.ptt.invert);
    CHECK(plus.ptt.method == PttMethod::dtr);
}

TEST(radio_options_cat_values) {
    struct Address {
        const char* text;
        int value;  // -1: refused
    };
    const Address addresses[] = {{"0x94", 0x94}, {"94h", 0x94}, {"0XA4", 0xA4}, {"a4H", 0xA4}, {"0x0", 0x00},
                                 {"0xdf", 0xDF}, {"94", -1},    {"0xE0", -1},   {"0xFE", -1},  {"0x100", -1},
                                 {"", -1},       {"0x", -1},    {"h", -1},      {"zzh", -1},   {"-0x94", -1}};
    for (size_t i = 0; i < count_of(addresses); ++i) {
        uint8_t address = 0x11;
        std::string error;
        const bool accepted = unlimited::pc::parse_cat_address(addresses[i].text, address, error);
        if (!CHECK_EQ(accepted, addresses[i].value >= 0)) NOTE("--cat-addr '%s'", addresses[i].text);
        if (accepted) CHECK_EQ(static_cast<int>(address), addresses[i].value);
        if (!accepted) CHECK(error.find("--cat-addr") == 0);
    }
    const uint32_t rates[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
    for (size_t i = 0; i < count_of(rates); ++i) {
        uint32_t rate = 0;
        std::string error;
        CHECK(unlimited::pc::parse_cat_rate(std::to_string(rates[i]), rate, error));
        CHECK_EQ(rate, rates[i]);
    }
    const char* const bad_rates[] = {"", "0", "300", "14400", "230400", "19200 ", "fast"};
    for (size_t i = 0; i < count_of(bad_rates); ++i) {
        uint32_t rate = 7;
        std::string error;
        CHECK(!unlimited::pc::parse_cat_rate(bad_rates[i], rate, error));
        CHECK_EQ(rate, 7u);
        CHECK(error.find("115200") != std::string::npos);
    }

    RadioOptions custom(k_modem);
    std::string error;
    const char* const cat[] = {"--ptt",      "cat",       "--ptt-device", "/dev/ttyUSB1",     "--cat-rate", "9600",
                               "--cat-tx-on", "FEFE94E01C0001FD", "--cat-tx-off", "FE FE 94 E0 1C 00 00 FD"};
    REQUIRE(parse(custom, words(cat, count_of(cat)), error));
    CHECK_EQ(custom.ptt.cat_rate, 9600u);
    CHECK_EQ(unlimited::pc::hex_text(custom.ptt.cat_on), std::string("FE FE 94 E0 1C 00 01 FD"));
    CHECK_EQ(unlimited::pc::hex_text(custom.ptt.cat_off), std::string("FE FE 94 E0 1C 00 00 FD"));
    RadioOptions icom(k_encoder);
    const char* const ic705[] = {"--ptt", "icom", "--ptt-device", "/dev/cu.usbmodem1", "--cat-addr", "0xA4"};
    REQUIRE(parse(icom, words(ic705, count_of(ic705)), error));
    CHECK_EQ(static_cast<int>(icom.ptt.cat_address), 0xA4);
    RadioOptions bad_hex(k_encoder);
    const char* const odd[] = {"--cat-tx-on", "FEF"};
    CHECK(!parse(bad_hex, words(odd, count_of(odd)), error));
    CHECK(error.find("--cat-tx-on: ") == 0);
}

TEST(radio_options_rules_between_options) {
    struct Case {
        std::vector<std::string> args;
        const char* reason;  // nullptr: accepted
    };
    const char* const vox_default[] = {"-d", "null"};
    const char* const rts_no_device[] = {"--ptt", "rts"};
    const char* const icom_no_device[] = {"--ptt", "icom"};
    const char* const device_vox[] = {"--ptt-device", "/dev/ttyUSB0"};
    const char* const invert_icom[] = {"--ptt", "icom", "--ptt-device", "/dev/ttyUSB0", "--ptt-invert"};
    const char* const minus_then_icom[] = {"--ptt", "-rts", "--ptt", "icom", "--ptt-device", "/dev/ttyUSB0"};
    const char* const rate_rts[] = {"--ptt", "rts", "--ptt-device", "/dev/ttyUSB0", "--cat-rate", "9600"};
    const char* const addr_yaesu[] = {"--ptt", "yaesu", "--ptt-device", "/dev/ttyUSB0", "--cat-addr", "0x70"};
    const char* const on_kenwood[] = {"--ptt", "kenwood", "--ptt-device", "/dev/ttyUSB0", "--cat-tx-on", "00"};
    const char* const cat_half[] = {"--ptt", "cat", "--ptt-device", "/dev/ttyUSB0", "--cat-tx-on", "00"};
    const char* const device_dash[] = {"--ptt", "rts", "--ptt-device", "--ptt-invert"};
    const char* const yaesu_ok[] = {"--ptt", "yaesu", "--ptt-device", "/dev/ttyUSB0", "--cat-rate", "38400"};
    const char* const dtr_ok[] = {"--ptt", "-dtr", "--ptt-device", "/dev/cu.usbserial-A1"};
    const Case cases[] = {{words(vox_default, count_of(vox_default)), nullptr},
                          {words(rts_no_device, count_of(rts_no_device)), "--ptt rts needs --ptt-device"},
                          {words(icom_no_device, count_of(icom_no_device)), "--ptt icom needs --ptt-device"},
                          {words(device_vox, count_of(device_vox)), "the PTT is vox"},
                          {words(invert_icom, count_of(invert_icom)), "--ptt-invert (and -rts, -dtr) applies"},
                          {words(minus_then_icom, count_of(minus_then_icom)), "--ptt-invert (and -rts, -dtr) applies"},
                          {words(rate_rts, count_of(rate_rts)), "--cat-rate applies"},
                          {words(addr_yaesu, count_of(addr_yaesu)), "--cat-addr applies to --ptt icom"},
                          {words(on_kenwood, count_of(on_kenwood)), "--cat-tx-on and --cat-tx-off apply"},
                          {words(cat_half, count_of(cat_half)), "--ptt cat needs both"},
                          {words(device_dash, count_of(device_dash)), "--ptt-device needs a serial port"},
                          {words(yaesu_ok, count_of(yaesu_ok)), nullptr},
                          {words(dtr_ok, count_of(dtr_ok)), nullptr}};
    for (size_t i = 0; i < count_of(cases); ++i) {
        RadioOptions options(k_modem);
        std::string error;
        const bool accepted = parse(options, cases[i].args, error);
        if (cases[i].reason == nullptr) {
            if (!CHECK(accepted)) NOTE("case %zu: %s", i, error.c_str());
            continue;
        }
        CHECK(!accepted);
        if (!CHECK(error.find(cases[i].reason) != std::string::npos)) NOTE("case %zu: %s", i, error.c_str());
    }
}

TEST(radio_options_help_lists_each_option) {
    const std::string modem = RadioOptions(k_modem).help();
    const char* const all[] = {"--list-devices", "-d SPEC", "--input SPEC", "--output SPEC", "-r HZ", "--ptt METHOD",
                               "--ptt-device DEV", "--ptt-invert", "--cat-rate BAUD", "--cat-addr ADDR",
                               "--cat-tx-on HEX", "--cat-tx-off HEX"};
    for (size_t i = 0; i < count_of(all); ++i)
        if (!CHECK(modem.find(all[i]) != std::string::npos)) NOTE("missing %s", all[i]);
    const std::string decoder = RadioOptions(k_decoder).help();
    CHECK(decoder.find("--ptt") == std::string::npos);
    CHECK(decoder.find("--output") == std::string::npos);
    CHECK(decoder.find("the input device") != std::string::npos);
    NOTE("unlimited_modem's shared options:\n%s", modem.c_str());
}
