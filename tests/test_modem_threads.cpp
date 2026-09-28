// The KISS modem's four contexts on their own threads (spec 12.2), as unlimited_modem runs them: for each of two modems a
// computer thread (host_input(), with back-pressure), a control thread (tick() on the audio clock) and a receiving
// thread (audio_input()); one audio thread plays both (audio_output()) and hands each one's audio to the other's
// receiving thread; a display thread reads the snapshots, as the --tui view does. The frames written into A come out
// of B byte for byte, and B's answer out of A. Under ThreadSanitizer this is the test of the core's hand-offs: the
// harness adds no ordering of its own between the modem's contexts (its wake-ups and its clock are relaxed atomics),
// so a value one context gives another without load_acquire()/store_release() shows up as a race.
#include "test_harness.hpp"
#include "unlimited/modem.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using unlimited::KissDecoder;
using unlimited::KissStep;
using unlimited::Modem;
using unlimited::ModemConfig;
using unlimited::ModemCounters;
using unlimited::ModemWake;

namespace {

using std::int16_t;
using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

typedef std::vector<uint8_t> Bytes;

const size_t k_step_samples = 80;                  // the audio thread plays 10 ms at a time
const uint64_t k_samples_per_ms = unlimited::k_modem_rate_hz / 1000;
const size_t k_ring_samples = 1 << 16;             // 8 s of audio between the audio thread and a receiving thread
const size_t k_ring_room = k_ring_samples / 2;     // the audio thread waits while a ring is fuller than this
const size_t k_pop_samples = 256;
const std::chrono::milliseconds k_nap(1);          // a thread with nothing to do sleeps this long, or until woken
const std::chrono::seconds k_limit(60);            // real time allowed for the whole exchange

// A wake-up a thread naps on. It must not order the modem's contexts itself (a mutex or a condition variable would
// hide a hand-off the core lacks), so it is a relaxed flag, looked at after each nap: here, and only here, a thread
// polls.
class Wake {
public:
    void post() { posted_.store(true, std::memory_order_relaxed); }

    void wait() {
        if (!posted_.exchange(false, std::memory_order_relaxed)) std::this_thread::sleep_for(k_nap);
    }

private:
    std::atomic<bool> posted_{false};
};

// Audio from the audio thread to a receiving thread: one producer, one consumer, no lock.
class Ring {
public:
    Ring() : data_(k_ring_samples), written_(0), read_(0) {}

    size_t fill() const { return written_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire); }

    void push(const int16_t* samples, size_t count) {
        const size_t written = written_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; ++i) data_[(written + i) % k_ring_samples] = samples[i];
        written_.store(written + count, std::memory_order_release);
    }

    size_t pop(int16_t* out, size_t count) {
        const size_t read = read_.load(std::memory_order_relaxed);
        const size_t available = written_.load(std::memory_order_acquire) - read;
        const size_t taken = std::min(count, available);
        for (size_t i = 0; i < taken; ++i) out[i] = data_[(read + i) % k_ring_samples];
        read_.store(read + taken, std::memory_order_release);
        return taken;
    }

private:
    std::vector<int16_t> data_;
    std::atomic<size_t> written_;
    std::atomic<size_t> read_;
};

struct Station {
    std::unique_ptr<Modem> modem;
    Wake control;
    Wake host;
    Ring input;
    std::mutex received_mutex;
    Bytes received;  // what the modem gave its computer
    std::atomic<uint32_t> keys{0};

    static void on_host(const uint8_t* data, size_t size, void* context) {
        Station& station = *static_cast<Station*>(context);
        std::lock_guard<std::mutex> lock(station.received_mutex);
        station.received.insert(station.received.end(), data, data + size);
    }

    static void on_ptt(bool on, void* context) {
        if (on) ++static_cast<Station*>(context)->keys;
    }

    static void on_wake(ModemWake what, void* context) {
        Station& station = *static_cast<Station*>(context);
        if (what == ModemWake::control) {
            station.control.post();
        } else {
            station.host.post();
        }
    }

    std::vector<Bytes> frames() {
        std::lock_guard<std::mutex> lock(received_mutex);
        KissDecoder kiss;
        std::vector<Bytes> out;
        Bytes frame;
        for (size_t i = 0; i < received.size(); ++i) {
            uint8_t value = 0;
            const KissStep step = kiss.feed(received[i], value);
            if (step == KissStep::data) frame.push_back(value);
            if (step == KissStep::end) {
                out.push_back(frame);
                frame.clear();
            }
            if (!kiss.in_data()) frame.clear();
        }
        return out;
    }
};

Bytes kiss_frame(const Bytes& data) {
    Bytes out(1, unlimited::k_kiss_fend);
    out.push_back(unlimited::k_kiss_data);
    for (size_t i = 0; i < data.size(); ++i) {
        uint8_t escaped[unlimited::k_kiss_escaped_max];
        const uint8_t size = unlimited::kiss_escape(data[i], escaped);
        out.insert(out.end(), escaped, escaped + size);
    }
    out.push_back(unlimited::k_kiss_fend);
    return out;
}

ModemConfig config_at(uint16_t centi, uint32_t seed) {
    ModemConfig config;
    config.signal.slot_us = unlimited::slot_us_for_centi_speed(centi);
    if (!unlimited::passband_fit(config.signal).fits) {
        config.signal.passband.low_hz = unlimited::k_am_passband_low_hz;
        config.signal.passband.high_hz = unlimited::k_am_passband_high_hz;
    }
    config.receiver.slot_us = config.signal.slot_us;
    config.receiver.passband = config.signal.passband;
    config.access.seed = seed;
    config.min_frame_bytes = 0;  // frames of 1..12 bytes: every reception streams (V23 off)
    return config;
}

}  // namespace

TEST(modem_runs_its_four_contexts_on_four_threads) {
    const uint16_t centi = 2500;
    Station a;
    Station b;
    a.modem.reset(new Modem(config_at(centi, 1), &Station::on_host, &Station::on_ptt, &a, &Station::on_wake));
    b.modem.reset(new Modem(config_at(centi, 2), &Station::on_host, &Station::on_ptt, &b, &Station::on_wake));
    std::vector<Bytes> questions;
    questions.push_back(Bytes(10, 'q'));
    const uint8_t special[] = {0xC0, 0xDB, 0xDC, 0xDD, 0x00, 'T', 'S', 'a', 'n'};
    questions.push_back(Bytes(special, special + sizeof(special)));
    questions.push_back(Bytes(1, '!'));
    const Bytes answer(12, 'a');

    std::atomic<bool> done(false);
    std::atomic<uint64_t> clock(0);  // samples played: the audio clock the control threads tick on (relaxed)
    std::atomic<uint32_t> odd_snapshots(0);

    std::vector<std::thread> threads;
    // Audio: both modems play in lockstep, as one sound card would; each hears the other.
    threads.push_back(std::thread([&] {
        int16_t out_a[k_step_samples];
        int16_t out_b[k_step_samples];
        while (!done.load()) {
            if (a.input.fill() > k_ring_room || b.input.fill() > k_ring_room) {
                std::this_thread::sleep_for(k_nap);
                continue;
            }
            a.modem->audio_output(out_a, k_step_samples);
            b.modem->audio_output(out_b, k_step_samples);
            b.input.push(out_a, k_step_samples);
            a.input.push(out_b, k_step_samples);
            clock.fetch_add(k_step_samples, std::memory_order_relaxed);
        }
    }));
    // Display: the snapshots, from a thread of its own; the frame on the air never shows more bytes sent than it has.
    threads.push_back(std::thread([&] {
        while (!done.load()) {
            for (size_t s = 0; s < 2; ++s) {
                const Modem& modem = s == 0 ? *a.modem : *b.modem;
                const ModemCounters counters = modem.counters();
                if (counters.on_air_size != 0 && counters.on_air_sent > counters.on_air_size) ++odd_snapshots;
                if (counters.queued_frames > unlimited::ModemTransmitter::k_frame_slots) ++odd_snapshots;
                (void)modem.dcd();
                (void)modem.transmitting();
                (void)modem.channel_state();
            }
            std::this_thread::sleep_for(k_nap);
        }
    }));
    Station* stations[] = {&a, &b};
    for (size_t s = 0; s < 2; ++s) {
        Station& station = *stations[s];
        // Receiving side.
        threads.push_back(std::thread([&station, &done] {
            int16_t in[k_pop_samples];
            while (!done.load()) {
                const size_t count = station.input.pop(in, k_pop_samples);
                if (count == 0) {
                    std::this_thread::sleep_for(k_nap);
                    continue;
                }
                station.modem->audio_input(in, count);
            }
        }));
        // Control side, on the audio clock.
        threads.push_back(std::thread([&station, &done, &clock] {
            while (!done.load()) {
                station.modem->tick(static_cast<uint32_t>(clock.load(std::memory_order_relaxed) / k_samples_per_ms));
                station.control.wait();
            }
        }));
    }
    // A's computer: the questions, with back-pressure.
    threads.push_back(std::thread([&] {
        Bytes stream;
        for (size_t i = 0; i < questions.size(); ++i) {
            const Bytes kiss = kiss_frame(questions[i]);
            stream.insert(stream.end(), kiss.begin(), kiss.end());
        }
        size_t sent = 0;
        while (!done.load() && sent < stream.size()) {
            sent += a.modem->host_input(&stream[sent], stream.size() - sent);
            if (sent < stream.size()) a.host.wait();
        }
    }));
    // B's computer: an answer once every question has arrived.
    threads.push_back(std::thread([&] {
        while (!done.load() && b.frames().size() < questions.size()) std::this_thread::sleep_for(k_nap);
        const Bytes kiss = kiss_frame(answer);
        size_t sent = 0;
        while (!done.load() && sent < kiss.size()) {
            sent += b.modem->host_input(&kiss[sent], kiss.size() - sent);
            if (sent < kiss.size()) b.host.wait();
        }
    }));

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < k_limit && a.frames().empty()) std::this_thread::sleep_for(k_nap);
    done.store(true);
    a.control.post();
    b.control.post();
    a.host.post();
    b.host.post();
    for (size_t i = 0; i < threads.size(); ++i) threads[i].join();

    CHECK(b.frames() == questions);
    const std::vector<Bytes> answers = a.frames();
    REQUIRE(answers.size() == 1);
    CHECK(answers[0] == answer);
    CHECK_EQ(a.keys.load(), static_cast<uint32_t>(questions.size()));
    CHECK_EQ(b.keys.load(), 1u);
    CHECK_EQ(odd_snapshots.load(), 0u);
    NOTE("%zu frames A -> B and 1 back, %.1f s of audio in %lld ms on 9 threads", questions.size(),
         clock.load() / static_cast<double>(unlimited::k_modem_rate_hz),
         static_cast<long long>(
             std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()));
}
