# Unlimited — Specification (SDD)

Unlimited is a C++11 API for robust data transmission over HF (SSB/AM) and VHF/UHF (FM) radio through the
audio path of an ordinary transceiver. Data rides on tone *peaks*: every data peak carries k = 1..8 bits by its
position on a tone grid anchored to the START/STOP marker tone (phase-flip markers). The sender dictates everything
(slot length T, bits per peak k, peaks per frame N, tone spacing, grid side); the receiver needs no speed,
frequency, mode or sideband setting: it chooses only the range of slot lengths it accepts (its profile, §1.4). The
primary target is HF SSB (USB or LSB, any tuning shift inside the tolerance of §1.3). The core targets PC (Linux,
macOS, Windows) and MCUs (Arduino, STM32, ESP8266, ESP32).

**Status (2026-09-26): v0.2 implemented, hardened, and its public API FROZEN (§5).** Verification on the frozen
tree: `make test` 213/213; `make test_long` 159 PASS / 0 FAIL / 87 REPORT; `make check_embedded` (host, xtensa, AVR,
decoder and queue variants, heap trap, AVR ISR cycle gate); `make arduino_check`; `make demo_run` 5/5; unit tests
under ASan + UBSan clean. v0.1 (single-tone OOK peaks) is **superseded**: its OOK data symbol is not kept as a mode.
This file is normative: every change to behaviour, API, layout or tests is recorded here first (Spec-Driven
Development). Where this file and the code disagree, the disagreement is a defect to be resolved here first; the
open ones are listed in §11.

**Scope of v0.2:** multi-bit peaks (MFSK on a marker-anchored grid), the 8-slot mode header, exact T, N ∈ {8, 16,
32}, short final frame, streaming slot bank, per-bin background, MFSK lock confirmation and LOST, purity-gated alias
audit, mode memory (including relock of a running stream), slot events. Kept from v0.1: markers, tune tone, sync
train, acquisition, AFC, blanker, profiles, packet layer, WAV codec, audio I/O, PC helpers, TUI, demos, examples.
Cold late join is specified (§3.13) but ships as v0.2b.
**Repository:** github.com/solariun/unlimited (public), MIT license.

### Changelog

| Date | Change |
|---|---|
| 2026-09-25 | Initial spec: design panel synthesis (3 independent proposals, 2 judges) + Gustavo's decisions (§0.3). |
| 2026-09-25 | §12 added: `unlimited_modem` (local audio, device listing, virtual serial speaking KISS, separate modem/serial speeds) and full documentation, scheduled after the core API is stable. Packet LEN to become 16-bit during stabilization. |
| 2026-09-26 | Recorded from the v0.1 implementation: packet LEN is 16-bit with `UNLIMITED_PACKET_MAX` (PC 1024, AVR 256) (§2.6, §5); the v0.1b front-end refinements (tone search, train and guard logic) live in `decoder.cpp`/`dsp.cpp` and are kept by v0.2 where §3 says so. |
| 2026-09-26 | **v0.2: multi-bit peaks.** Decisions D28–D49 (§0.4) from the v0.2 design panel (3 proposals, 2 judges, synthesis with confirmation runs); open-question defaults (§0.5). Waveform (§1), transmission (§2), decoder (§3), performance (§4), public API (§5), test plan (§8), risks (§10) rewritten for v0.2. v0.1 OOK data slots, the adaptive ρ threshold, `DecisionMode`, the gap-noise estimator and the OOK guard gates are removed. `fm_fast` becomes 6 ms k = 3; minimum T 6 ms. |
| 2026-09-26 | **v0.2 implemented.** Implementation deviations from the design recorded in §1–§3 and §6–§8 (slot blanker, tone search, sync, train fit, header ML, confirmation, TRACK, mode-memory relock). Gate decisions G1–G4, C8′, C12, L9, L10′, U28 (§0.6): the `fm` preset is FM-only, dense spacing needs T ≥ 32 ms, A5 hf_fast N32 is REPORT, C12 gates lock integrity only. |
| 2026-09-26 | **Hardening and API freeze v0.2** (24 review findings: 21 fixed, 1 fixed for T ≥ 32 ms only, 2 deferred; §0.6 H1–H7). **On-air change:** the D33 mapping is now truly Gray (§1.5, new vector 14 6 2 11 23 25 3 24): not interoperable with earlier v0.2 builds. AVR encoder reworked (ISR max 1,493 of 2,000 cycles, gated at 1,600). Measured performance replaces provisional numbers (§3.15, §4.8, §8). **Public API since v0.1** (details §5.2): protocol.hpp drops `k_bits_per_frame`, `k_slots_per_frame`, `k_sync_markers`, `k_one_energy`, `k_default_tone_hz`, `k_default_fm_tone_hz`, `k_quarter_sine_q15[65]`; adds the grid, header, mapping and limit constants, `Spacing`, `GridSide`, `HeaderFields`, `header_word/fields/symbol`, `gray_encode/decode`, `tone_rotation`, `peak_tone/symbol`, `k_quarter_sine[257]`; `k_min_slot_us` 4000 → 6000. platform.hpp: `rom_read_i16` → `rom_read_u16`, adds `release_fence()`, `acquire_fence()`. encoder.hpp: adds `k_min/max_sample_rate_hz`, `ConfigError`, `EncoderConfig::check()`, `bits_per_peak`, `data_slots`, `spacing`, `side`, `frame_bytes()`, `Encoder::queued()`, `EncoderSegment::header`, `SlotKind::peak`, `EncoderStatus::slot/symbol/tone`; removes `SlotKind::one/zero`, `EncoderStatus::bit_index`; queue capacity is exactly `k_queue_size`. decoder.hpp: removes `DecisionMode`, `DecoderConfig::profile`, `decision_mode`, `fixed_ratio`, `event_flag_weak`, `Event::level_pct[8]`, `threshold_pct[8]`; adds `EventType::slot`, `LostReason::no_header/unsupported_mode`, `event_flag_erasure/mode_memory/blind_mode`, `Event::index/tone/level_pct/confidence/bits_per_peak/data_slots/spacing/side`, `Decoder::bits_per_peak()/data_slots()`. packet.hpp: `end`/`lost` rescan before reset. **Public API since the first v0.2 headers** (the design listing of the "v0.2: multi-bit peaks" row): removed `k_sync_markers`, `DecoderConfig::profile`, `rom_read_i16`, `k_quarter_sine_q15`; added `k_min_sync_markers`, `k_max_sync_markers`, `k_min_data_slots`, `k_default_sync_markers`, `k_min_dense_slot_us`, `k_quarter_sine[257]` (`k_quarter_table_bits` 6 → 8, `k_quarter_table_size` now `uint16_t`), `rom_read_u16`, `release_fence`, `acquire_fence`, `k_min_sample_rate_hz`, `k_max_sample_rate_hz`, `ConfigError`, `EncoderConfig::check()`, `Encoder::queued()`; changed: `peak_tone`/`peak_symbol` (Gray fix), queue capacity, thread contract, PacketReader rescans. `dsp.hpp` declared internal (not frozen). |
| 2026-09-26 | Long-suite A5 genie fixed to the on-air mapping (`peak_symbol`): A5 now measures for real; 4 rows exceed the 1.5 ratio (open, §11). Stale comments fixed, unused `k_header_bg_scale` removed, library version 0.2.0. |

**Evidence.** v0.1 numbers come from the v0.1 design panel prototypes (proposals A "faithful", B
"robustness-first", C "embedded-first") and both judges' checks. v0.2 design numbers come from the v0.2 design panel
(A "extend", B "throughput", C "embedded"), the judges' re-runs and the synthesis runs (`scratchpad/mb_synthesis`,
built on the v0.1b snapshot, genie bench on the real `pc/channel` simulator). **Integrated numbers** (the real
streaming decoder on the real encoder through `pc/channel`) come from `make test_long` on the frozen tree
(2026-09-26, deterministic, one seed set per point) and are marked *measured*; design and theory values are marked as
such. Section 9 lists the sources.

---

## 0. Decisions

### 0.1 Overall choice

**v0.1** was built on proposal B:
- a single tone for everything;
- a data "1" that is a Tukey peak, and a "0" that is silence (OOK);
- START/STOP markers with the same crest on the same tone, carrying a shaped 0.25T phase reversal;
- chained frames `M b7..b0 M`;
- T = (t_STOP − t_START)/9.

**v0.2** keeps the markers, the tune tone, the sync train, acquisition, AFC, blanker, audit machinery, profiles,
packet layer, WAV codec, audio I/O and TUI, and changes only what multi-bit data slots need. Its base is the
v0.2 proposal A (faithful extension, chosen by both judges as the integration base), with:
- from C: the header code, the streaming SlotBank, exact T and mode memory;
- from B: Tukey α 0.25 data peaks, the per-slot amplitude for soft values, the slot-path blanker and the
  MFSK-aware audit;
- from the judges: 8/(7T) spacing, per-bin background subtraction, replacing v0.1b's `zeros_quiet`, narrow
  (1.2–1.6 kHz) presets instead of 2 kHz ones;
- new in the synthesis: N ∈ {8, 16, 32}, an optimised header slot order and a header decoder without training
  slots, the short final frame closed by the EOT, the waterfall band-edge search for late join.

| | v0.1 (hf, T = 32 ms) | **v0.2 (hf, T = 32 ms, k = 5)** |
|---|---|---|
| Data symbol | 1 bit: Tukey peak or silence (OOK) on f0 | **5 bits: one Tukey α 0.25 peak on one of 32 tones** at f_ref + σ(5 + n·8/7)/T |
| Net rate (8 data slots per frame) | 27.8 bit/s | **138.9 bit/s** (5×) |
| SNR for BER 1e-3 (key-down tone in 2500 Hz, AWGN, genie timing and frequency; measured on the bench) | −3.2 dB | **−5.9 dB** |
| Integrated decoder at its A1′ gate, −4.5 dB (*measured*, §4.8) | — | BER 1.07e-4, 100% of frames, 128/128 locks |
| CCIR moderate, 30 dB (genie) | 4.4e-3 (floor) | **7.9e-5** (integrated, *measured*: 3.7e-5) |
| CCIR poor, 30 dB (genie) | 1.6e-2 (floor) | **3.1e-4** (integrated at 20 dB, *measured*: 5.4e-4) |
| Occupied audio band | ≈ 100–300 Hz | 1.26 kHz centred on 1500 Hz, tuning tolerance ≈ ±500 Hz |
| Receiver configuration | T range (profile) | **T range (profile) only**: k, N, spacing, exact T and USB/LSB orientation come from an 8-slot header |

### 0.2 v0.1 decision table (D1–D27)

Rows tagged *v0.2* are amended or superseded by §0.4 and §0.6; the rest stand.

| # | Topic | Decision | Why |
|---|---|---|---|
| D1 | Marker signature | **B's shaped phase reversal.** The envelope is `w(u)·r(u)`. r is a cosine from +1 to −1 over 0.375T..0.625T. The carrier sign persists after the marker. | A's 0.1T notch splatters: −40 dB width 1762 Hz against 494 Hz for a data peak at T = 20 ms (judge 1). B's marker is 481 Hz, the same as data. C's two-tone pair costs 11 slots per byte, adds PA intermodulation, is ~3 dB less efficient and loses the same-tone reference. |
| D2 | START/STOP on the data tone | **Yes, same tone and same crest.** They remain the "1" set point, interpolated across the frame. *v0.2: the markers stay on f_ref with the same crest A as the data peaks, but the data now sit on the grid (D28) and the marker crest decides nothing (§3.10.1): the frame's START crest (no longer interpolated) gates markers and scales telemetry.* | With a separate reference tone at +250 Hz, BER on CCIR good at 30 dB is 3.0e-2, against 1.25e-4 same-tone (B E5, confirmed by judge 1). |
| D3 | Preamble | **New.** Lead-in silence, then a *tune tone* (≥ 6 slots, ≥ 250 ms), then B's 8-marker sync train. *v0.2: an 8-slot mode header follows the train (D34).* | A marker train is biphase: the sign flips every T. At T = 8 ms it puts the carrier energy at f ± 62.5 Hz, so a Goertzel search mis-centres by about 60 Hz. The coarse search and fine AFC also need 150–500 ms. A pure tone gives both almost for free. It also sets SSB PEP and ALC, and triggers VOX. |
| D4 | Decision rule | A's adaptive ρ plus B's 2.6σ floor, noise from the slot-edge gaps; fixed 0.70 selectable. *v0.2: **superseded** by D28/D39/D47 (argmax over the grid, no amplitude threshold).* | Adaptive ρ was within 0.1 dB of ideal non-coherent OOK. |
| D5 | Speed range per decoder | *(Ranges come from profiles, D20.)* Each decoder instance accepts T in [T_min, 8·T_min], a span below 9:1. *v0.2: the smallest sender T is 6 ms (D35, D48).* | A marker train of period P and a frame chain with T = P/9 are indistinguishable by flips. With a span below 9 both readings cannot be in range. Block = T_min/8, so the history is 800 cells for every range. |
| D6 | Alias protection | Late-join from the candidate list, picking the smallest T. Then a 4-frame guard with a **flip audit** of half-slot positions, using a balanced flip statistic and 4-frame evidence. *v0.2: 2N+1 positions with a purity gate (D42); late join needs mode memory (D44).* | Fixes B's T/2, 2T and 9T locks and C's 2T lock (§9.2). |
| D7 | Byte release | A frame is released when its STOP is detected; flywheel frames are held until the next detected STOP; the reference is always measured at the predicted marker. *v0.2: a frame is also released when ¾ of its slots are confident (D41); at most 2 held frames; absent frames are dropped (§3.9).* | Missing bytes, never garbage after a lost signal; bounded latency. |
| D8 | Lock loss | LOST when 3 of the last 4 STOPs lack presence (q < 1); LOST(alias) when the audit evidence reaches 12. *v0.2: LOST(signal_gone) needs STOP absent **and** data absent (D41).* | B's "3 consecutive" rule never fires on a sub-harmonic lock (judge 1). |
| D9 | History | int32 **wrapping** prefix sums (stored `uint32_t`), 800 cells, owned by `Decoder`. Positions are float offsets from a rebased integer origin. | Float prefix sums drift after hours (judge 2: 47σ after 24 h). |
| D10 | Front end | Integer NCO with a quarter-sine table, `(x·cos) >> 10`, and a **CIC-2** block integrator. Float math only at block rate. *v0.2: the table is `k_quarter_sine[257]` (H7).* | CIC-2 fixes the 2f image for low tones. The integer path is bit-exact across platforms, so chunk-size invariance is exact. |
| D11 | Tone search | Goertzel bank, 49 bins, 50 Hz grid, integer. Fast power lock, a slow keyed path, a steady-carrier mask from excess variance, and bin bans with exponential back-off. *v0.2: half-bin powers, a recent floor, weak-tone and quiet-bin locks, a train-onset watch (§3.6).* | Carriers are ignored. The tune tone locks in about 0.2 s. |
| D12 | Impulse blanking | B's two stages: spike, then out-of-bin residual, with the run-limit deadlock guard. A's per-sample clipper is rejected for the marker path. *v0.2 adds a separate per-sample blanker on the slot path (D39, §3.2).* | B measured 170× (QRN at +6 dB), with no loss on clean AWGN or FM. |
| D13 | Slowest T | T_max = 128 ms. | The phase signature needs coherence (97% missed markers at T = 512 ms with 1 Hz offset). Beyond this, add FEC instead of slowing down. |
| D14 | Packet | `[0x2D 0xD4][LEN][payload][CRC-16/CCITT-FALSE]`; after a CRC failure, hunting restarts at the next byte. LEN is 16-bit (§2.6). | A sync word allows mid-stream resync. |
| D15 | Encoder API | Streaming queue, lock-free single producer / single consumer. Queue underrun at a frame boundary is a normal end of transmission (EOT). *v0.2: a partial frame at the end becomes a short final frame (D38); capacity exactly `k_queue_size`, release/acquire fences (H4, §2.5).* | A frame chain cannot pause. |
| D16 | Callbacks | Function pointer plus `void*` context. `enum class` throughout. | A virtual listener fails `-Wextra -Werror` (judge 2). |
| D17 | Dropped options | A's plain marker mode, C's 4-bit frames. | Plain mode is measurably inferior. |
| D18 | Decoder input rate | Fixed 8000 Hz int16 in the core. The PC side resamples. | Simpler and cheaper core. MCU ADCs sample at 8 kHz directly. |
| D19 | SNR convention | Key-down tone power (A²/2) over noise in 2500 Hz. CCIR 520: good 0.5 ms/0.1 Hz, moderate 1 ms/0.5 Hz, poor 2 ms/1 Hz. *v0.2: average power is −0.9 dB of PEP (OOK was −4.34 dB), §1.3.* | Corrects B's "3 dB" average-power figure and B's doubled Doppler values. |
| D20 | Universal decoder | **One `Decoder` class for every radio**, parametrized by `Profile` {`ssb`, `am`, `fm`} plus overridable fields: accepted slot range (`min_slot_ms`, span 8:1), tone search range, blanker. *v0.2: a profile only fills `DecoderConfig`'s fields; `DecoderConfig` has no profile field (H6).* | Gustavo: "universal decoder … hf + SSB (LSB/USB) … and a fm/am option". |
| D21 | Block size | `B = min_slot_ms` samples at 8 kHz (T_min/8), any integer 4..32. The CIC-2 dump divides by B. | Lets `min_slot_ms` be any value. |
| D22 | Audio boundary | The core never touches an audio device. `audio_io.hpp` defines `SampleSource`, `SampleSink`, `AudioOutput`, `AudioInput`; drivers own the loop; `wait()` blocks until drained. `EncoderSource` / `DecoderSink` adapt Encoder/Decoder. | Detachable audio connector for PC and MCU drivers. |
| D23 | Portable WAV codec | `wav_codec.hpp/.cpp` in the core: RIFF/WAVE build and parse with explicit little-endian handling, no OS/STL/heap, through `ByteSink` / `ByteSource`; exact, patched or streaming sizes. `pc/wav.*` is a thin adapter. | Gustavo: WAV generation usable on MCUs and PC alike. |
| D24 | Demo TUI | `--tui` on both demos: scope, peaks/frame view, spectrum strip, status. ANSI/VT100 only. `--realtime` paces file audio. *v0.2: the peaks view shows slot events (tone, level with the 70% line, confidence) and the spectrum strip draws the grid (§6.5).* | Gustavo: see the wave during encoding and decoding. |
| D25 | Telemetry | `Encoder::status()` exposes the segment, slot kind and what is being sent; the decoder emits events. *v0.2: `EncoderStatus` gives slot, symbol and tone; per-frame `[8]` arrays in `Event` are replaced by `slot` events (D46).* | Keeps the core free of UI code; one observation path. |
| D26 | Channel simulator | The implemented `unlimited::sim` API (`pc/channel.hpp`) plus steady carrier and keyed CW interferers, QSB, TX clock error, CCIR/ITU fading presets (implemented). | Built and physically verified. |
| D27 | License | MIT, Copyright (c) 2026 Gustavo Campos. | Gustavo's choice. |

### 0.3 Gustavo's decisions (2026-09-25, v0.1)

| Question | Decision |
|---|---|
| Default decision threshold | Adaptive (0.50..0.75 of the interpolated START/STOP reference), fixed 70% selectable. *Superseded in v0.2: argmax over the grid needs no threshold; the 70% line survives as a TUI reference on `level_pct`.* |
| START/STOP marker | **Same crest as data, shaped mid-slot 180° phase reversal**, plus a **tune tone** at the start of each transmission. *Kept; in v0.2 the markers sit on f_ref and anchor the grid.* |
| Speed range | **Universal decoder parametrized by profile** (D20): `ssb` (HF, USB or LSB), `am`, `fm`; all fields overridable. |
| Repository / license | `unlimited`, public at github.com/solariun/unlimited, **MIT**. |
| Decoder input rate / AVR | Decoder input fixed at 8 kHz int16 (PC resamples); AVR runs the encoder only. |

**Standing requirements (v0.2):** HF SSB first; the sender dictates everything (T, k, N, spacing, side) with
**no receiver configuration** beyond the accepted T range; the decoder never expects a specific tone; code simple and
readable for Arduino/STM32/ESP32; C++11.

### 0.4 v0.2 decision table (D28–D49)

"Measured" means the synthesis runs unless another source is named. The genie bench uses the real `pc/channel`
simulator with ≥ 1.5e5 bits per AWGN point and ≥ 2e5 per fading point. Amendments from the implementation and the
hardening are in italics and in §0.6.

| # | Topic | Decision | Why and evidence |
|---|---|---|---|
| D28 | Data symbol | Each data slot carries **one peak on one of M = 2^k tones**, k = 1..8. **No silence symbol.** Gray labels. | AWGN thresholds within 0.1–0.2 dB of noncoherent MFSK theory (§4.1). Argmax needs no amplitude threshold, so the OOK fading floors disappear (§4.2). Two-tone FSK already beats OOK by 3.1 dB at equal rate. |
| D29 | Data envelope | Data and header peaks use **Tukey α 0.25** (0.125T ramps, energy 0.84375). Markers and tune keep α 0.5 plus the reversal. | +0.8–1.0 dB against α 0.5 at every T, never worse in fading (A, B, judge 1). |
| D30 | Tone spacing | Two spacings. **Standard c = 8/7:** Δf = 8/(7T), the spectral zero of Tukey α 0.25. **Dense c = 1:** Δf = 1/T, opt-in. Signalled in the header. *Dense needs T ≥ 32 ms (G2).* | AWGN unchanged (−5.90 dB at 8/7 against −5.9 at 1/T); fading floors roughly halve (§4.2); cost +14% bandwidth. |
| D31 | Guard | First data tone at **5/T** from f_ref. | Blind chain at 10–50 dB: 0 alias LOSTs, 100% of frames at T = 8..128 (§4.4). In CCIR poor, 5/T has the fewest alias LOSTs of all guards tried (9 against 22 per 50 tx for 3/T). |
| D32 | Side and placement | `GridSide` is an explicit sender field. **HF presets put f_ref at the top with the grid below**, centring the band on 1500 Hz with f_ref ≥ 1000 Hz (so the `fm` profile decodes them too). FM presets use f_ref 2650 Hz with the grid below. *The FM presets are FM-only (G1).* | Tuning tolerance ≈ ±380..600 Hz (§1.4). A 2 kHz grid breaks at +300 / −150 Hz (judge 1), so no 2 kHz preset. |
| D33 | Mapping | Tone n_i = (gray⁻¹(s_i) + (i−1)·r) mod M, r = (M/8) \| 1: the label of the de-rotated tone n is gray(n). *Corrected by the hardening (H1): the design's gray(s_i) + (i−1)·r put neighbouring tones up to 4 label bits apart.* | Adjacent-tone errors cost exactly 1 bit (cyclic Gray, U23). The rotation spreads repeated bytes over the band (fixed notch or interferer) and gives blind side/M estimation a full band edge. |
| D34 | Mode header | One 8-slot frame after the sync train. **RS(8,3) + x³ coset over GF(8)**, 9 bits: k−1, T mod 8 ms, N code, spacing. **Optimised** slot order (misaligned-window distance 3/7 instead of 1/7). **Self-background** ML over 512 words × 2 sides. Accept when margin ≥ 12 and agreement ≥ 6. *Implemented with a per-tone floor and spread-based noise, carrier-aware scaling and a 1-slot look-ahead (§3.8).* | Monte Carlo (§4.5): noise 2e-5, steady carrier 0, chirp ≤ 4e-5; detection 98.0% at per-slot Es/N0 9 dB and 100% at 12 dB (data threshold ≈ 12–13 dB). |
| D35 | Exact T | `slot_us` is a **whole number of ms**, 6..128. The header carries T mod 8; the receiver snaps its measured T. | 0.3–0.5% T error breaks 128–256-tone grids. Measured T is always well within ±4 ms. |
| D36 | Frame length | **N ∈ {8, 16, 32}** data peaks per START/STOP frame, chosen by the sender and sent in the header. Default 8. **(N+1)·T ≤ 1152 ms**, v0.1's longest validated frame. | N = 16/32 add only +4–9% effective rate (§4.7) but slow AFC capture and late join; cold late join fails when M ≤ N (§4.6). Gustavo's requirement. |
| D37 | Bytes per frame | N·k/8 bytes per frame, always an integer. One MSB-first bit string; slot i carries bits [(i−1)k, ik). | No packing problem for any k. |
| D38 | Last frame | **Short final frame:** d = ⌈8q/k⌉ data slots, then STOP and the 2 EOT markers. The decoder finds the flip triple inside the frame and releases exactly ⌊d·k/8⌋ = q bytes. *A short final frame always ends the transmission.* | No pad slots, no amplitude decision, no extra bytes. Data slots never flip, so a triple inside a frame is unambiguous. |
| D39 | Slot detector | Streaming **SlotBank**: int32/Q14 (or float) Goertzel on raw 8 kHz samples, matched α 0.25 window, predicted slot windows, no sample buffer. **Per-bin 25% background.** **Max-log LLR** with the slot's own amplitude. **Slot-path impulse blanker.** *The blanker zeroes only peaky samples (±0.5 ms around them) and their ringing (§3.2).* | C's bank gives BER identical to a double-precision reference. Background subtraction takes an in-grid carrier from BER 0.41 to 1.2e-2 at ≤ 0.05 dB cost. Self-amplitude LLRs give 5× lower coded BER in CCIR poor. Without the blanker, T = 8 floors in QRN. |
| D40 | Timing | Exact T × (1+ε) drift, plus a **frame-phase loop** with gain 0.5 on the measured START. TRACK is split into an early step (STOP) and a late step (EOT and release). | Loop gain 0.5 beats raw START/STOP and pure flywheel. Blind timing sd 2–3.6% of T near threshold (§4.4). |
| D41 | Lock confirmation and LOST | **Confirm on header plus data presence.** v0.1b's OOK gates are **deleted**: `zeros_quiet`, `guard_threshold_too_high`, `guard_levels_too_high`, `guard_clean`, the ρ solver and the gap noise. LOST(signal_gone) needs **STOP absent and data absent**. *Confirmation also needs STOPs found, varying tones and quiet marker edges (§3.9).* | With the OOK guard, 20–25% of transmissions are lost at genie + 1 dB (§4.4). `zeros_quiet` gives 0/20 locks at 50 dB on MFSK (judge 1). |
| D42 | Alias audit | 2N+1 positions, evaluated progressively, stored as int8. **Purity gate:** a flip counts only if A_mk ≥ 0.5 × the neighbouring winner amplitude. | Without it the v0.1 audit and LOST lose 4–25% of frames in CCIR moderate/poor (§4.4, judge 1). |
| D43 | Noise | N_bin = median of the per-bin backgrounds; it feeds the LLRs. *The marker statistics and the SNR report take their noise from the STOP slot's grid window instead (§3.10).* | Noise taken below 300 Hz caused false alias LOSTs; in-band bins fix it. |
| D44 | Late join in core | **Mode memory**: remember f_ref, T, N, k, spacing and side for 60 s. A header-less lock that matches inherits the mode. *Extended to relock a running stream after a fade (§3.12).* | Covers the common HF case of fade → LOST → relock. |
| D45 | Cold late join (v0.2b) | Waterfall **band-edge candidates**, then T/N from the marker period plus a slot-boundary fold, then side/M from the band. Supported when M ≥ 2N. | The v0.1 power lock picks a data tone in 85–100% of late joins; the band-edge rule finds the marker bin in 85–100% when M ≥ 2N (§4.6). |
| D46 | Events | **Byte events are kept** (PacketReader unchanged). **New `slot` event** for telemetry. Per-frame `[8]` arrays removed. Soft values stored internally as 4-bit, emitted as int8. | The API does not depend on N or k. |
| D47 | Configuration | `DecisionMode` and `fixed_ratio` removed. Compile-time caps `UNLIMITED_MAX_BITS_PER_PEAK` and `UNLIMITED_MAX_FRAME_BYTES`. **No new `DecoderConfig` field.** *`DecoderConfig::profile` removed too (H6).* | Argmax needs no threshold; the receiver stays configuration-free. |
| D48 | Presets | Same six names, new modes (§1.4). `fm_fast` moves from 4 ms to **6 ms** because the 8-tone header needs 13/T ≤ 2.4 kHz. | — |
| D49 | FEC | Not in v0.2. N code 3 is reserved as an extension escape (e.g. a later "FEC header follows"). | Coded thresholds sit ≈ 3.5 dB below where the markers lock. |

### 0.5 v0.2 open questions: defaults in force

The design left seven questions open. These are Claude's defaults, recorded 2026-09-26; **Gustavo may override
any of them** (the change then goes through this file first).

| Question | Default in force |
|---|---|
| Average power: a peak in every slot is ≈ 0.81 of PEP on average and 100% duty (like FT8), against 0.37 for OOK | Output level stays **−3 dBFS** (`amplitude` 23197) at full PEP. The operating guide (§12.2) recommends reducing drive for long transmissions on duty-limited rigs. |
| Cold late join (v0.2b) | **Deferred to v0.2b**: specified (§3.13, tests V1–V3), not implemented in v0.2. **Mode memory ships** in v0.2 (§3.12). |
| Dense spacing (1/T) | **Kept**: it enables 8 bits per peak (T128 k8 in 2.03 kHz). Opt-in by config only, no preset; T ≥ 32 ms (G2). |
| N rule | **N ∈ {8, 16, 32}, default 8**, with (N+1)·T ≤ 1152 ms (N = 32 up to T = 34 ms, N = 16 up to 67 ms). |
| Minimum T | **6 ms**; `fm_fast` becomes 6 ms k = 3 (444 bit/s); v0.1's 4 ms mode is retired. |
| HF f_ref placement | **f_ref at the top of the band with the grid below** (tune tone ≈ 2.1 kHz), band centred on 1500 Hz. |
| Bytes decoded via mode memory (and v0.2b blind mode) | **Released, flagged** `event_flag_mode_memory` (or `event_flag_blind_mode`); the packet CRC protects the application. |

### 0.6 Implementation and hardening decisions (2026-09-26)

Gate decisions (G, C, L, U) were taken on the integrated long suite; hardening decisions (H) on the 24 findings of
the review. No gate was relaxed silently: every change below is a decision with its evidence.

| # | Topic | Decision | Evidence |
|---|---|---|---|
| G1 | `fm` preset over SSB/AM | The FM presets (f_ref 2650 Hz) are **FM-only**. The SSB/AM gate rows of A1′/A3′/A4/A5 use **T8 k3 centred** (f_ref 2313 Hz, `am` profile, +1.5 dB); "fm preset (am profile, SSB)" rows are REPORT. An `am` receiver decodes the `fm` preset only on a path passing ≈ 2.9 kHz. | f_ref 2650 sits on the 2700 Hz −6 dB edge: half the T8 marker spectrum is cut, 0.5% locked at +1.5 dB on SSB (2400 Hz: 99.0%, 2313 Hz: 98.5%). On the FM channel the preset locks 100% (*measured*, A3′). Moving f_ref to 2400 would cost ≈ 0.4 dB on FM. |
| G2 | Dense spacing | **Dense needs T ≥ 32 ms** (`k_min_dense_slot_us`, `ConfigError::dense_slot`). The dense T16 k5 mode (278 bit/s) is removed. | Its 2250 Hz span leaves 150 Hz of the 300–2700 Hz passband while the T16 marker needs ≈ 125 Hz above f_ref: 0% locked at a +50 Hz offset, 45% frames in A1′. |
| G3 | A3′ acquisition gates | Kept at **≥ 99% at the gate, ≥ 90% at gate − 1 dB**; met by the decoder work of §3.6–§3.8 (no fallback to 98% / 85%). | *Measured*: every gated A3′ row passes (§4.8). |
| G4 | A5 hf_fast N32 | **REPORT** (design limit: timing from START/STOP only across 33-slot frames at T = 16 ms). | Ratio 2.10 before the STOP on-time fix, 1.02 at the last valid genie run. |
| C8′ | Header detection under a carrier | Gate on a +10 dB carrier at a **random in-band frequency** of the header band; the on-tone case is reported. | *Measured* 99.2% / 100% (design Monte Carlo on-tone: 65%). |
| C12 | Flutter (0.5 ms / 10 Hz) | **Report-only** for BER and wrong bytes (channel errors under flutter); **lock integrity gated**: unmapped/extra bytes ≤ 1 per 1000 released. | *Measured* 0.37 per 1000 unmapped; wrong bytes 13.0 per 1000 (not gated). |
| L9 | LOST latency after a PTT cut | **≤ 5 frames** (was 4). | Noise alone gives STOP q ≥ 1 at 7% of STOPs, so one frame more than the 3 absent ones is common; *measured* 3.3–4.3. |
| L10′ | Late-join latency | **8/8 late joins at 10 and 30 dB for every HF preset, 0 wrong bytes; hf_fast TRACK within 7 frames of the cut in ≥ 7 of 8.** Slower presets' latency is reported. | TRACK entry = search (1–2.5 frames) + 3 frame periods for 4 markers; *measured* 4.4–7.8 frames (hf_fast), up to 14.5 (hf_robust), 17.5 (hf_weak). |
| U28 | ln I0 table | Absolute error **≤ 0.02 nat**, relative ≤ 1% for x ≥ 3. | The 33-point linear table has 2.7% relative error near x = 1, 0.015 nat absolute, against a 1-nat LLR step. |
| H1 | D33 mapping | Mapping corrected to gray⁻¹ (§1.5); U23 is a hard check again (1 bit max for every k). **On-air change** against earlier v0.2 builds. | Before: 1.25–1.33 label bits between neighbours on average, up to 4 at k = 7, 8. |
| H2 | Frequency step in TRACK | A missed STOP's rotation corrects the NCO (§3.9 rotation AFC) **only at T ≥ 32 ms**; hf_fast keeps its limit of about 0.4 spacing (≈ 28 Hz). | At T = 16 ms a two-path fade mimics the rotation: enabling it there raised wrong bytes 15 → 59 and 23 → 123 in fading. |
| H3 | Keyed CW on a grid tone (+3..+6 dB) | **Deferred** to FEC (§13). | 57–66% of released bytes wrong, 55–76% of those without an erasure flag; a per-bin tracker costs ≥ 64–128 B and the (7, 16) float build has 40 B headroom. |
| H4 | Encoder queue and threads | Capacity **exactly `k_queue_size`** (free-running indices); single producer / single consumer with **release/acquire fences** (`platform.hpp`); `abort()` and `status()` are consumer-side. | The queue held `k_queue_size − 1` bytes (a 16-byte queue with 8-byte frames split a stream into a transmission per 15 bytes); a compiler barrier alone is unsafe across cores (arm64, dual-core ESP32). |
| H5 | Configuration errors | `enum class ConfigError` and `EncoderConfig::check()` name the first rule broken; applications map the enum and never copy the rules. Named minimums `k_min_sync_markers`, `k_max_sync_markers`, `k_min_data_slots` replace defaults used as limits. | The demo's copy of the rules had drifted (G2 missing). |
| H6 | API freeze | The v0.2 public headers are **frozen** (§5). `dsp.hpp` is internal (not frozen). **Deferred API** (not needed by §12): `DecoderConfig::check()`, an abort request from the producer thread, an `Encoder` reconfigure call — the modem builds a new `Encoder` while idle and mirrors DCD (`state() != search`) into an atomic. | — |
| H7 | AVR encoder arithmetic | 257-entry `uint16_t` quarter-sine table with one 16×16 multiply, ramps from one lookup, the 64-bit slot phase in two 32-bit words, header tones precomputed by `start()`, no division or GF(8) in the ISR (§1.7). Gate: ISR ≤ 1,600 cycles on an ATmega328P model. | Before: hf mean 1,648–1,965 / max 3,638 cycles, 709 lost ticks; after (*measured*): mean 602–721, max 1,422–1,493 of 2,000, 0 lost ticks. |

---

## 1. Waveform (normative)

### 1.1 Envelopes

Notation: `u ∈ [0,1)` is the position inside a slot of length T.

```
markers and tune (Tukey alpha 0.5, unchanged from v0.1):
w(u) = sin²(2πu)          u < 0.25          ramps of 0.25T, flat top of 0.5T
     = 1                  0.25 ≤ u ≤ 0.75
     = sin²(2π(1−u))      u > 0.75

r(u) = +1                 u ≤ 0.375         shaped 180° reversal
     = cos(4π(u−0.375))   0.375 < u < 0.625
     = −1                 u ≥ 0.625

data and header peaks (Tukey alpha 0.25):
p(u) = sin²(4πu)          u < 1/8           ramps of 0.125T, flat top of 0.75T
     = 1                  1/8 ≤ u ≤ 7/8
     = sin²(4π(1−u))      u > 7/8

marker/tune:  y[n] = A · s · e(u) · sin(φ_ref[n])       φ_ref: the f_ref NCO
peak:         y[n] = A · p(u) · sin(φ_data[n])          φ_data: the data NCO (§1.6)
```
- `s ∈ {+1, −1}` is the persistent carrier sign of the f_ref NCO.
- Energies in T·A²/2 units: peak 0.84375 (`k_peak_energy`), marker 0.5625 (`k_marker_energy`), tune ≈ 1.

### 1.2 Slot kinds

| Kind | e(u) | Tone | Sign rule | Energy (T·A²/2 units) |
|---|---|---|---|---|
| silent (lead-in, tail) | 0 | – | – | 0 |
| tune (segment of N_tune ≥ 6 slots) | ramp-up w(u) in its first slot for u < 0.25; 1 in between; ramp-down in its last slot for u > 0.75 | f_ref | s | ≈ 1 per slot |
| peak (header or data) | p(u) | a grid tone (§1.3) | – | 0.84375 |
| marker (sync, START/STOP, EOT) | w(u)·r(u) | f_ref | s during the slot; **s ← −s after it** | 0.5625 (−1.76 dB against a peak) |

Properties:
- Peaks never reverse phase. Only markers do; they sit on f_ref, never on a grid tone.
- A marker's crest equals a peak's crest: A is the key-down set point.
- The envelope is 0 at every slot edge, so there are no key clicks.
- Every slot of the header and of the frames carries energy: there are no silent data slots, so no amplitude
  decision exists anywhere in the data path.

### 1.3 Tone grid, band and levels

```
T   = slot_us (a whole number of ms)
c   = 8/7 (Spacing::standard) | 1 (Spacing::dense, T ≥ 32 ms)
σ   = +1 (GridSide::above) | −1 (GridSide::below)        as sent; the receiver sees σ_rx = σ·(−1 if inverted)
G   = k_grid_guard = 5
data tone n (0..M−1):   f_n = f_ref + σ·(G + n·c)/T
header tone h (0..7):   f_h = f_ref + σ·(G + h·8/7)/T     always the standard grid, even in dense modes
span W (f_ref to the farthest tone) = max(G + (M−1)·c, G + 8)/T
```

Spans (dense spacing exists only from T = 32 ms, G2):

| Mode | Standard (8/7) | Dense (1) |
|---|---|---|
| T6 k3 | 2167 Hz | — |
| T8 k3 | 1625 Hz | — |
| T16 k4 | 1384 Hz | — |
| T32 k5 | 1263 Hz | — |
| T32 k6 | — | 2125 Hz |
| T64 k6 | 1203 Hz | — |
| T64 k7 | — | 2063 Hz |
| T128 k7 | 1173 Hz | — |
| T128 k8 | — | 2031 Hz |

- **Band rule (sender, exact integer check in `EncoderConfig::check()`, `ConfigError::band`):** f_ref and every
  data and header tone lie in [300, 2700] Hz (`k_min_tone_hz`, `k_max_tone_hz`).
- **Placement:** HF presets use f_ref = ⌈1500 + W/2⌉ Hz with the grid below, so the occupied band is centred on
  `k_band_centre_hz` = 1500 Hz and f_ref ≥ 1000 Hz. FM presets use f_ref = `k_fm_tone_hz` = 2650 Hz with the grid
  below and are FM-only (G1).
- **Orientation:** the tone index is the distance from f_ref, so USB/LSB inversion (or the sender's choice of
  side) only flips σ_rx. The header ML decides σ_rx; the receiver never learns "USB/LSB".
- **Tuning tolerance is finite:** the whole band must stay inside the receiver passband. ≈ ±380..600 Hz for the
  HF presets (C14 gates them at those offsets, §8.4), ≤ ±170 Hz for the dense modes (design estimate; v0.1
  accepted any offset).
- **Decoder:** it needs no knowledge of any tone. f_ref comes from the tune tone and markers (tone search), the
  grid from the header. A decoder with `min_slot_ms` < 8 (the `fm` profile) requires f_ref ≥ 1000 Hz (CIC-2 image
  rejection with blocks shorter than 8 samples); grid tones are measured by the slot bank on raw samples and have
  no such limit. The receiver chooses only its accepted T range: a sender using T < 16 ms over SSB needs the `am`
  profile (§1.4).
- **Amplitude:** A is the PEP in output units. Default −3 dBFS (23197). ALC must stay inactive; set the audio
  level so the **tune tone** gives the wanted PEP.
- **Average power:** 0.8125 of key-down (−0.90 dB) at N = 8, 100% duty. §0.5: keep −3 dBFS; reduce drive for long
  transmissions on duty-limited rigs. Per unit of average power, T32 k5 needs 0.7 dB more than v0.1 OOK but carries
  5× the bits: Eb/N0 is 6.3 dB better.

### 1.4 Speed, modes, presets and profiles

The sender chooses, and the header announces:
- `slot_us`: a whole number of ms, 6000..128000 (`k_min_slot_us`, `k_max_slot_us`, `k_slot_quantum_us`);
- `bits_per_peak` k = 1..8 (M = 2^k tones);
- `data_slots` N ∈ {8, 16, 32} (`k_min_data_slots` << N code), with (N+1)·T ≤ 1152 ms (`k_max_frame_us`);
- `spacing` (standard 8/7, or dense 1 when T ≥ 32 ms, `k_min_dense_slot_us`) and `side` (above or below f_ref).

Net rate = N·k/((N+1)·T) bit/s; bytes per frame B = N·k/8.

**Presets** (every preset: N = 8, standard spacing, grid below). Genie thresholds are *measured* on the design
bench (§4.1–§4.3); the integrated gates are the §8.3 A1′ points, all *measured* passing (§4.8).

| Preset | T | k | f_ref | Tune / sync / lead-in | Span | Raw / net bit/s | Tuning tolerance | AWGN genie at 1e-3 | Integrated A1′ gate | CCIR moderate, 20 dB (genie) | CCIR poor, 20 dB (genie) | Decoded by default profiles |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `fm_fast` | 6 ms | 3 | 2650 | 250 ms / 8 / 300 ms | 2167 Hz | 500 / 444 | FM n/a | FM CNR 4.9 dB | FM CNR 7 dB | — | — | fm (FM-only, G1) |
| `fm` | 8 ms | 3 | 2650 | 250 / 8 / 300 | 1625 | 375 / 333 | n/a | CNR 2.3 | FM CNR 4.5 | 3.7e-3 | 1.6e-2 | fm (FM-only, G1) |
| `hf_fast` | 16 ms | 4 | 2192 | 250 / 8 / 0 | 1384 | 250 / 222 | ±380 Hz | −3.3 dB | −1.5 dB | 8.1e-4 | 2.4e-3 | ssb, am, fm |
| **`hf`** (default) | 32 ms | 5 | 2132 | 250 / 8 / 0 | 1263 | 156 / **139** | ±500 | **−5.9** | **−4.5** | 2.4e-4 | 4.5e-4 | ssb, am, fm |
| `hf_robust` | 64 ms | 6 | 2102 | 500 / 16 / 0 | 1203 | 94 / 83.3 | ±560 | −8.4 | −7.0 | 1.6e-4 | 2.4e-4 | ssb, am |
| `hf_weak` | 128 ms | 7 | 2087 | 1500 / 16 / 0 | 1173 | 55 / 48.6 | ±590 | −11.4 | −9.5 | 2.2e-4 | 6.9e-4 | ssb (stable paths) |

**Opt-in by config, no preset.** Dense "+1 bit" modes (T ≥ 32 ms), 2.0–2.1 kHz spans: T32 k6 167 bit/s at
−5.65 dB (genie); T64 k7 97 bit/s at −8.35 dB; T128 k8 55.6 bit/s at −11.0 dB (8 bits per peak). Their derived
integrated gates (−4.25, −6.95, −9.10 dB) pass (*measured*, §4.8).

**Choosing N** (§4.7): default N = 8 everywhere; N = 16 for bulk data at T ≤ 64 ms on stable paths (+4–6%
throughput); N = 32 only at T ≤ 34 ms, on stable paths, where cold late join is not needed. Larger N slows AFC
capture (±1/(2(N+1)T): 1.7 Hz at hf/N8, 0.47 Hz at N32), LOST latency (3 frames), late join (≥ 4 markers) and
thins the marker references in fading.

**Decoder profiles (D20).** A profile only fills `DecoderConfig`'s fields (`for_profile()`); the receiver chooses
nothing else.

| Profile | `min_slot_ms` → accepted T | f_ref search | Blanker | Radio path |
|---|---|---|---|---|
| **`ssb`** (default) | 16 → 16–128 ms | 300–2700 Hz | on | HF SSB, USB or LSB |
| `am` | 8 → 8–64 ms | 300–2700 Hz | on | AM (HF or VHF airband); also SSB for senders with T < 16 ms |
| `fm` | 4 → 4–32 ms (senders use ≥ 6) | 1000–2700 Hz | on (FM clicks) | VHF/UHF NBFM |

Every profile field can be overridden (`min_slot_ms` 4..32, f_ref range, blanker). A decoder never emits bytes
for a T outside its range, or for a mode above its compile-time caps (§3.15): such transmissions produce no
`locked` event (the latter gives `lost(unsupported_mode)`).

Channel limits:
- **HF:** keep T ≥ 10× the delay spread; T = 8 ms is unusable on CCIR poor (25–32% of frames).
- **Coherence at the receiver:** residual |Δf|·T < 0.1 after AFC (this is also exactly the MFSK requirement:
  0.1/T costs 0.14–0.43 dB, 0.2/T costs 0.5–1.4 dB), and Doppler spread·T < 0.25. At T = 128 ms that means
  Doppler ≤ 2 Hz; ≤ 0.5 Hz is recommended.

### 1.5 Symbols, mapping, packing

```
frame payload B = N·k/8 bytes b_0..b_{B−1} → one bit string, MSB of b_0 first
data slot i = 1..N: s_i = bits [(i−1)k, ik)  (MSB first)
tone index  n_i = (gray⁻¹(s_i) + (i−1)·r) mod M,   gray(v) = v ^ (v >> 1),  r = (M/8) | 1
            r: k = 1..3 → 1, k = 4 → 3, 5 → 5, 6 → 9, 7 → 17, 8 → 33
receiver:   s_i = gray((n̂_i − (i−1)·r) mod M)
```
The label of the de-rotated tone n is gray(n), so cyclically neighbouring tones differ in exactly one bit for every
k: an adjacent-tone error (frequency offset, Doppler, timing) costs one bit.

Bit-exact vector (k = 5, N = 8, bytes 48 69 21 00 FF): symbols 9, 1, 20, 18, 2, 0, 7, 31; **tones 14, 6, 2, 11,
23, 25, 3, 24** (earlier v0.2 builds, with the uncorrected mapping, sent 13, 6, 8, 10, 23, 25, 2, 19).

In code: `peak_tone(symbol, i − 1, k)` and `peak_symbol(tone, i − 1, k)` (§5). The decoder derives every label
(decision and LLRs) through `peak_symbol`, never a hand-rolled Gray step.

### 1.6 Phase handling

- **Two NCOs.** The f_ref NCO advances on every sample from `start()` to the tail, whatever the segment. The
  markers' persistent sign and the START→STOP phase AFC therefore stay valid.
- **Data NCO.** It advances continuously; only its step changes at slot edges, where the envelope is 0. Both NCO
  phases are set to 0 by `start()` and never reset during a transmission.
- **No phase continuity is needed** between data slots (noncoherent detection).

### 1.7 Encoder arithmetic (AVR-friendly, integer only)

```
tone_step    = round(f_ref·2^32/rate)                                   f_ref NCO
unit_step    = round(2^32·10^6/(rate·slot_us))                          1/T
std_step     = round(8·2^32·10^6/(7·rate·slot_us))                      8/(7T)
data_step_c  = spacing == standard ? std_step : unit_step
per data slot:   step = tone_step ± (5·unit_step + n·data_step_c)      (mod 2^32; + above, − below)
per header slot: step = tone_step ± (5·unit_step + h·std_step)
slot timing: a 64-bit slot phase held as two 32-bit words (32-bit adds only; 2^64 = one slot, a slot ends on
             overflow); step = ceil(2^64·10^6/(rate·slot_us)), so slot j starts exactly at sample ceil(j·L),
             L = rate·slot_us/10^6; the lead-in is a partial first slot, then whole slots (on the slot grid)
sine: k_quarter_sine[257] = round(65534·sin(π/2·i/256)) (uint16, PROGMEM on AVR); sine_q15 interpolates linearly
      with one 16×16 multiply: ≤ 1 LSB of 32767·sin (U1)
u = the high word of the slot phase
ramp:        sin²(2πu) = (1 − cos(4πu))/2 = (32767 − cosine_q15(u << 1) + 1) >> 1     one lookup, no multiply
marker w·r:  ramp for u < 0.25 and u > 0.75, 1 between; cosine_q15((u − 0x60000000) << 1) over 0.375..0.625;
             negated after the reversal
peak p:      ramp(u << 1) for u < 1/8 and u > 7/8, else 1
output:      mul_q15(mul_q15(A, e), sine_q15(phase)), mul_q15(a, b) = (a·b + 2^14) >> 15 (taken as the high
             word of twice the sum: byte moves on AVR); phase = f_ref phase (marker, tune) or data phase (peak)
sign flip after a marker: f_ref phase += 2^31 at the slot edge (exact negation of sine_q15), no NCO jump
header tones: computed by start() (8 × 3 bits), consumed slot by slot: no GF(8) arithmetic in the ISR
short final frame: d = ⌈8q/k⌉ by repeated subtraction: no division in the ISR
```
- Per sample: the f_ref, data and slot-phase adds (32-bit), at most two `sine_q15` evaluations (the envelope's ramp
  or reversal, and the output) and two `mul_q15`: at most four 16×16 multiplies, no division. Per slot: one symbol
  extraction and one step computation.
- Worst tone error from step rounding: 1.2e-4 Hz (design); U25 *measures* every peak within 1.4e-3 Hz of the
  exact grid (gate 0.01 Hz).
- No float anywhere in the encoder, including `EncoderConfig::check()` (a float check pulled soft-float into AVR
  sketches: 6,820 B against 4,332 B). `arduino_check` scans the linked `tx_uno` for soft-float routines.
- **AVR ISR (*measured*, `check_embedded`, ATmega328P cycle model, 16 MHz, 8 kHz → 2,000 cycles per tick):** every
  preset and T128 k8 dense: mean 602–721 cycles, max 1,422–1,493, load 30–36%, 0 lost ticks, output identical to
  the host encoder; the ISR is busy at most 0.09 ms, so serial input is not lost. Gate (B5): max ≤ 1,600 cycles,
  mean load ≤ 50%, no lost tick. The model omits the Timer0 and USART interrupts (≈ 60–100 cycles each).

---

## 2. Transmission format

### 2.1 Layout

```
[lead-in][tune: N_tune slots at f_ref][sync: N_sync markers][HEADER: 8 peaks + STOP]
[frame 0: N peaks + STOP] ... [frame F−1] [short final frame: d peaks + STOP]? [EOT: 2 markers][tail]
```
- The last sync marker is the header START.
- The header STOP is data frame 0's START.
- Each STOP is the next frame's START.

| Field | Rule | Default |
|---|---|---|
| lead-in | `lead_in_ms` of silence (PTT and relay settling, FM TX delay), on the slot grid | 0 on HF presets, 300 ms on FM presets (`k_default_fm_lead_in_ms`) |
| tune | `N_tune = max(⌈tune_ms/T⌉, 6)` | 250 ms (`k_default_tune_ms`; hf_robust 500, hf_weak 1500) |
| sync | `N_sync = sync_markers`, `k_min_sync_markers`..`k_max_sync_markers` = 8..32 | 8 (`k_default_sync_markers`; hf_robust, hf_weak 16) |
| header | 8 peaks on the header grid (§2.2), then its STOP | – |
| frames | N peaks + STOP each; a short final frame when fewer than B bytes remain (§2.4) | N = 8 (`k_default_data_slots`) |
| EOT | markers at +T and +2T after the final STOP; flips at +T and +2T never occur inside data | – |
| tail | silence | 100 ms (`k_default_tail_ms`) |

**Duration** for n ≥ 1 bytes, with D = ⌈8n/k⌉ data slots, within ±1 sample (`Encoder::duration_samples()`, 0 for an
invalid configuration, saturating at 2³² − 1):

`lead + (N_tune + N_sync + 9 + D + ⌈D/N⌉ + 2)·T·rate + tail`

### 2.2 Mode header (bit-exact)

| Bits | Field | Values |
|---|---|---|
| 0..2 | a = k − 1 | 0..7 |
| 3..5 | b = T_ms mod 8 | 0..7 |
| 6 | spacing | 0 standard (8/7), 1 dense (1) |
| 7..8 | N code | 0 → 8, 1 → 16, 2 → 32, 3 → reserved (decoder: `lost(unsupported_mode)`) |

```
word = a | b<<3 | c<<6,   c = bits 6..8 = (N_code<<1) | spacing
GF(8) = GF(2)[x]/(x^3 + x + 1), elements as 3-bit polynomial values (α = 2)
slot element x_j (j = 0..7) = {0, 1, 2, 4, 3, 6, 7, 5}        // 0, then α^0..α^6 in the optimised order
header tone of slot j:  h_j = x_j^3 ⊕ c·x_j^2 ⊕ b·x_j ⊕ a    // RS(8,3) + x^3 coset, GF(8) arithmetic
```

Properties, verified by enumeration (U21):
- 512 words, minimum symbol distance 6.
- No tone appears more than 3 times in a word, so a steady carrier can match at most 3 slots.
- A window misaligned by ±1 slot is ≥ 3/7 symbols from every codeword; by ±2, ≥ 2/6. This slot order is the
  optimum of all 40,320 (the natural order x_j = j gives only 1/7).

Test vectors (header tones, slots 0..7; the header is not affected by the D33 correction):

| Configuration | Word | Tones |
|---|---|---|
| fm_fast T6 k3 N8 | 0x032 | 2 5 6 2 7 7 4 7 |
| fm T8 k3 N8 | 0x002 | 2 3 1 7 6 5 0 4 |
| hf_fast T16 k4 N8 | 0x003 | 3 2 0 6 7 4 1 5 |
| hf T32 k5 N8 | 0x004 | 4 5 7 1 0 3 6 2 |
| hf_robust T64 k6 N8 | 0x005 | 5 4 6 0 1 2 7 3 |
| hf_weak T128 k7 N8 | 0x006 | 6 7 5 3 2 1 4 0 |
| T32 k5 N16 | 0x084 | 4 7 4 6 1 7 0 7 |
| T16 k4 N32 | 0x103 | 3 6 6 3 5 7 6 4 |
| T128 k8 dense | 0x047 | 7 7 0 4 6 2 6 6 |

In code: `header_word()`, `header_fields()`, `header_symbol()` (§5). GF(8), Gray and the header code live in
`tables.cpp`.

### 2.3 Frame

- `M d_1..d_N M`: N+1 slots, 1/(N+1) marker overhead.
- A STOP counts as detected only when it lies within 0.02·min(N+1, 9)·T of its prediction (§3.9); otherwise the
  frame is flywheeled. (The cap at 9 slots keeps a noise flip half a slot away from passing for the STOP of an
  N = 16/32 frame after a miss.)
- The slot grid itself uses the exact T from the header (× (1+ε) drift, §3.9).
- Reference crest for data slot i (telemetry and gates only): the frame's START crest when detected, else the
  running marker reference (`level_pct`, §3.10).

### 2.4 Short final frame and end of transmission

At each frame boundary (the end of the header STOP for frame 0) the encoder counts the queued bytes q:
- **q = 0:** EOT.
- **q ≥ B:** a full frame.
- **0 < q < B:** a final frame of d = ⌈8q/k⌉ data slots (unused bits in the last slot are 0), then STOP at slot
  d+1, then EOT markers at d+2 and d+3, then the tail. **A short final frame always ends the transmission**: bytes
  written while it is sent stay queued for the next `start()` (otherwise the decoder's flip triple would break).

The decoder releases ⌊d·k/8⌋ = q bytes. This is exact for every k ≤ 8 (U24).

- **END** is declared from the EOT evidence (§3.9: the short-EOT triple, or the v0.1 rule after a full frame).
- **No EOT** (PTT dropped, fade): LOST after 3 of 4 frames with neither STOP nor data (§3.9). Held frames are
  discarded, so nothing is emitted after the signal disappears.

### 2.5 Encoder queue and threads

- **Capacity** is exactly `Encoder::k_queue_size` = `UNLIMITED_ENCODER_QUEUE` (a power of two, 16..128, default 64;
  build-wide define): free-running `uint8_t` head and tail indices, `queued()` = head − tail (the frame being sent
  included), `queue_free()` = `k_queue_size − queued()`; `write()` returns false when full.
- `check()` requires `frame_bytes()` ≤ `k_queue_size/2`, so the application can keep one whole frame queued
  behind the one being sent.
- `start()` fails when the encoder is busy, the queue is empty or the configuration is invalid. Frame 0 is taken
  from the queue at the end of the header STOP; each later frame at its predecessor's STOP. An empty queue at a
  boundary means EOT; a partial frame means a short final frame and the end (§2.4).
- The bytes of a frame stay in the queue until its STOP ends (they feed `status().byte`); then their room is
  handed back.
- **Threads and ISRs.** One producer calls `write()`, `queue_free()`, `queued()`, `busy()` and, while idle,
  `start()`. One consumer (an ISR, or the audio thread) calls `next_sample()` or `render()`. The hand-over uses
  release/acquire fences (`platform.hpp`: `__atomic_thread_fence`; on a single core they act as a compiler
  barrier): a byte is stored before the head that publishes it; the consumer reads a frame's bytes before it moves
  the tail; `start()` publishes every field before `segment_`; the consumer's reads complete before it publishes
  idle. The producer and consumer may run on different cores. `abort()` and `status()` touch the consumer's
  state: call them from the consumer, or with it stopped (interrupts masked around the call). `abort()` empties
  the queue, then publishes idle.

### 2.6 Packet layer (optional helper)

```
[0x2D][0xD4][LEN hi][LEN lo][payload, LEN bytes][CRC hi][CRC lo]     LEN 1..k_packet_max_payload
CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, over the two LEN bytes + payload; check "123456789" → 0x29B1
k_packet_max_payload = UNLIMITED_PACKET_MAX: PC 1024, AVR 256 (build-wide define, 1..65529)
```

`packet_build()` returns the packet size, or 0 when the payload is empty, too large or `out` too small; the
payload may already sit at `out + k_packet_header`.

`PacketReader`:
- consumes `byte` events and ignores `slot` events;
- hunts for `2D D4`; LEN 0 or above the maximum is rejected at once;
- on CRC failure it counts `crc_errors()` and rescans the bytes already buffered, starting after the failed 0x2D,
  so no byte is lost;
- on `end` and `lost` it first rescans the bytes buffered behind a candidate that is still incomplete (e.g. one
  whose LEN was corrupted, so an intact packet behind it is still delivered), then resets;
- several packets may follow each other in one transmission; the handler gets the OR of the byte flags of the
  packet (after a rescan, also of the discarded bytes before it), so `mode_memory` / `blind_mode` / `erasure` reach
  the application.

**FEC plug point (roadmap, out of scope):**
- TX side: `packet_build → FEC + interleaver across frames → Encoder::write`.
- RX side: byte events' `soft[8]` (16 per nat) and `erasure` flags → deinterleave/FEC → PacketReader.
- N code 3 is reserved as the escape for a later FEC header (D49).

---

## 3. Decoder algorithm

The algorithm below is normative at the level of its rules and named constants (§3.14). Where a rule refers to a
constant, the value in `decoder.cpp` / `dsp.hpp` / `dsp.cpp` is the one listed in §3.14.

### 3.1 Units and notation

**Rates and blocks:**
- fs = 8000 Hz.
- Block size B = T_min/8 = `min_slot_ms` samples (4, 8, 16 for the fm, am, ssb profiles; any integer 4..32).

**Window sums (marker path):**
- `S[a,b)` is the complex window sum over fractional block positions a..b, converted to float and divided by the
  mixer gain `k_mixer_gain = 2¹⁵/2¹⁰ = 32`.
- For a tone of crest A over n samples: |S| = A·n·g/2, where g is the mean envelope over the window.
- For white input noise of variance σ²: var(S) = σ²·n_eff, with `n_eff = max(M − 1/3, M/2)·B` for M blocks.

**Slot bank (data and header path):**
- E_t = |Σ x[n]·w[n]·e^{−jω_t n}|² over one slot window, in input units; w = p(u), Σw = 0.875·L,
  Σw² = 0.84375·L for a window of L samples.
- For white noise of variance σ²: E[E_t] = σ²·Σw². A tone of crest A at bin t gives √E_t ≈ A·Σw²/2.

**Positions:** float block offsets from `origin_block_` (uint32), rebased when the span exceeds `k_rebase_blocks`
= 4096 blocks.

**Marker half-window:** W = 0.35·T blocks, for every T-dependent marker measurement.

### 3.2 Per sample (integer)

In `process_sample()`, in this order:
1. **Slot path** (PREAMBLE and TRACK, while a window is scheduled or open), on the raw sample:
   - **Slot blanker** (only when `impulse_blanker`). A sample is *loud* when x² > 16·r (|x| > 4 RMS); it is an
     *impulse* when it is also *peaky*: x² > 5 × the mean x² of the 16 samples on each side. An impulse zeroes
     itself and 4 samples on each side; every loud sample in the 32 samples after an impulse (its ringing) is
     zeroed the same way. r follows `r ← r + (min(x², 16r) − r)/800` over samples outside zeroed spans (a plain mean
     over the first 800 samples); a trigger density (+2 per trigger, −1 per other sample) above 64 is a level change,
     not an impulse, and r then follows at α 1/8. Output delay 20 samples (`k_slot_blank_delay`); the slot path's
     sample index accounts for it. A tone is never peaky (a sine's crest is 2 × its mean), so a peak much louder than
     r (a tilted channel, a fade up) is not blanked.
   - **SlotBank:** w = p(u) as Q15 from a 32-bit slot-phase accumulator started at the window's placement
     (`open(length, skip)`: a window opened late or between two samples still ends where it was placed);
     `x_w = (x·w) >> 15`. Per bin: `s0 = x_w + ((c_q14·s1) >> 14) − s2`, int32 state, int64 product (10 B per bin).
     With `UNLIMITED_BANK_FLOAT` the state is float (12 B per bin). At the window end: `E_t = s1² + s2² − c·s1·s2`,
     then reset. Bin frequencies are clamped to 100..3900 Hz (int32 headroom) and change only between windows.
   - Bins: the **header bank** (16 bins: 8 tones × 2 sides) in PREAMBLE; the **grid bank** (max(M, 8) bins) in
     TRACK. One window at a time, scheduled ahead of its first sample.
2. Block energy for the block blanker: `p += (x>>4)²`.
3. NCO at f_ref: `c = cos_q15(φ)`, `s = sin_q15(φ)`, `φ += step`.
4. Mixer: `m_re = (x·c) >> 10`, `m_im = −(x·s) >> 10`; CIC-2 integrators in uint32 wrapping arithmetic.
5. **Tone search** (`ToneSearch::push(x)`) in SEARCH and while the watch runs (§3.7); a block the search missed
   samples of is dropped on return (`interrupt()`).
6. Every B samples, run the block step (§3.3).

### 3.3 Per block

**1. CIC-2 comb.** `d1 = i2 − c1`, `c1 = i2`, `d2 = d1 − c2`, `c2 = d1`, then `h = (int32)d2 / B`.
h is a triangular-weighted sum over 2B−1 samples with gain B; |h| ≤ 2²⁴ and any window of ≤ 64 blocks ≤ 2³⁰.

**2. Impulse blanker** (2-block delay; the history receives h_{k−2}, zeroed when blanked):
- In-bin energy `e_k = 2|h_k|²/(B·32²·256)`, scaled so that e_k = p_k for a steady in-bin tone.
- **Stage 1 (spike):** blank blocks k and k+1 when `p_k > 10·max(p_{k−2}, p_{k+2})` **and**
  `p_k > 10·floor_p`.
- **Stage 2 (residual):** with `r_k = max(p_k − e_k, 0)`, blank k when `r_k > 8·r_avg` **and**
  `r_k > 2·max(e_{k−1}, e_k, e_{k+1})`; at most 2 consecutive stage-2 blanks, a 3rd trigger adapts `r_avg`.
- Unchanged in v0.2. The grid tones are outside the f_ref bin and count as residual for stage 2; the integrated
  suite runs it on every channel (C6′ clean on/off BER ratio 1.00, *measured*).

**3. History.** `PrefixHistory::push(h, blanked)`: `P_{k+1} = P_k + h_k` (uint32 wrap), plus a blank bitmap. After
the NCO is set (`lock_tone()`) or a large AFC correction, the history is forgotten and 3 settle blocks are skipped.

**4. Noise σ²** (per sample, marker path):
- **SEARCH/ACQUIRE/PREAMBLE:** the 25% `QuantileTracker` of `|S_8|²/n_eff(8)` every 8 blocks, skipping windows
  that hold a blank or exceed 4× the estimate. On a lock from the search the estimate is **seeded from the tone
  search's floor before the tone appeared** and **frozen** (a weak tune passes the outlier test and would drag the
  25% point up by ≈ 3 dB); it is measured only on a stream relock (§3.12), where f_ref is quiet between markers. A
  frozen estimate is replaced by the search's recent floor when that is under half of it (a receiver AGC lowers the
  noise under a tune by up to the SNR).
- **TRACK:** a `NoiseTracker` (running mean, then an exponential average over ≈ 64 inputs, inputs clipped at 10×)
  fed once per frame from the grid window over the **STOP marker** (window 0 of the next frame, which holds no peak):
  `window_noise()/Σw²`, skipped when blanked (§3.10). At LOST, ACQUIRE continues from the TRACK estimate.

**5. Fine AFC** (ACQUIRE unless the tone is already confirmed, and PREAMBLE): decimate to 125 Hz, 65 leaky DFT bins
on **w²** (removes the marker sign): 1 Hz apart within ±20 Hz (squared; ±10 Hz on the tone), 3.33 Hz apart out to
±60 Hz (±30 Hz on the tone), λ 0.984 (outer bins 0.95). The peak must reach 8× the mean of the bins outside ±3 bins
of it; the search is ±16.7 Hz on the tone for a steady (tune) lock and ±30 Hz otherwise. First look after 16 inputs,
then every 0.25 s; a correction ≥ 0.2 Hz is applied with `nco.adjust(offset)`. In PREAMBLE a correction above 0.25
cycles per slot is ignored (the train already holds the tone within a fraction of 1/T). A first large correction on a
non-tune lock in ACQUIRE (rotation over the slowest marker half-window > 0.5 rad) forgets the history. The AFC is
frozen on a stream relock (the stream's peaks next to f_ref would pull it). It is reset on every ACQUIRE entry; in
TRACK its storage is reused by the grid bank (§3.15).

**6.** Run the state machine (§3.5).

### 3.4 Flip measurement at centre c with half-window W

```
S_b = S[c−W, c)       S_a = S[c, c+W)       n_e = n_eff(W)
q      = −Re(S_b·S_a*) / (σ²·n_e)                       reversal strength
q_bal  = q − | |S_b|² − |S_a|² | / (2·σ²·n_e)           balanced: pulse onsets and one-sided energy go negative
κ      = −2·Re(S_b·S_a*) / (|S_b|² + |S_a|²)            ≈ +1 flip, ≈ −1 continuous
A_mk   = |S_b − S_a| / (W·B·g_m),  g_m = 0.8355          marker crest (flip-compensated matched filter)
steady = |S_b + S_a| / (W·B·g_m)                         a carrier continuous across c, same scale
energy = (|S_b|² + |S_a|²) / (2·σ²·n_e)                  mean half-window energy over the noise
Δφ     = arg(−S_a·S_b*)                                 phase advance over W (for AFC)
```
- **A flip** (`is_flip`): q ≥ 4, κ ≥ κ_min (0.3; 0.5 after a missed STOP) and κ ≥ min(1 − 1/E − 4.2/√E, 0.75)
  with E = q/κ (a true reversal of energy E has κ ≈ 1 − 1/E with spread √(2/E); a strong "flip" far below that is
  a smeared marker of another speed or a mistuned tone).
- **Plain q with a κ gate:** predicted markers and sync evidence. **q_bal:** candidates and the audit.

`search_flip(c, span, W, κ_min)`: evaluate at `c + δ`, δ ∈ [−span, +span] in steps of 0.05T (at most ±10 steps),
take the maximum q among positions with κ ≥ κ_min, refine parabolically.

### 3.5 State machine

```
            tone found              sync evidence OK          header accepted (or memory match)
 SEARCH ───────────────► ACQUIRE ───────────────► PREAMBLE ─────────────────────────────► TRACK ──► slot, byte events
   ▲    ◄─timeout(+ban?)─  │   │                     │                                    │
   │                       │   └── late join with mode memory ──────────────────────────► TRACK (guard)
   │                       ▲                         │ no header, no memory: lost(no_header)
   │                       │                         │ mode over caps / N code 3: lost(unsupported_mode)
   │                       └──── LOST(signal_gone | alias | preamble_timeout | no_header | unsupported_mode) ◄─┤
   └──────────── END (EOT) / reset() ◄───────────────────────────────────────────────────────────────────────┘
```

`locked` is emitted when a TRACK lock is **confirmed** (§3.9 late step 6, or the late-join guard), never on
TRACK entry alone. This is the event that false-lock tests count. No byte is ever released without a mode.
`reset()` emits `lost(reset)` when in PREAMBLE or TRACK, then a `state` event if the state was not SEARCH. The
ACQUIRE timeout bans the tone only when it is still present (§3.7).

### 3.6 SEARCH (tone search)

Per 20 ms search block (160 samples), 49 Goertzel bins at 300..2700 Hz (or the configured range), 50 Hz step, int16
Q14 coefficients, int32 state, int64 products:
- `fast_b` = EMA(1/8) of power P_b; `slow_b` = EMA(1/128); `slow_sq_b` = EMA(1/128) of P_b² (1/n warm-up).
- **Half-bin powers** `|X_b − X_{b+1}|²/2` (the DFT halfway between two bins, same noise), fast-averaged; in a
  **quiet bin** (slow_b ≤ 2·floor) the bin's level is the largest of its own average and its two half-bins (cuts the
  scalloping loss of a tone between bins from 3.9 dB to 1.2 dB).
- **Floor:** mean of the lower half of `slow_b`, divided by its bias (1 − 0.8/√N_avg), at least 1 LSB rms.
  **Recent floor:** mean of the lower half of one block's powers / 0.309, averaged with α 1/4 (follows a receiver
  AGC within ≈ 100 ms). Locks compare against **max(floor, recent floor)**.
- **Excess variance:** `ev_b = (slow_sq_b − slow_b²) − (2·slow_b·floor − floor²)`: ≈ 0 for noise and steady
  carriers, ≈ d(1−d)·C² for keyed signals.
- **Steady mask:** after 128 blocks, a bin is masked while `slow_b ≥ 4·floor` and `ev_b < 0.05·(slow_b − floor)²`.
- **Fast lock:** level ≥ 4× the lock floor in a quiet bin (6× otherwise, where de-emphasised FM noise sits above the
  floor), not masked, not banned, not excluded, a local peak, after 8 warm-up blocks, and the current block's level
  ≥ 2× the lock floor; the same bin (±1) for 3 blocks, or for 8 blocks (`k_long_run_blocks`) when below 6×.
- **Slow lock:** after 96 blocks, `slow_b ≥ 2·floor` and `ev_b ≥ 1.0·floor²`, local peak, for 3 blocks.
- **Tone estimate:** a steady tune: the block-to-block phase product gives the offset modulo one bin; the whole-bin
  alias comes from the half-block phase when the run's half-block products are coherent (≥ 0.3), otherwise from the
  sinc² pattern fit. Data (reversals, gaps): the doubled phase modulo half a bin, the alias nearest the power
  interpolation (a wrong one is 25 Hz off, inside the fine AFC pull-in).
- **Train onset** (`train_onset(tone, min_products)`): a tone steady over at least `min_products` block products
  (coherence ≥ 0.9) whose product then breaks off its steady phase by > 37° and > 3.6σ (both blocks ≥ 10× the floor).
- **Exclusion:** no lock within ½ bin + 1/(2·T_min) of the tone ACQUIRE holds (its own train's first lines).
- **Ban:** `ban(tone, 10 s << min(strikes, 3))` on the bin and its neighbours; strikes decay one per 5 min.
- **`present(tone)`:** level ≥ 4× the lock floor now.
- **On lock (in SEARCH):** a tone inside the remembered grid (§3.12) that is not steady locks the **remembered f_ref**
  instead (stream relock); a steady tone inside it but more than 50 Hz from its f_ref must hold 8 blocks
  (`long_run()`) before it locks as a tune. Otherwise: tone estimate, `nco.set_frequency`, history/candidates/AFC
  reset, noise seeded from the onset floor and frozen, exclusion set, watch on, ACQUIRE.

In v0.2 the tune tone is what makes a fresh acquisition find f_ref: in a running MFSK stream the power search
prefers a data tone in 85–100% of cases (§4.6), which is why late join needs mode memory (§3.12) or the v0.2b
band-edge search (§3.13).

### 3.7 ACQUIRE

**Timeout:** `max(3 s, 40·T_max)`, counted from `lock_tone()` or the end of a confirmed lock (a PREAMBLE that found
no header does not restart it); on a stream relock at least 8 remembered frames. At the timeout the tone is banned
only if it is still **present**, not tone-confirmed and not the remembered f_ref (±50 Hz): a tune heard alone whose
train was missed does not ban the station's retry. Then SEARCH.

**Watch.** A lock taken from the search keeps the tone search running (§3.2) and moves to another tone as soon as
that one turns from steady into a marker train:
- ACQUIRE: a train onset of ≥ 3 steady products (≥ 4 while ACQUIRE holds a tune);
- PREAMBLE, while g ≤ L + 1: ≥ 10 steady products (the stream's peaks and the train's own markers stay steady at most
  128 ms in other bins; a tune ≥ 250 ms is steady longer), not the tone it left (±50 Hz), and not one of the
  preamble's own train lines (2m+1)/(2T) from f_ref, m ≤ 7, ±5 Hz;
- never an odd harmonic image (3rd, 5th, 7th of the held tone folded at 8 kHz, ±50 Hz) of a saturated input;
- in ACQUIRE, a held tone that the search masks as a steady carrier returns to SEARCH (statistics kept), so a tone
  next to the carrier can be taken.
The watch is off after a loss from TRACK, on a stream relock, and from the header on.

**Candidates:** per block and per scale W_s ∈ {3, 4, 6, 8, 11, 16, 22} blocks, local maxima with `q_bal ≥ 3` and
`κ ≥ 0.5`, parabolic position, merged within 0.35·8 blocks into a 16-entry ring. On a stream relock only candidates
with A_mk ≥ 0.35 × the remembered crest enter. Each new candidate triggers `try_sync`, then `try_late_join`, once the
lock is *tuned* (a steady tune, a confirmed tone, an AFC look, or 1 s elapsed).

**try_sync(c):**
1. Hypotheses T = (c − b)/m for older candidates b and m = 1..7, within the range (±6% tolerance at its ends), not
   banned, merged within 3%.
2. Positions c − iT, i = 0..7, each `search_flip(±0.1T)`. A **hit** is a flip with A_mk ≥ 0.5 × the strongest flip;
   a **weak marker** has q ≥ 2, κ ≥ 0.3 and A_mk ≥ 0.3 × the strongest. Evidence E = Σ clamp(q, ±8) (κ-gated; a flip
   that is not a hit counts 0) over the best prefix of ≥ 5 positions. Two or more flips at the 4 newest midpoints
   reject T (a far-off tone rotates everywhere).
3. Accept on E ≥ 24 and either the **hit rule** (≥ 5 hits, or ≥ 4 hits among the newest 5 positions with ≥ 2 odd and
   ≥ 2 even) or the **weak-marker rule** (≥ 3 hits with ≥ 1 per parity, ≥ 5 markers with ≥ 2 per parity, at most 1
   scored position without a marker).
4. **Boundary check:** the mean excess energy in ±0.1T around the boundaries between neighbouring markers, over a
   tone of the train's crest, must be ≤ 0.25 + 3σ of its noise (between train markers the envelope is null).
5. Choice: a reading on weak markers yields to a hit-rule reading within 10% of its T; best E; then the smallest
   integer sub-multiple with E ≥ 0.8·E_max; then, while ≤ 1 hit lies at odd positions (a 2T train read at T/2), T
   doubles while the doubled evidence is higher (a double out of range rejects: a slower sender).
6. Least-squares refine (2 passes, ≥ 3 flips, change ≤ 10%); T must stay in range; on a stream relock T must be within
   10% of T_mem. Go to PREAMBLE with the train's crest and hit/marker positions.

**try_late_join(c)** (memory only; cold late join is v0.2b, §3.13). With a usable memory (§3.12):
1. Hypothesis T = T_mem·(1 ± 3%), period P = (N_mem+1)·T.
2. Hits: candidates with q_bal ≥ 4 within ±0.1T of `c − j·P` for j = 1..3.
3. Accept on 3/3 hits; refine c by `search_flip(c, 0.1T, W)`.
4. START = c. Go to TRACK with `late_join | mode_memory` and the guard active (§3.12).

### 3.8 PREAMBLE: sync train and mode header

**Train.** For each grid index g (position `grid + (g − L)·T`, L = the last train flip):
- `search_flip(pos, 0.1T)` (0.25T at the header STOP position g − L ≥ 9, where a 2% T error has moved it 0.18T).
- A flip counts when A_mk ≥ 0.5·ref (0.3·ref at the header STOP), or as a **faded marker**: g = L + 1, q ≥ 16,
  κ ≥ 0.8, A_mk ≥ 0.2·ref and within ±0.05T of its grid position (Rayleigh fading swings a train's crests by 10 dB).
- A flip with gap ≤ 2 continues the train (L = g). **T and the grid come from a weighted least-squares line** through
  every train marker (weights clamp(q, 1, 8)), seeded with the sync's positions (q ≥ 2, κ ≥ 0.3; a marker found at the
  edge of ±0.1T is searched again over ±0.25T), used from 3 markers' weight on when within 10% of the current T;
  otherwise `T += 0.2·((pos − grid)/gap − T)`. The reference crest follows each train flip (α 0.25). A T that leaves
  the range bans T and gives `lost(alias)`.
- **Sub-rate check:** equal gaps k ≥ 2 seen ≥ 2 times and ≥ 2× the gaps of 1 (seeded with the sync's adjacent hits)
  reveal a T/k reading: T × k (if in range), refit, restart the grid and the header search.

**Header.**
1. **Windows.** Every grid slot g gets a header-bank window of one T centred on it, over the 16 header bins at
   f_ref ± (5 + 8h/7)/T_train (coefficients follow the AFC). The last 9 slots' energies are kept as log2 Q8.8
   (9 × 16 × 2 B = 288 B).
2. **Hypotheses.** H(s) (header START at grid index s, peaks s+1..s+8) is evaluated when window s+8 closes, for
   L − 2 ≤ s ≤ L + 3 (a noise flip on a first header peak can move L onto it; up to 3 faded last train markers), when
   at most one detected flip lies in s+1..s+8 (a train that went on is no header).
3. **Floor, noise and z** (`decide_header`), per side d and tone h over the 8 slots: floor[d][h] = mean of the 4
   smallest energies (a codeword uses a tone at most 3 times); spread = (floor − smallest)/0.2411 (the spread of
   exponential noise over its mean). A side's own noise = the 5th smallest of its 8 spreads; N_h = the 9th smallest of
   all 16. When the two sides' own noise differs by more than 4× (one side in a receiver stopband, or under a
   carrier), each side uses max(own, N_h). A **carrier tone** has floor > 3·noise and smallest > 0.3·floor; it is
   scaled by max(own spread, noise). A **carrier side** (a carrier tone, sides disagreeing, its noise above the
   other's) scales each tone by max(own spread, the clean side's noise), and its decided noise is the clean side's.
   `z[d][j][h] = (E − floor[d][h]) / scale`.
4. **ML.** `S(w, d) = Σ_j z[d][j][h_j(w)]` over 512 words × 2 sides; best S1 at (w1, d1), second best S2.
   Agreement A = slots whose strongest tone is h_j(w1). **Accept iff S1 − S2 ≥ 12 and A ≥ 6** (A ≥ 5 on a carrier
   side when S1 − S2 ≥ 24). A header with A ≤ 6 is **weak** (§3.9).
5. **Plausibility.** The exact T (step 7) must lie within 5% of T_train (header windows placed at a T 5% off have
   drifted half a slot by the header's end), and the grid received from f_ref must stay within 200..2800 Hz.
6. **Look-ahead.** An accepted H(s) waits for H(s+1); the larger margin wins. An H(s) with s < L waits until H(L+1)
   has been evaluated (a window misaligned by 2 slots can still match 6 slots of some codeword). An H(s) whose inner
   slots gained train flips meanwhile is dropped.
7. **Validate.** k ≤ `UNLIMITED_MAX_BITS_PER_PEAK`, N code ≠ 3, N·k/8 ≤ `UNLIMITED_MAX_FRAME_BYTES`; T_exact = the
   whole ms ≡ b (mod 8) nearest T_train (at an end of the profile range, the in-range neighbour when within 6 ms),
   inside [max(`min_slot_ms`, 6), 8·`min_slot_ms`], (N+1)·T ≤ 1152 ms. Otherwise ban T for 10 s and
   `lost(unsupported_mode)` (no bytes).
8. **Enter TRACK.** START of the header frame = its START at s; its STOP (data frame 0's START) is searched like any
   other (±0.25T). σ_rx = d1, frame_index = 0, unconfirmed, ε = 0. Grid bank:
   f_t = f_ref + σ_rx(5 + t·c)/(T_exact(1+ε)), background seeded with the decided side's noise; in standard spacing, grid tones 0..7 whose header floor exceeds
   2× that seed start from the header floor (a carrier seen in the header).
9. **No acceptance.** With a mode memory that fits (§3.12):
   - an H(s) whose remembered word agrees in ≥ 6 slots on the remembered side enters TRACK with `mode_memory`
     (weak when exactly 6); an H(s) with ≥ 4 slots at z ≥ 8 marks the header as *seen*;
   - at s = L + 1, with no header seen and no marker at L+1, L+2 (q < 1: the train ended), TRACK on the train's phase
     with `mode_memory`, weak.

   Otherwise, once s ≥ L + 3: `lost(no_header)` → ACQUIRE on the same tone with its candidates; a tone inside the
   remembered grid but more than 50 Hz from its f_ref relocks the remembered f_ref instead.

**Timeouts:** g − L > 13 (`k_header_search_end`, L + 13, a bound on the history clock in case no header window
closes) → `lost(no_header)` as above, without the memory checks; g > 72 → `lost(preamble_timeout)`.

### 3.9 TRACK

Frame f (the header frame first, then data frames) with START S_f and predicted STOP p = S_f + (N+1)·T_exact·(1+ε)
(N = 8 for the header frame). The early and late steps and the audit run on the history clock; slot decisions on
the sample clock (§3.10). "ref" is the running marker reference (α 0.25 over detected markers) for the STOP, the
v0.1 END rule and the rotation AFC, and the frame's START crest (the running reference when the START was missed) for
the audit, the tune detection, the active centre and the short-EOT evidence.

**Early step**, when every audit position of the frame is measured and p + (span + 0.35)·T + 1 block is in the
history:
1. `search_flip(p, span, W, κ_min)`: span 0.15T (0.5T after a miss, 0.25T for the header frame), κ_min 0.3 (0.5 after
   a miss).
2. **Detected** = a flip, A_mk ≥ 0.3·ref, and |m − p| ≤ 0.02·min(N+1, 9)·T. The STOP's clipped q (≤ 0 when not
   marker-like) accumulates into the lock's **STOP evidence**.
3. If detected: S_{f+1} = p + 0.5·(m − p); ε ← clamp(ε + 0.05·(m − p)/((N+1)T), ±1e-3); the reference crest follows
   (α 0.25); before confirmation the marker edges are measured. Otherwise S_{f+1} = p.
4. **START/STOP AFC:** on a detected STOP with q ≥ 8 and the previous START's halves cached:
   `offset = arg(−(S_b,stop·S_b,start* + S_a,stop·S_a,start*)) / (2π·span_s)` (the same half of both markers: the
   receiver filter shifts both alike), `nco.adjust(clamp(0.1·offset, ±0.1/T))`; the STOP's halves are cached as the
   next START's.
5. **Rotation AFC** (T ≥ 32 ms only, H2): a missed STOP whose halves are balanced (q − q_bal ≤ 0.25·energy), with
   energy ≥ 16, level √(A_mk² + steady²) in 0.5..1.5 × ref and |Δφ| ≥ 0.6 rad reads the offset
   Δφ/(2π·0.383·T); two in a row that agree (same sign, within 50% of the larger) move the NCO by 0.75 × their mean.
   A frequency step (VFO, RIT, drift) of ≥ ¼ spacing makes κ fail and the START/STOP AFC starve; this recovers it in
   about 2 frames.
6. The new grid coefficients load at the next slot boundary; slot 1 of frame f+1 was already opened on the
   prediction, slots 2..N use S_{f+1}.

**Progressive work** (every block):
- **Audit** positions S_f + j·T/2 (j = 1..2N+1), each once the history holds it and the slots under its half-windows
  are decided (§3.11), so the history need stays ≈ 4·T_max whatever N is.
- **Tune detection:** 6 audit positions in a row with κ ≤ −0.5, q/κ ≥ 4 and steady ≥ 0.5·ref are the next
  transmission's tune on f_ref (this one's EOT was missed) → `lost(signal_gone)`.
- **Active centre:** the first slot centre of the frame with energy ≥ 8 and max(A_mk, steady) ≥ 0.5·ref holds a marker
  or a carrier, not a peak.
- **EOT evidence** at slot centres: clamp(q, ±8) when κ ≥ 0.3 and A_mk ≥ 0.3·ref, else min(q, 0); stored in 1/15.

**Short EOT**, checked as each centre is measured: for j = 2..N+1 (and j = 0 on a confirmed lock: the START was the
final STOP whose END was missed), three consecutive centres j, j+1, j+2 each ≥ 3 and summing ≥ 15 → END: release the
held frames plus ⌊(j−1)k/8⌋ bytes of this frame (an unconfirmed lock must first pass the confirmation tests without
the tone-variation rule and without a weak header), emit `end`, go to SEARCH.

**Late step**, when p + (2.35 [+1 unconfirmed])·T + 1 block is in the history (unconfirmed: and slot 3 of the next
frame decided). In order:
1. **END** (v0.1 rule): flips at +T and +2T with κ ≥ 0.3 and A_mk ≥ 0.3·ref, clamped q summing ≥ 10. An unconfirmed
   lock releases the frame only when *clean*: not weak, present, no strong inner flip, decodable, STOPs found, tones
   vary, and `eot_clean` (quiet EOT edges, no flip at +3T, slot 3 after the STOP not confident).
2. **Alias** (§3.11): the AuditRing reaching 12, or a strong inner flip (evidence ≥ 8) in ≥ 3 of the last 4 frames →
   `lost(alias)`; T is banned (±5%, 10 s) only when the lock came from mode memory (a header lock's T is exact).
3. **Truncation:** a frame whose STOP was missed and whose first active centre is j ≤ N keeps only ⌊(j−1)k/8⌋ bytes
   (a short final frame whose EOT the next transmission's tune masked).
4. **Presence.** A frame is present when ≥ ½ of its slots are confident (§3.10); it is *alive* when present or its
   STOP q ≥ 1.
5. **LOST.** When ≥ 3 of the last 4 frames are not alive: discard held frames, `lost(signal_gone)` (keep the tone;
   the mode memory is stored).
6. **Confirm** (unconfirmed lock; alive frames are held meanwhile):
   - **fresh** (header, or memory at the header): at frame 0 or 1 when the frame is present, holds no strong inner
     flip, the marker edges are quiet (mean boundary excess ≤ 0.3 + 3σ), the lock is **decodable**
     (snr ≥ −5.9 − 10·log10(T/32 ms) − 5 dB), its **STOPs are found** (STOP evidence ≥ 4), its **tones vary**
     (k ≥ 3 and ≥ 5 confident slots: 2·changes + 1 ≥ confident), and a weak header also has STOP evidence ≥ 12 and
     k ≥ 3: emit `locked` (with k, N, spacing, side, exact T; frame_index of the first frame released) and release.
     Otherwise after 2 frames `lost(signal_gone)` (keep the tone).
   - **late join:** the v0.1 guard over 4 frames (§3.12); its frames 2 and 3 are held.
7. **Release** (confirmed). If the STOP is detected or ≥ ¾ of the slots are confident: release the held frames, then
   this one (flag `flywheel_stop` if the STOP was missed, `flywheel_start` if its START was). Else, if alive, hold it
   (at most 2; a third drops the oldest). An absent frame is dropped.
8. `S_f ← S_{f+1}`, `frame_index++`.

Removed from v0.1b: `zeros_quiet`, `guard_threshold_too_high`, `guard_levels_too_high`, `guard_clean`,
`decision_ratio`, `push_gap_noise`, the loud-zero and guard-level tests, the TRACK timing gain on measured T
(replaced by the frame-phase loop), and the "anti" STOP test (T/2 lock; the header rules those out).

### 3.10 Slot decision (at each grid window end, TRACK)

- **Windows.** Windows 1..N of a frame are its peaks; every frame after the first also opens window 0 on its START
  marker, i.e. the previous frame's STOP (no peak: TRACK noise, §3.3).
- **Background.** Per-bin log2 Q8.8 quantile tracker bg_t: +22 LSB when E_t > bg_t, −66 LSB otherwise (×4 during
  the first 32 slots after a reset, so a carrier 30 dB over the noise is learnt within 32 slots), which settles on the
  25% quantile; mean = 2^((bg_t + 460)/256). Seeded at TRACK entry (§3.8 step 8; a late join seeds from the marker
  noise). Not used for k ≤ 2.
- **Noise and z.** N_bin = median over t of mean(bg_t); for k ≤ 2, N_bin = the mean background of the unused bins
  M..7. `z_t = max(E_t − max(mean(bg_t) − N_bin, 0), 0)` (k ≤ 2: z_t = E_t).
- **Decision.** t* = argmax z_t over the M tones, t2 = runner-up. n̂ = t*, s = `peak_symbol(t*, i − 1, k)`.
- **Amplitude and LLR.**
  - â = √max(z_t* − N_bin, 0.05·N_bin); x_t = 2·â·√z_t / N_bin.
  - m_t = ln I0(x_t), from a 33-entry table over [0, 16] with linear interpolation; above 16,
    m = x − ½·ln(2πx).
  - LLR_b = max_{t: bit_b(label_t)=1} m_t − max_{t: bit_b=0} m_t, label_t = `peak_symbol(t, i − 1, k)`.
  - Clamped **as a float** to ±7, then rounded: q4 (1 nat per step); the sign always carries the decision
    (|q4| ≥ 1). `Event.soft` = 16·q4 (|soft| ≤ 112).
- **Presence.** A slot is confident when z_t* ≥ 2·H_{M′} × the mean z of the other open bins, H_{M′} = Σ_{j=1..M′} 1/j,
  M′ = max(bins, 8). Noise alone gives about H_M.
- **Telemetry.** crest = 2√max(z_t* − N_bin, 0)/Σw² (input units; a full peak reads its crest A); `level_pct` =
  crest as a % of the frame's START crest (the running reference when the START was not detected; the STOP is not
  known when a slot event is sent), ≤ 255. `confidence` = 10·log10(z_t*/z_t2) in 0.5 dB steps (255 when z_t2 = 0).
  The `erasure` flag is set on the bytes of a slot with z_t* < 2·z_t2.
- **Marker noise** (`window_noise`): on the STOP window, the (bins/4)-th smallest energy over its expectation for
  exponential noise (Σ_{i ≤ rank} 1/(bins − i)); a peak's matched window leaks −29..−37 dB into every other bin, a
  marker below −40 dB, so this holds up to per-slot Es/N0 ≈ 40 dB. σ² per sample = that / Σw². The SNR report:
  `snr_db = 10·log10((ref_avg² − N_m)·8000/(4·2500·σ²))` (key-down tone in 2500 Hz), N_m the expected square of a
  crest measured on noise alone (removes a 0.3 dB bias at the gates).
- **Bytes.** Symbols are appended MSB first to a bit accumulator; every 8 bits make a byte of the current frame
  buffer with its 8 q4 LLRs (4-bit, two per byte) and flags (`erasure` if any contributing slot was an erasure,
  `blanked` if any of its slots had blanked samples).

#### 3.10.1 What the START/STOP ("70%") reference does now

It **decides nothing**: argmax needs no threshold, which is why the fading floors are gone. It still provides the
marker gates (STOP 0.3·ref, train 0.5·ref, audit 0.3·ref), `level_pct` telemetry (the TUI draws the 70% line as a
visual reference) and the SNR report. Erasures use the slot's own best/second ratio instead: it catches 90–99% of
symbol errors in fading, where "level < 0.7·ref" flags 8–24% of slots.

### 3.11 Flip audit (alias protection)

For each frame and each position j = 1..2N+1 at `S_f + 0.5j·T` (slot centres and inter-slot boundaries), with
W = 0.35T, measured once the slots under its half-windows are decided:
```
evidence_j = clamp(q_bal, −4, 8)   if κ > 0, A_mk ≥ 0.3·ref and A_mk ≥ 0.5·max(winner crest of the slots under its halves)
           = clamp(q_bal, −4, 0)   otherwise
```
- ref is the frame's START crest (the running reference when the START was missed).
- The purity gate (0.5 × neighbouring winner) is new in v0.2 (D42): a data peak leaks into the f_ref windows.
- `AuditRing` keeps the last 4 frames as int8 in 1/15 units, filled progressively and committed at the early step.
- The alias test is `max_j Σ_4 evidence_j ≥ 12`, or a strong inner flip (evidence ≥ 8) in 3 of the last 4 frames.
- Under a correct lock no audit position is within 0.35T of a flip; a 2T lock puts a real marker on a boundary
  position, a 3T lock on slot centres.

### 3.12 Lock sources, guard and mode memory

| Source | Mode from | Guard | Frames released |
|---|---|---|---|
| Fresh (tune + sync + header) | header | frame 0 or 1: present, no strong inner flip, edges quiet, decodable, STOPs found, tones vary (weak header: STOP evidence ≥ 12, k ≥ 3) | all from frame 0 |
| Header lost, memory match | mode memory | same as fresh; weak when the remembered word matched only 6 slots or no header was seen | all, flagged `mode_memory` |
| Late join, memory match | mode memory | v0.1 guard: 4 frames, ≥ 4 of 5 markers detected, ≥ 3 of 4 frames present, audit clean, no strong inner flip, edges quiet, decodable | guard frames 2 and 3 (held), flagged `late_join` + `mode_memory` |
| Cold late join (v0.2b) | blind (§3.13) | same as late join | flagged `late_join` + `blind_mode` |

A late join that fails its guard gives `lost(alias)` (T banned) when a strong inner flip was seen, else
`lost(signal_gone)`.

**Mode memory:**
- **Stored:** {f_ref after AFC, T_exact, N, k, spacing, σ_rx, the marker crest}, at END or LOST of a **confirmed**
  lock.
- **Lifetime:** 60 s of input; cleared by `reset()`.
- **Usable:** the NCO within 10 Hz of the remembered f_ref. **Fits** (header lost): T_train within ±3% of T_mem.
  **Late join:** a flip period within ±3% of (N_mem+1)·T_mem, 3 of 3.
- **Remembered grid:** f_ref to its farthest tone, ± 50 Hz (one search bin).

**Stream relock** (fade → LOST → relock, D44). ACQUIRE on the remembered f_ref with no tune heard (after a LOST from
TRACK, or when the search found an unsteady tone inside the remembered grid) reads the station's running stream:
- only marker-like candidates (A_mk ≥ 0.35 × the remembered crest: the stream's peaks leak into f_ref at up to 0.16 of
  the crest and would fill the candidate list within a frame);
- a sync must give T within 10% of T_mem (peak leaks read as trains of other T);
- no watch (a stream's peaks at T ≥ 64 ms look like tunes and trains), the fine AFC frozen (the peaks 39 Hz from f_ref
  at T = 128 ms would pull it), noise measured on f_ref;
- the ACQUIRE timeout lasts at least 8 remembered frames (LOST comes 3 frames into a fade; the late join needs 3
  frame periods of chain after the signal is back);
- a tune on the held f_ref, steady over half-windows of max(22 blocks, 0.75·T_mem) (κ ≤ −0.5, energy ≥ 4,
  steady ≥ 0.5 × the remembered crest), ends the stream reading: the next transmission's tune is taken as such.

### 3.13 Cold late join (v0.2b: specified, not implemented in v0.2)

1. **SEARCH, band edges.** From 2 s after entering SEARCH (4 s once T ≥ 64 ms is detected by the period), keep
   the mean power of each of the 49 tone-search bins; floor = their 25% quantile.
   - A bin is occupied when its mean ≥ max(1.25·floor, strongest − 8 dB).
   - Take the run of occupied bins (gaps ≤ 14 bins allowed) that contains the strongest bin.
   - Candidates, up to 4: each end bin of the run, and the strongest bin within 3 bins of each end.
   - ACQUIRE probes them in turn; the v0.1 timeout applies; failed probes get a 10 s ban without strike
     escalation.
2. **ACQUIRE.** Flip candidates, then a period P from 3 equal intervals. Hypotheses T = P/(N+1) for N ∈ {8, 16,
   32}, kept when T is a whole ms within 2%.
3. **T/N fold.** Over the next 2 frames, fold the per-block wideband energy (blanker's p_k) at each hypothesised T.
   The smallest T with mean(boundary blocks)/mean(centre blocks) ≤ 0.7 wins.
4. **Side and M.** σ = the side of f_ref where the band lies. M = 2^round(log2((W·T − 5)/c + 1)) for c = 8/7 and
   for c = 1 (T ≥ 32 ms); one frame of grid bank per spacing hypothesis; keep the higher mean z_t*/mean z.
5. **Guard:** as a late join. Bytes are flagged `blind_mode`.
6. **Requirement:** M ≥ 2N; otherwise the decoder waits for the next tune tone or a memory match.

### 3.14 Constants (named, in `decoder.cpp`, `dsp.hpp` or `dsp.cpp`)

Values in the code are normative; this table lists them by stage.

| Stage | Constants |
|---|---|
| Profiles | `k_ssb_min_slot_ms` 16, `k_am_min_slot_ms` 8, `k_fm_min_slot_ms` 4, `k_fm_min_tone_hz` 1000, block 4..32 samples, `k_small_block_samples` 8 (below it f_ref ≥ 1000 Hz) |
| Front end | `k_decoder_rate_hz` 8000, `k_blocks_per_min_slot` 8, `k_speed_span` 8, `k_history_cells` 12·8·8 + 32 = 800, `k_mix_shift` 10 (`k_mixer_gain` 32), `k_rebase_blocks` 4096, `k_cic_overlap` 1/3, `k_g_marker` 0.8355, `k_min_noise_variance` 1/12, `k_energy_shift` 4, settle 3 blocks, history lag 2.5 blocks |
| Block blanker | `k_spike_ratio` 10, `k_spike_floor` 10, `k_residual_ratio` 8, `k_residual_tone` 2, `k_residual_run` 2, `k_residual_adapt` 0.2, residual α 1/64 |
| Noise | `k_noise_blocks` 8, `k_noise_outlier` 4, `k_noise_drop` 0.5, quantile scale 3.476 (step 1/64, warm-up 1/8), NoiseTracker clip 10, average 64, seed weight 8 |
| Fine AFC | 125 Hz (`k_afc_decimation_samples` 64), 65 bins (1 Hz within ±20 Hz squared, out to ±60 Hz), λ 0.984 / 0.95, peak ratio 8, lobe 3 bins, `k_afc_eval_ms` 250, `k_afc_min_inputs` 16, `k_afc_wait_ms` 1000, `k_afc_min_offset_hz` 0.2, `k_afc_reset_rotation` 0.5 rad, `k_preamble_afc_limit` 0.25 cycles/slot |
| Tone search | 49 bins, 50 Hz, 160-sample blocks, Q14; fast α 1/8, slow α 1/128; `k_fast_lock` 6, `k_fast_lock_quiet` 4 in a quiet bin (`k_quiet_bin` 2), current block 2; `k_slow_lock` 2, `k_keyed_min` 1.0, `k_steady_max` 0.05, steady level 4, mask after 128 blocks, slow after 96; lock 3 blocks, `k_long_run_blocks` 8, warm-up 8; lower-half bias 0.8/√N, recent floor 0.309 with α 1/4; half coherence 0.3, phase coherence 0.9, alias ≤ 0.75 bin; onset break cos 0.8 (37°), 3.6σ, 10× floor, 3 steady products; ban 10 s base, `k_max_strike_shift` 3, strike decay 5 min |
| Watch | `k_onset_products` 3, `k_onset_products_tune` 4, `k_onset_products_preamble` 10, `k_train_line_orders` 7, `k_train_line_hz` 5, harmonic images 3rd–7th, `k_image_reach_hz` 50, `k_memory_search_margin_hz` 50 |
| Geometry (× T) | `k_marker_half` 0.35, `k_search_step` 0.05, `k_position_search` 0.10, `k_first_stop_search` 0.25, `k_track_search` 0.15, `k_track_search_miss` 0.5, `k_max_search_steps` 10 |
| Flips | `k_q_candidate` 3, `k_kappa_candidate` 0.5 (on q_bal), `k_q_track` 4, `k_kappa_track` 0.3, `k_kappa_after_miss` 0.5, `k_q_present` 1, `k_evidence_clip` 8, `k_kappa_spread` 4.2, `k_kappa_floor_max` 0.75, `k_candidate_scale_blocks` {3, 4, 6, 8, 11, 16, 22}, `k_candidate_merge_blocks` 2.8, 16 candidates |
| Sync | `k_sync_positions` 8, `k_sync_min_positions` 5, `k_sync_min_hits` 5, `k_sync_dense_hits` 4 (newest 5, ≥ 2 per parity), `k_sync_weak_q` 2, `k_sync_min_strong` 3, `k_sync_min_parity_strong` 1, `k_sync_missing_markers` 1, `k_sync_evidence` 24, `k_sync_midpoints` 4, `k_sync_max_midpoint_flips` 2, `k_boundary_half` 0.1, `k_boundary_max` 0.25, `k_marker_edge_sigmas` 3, `k_prefer_smaller_t` 0.8, `k_hypothesis_merge` 0.03, `k_max_hypotheses` 64, `k_max_accepted` 16, `k_refine_passes` 2, `k_refine_min_points` 3, `k_refine_max_change` 0.1, `k_range_tolerance` 0.06, `k_half_rate_odd_hits` 1 |
| Late join, stream | `k_late_join_intervals` 3, `k_late_join_q` 4, `k_stream_marker_ratio` 0.35, `k_stream_timeout_frames` 8, `k_stream_tune_half` 0.75 |
| Preamble | `k_preamble_max_slots` 72, `k_train_nudge` 0.2, `k_fit_min_weight` 1, `k_fit_min_points` 3, `k_train_max_gap` 2, `k_sub_rate_gaps` 2, `k_sub_rate_ratio` 2, `k_train_amplitude_ratio` 0.5, `k_flip_amplitude_ratio` 0.3, `k_reference_alpha` 0.25, `k_faded_marker_q` 16, `k_faded_marker_ratio` 0.2, `k_faded_marker_offset` 0.05, `k_faded_marker_kappa` 0.8 |
| Header | `k_header_max_inner_flips` 1, `k_header_max_skip` 3, `k_header_search_end` L + 13, `k_header_min_peaks` 4, `k_header_peak_z` 8, `k_band_margin_hz` 100, `k_snap_reach_ms` 6, `k_snap_tolerance` 0.05, `k_header_carrier` 2 (grid seeding), `k_header_slot_ms_modulo` 8; ML: `k_header_bg_slots` 4, spread scale 1/0.2411, `k_header_side_noise_rank` 5, `k_header_noise_rank` 9, `k_header_side_ratio` 4, `k_header_carrier_level` 3, `k_header_carrier_steady` 0.3, `k_header_margin` 12, `k_header_agree` 6, carrier margin 2× and 1 slot more disagreement |
| Track | `k_phase_gain` 0.5, `k_drift_gain` 0.05, `k_max_drift` 1e-3, `k_frame_t_tolerance` 0.02, `k_frame_t_slots` 9, `k_afc_gain` 0.1, `k_afc_min_q` 8, `k_afc_clamp` 0.1 cycles/slot; rotation: `k_rotation_energy` 16, `k_rotation_min_rad` 0.6, `k_rotation_span` 0.383, `k_rotation_agreement` 0.5, `k_rotation_gain` 0.75, `k_rotation_min_slot_ms` 32, `k_rotation_balance` 0.25, `k_rotation_max_level` 1.5 |
| End, loss, audit | `k_end_evidence` 10, `k_short_eot_evidence` 15, `k_short_eot_min` 3, `k_short_eot_flips` 3, `k_short_eot_first` 2, `k_lost_absent` 3 of `k_presence_window` 4, `k_audit_low` −4, `k_audit_high` 8, `k_audit_threshold` 12, `k_audit_purity` 0.5, `k_audit_scale` 15, `k_strong_frames` 3; tune: `k_tune_kappa` −0.5, `k_tune_crest` 0.5, `k_tune_energy` 4, `k_tune_positions` 6 |
| Confirmation | `k_marker_edge_max` 0.3, `k_model_slot_ms` 32, `k_model_snr_db` −5.9, `k_confirm_margin_db` 5, `k_confirm_frames` 2, `k_guard_frames` 4, `k_guard_min_detected` 4, `k_guard_min_present` 3, `k_guard_released` 2, `k_eot_clean_slot` 3, `k_min_tone_changes` 5, `k_weak_stop_factor` 3 (× `k_q_track`), `k_presence` 0.5, `k_strong_presence` 0.75, `k_frame_buffers` 4, `k_max_held_frames` 2 |
| Slot path | `k_slot_blank_ratio` 4, `k_slot_blank_hold` 4, `k_slot_blank_reach` 16, `k_slot_blank_crest` 5, `k_slot_blank_delay` 20, `k_slot_blank_tail` 32, trigger run 64 (+2/−1), fast α 1/8, `k_slot_rms_samples` 800; bank clamp 100..3900 Hz; `k_bg_step_up` 22, `k_bg_step_down` 66, `k_bg_mean_offset` 460, `k_bg_warmup_slots` 32, `k_bg_warmup_factor` 4, `k_bg_min_bits` 3; `k_presence_factor` 2 (× H_M), `k_amp_floor` 0.05, `k_erasure_ratio` 2, `k_confidence_step_db` 0.5, `k_llr_q4_max` 7, `k_soft_scale` 16, `k_ln_i0_points` 33 over [0, 16], `k_noise_rank_divisor` 4 |
| Memory, bans | `k_mode_memory_ms` 60000, `k_mode_memory_hz` 10, `k_mode_memory_t` 0.03; `k_acquire_timeout_ms` 3000 / `k_acquire_timeout_slots` 40 (× T_max); `k_alias_ban_ms` 10000 (T ± 5%), `k_search_ban_ms` 10000 |
| v0.2b (not implemented) | `k_band_rel_db` 8, `k_band_floor` 1.25, `k_band_gap` 14, `k_band_edge` 3, `k_band_obs_s` 2 (4), `k_fold_ratio` 0.7 |

**Removed:** `k_rho_*`, `k_floor_sigma`, `k_gap_window`, `k_slot_window`, `k_g_slot`, `k_weak_margin`,
`k_loud_zero`, `k_guard_max_level`, `k_end_max_level`, the OOK marker-edge and zero-pair constants,
`k_default_fixed_ratio`, `k_timing_gain` in TRACK, `k_preamble_max_gap`. `k_header_bg_scale` (2.732, the design's
self-background factor) is still declared in `dsp.hpp` but no longer used: the header ML subtracts the per-tone floor
instead (§3.8 step 3; §11).

### 3.15 Memory and CPU

**Compile-time caps** (build-wide defines, `dsp.hpp`): `UNLIMITED_MAX_BITS_PER_PEAK` (default 8) and
`UNLIMITED_MAX_FRAME_BYTES` (default 32). RAM-tight MCUs (Cortex-M3, ESP8266) build with 7 and 16. A header over
the caps gives `lost(unsupported_mode)`.

**RAM.** The grid bank (10 B per bin int, 12 B float) and the per-bin background (2 B per bin) share storage with
FineAfc (1,044 B), which is idle in TRACK and reset on every ACQUIRE entry. The header bank (16 bins) is separate
because FineAfc runs in PREAMBLE.

`sizeof(Decoder)`, *measured* (xtensa-esp32 unless stated):

| Caps (k_max, frame bytes) | Int bank | Float bank |
|---|---|---|
| (6, 16) | 11,448 B | — |
| **(7, 16), MCU** | **11,960 B** | **12,248 B** (40 B under the gate) |
| (8, 32), default | 13,880 B | 14,424 B |
| (8, 32), host 64-bit | 13,888 B | — |

- **Gate:** `static_assert(sizeof(Decoder) <= 12288)` in `decoder.cpp` for every build with caps ≤ (7, 16);
  `check_embedded` compiles (7, 16) and (1, 1), each with both banks, on the host, xtensa and (when installed) ARM.
- **Event:** 40 B (`decoder_memory_budget` checks ≤ 40).
- **Encoder:** 157 B on AVR with the 64-byte queue, 164 B on xtensa and the 64-bit host. Gate (B5): on AVR
  `sizeof(Encoder) − k_queue_size ≤ 96` (a `static_assert`, so queues 16..128 build); no soft-float symbol in the
  linked `tx_uno`.
- **PacketReader:** 1,064 B on the PC (`UNLIMITED_PACKET_MAX` 1024).
- **Arduino builds (*measured*, `arduino_check`):** `tx_uno` 8,884 B flash, 547 B RAM (Uno); `rx_esp32` 341,656 B
  flash, 42,116 B RAM; `loopback_esp32` 335,428 / 66,884; `wav_sd_esp32` 347,510 / 23,712.

**CPU.** Bank bin-updates per second = max(M, 8)·8000·N/(N+1). Cycles per bin from compiled inner loops: ESP32
float ≈ 11, STM32F4 float ≈ 10, STM32F1 int ≈ 14, ESP8266 int split-16 ≈ 22 (**estimates, not run on hardware**).
The ESP32 column assumes the float bank at -O2; the Arduino examples build the int bank at -Os (not measured).

| k (M) | Bin-updates/s | ESP32 240 MHz | STM32F4 168 MHz | STM32F1 72 MHz | ESP8266 160 MHz |
|---|---|---|---|---|---|
| 3 (8) | 57 k | 0.3% | 0.3% | 1.1% | 0.8% |
| 4 (16) | 114 k | 0.5% | 0.7% | 2.2% | 1.6% |
| 5 (32) | 228 k | 1.0% | 1.4% | 4.4% | 3.1% |
| 6 (64) | 455 k | 2.1% | 2.7% | 8.8% | 6.3% |
| 7 (128) | 910 k | 4.2% | 5.4% | 18% | 12.5% |
| 8 (256) | 1.82 M | 8.3% | 10.8% | 35% | 25% |

- Add the v0.1 front end: < 3% on ESP32/F4; the ESP8266 soft-float ACQUIRE peak is ≈ 10–12% at 160 MHz.
- The tone search (49 int32 Goertzel bins per sample, 392 k bin-updates/s) runs in SEARCH, ACQUIRE and the PREAMBLE
  train (the watch), alongside the 16 header bins in PREAMBLE: comparable to a k = 6 grid bank.
- Negligible: per-slot LLRs (k·M compares + M table lookups) and the header ML (≈ 9k add/compare per hypothesis,
  ≤ 6 hypotheses per preamble).
- Recommended caps: k ≤ 8 on ESP32/F4; k ≤ 6 on F1/ESP8266. The `fm` profile (4-sample blocks) needs an FPU.
- AVR: encoder only (ISR cost §1.7).
- **PC (*measured*):** the decoder runs ≈ 4,000–4,300× real time (host clang -O2, the development Mac); the resampler
  ≥ 500× (gated, `resampler_speed`).

---

## 4. Expected and measured performance

SNR is key-down (PEP) tone power over noise in 2500 Hz unless stated. "Genie" = known timing and grid.
§4.1–§4.7 are the **design bench** (the synthesis prototypes on the real channel simulator: *measured*, with
**theory** columns where the noncoherent-MFSK formula applies); they set the gates. **§4.8 is the integrated decoder,
*measured*** by `make test_long` on the frozen tree.

### 4.1 AWGN, genie (α 0.25, G = 5/T, USB +37 Hz, ≥ 1.5e5 bits per point)

| Mode | BER at the listed SNRs (dB) | **SNR at 1e-3** | Theory | v0.1 OOK at the same T |
|---|---|---|---|---|
| T8 k3 c8/7 | −2: 8.2e-3 · −1: 2.1e-3 · 0: 4.6e-4 · 1: 6.6e-6 | **−0.5** | −0.53 | +2.9 |
| T16 k4 c8/7 | −5: 1.2e-2 · −4: 3.7e-3 · −3: 6.0e-4 · −2: 1.3e-4 | **−3.3** | −3.19 | −0.1 |
| T32 k5 c8/7 | −8: 1.9e-2 · −7: 5.8e-3 · −6: 1.25e-3 · −5: 1.2e-4 | **−5.9** | −5.88 | −3.2 |
| T64 k6 c8/7 | −11: 2.9e-2 · −10: 9.0e-3 · −9: 2.6e-3 · −8: 4.8e-4 | **−8.4** | −8.60 | −6.1 |
| T128 k7 c8/7 | −13: 1.4e-2 · −12: 3.8e-3 · −11: 4.0e-4 · −10: 2.6e-5 | **−11.4** | −11.34 | −9.2 |
| T16 k5 dense (not allowed since G2) | −4: 6.8e-3 · −3: 1.7e-3 · −2: 2.9e-4 | −2.7 | — | — |
| T32 k6 dense | −7: 9.3e-3 · −6: 2.2e-3 · −5: 2.3e-4 | −5.65 | −5.59 | — |
| T64 k7 dense | −10: 1.4e-2 · −9: 3.0e-3 · −8: 5.6e-4 | −8.35 | — | — |
| T128 k8 dense | −12: 6.0e-3 · −11: 9.6e-4 · −10: 5.2e-5 | −11.0 | −11.09 | — |

**Theory check.** SNR2500 = Eb/N0 + 10·log10(k) − 10·log10(2500·T·E_env), E_env = 0.84375 (α 0.25), 0.6875
(α 0.5); Eb/N0 at 1e-3 = 6.97 / 6.07 / 5.42 / 4.52 dB at M = 8 / 16 / 32 / 128. Predictions match the
measurements within 0.2 dB. Frequency-selective fading across the grid is not the dominant limit: floors follow
delay spread against T, and Doppler against spacing. (The bench used the uncorrected D33 labels; the correction
changes only which bits an adjacent-tone error hits, not the symbol error rate: AWGN BER is unaffected.)

**Other impairments:**
- **LSB** (pivot 3000 Hz): T32 k5 at −83 Hz −5.86 dB; T128 k7 at −61 Hz −11.35 dB. Both equal USB.
- **AGC 2/500 ms:** T32 k5 1.26e-3 at −6 dB, the same as AWGN.
- **QRN 20/s at +40 dB, without the slot blanker:** T32 k5 1.9e-3 at −6 dB, 0 errors at −4 and −2 dB; T8 k3
  floors at ≈ 4e-4 (5.8e-3 at 0 dB, 1.2e-3 at +3, 4.2e-4 at +6). B's ±0.5 ms slot blanker removed the T8 floor (0
  errors at +8.5 dB); the implemented blanker (§3.2) gives 9.1e-5 at +6 dB in C6′ (§4.8).
- **Sensitivities:** frequency error 0.1/T costs 0.14–0.43 dB, 0.2/T 0.5–1.4 dB; timing error 0.05T ≈ 0.4 dB,
  0.1T 0.7–1 dB.
- **In-grid interference:** a steady carrier on a grid tone at 0 dB relative to key-down gives BER 0.41 at 10 dB
  without background subtraction, 1.2e-2 with it (4.7e-2 at +6 dB); keyed CW (20 wpm) at 0 dB gives 0.145–0.16
  either way (needs erasures and FEC).

### 4.2 HF fading, genie (≥ 2e5 bits per point, α 0.25, 8/7, G5; BER at 10 / 15 / 20 / 30 dB)

| Mode | CCIR moderate (1 ms, 0.5 Hz) | CCIR poor (2 ms, 1 Hz) |
|---|---|---|
| T8 k3 | 1.4e-2 / 6.3e-3 / 3.7e-3 / 2.6e-3 | 2.6e-2 / 1.8e-2 / 1.6e-2 / 1.5e-2 (unusable: T < 10× delay spread) |
| T16 k4 | 6.1e-3 / 2.1e-3 / 8.1e-4 / 3.3e-4 | 7.8e-3 / 3.2e-3 / 2.4e-3 / 1.7e-3 |
| **T32 k5** | **3.0e-3 / 1.05e-3 / 2.4e-4 / 7.9e-5** | **3.6e-3 / 1.06e-3 / 4.5e-4 / 3.1e-4** |
| T64 k6 | 1.7e-3 / 6.3e-4 / 1.6e-4 / 6.4e-5 | 1.4e-3 / 5.6e-4 / 2.4e-4 / 1.6e-4 |
| T128 k7 | 9.7e-4 / 2.7e-4 / 2.2e-4 / 1.8e-4 | 1.1e-3 / 8.5e-4 / 6.9e-4 / 6.5e-4 |
| v0.1 OOK T32, real decoder, at 10/20/30 | 8.1e-3 / 4.5e-3 / 4.4e-3 | 2.0e-2 / 1.8e-2 / 1.6e-2 |
| v0.1 OOK T64, real decoder, at 10/20/30 | 1.8e-2 / 1.6e-2 / 1.5e-2 | 4.4e-2 / 4.3e-2 / 4.2e-2 |

T32 k5, other channels: CCIR good 9.5e-3 at 5 dB, 3.9e-3 at 10, 4.1e-4 at 20, 4.0e-5 at 30; flat Rayleigh 1 Hz
2.6e-3 at 10 dB, 3.3e-4 at 20, 5.0e-5 at 30 (v0.1: 1.05e-2 at 20, 9.9e-3 at 30). At the same T, v0.2 floors are
14–56× lower than v0.1 in moderate and 50–260× lower in poor.

**Spacing evidence (D30)** (genie, α 0.25, G = 5/T, BER at 10/15/20/30 dB):

| Channel | Mode | c = 8/7 | c = 1 |
|---|---|---|---|
| CCIR moderate | T16 k4 | 6.1e-3 / 2.1e-3 / 8.1e-4 / 3.3e-4 | 6.8e-3 / 2.2e-3 / 1.1e-3 / 7.5e-4 |
| CCIR moderate | T32 k5 | 3.0e-3 / 1.1e-3 / 2.4e-4 / 7.9e-5 | 3.0e-3 / 8.1e-4 / 4.4e-4 / 1.4e-4 |
| CCIR poor | T16 k4 | 7.8e-3 / 3.2e-3 / 2.4e-3 / 1.7e-3 | 8.4e-3 / 4.7e-3 / 3.1e-3 / 2.8e-3 |
| CCIR poor | T32 k5 | 3.6e-3 / 1.1e-3 / 4.5e-4 / 3.1e-4 | 3.4e-3 / 1.3e-3 / 8.0e-4 / 5.8e-4 |
| CCIR poor | T128 k7 | 1.1e-3 / 8.5e-4 / 6.9e-4 / 6.5e-4 | 1.6e-3 / 1.2e-3 / 9.4e-4 / 1.2e-3 |

### 4.3 NBFM (genie; pre-emphasis + de-emphasis 750 µs, 3 kHz deviation, 5 kHz limiter, f_ref 2650, grid below; CNR in 12.5 kHz = SNR − 7 dB)

| Mode | BER vs CNR | CNR at 1e-3 |
|---|---|---|
| fm_fast T6 k3 (444 bit/s) | 3: 1.8e-2 · 5: 8.8e-4 · 7: 0 | **4.9 dB** |
| fm T8 k3 (333 bit/s) | 1: 1.1e-2 · 3: 2.5e-4 · 5: 0 | **2.3 dB** |
| T16 k4 (222 bit/s) | −1: 3.4e-2 · 1: 2.3e-3 · 3: 1.3e-5 | 1.3 dB |
| v0.1 T16 OOK (55.6 bit/s) | 1.7e-3 at CNR 2 | — |

AM (v0.1, B): m = 0.8, 6.3e-4 at CNR 0 dB, 0 errors at ≥ 2 dB (OOK). v0.2 on AM: C11, *measured* in §4.8.

### 4.4 Blind chain (historical: v0.1b front end with `zeros_quiet` disabled, old OOK guard otherwise)

The design's pre-implementation estimate of the integrated chain, kept as the "before" baseline; the integrated
decoder (§4.8) replaced it. Header by ML, exact-T snap, streaming phase loop 0.5; 20 tx × 20 frames (12 at T128);
USB +37 Hz. Frames delivered / sent, with BER; "thr" is the genie SNR at 1e-3 from §4.1.

| Mode | thr + 1 | thr + 2 | thr + 3 | 10 … 50 dB |
|---|---|---|---|---|
| T8 k3 (am profile) | 41% | 90%, 0 errors | 90%, 0 | 100%, 0 errors, 0 alias LOSTs |
| T16 k4 | 75%, 4e-4 | 75%, 0 | 100%, 0 | 100% |
| T32 k5 | 75%, 2.5e-4 | 100%, 0 | 100%, 0 | 100% |
| T64 k6 | 80%, 0 | 100%, 0 | 100% | 100% |
| T128 k7 | 60%, 6e-4 | 95%, 1.6e-4 | 100% | 100% |

- LSB, −150 Hz: 95–100% at thr + 3 and 100% at 30 dB, orientation 100% correct.
- Losses near threshold were LOST(signal_gone) from the OOK guard; D41's confirmation replaced it.

Fading, same chain (frames delivered, BER), at 10 / 20 / 30 dB:

| Mode | CCIR moderate | CCIR poor |
|---|---|---|
| T16 k4 | 88% 4.0e-3 / 99.8% 5.2e-4 / 100% 1.6e-4 | 93% 7.1e-3 / 87% 2.0e-2 / 90% 1.6e-3 |
| T32 k5 | 96% 5.3e-3 / 89% 5.6e-4 / 87% 2.7e-3 | 93% 2.5e-3 / 96% 6.1e-4 / 77% 3.3e-4 |
| T64 k6 | 93% 1.5e-3 / 99.7% 1.7e-4 / 98.8% 3.5e-5 | 93% 1.5e-3 / 93% 3.4e-4 / 90% 1.9e-4 |
| T128 k7 | 73% 7.4e-4 / 86% 2.9e-4 / 89% 1.7e-2 | 79% 9.4e-4 / 76% 3.9e-4 / 80% 8.7e-4 |

### 4.5 Header Monte Carlo (design: square-law bins, self-background, 5e4 trials)

| Input | P(accept wrong / accept correct) |
|---|---|
| Noise only | 2e-5 |
| Carrier +10 / +20 dB on a header tone | 0 / 0 |
| Keyed CW +15 dB | 6.3e-3 |
| Chirp across the 8 tones, +10 / +20 dB | 4e-5 / 0 |
| Speech-like (3 drifting harmonics), +10 / +20 dB | 6.5e-3 / 1.2e-2 |
| Window misaligned by 1 slot, 10 / ≥ 20 dB | 3.2e-4 / 0 |
| k = 3 data frame read as a header, 20 dB (never evaluated by §3.8) | 4.3e-2 |
| True header, per-slot Es/N0 3 / 5 / 7 / 9 / 12 dB | 1.5% / 14% / 64% / **98.0%** / **100%** |
| True header, Rayleigh per slot, 15 / 20 dB | 97.8% / 99.9% |
| True header, per-tone selective fading, 15 / 20 dB | 94.3% / 98.8% |
| True header + carrier 11 dB above the peaks | 65% |

With the implemented floor-based ML (§3.8) U22 *measures* 98.89% detection at 9 dB and 1e-5 noise accepts; the
integrated C8′ detects 99.2% of headers under a +10 dB in-band carrier (§4.8). Alternatives rejected: 14 training
slots (99.3% at 9 dB, but PREAMBLE has only 1–3 train slots left after try_sync); A ≥ 7 (removes the speech-like
accepts but drops per-tone fading at 15 dB from 94% to 85%).

### 4.6 Late-join marker search (design, random join point, 40 runs per cell, ±200 Hz offset)

| Mode, N | v0.1 fast lock picks the marker | Band-edge union (≤ 4 candidates) contains the marker, at thr+0.5 / +2.5 / 20 / 40 dB |
|---|---|---|
| T16 k4, N8 | 0–3% | 95 / 95 / 98 / 100% |
| T32 k5, N8 | 0% | 93 / 95 / 100 / 100% |
| T64 k6, N8 | 0–13% | 73 / 95 / 100 / 100% |
| T128 k7, N8 | 3–15% | 53 / 98 / 100 / 100% |
| T32 k5, N16 | 0% | 55 / 85 / 95 / 90% |
| T64 k6, N16 | ≤ 8% | 30 / 83 / 100 / 100% |
| T32 k5, N32 (M = N) | 0% | 20 / 53 / 13 / 13% (fails) |
| T8 k3, N8 (M = N) | 0% | 0–8% (fails) |
| CCIR moderate / poor, 5–30 dB, N8, T16..128 | 0–20% | 85–100% |

Basis for "cold late join needs M ≥ 2N" and for mode memory as the core path.

### 4.7 Choosing N (computed effective rates; one packet per transmission; payload + 5 B framing; tail 100 ms)

| Mode | N | (N+1)T | Net (frames) | 16 B | 100 B | 255 B | 1024 B |
|---|---|---|---|---|---|---|---|
| hf_fast T16 k4 | 8 / 16 / 32 | 144 / 272 / 528 ms | 222 / 235 / 242 | 90 / 93 / 94 | 180 / 189 / 194 | 204 / 215 / 221 | 213 / 225 / 232 |
| hf T32 k5 | 8 / 16 / 32 | 288 / 544 / 1056 | 139 / 147 / 152 | 58 / 60 / 61 | 114 / 120 / 123 | 128 / 135 / 139 | 133 / 141 / 145 |
| hf_robust T64 k6 | 8 / 16 | 576 / 1088 | 83 / 88 | 29 / 30 | 64 / 67 | 75 / 79 | 80 / 84 |
| hf_weak T128 k7 | 8 | 1152 | 48.6 | 15 | 36 | 42 | 46 |

(Computed with the 5-byte framing of the original packet; the 16-bit LEN adds one byte, a < 1% change at ≥ 100 B.)

### 4.8 Integrated decoder (*measured*, `make test_long`, frozen tree, 2026-09-26)

159 PASS / 0 FAIL / 87 REPORT in 232 s (10 cores). One seed set per point; results are deterministic (identical on
rerun). USB offsets −50..+50 Hz unless stated. Full result lines: §9.

**AWGN at the A1′ gates** (BER ≤ 1e-3 and frames ≥ 95% gated):

| Row | Gate SNR (genie 1e-3 SNR) | BER | Frames | Locks |
|---|---|---|---|---|
| hf_fast | −1.5 dB (−3.3) | 1.95e-5 | 100.00% | 128/128 |
| hf | −4.5 (−5.9) | 1.07e-4 | 100.00% | 128/128 |
| hf_robust | −7.0 (−8.4) | 9.79e-6 | 99.56% | 128/128 |
| hf_weak | −9.5 (−11.4) | 3.45e-5 | 99.00% | 127/128 |
| fm, FM channel | CNR 4.5 (2.3) | 1.95e-5 | 100.00% | 128/128 |
| fm_fast, FM channel | CNR 7.0 (4.9) | 0 | 100.00% | 128/128 |
| T8 k3 centred, am profile, SSB (G1) | +1.5 (−0.5) | 9.77e-6 | 100.00% | 128/128 |
| hf N16 / hf N32 | −4.5 | 7.81e-5 / 1.17e-4 | 100.00% / 100.00% | 128/128 |
| hf_fast N32 / hf_robust N16 | −1.5 / −7.0 | 2.49e-5 / 4.92e-5 | 97.30% / 99.22% | 127 / 128 |
| hf grid above (f_ref 868) | −4.5 | 2.93e-5 | 100.00% | 128/128 |
| dense T32 k6 / T64 k7 / T128 k8 | −4.25 / −6.95 / −9.10 (derived) | 3.91e-5 / 2.93e-5 / 0 | 99.47 / 99.70 / 100.00% | 128/128 |
| fm preset, am profile, SSB (REPORT, G1) | +1.5 | 3.85e-4 | 11.40% | 15/128 |

Measured against theory: the gates sit 1.4–2.2 dB above the genie 1e-3 points and the BER there is 1e-5..1e-4, so
slot decisions are not the limit; acquisition is (A3′).

**Acquisition A3′** (400 transmissions at the gate, 300 at gate − 1 dB; locked, gated ≥ 99% / ≥ 90%): hf_fast
100 / 98.67%; hf 100 / 98.67%; hf_robust 99.25 / 98.67%; hf_weak 99.25 / 97.67%; fm (FM) 100 / 100%; fm_fast (FM)
100 / 100%; hf N16 100 / 99.33%; hf N32 99.00 / 98.00%; hf_fast N32 99.75 / 97.33%; hf_robust N16 99.25 / 98.33%;
grid above 99.75 / 98.00%; T8 k3 centred 99.75 / 97.67%; dense T32 k6 99.75 / 99.67%, T64 k7 99.75 / 98.67%, T128 k8
99.75 / 98.67%. 0 wrong-mode locks in every row. fm preset over SSB (REPORT): 12.25 / 2.00%.

**SNR report A4** (mean error, gate .. gate + 20 dB, gated ±1.5 dB): hf_fast −0.66..−0.50 dB; hf −0.18..+0.25;
hf_robust +0.12..+0.34; hf_weak +0.22..+0.44; T8 k3 centred (fm profile) −0.25..+0.16, (am profile) −1.01..−0.30.
fm preset over SSB (REPORT): −6.4..−1.8.

**A5 (integrated vs genie on the same audio, gate − 1 dB).** Integrated BER: hf_fast 3.27e-4, hf 6.01e-4,
hf_robust 2.73e-4, hf_weak 2.43e-4, hf N16 6.50e-4, hf N32 7.40e-4, hf_fast N32 3.87e-4 (REPORT, G4), hf_robust
N16 2.95e-4, grid above 5.52e-4, T8 k3 2.32e-4, dense T32 k6 7.04e-4, T64 k7 5.28e-4, T128 k8 1.21e-4. The
long-suite genie now labels tones with `peak_symbol` (fixed 2026-09-26). *Measured* ratios: hf_fast 1.14, hf 1.24,
hf_robust 1.17, **hf_weak 1.92**, hf N16 1.16, **hf N32 1.50**, hf_fast N32 1.45 (REPORT, G4), hf_robust N16 1.30,
**grid above 1.51**, T8 k3 centred 1.24, dense T32 k6 1.28, dense T64 k7 1.00, **dense T128 k8 2.27**. The four bold
rows FAIL the ≤ 1.5 gate: an implementation loss of about 0.3–0.5 dB against the genie, mostly at T = 128 ms, with every
absolute BER still below 1e-3 (open, §11). The unit test `decoder_a5_genie_short` passes.

**Channels** (hf unless stated):

| Test | Point | Measured | Gate |
|---|---|---|---|
| C1 CCIR good | 25 dB; 10–28 dB | BER 1.01e-4; loss 0% at every point | ≤ 1e-3; loss ≤ 2% |
| C2′ CCIR moderate | 20 dB | BER 4.84e-4, frames 99.89% | ≤ 1e-3, ≥ 97% |
| | hf_robust 20 dB | 1.92e-4, 99.63% | ≤ 5e-4 |
| | 10 / 30 dB; hf_fast, hf_weak, N16, hf_fast N32 at 20 dB (REPORT) | 3.31e-3 / 3.69e-5; 1.02e-3, 1.10e-4, 6.59e-4, 5.52e-4, frames ≥ 99.4% | — |
| C3′ CCIR poor | 20 dB | 5.43e-4, 99.96% | ≤ 2e-3, ≥ 93% |
| | 30 dB, 250 tx | 0 alias LOSTs | ≤ 1 per 50 |
| | hf_fast / hf_robust / hf_weak 20 dB (REPORT) | 2.58e-3 / 2.98e-4 / 6.50e-4; frames 98.5 / 98.1 / 95.1% | — |
| C4 flat Rayleigh 1 Hz | 30 dB | 6.91e-5 (genie 5e-5) | ≤ 1.5e-2 (v0.1 gate) |
| C5 QSB 20 dB at 0.2 Hz | 15 dB at the crest | 99.87% of bytes correct | ≥ 95% |
| C6′ QRN 20/s +40 dB, T8 k3 centred | +6 dB | 9.14e-5 (blanker off 3.47e-4); clean on/off ratio 1.00 | ≤ 1e-3; ≤ 1.1 |
| C7 AGC 1/300 ms | −4.5 dB | ratio to no AGC 0.85 | ≤ 2 |
| C8′ carrier on grid tone 3, 0 dB | 10 dB | 1.08e-2 (+6 dB carrier: 1.47e-2) | ≤ 2e-2 |
| | +10 dB carrier, random in the header band | header detected 99.2% (on a header tone: 100%) | ≥ 95% |
| C9′ keyed CW 20 WPM, −6 dB on a grid tone | 10–20 dB | 0 errors (REPORT; +3..+6 dB: H3) | report |
| C10 FM, hf_fast | CNR 6 / 8–14 dB, flat or pre-emphasised TX | 0 errors; 0 bytes lost at CNR 14 (fm, fm_fast too) | ≤ 1e-3; 0 in ≥ 1e4 bits |
| C11 AM m = 0.8 | CNR 2 dB (6 kHz IF) | 0 errors; hf_fast 0; fm preset 0% (REPORT, G1) | ≤ 1e-3 |
| C12 flutter 0.5 ms / 10 Hz, hf_fast | 30 dB | unmapped 0.37 per 1000; BER 1.62e-3, wrong 13.0 per 1000 (REPORT) | unmapped ≤ 1 per 1000 |
| C13 AGC + CCIR moderate | 20 dB | 3.02e-4, ratio 0.90 | ≤ 1e-3 and ≤ 2× |
| | 23 dB (REPORT) | frames 85.23%, 18/21 headers (spurious PREAMBLEs on data tones during the fades) | — |
| C14 sideband and shift in CCIR moderate, 20 dB | hf_fast ±380, hf ±500, hf_robust ±560, hf_weak ±590 Hz, USB and LSB (16 shifted rows) | every shifted row passes; frames 99.30–99.96%; LSB hf centred 3.12e-4, 99.93% | BER ≤ 2 × centred + 2e-4, frames ≥ centred − 3 points; LSB hf centred: the C2′ gate |
| C15 FM emphasis mismatch | hf_fast CNR 6–14 dB | 0 errors | C10 gates |

**C13 root cause** (fixed at the gated 20 dB point: 97.9% of frames before, 99.95% *measured* now): after a
transmission the receiver AGC raises the noise over ≈ 1 s, which the slow search floor (2.5 s) took for tones, so the
decoder was busy when the next tune came; under the tune the AGC lowers the noise by up to the SNR, so the frozen
noise read the markers weak; and each failed PREAMBLE restarted the ACQUIRE timeout. The recent floor, the
frozen-noise replacement and the timeout counted from `lock_tone()` (§3.3, §3.6, §3.7) fix these.

**Timing and integrity:**
- **L5** clock ±1000 ppm (TX, RX, both; N = 8 and 32), 10 min at T = 16 ms: 0 slips, 0 errors in all 8 runs.
- **F1–F4** (noise, drifting carrier, keyed CW, speech-like bursts; 30 min per profile): 0 `locked`, 0 bytes in every
  row; the 5 extra seeds × 30 min of F3/F4 (REPORT) also give 0 locks.
- **F5:** 0 CRC-valid wrong packets over F1–F4 and every C point (153 runs; 29,171 of 32,598 packets delivered).
- **F6:** longest wrong-byte run 4 at ≥ gate + 3 dB (≤ 8); 9 below it (REPORT).
- **F7:** header acceptances below the §4.5 Monte Carlo rates in every row (e.g. F4 speech, ssb: 22 in 5,191
  preambles against 249 expected).

---

## 5. Core public API (C++11, namespace `unlimited`) — FROZEN for v0.2

Rules for the core:
- no heap, exceptions, RTTI or STL; `float`, never `double` (AVR);
- no virtuals in `Encoder`, `Decoder`, `PacketReader` or the DSP blocks (ISR paths stay direct calls). Virtuals
  only at the driver boundary of `audio_io.hpp` and `wav_codec.hpp`, with protected non-virtual destructors;
- only `<stdint.h>`, `<stddef.h>`, `<math.h>`, `<string.h>` (and `<avr/pgmspace.h>` on AVR);
- the encoder is integer-only (AVR); the whole of `src/` still compiles for AVR.

The headers compile warning-free with `-std=c++11 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror` on
host clang, `avr-g++ -mmcu=atmega328p` and `xtensa-esp32-elf-g++` (`check_embedded`).

**The listings below are the frozen public headers**, generated from `src/unlimited.h` and `src/unlimited/*.hpp`
with only the `private:` sections removed (comments, includes and declarations verbatim). They were diffed against
the tree on 2026-09-26 and match exactly. Private members are implementation detail. Where a header comment
disagrees with §1–§4 (three comments, listed in §11), the numbered sections are normative.

```cpp
// src/unlimited.h
#pragma once

#include "unlimited/encoder.hpp"
#include "unlimited/decoder.hpp"
#include "unlimited/packet.hpp"
#include "unlimited/audio_io.hpp"
#include "unlimited/wav_codec.hpp"
```

```cpp
// src/unlimited/platform.hpp
#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(__AVR__)
#include <avr/pgmspace.h>
#define UNLIMITED_ROM PROGMEM
#else
#define UNLIMITED_ROM
#endif

namespace unlimited {

inline uint16_t rom_read_u16(const uint16_t* address) {
#if defined(__AVR__)
    return static_cast<uint16_t>(pgm_read_word(address));
#else
    return *address;
#endif
}

// Keeps the compiler from moving memory accesses across a hand-off to an ISR (single core).
inline void compiler_barrier() {
#if defined(__GNUC__)
    __asm__ __volatile__("" ::: "memory");
#endif
}

// Hand-off between a producer and a consumer that may run on different cores (a PC audio thread, a dual-core
// ESP32): stores before a release fence are seen by the other side before the store after it, and loads after an
// acquire fence see what was published before the load in front of it. On a single core (AVR) they emit nothing and
// act as compiler_barrier().
inline void release_fence() {
#if defined(__GNUC__)
    __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
}

inline void acquire_fence() {
#if defined(__GNUC__)
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
#endif
}

}  // namespace unlimited
```

```cpp
// src/unlimited/protocol.hpp
#pragma once

#include "unlimited/platform.hpp"

namespace unlimited {

// Transmission (spec 2.1): tune tone, sync train, an 8-slot mode header, then frames of a START marker and N
// data peaks; the STOP of a frame is the START of the next. N = k_min_data_slots << (header N code).
static const uint8_t k_min_sync_markers = 8;
static const uint8_t k_max_sync_markers = 32;
static const uint8_t k_eot_markers = 2;
static const uint8_t k_header_slots = 8;
static const uint8_t k_min_data_slots = 8;
static const uint8_t k_max_data_slots = 32;
// Defaults of EncoderConfig() and the presets.
static const uint8_t k_default_sync_markers = 8;
static const uint8_t k_default_data_slots = 8;
static const uint8_t k_max_bits_per_peak = 8;
static const uint8_t k_bits_per_byte = 8;

// Tone grid (spec 1.3): data tone n at f_ref + side * (k_grid_guard + n * c) / T, c = 8/7 (standard) or
// 1 (dense); header tone h at f_ref + side * (k_grid_guard + h * 8/7) / T in every mode.
static const uint8_t k_grid_guard = 5;
static const uint8_t k_standard_spacing_num = 8;  // 8 / (7 T): spectral zero of Tukey alpha 0.25
static const uint8_t k_standard_spacing_den = 7;

// Waveform, as fractions of the slot T; energies in T * A^2 / 2 units.
static const float k_tukey_ramp = 0.25f;  // markers and tune: Tukey alpha 0.5
static const float k_reversal_start = 0.375f;
static const float k_reversal_width = 0.25f;
static const float k_marker_energy = 0.5625f;
static const float k_data_ramp = 0.125f;  // data and header peaks: Tukey alpha 0.25
static const float k_peak_energy = 0.84375f;

// Speed is chosen by the sender only: T is a whole number of ms and (N + 1) T <= k_max_frame_us. A decoder
// accepts T in [T_min, k_speed_span * T_min].
static const uint32_t k_min_slot_us = 6000;  // the header needs 13 / T <= 2400 Hz
static const uint32_t k_max_slot_us = 128000;
static const uint32_t k_slot_quantum_us = 1000;
static const uint32_t k_max_frame_us = 1152000;
static const uint32_t k_min_dense_slot_us = 32000;  // G2: at T16 k5 dense the span leaves 150 Hz of SSB passband
static const uint8_t k_speed_span = 8;

// f_ref and every data and header tone lie in [k_min_tone_hz, k_max_tone_hz].
static const uint16_t k_min_tone_hz = 300;
static const uint16_t k_max_tone_hz = 2700;
static const uint16_t k_band_centre_hz = 1500;  // HF presets centre the occupied band here
static const uint16_t k_fm_tone_hz = 2650;

static const uint16_t k_default_tune_ms = 250;
static const uint16_t k_default_fm_lead_in_ms = 300;
static const uint16_t k_default_tail_ms = 100;

// Quarter-wave sine, 256 steps plus the end point, scaled to 65534 (twice the Q15 full scale): linear interpolation
// with one 16 x 16 multiply stays within 1 LSB of 32767 sin; phase is a full turn over 2^32.
static const uint8_t k_quarter_table_bits = 8;
static const uint16_t k_quarter_table_size = (1u << k_quarter_table_bits) + 1;
extern const uint16_t k_quarter_sine[k_quarter_table_size] UNLIMITED_ROM;

int16_t sine_q15(uint32_t phase);
int16_t cosine_q15(uint32_t phase);

enum class Spacing : uint8_t { standard, dense };  // 8 / (7 T) | 1 / T
enum class GridSide : uint8_t { above, below };    // as sent; USB/LSB inversion flips it at the receiver

// Mode header (spec 2.2): word = (k - 1) | (T_ms mod 8) << 3 | spacing << 6 | N code << 7, N code 0/1/2 =
// 8/16/32 data slots, 3 reserved. Sent as 8 tones, one per slot, of an RS(8,3) + x^3 coset code over GF(8).
static const uint16_t k_header_words = 512;
static const uint8_t k_header_slot_ms_modulo = 8;

struct HeaderFields {
    uint8_t bits_per_peak;   // 1..8
    uint8_t data_slots;      // 8, 16, 32; 0 = reserved N code
    uint8_t slot_ms_residue; // T_ms mod 8
    Spacing spacing;
};

uint16_t header_word(uint8_t bits_per_peak, uint8_t data_slots, uint32_t slot_us, Spacing spacing);
HeaderFields header_fields(uint16_t word);
uint8_t header_symbol(uint16_t word, uint8_t slot);  // tone 0..7 of header slot 0..7

// Data mapping (spec 1.5): slot i = 1..N of a frame carries symbol s on tone (gray(s) + (i - 1) r) mod M,
// M = 2^k, r = tone_rotation(k). `slot` below is i - 1.
uint8_t gray_encode(uint8_t value);
uint8_t gray_decode(uint8_t value);
uint8_t tone_rotation(uint8_t bits_per_peak);  // (M / 8) | 1
uint8_t peak_tone(uint8_t symbol, uint8_t slot, uint8_t bits_per_peak);
uint8_t peak_symbol(uint8_t tone, uint8_t slot, uint8_t bits_per_peak);

}  // namespace unlimited
```

The comment above `gray_encode` still describes the design's mapping (gray(s)); `peak_tone` implements §1.5
(gray⁻¹(s) + (i − 1)·r, H1).

```cpp
// src/unlimited/encoder.hpp
#pragma once

#include "unlimited/protocol.hpp"

// Queue size; like UNLIMITED_PACKET_MAX, only ever defined for the whole build (encoder.cpp must agree).
#ifndef UNLIMITED_ENCODER_QUEUE
#define UNLIMITED_ENCODER_QUEUE 64
#endif

namespace unlimited {

static const uint32_t k_min_sample_rate_hz = 8000;  // EncoderConfig::sample_rate_hz
static const uint32_t k_max_sample_rate_hz = 192000;

// Modes of spec 1.4; every preset has N = 8, standard spacing and the grid below f_ref.
enum class Preset : uint8_t {
    fm_fast,    // T 6 ms,   k 3, 444 bit/s,  f_ref 2650 Hz
    fm,         // T 8 ms,   k 3, 333 bit/s,  f_ref 2650 Hz
    hf_fast,    // T 16 ms,  k 4, 222 bit/s,  f_ref 2192 Hz
    hf,         // T 32 ms,  k 5, 139 bit/s,  f_ref 2132 Hz
    hf_robust,  // T 64 ms,  k 6, 83.3 bit/s, f_ref 2102 Hz
    hf_weak     // T 128 ms, k 7, 48.6 bit/s, f_ref 2087 Hz
};

// Why EncoderConfig::check() refuses a configuration.
enum class ConfigError : uint8_t {
    none,
    sample_rate,    // outside k_min_sample_rate_hz..k_max_sample_rate_hz
    tone,           // f_ref outside [k_min_tone_hz, k_max_tone_hz]
    slot,           // T not a whole number of ms in k_min_slot_us..k_max_slot_us
    bits_per_peak,  // k outside 1..8
    data_slots,     // N not 8, 16 or 32
    frame_length,   // (N + 1) T > k_max_frame_us
    spacing,        // not a Spacing value
    dense_slot,     // dense spacing needs T >= k_min_dense_slot_us
    side,           // not a GridSide value
    band,           // a data or header tone outside [k_min_tone_hz, k_max_tone_hz]
    queue,          // frame_bytes() > Encoder::k_queue_size / 2
    sync_markers,   // outside k_min_sync_markers..k_max_sync_markers
    amplitude       // not > 0
};

struct EncoderConfig {
    uint32_t sample_rate_hz;
    uint16_t tone_hz;        // f_ref: tune tone and markers
    uint32_t slot_us;        // whole ms, 6000..128000
    uint8_t bits_per_peak;   // k = 1..8
    uint8_t data_slots;      // N = 8, 16, 32
    Spacing spacing;
    GridSide side;
    int16_t amplitude;
    uint16_t lead_in_ms;
    uint16_t tune_ms;
    uint8_t sync_markers;
    uint16_t tail_ms;

    EncoderConfig();  // Preset::hf at 8000 Hz
    static EncoderConfig from_preset(Preset preset, uint32_t sample_rate_hz);
    // Integer only; the first rule broken, in ConfigError order (rate and T ranges also keep every tone 500 Hz
    // below rate / 2 and T >= 32 samples).
    ConfigError check() const;
    bool valid() const;  // check() == ConfigError::none
    uint8_t frame_bytes() const;  // N * k / 8
};

enum class EncoderSegment : uint8_t { idle, lead_in, tune, sync, header, frame, eot, tail };

enum class SlotKind : uint8_t { silent, tone, peak, marker };

// Read-only telemetry (TUI, full application). byte, slot, symbol and tone are 0 outside the header and frame
// segments.
struct EncoderStatus {
    EncoderSegment segment;
    SlotKind kind;               // kind of the slot being rendered
    uint8_t byte;                // frame segment: first byte of the frame being sent, else 0
    uint8_t slot;                // header 0..7, frame 0..N-1 peak slot; the STOP gives 8 (header) or N (frame)
    uint8_t symbol;              // k-bit value of the peak being rendered (header: 0..7)
    uint8_t tone;                // grid tone index of the peak being rendered
    uint32_t slot_index;         // slot being rendered, counted from 0 at start(), lead-in included; +1 per slot
    uint32_t samples_rendered;   // samples rendered since start()
};

// Threads and ISRs: one producer calls write(), queue_free(), queued(), busy() and, while idle, start(); one consumer
// (an ISR, or the audio thread) calls next_sample() or render(). The queue and start() hand over with release/acquire
// fences (platform.hpp), so the two may run on different cores. abort() and status() touch the consumer's state:
// call them from the consumer, or with it stopped (interrupts masked around the call).
class Encoder {
public:
    static const uint16_t k_queue_size = UNLIMITED_ENCODER_QUEUE;
    static_assert(k_queue_size >= 16 && k_queue_size <= 128 && (k_queue_size & (k_queue_size - 1)) == 0,
                  "queue size must be a power of two in 16..128");

    explicit Encoder(const EncoderConfig& config);

    bool start();
    bool write(uint8_t byte);
    size_t write(const uint8_t* data, size_t size);
    void abort();

    int16_t next_sample();                          // ISR-safe; 0 when idle
    size_t render(int16_t* out, size_t count);      // stops at the end of the transmission; returns samples written

    bool busy() const;                              // true from start() until the last tail sample is rendered
    size_t queue_free() const;                      // k_queue_size - queued()
    size_t queued() const;                          // bytes waiting, the frame being sent included
    uint32_t duration_samples(size_t data_bytes) const;
    EncoderStatus status() const;
    const EncoderConfig& config() const;
};

}  // namespace unlimited
```

```cpp
// src/unlimited/decoder.hpp
#pragma once

#include "unlimited/dsp.hpp"

namespace unlimited {

enum class Profile : uint8_t {
    ssb,  // HF USB or LSB: T = 16..128 ms, f_ref 300..2700 Hz
    am,   // AM: T = 8..64 ms, f_ref 300..2700 Hz
    fm    // VHF/UHF FM: T = 4..32 ms (a sender uses >= 6), f_ref 1000..2700 Hz
};

enum class DecoderState : uint8_t { search, acquire, preamble, track };

enum class EventType : uint8_t { state, locked, slot, byte, end, lost };

enum class LostReason : uint8_t { none, signal_gone, alias, preamble_timeout, reset, no_header, unsupported_mode };

enum EventFlag : uint8_t {
    event_flag_late_join = 0x01,
    event_flag_flywheel_start = 0x02,
    event_flag_flywheel_stop = 0x04,
    event_flag_blanked = 0x08,
    event_flag_erasure = 0x10,      // a slot of this byte had E_best < 2 E_second
    event_flag_mode_memory = 0x20,  // mode inherited from an earlier header
    event_flag_blind_mode = 0x40    // v0.2b: mode estimated from the data
};

struct Event {
    EventType type;
    LostReason reason;
    DecoderState state;           // new state for EventType::state, current state otherwise
    uint8_t flags;
    uint8_t value;                // byte: the byte; slot: the k-bit symbol
    uint8_t index;                // byte: 0..frame_bytes-1 in its frame; slot: 1..N
    uint8_t tone;                 // slot: grid tone index as received
    uint8_t level_pct;            // slot: winner amplitude, % of the interpolated START/STOP crest (<= 255)
    uint8_t confidence;           // slot: 10 log10(E_best / E_second), 0.5 dB steps (<= 255)
    int8_t soft[k_bits_per_byte]; // byte: bit LLRs MSB first; slot: soft[0..k-1]; > 0 means 1; 16 per nat, |soft| <= 112
    uint8_t bits_per_peak;        // mode, valid from locked to end/lost, else 0
    uint8_t data_slots;
    Spacing spacing;
    int8_t side;                  // +1 grid above f_ref as received, -1 below
    uint32_t frame_index;         // 0 = first data frame after the header
    float tone_hz;                // f_ref
    float slot_ms;                // exact T once the mode is known
    float snr_db;
};

static_assert(k_max_bits_per_peak <= k_bits_per_byte, "Event::soft holds the LLRs of one peak");

typedef void (*EventHandler)(const Event& event, void* context);

// The receiver chooses one thing: the T range it accepts (the tone search follows the radio path). A sender's mode is
// learnt from its header; T below 16 ms over SSB needs the am profile.
struct DecoderConfig {
    uint8_t min_slot_ms;   // accepted T = min_slot_ms .. 8 * min_slot_ms; block = min_slot_ms samples
    uint16_t min_tone_hz;  // f_ref search range
    uint16_t max_tone_hz;
    bool impulse_blanker;  // also the slot-path blanker

    DecoderConfig();  // for_profile(Profile::ssb)
    static DecoderConfig for_profile(Profile profile);  // fills the fields; the profile itself is not kept
    bool valid() const;
    uint16_t max_slot_ms() const;
};

class Decoder {
public:
    Decoder(const DecoderConfig& config, EventHandler handler, void* context);

    void reset();
    void process(const int16_t* samples, size_t count);
    void process_sample(int16_t sample);

    DecoderState state() const;
    float tone_hz() const;
    float slot_ms() const;
    float snr_db() const;
    uint8_t bits_per_peak() const;  // 0 until a mode is known
    uint8_t data_slots() const;
};

}  // namespace unlimited
```

```cpp
// src/unlimited/packet.hpp
#pragma once

#include "unlimited/decoder.hpp"

// Largest payload a packet may carry and PacketReader accepts. To change it, define it for the whole build
// (-DUNLIMITED_PACKET_MAX=N, on Arduino a build property), never in one source file: packet.cpp and every
// user of PacketReader must see the same value.
#ifndef UNLIMITED_PACKET_MAX
#if defined(__AVR__)
#define UNLIMITED_PACKET_MAX 256
#else
#define UNLIMITED_PACKET_MAX 1024
#endif
#endif

namespace unlimited {

// [0x2D 0xD4][LEN hi][LEN lo][payload, LEN bytes][CRC hi][CRC lo], LEN 1..k_packet_max_payload,
// CRC-16/CCITT-FALSE over the two LEN bytes and the payload.
static const uint8_t k_packet_sync_0 = 0x2D;
static const uint8_t k_packet_sync_1 = 0xD4;
static const uint8_t k_packet_header = 4;
static const uint8_t k_packet_crc = 2;
static const uint8_t k_packet_overhead = k_packet_header + k_packet_crc;
static_assert(UNLIMITED_PACKET_MAX >= 1 && UNLIMITED_PACKET_MAX <= 0xFFFF - k_packet_overhead,
              "UNLIMITED_PACKET_MAX must be 1..65529");
static const uint16_t k_packet_max_payload = UNLIMITED_PACKET_MAX;
static const uint16_t k_crc16_init = 0xFFFF;
static const uint16_t k_crc16_poly = 0x1021;
static const uint16_t k_crc16_check = 0x29B1;

uint16_t crc16_ccitt(const uint8_t* data, size_t size, uint16_t crc = k_crc16_init);

// Returns the packet size (size + k_packet_overhead), or 0 when size is 0, above k_packet_max_payload or
// out_size is too small. payload may overlap out (e.g. already sit at out + k_packet_header).
size_t packet_build(const uint8_t* payload, uint16_t size, uint8_t* out, size_t out_size);

// flags: OR of the event flags of the packet's bytes (after a rescan, also of the discarded bytes before it).
typedef void (*PacketHandler)(const uint8_t* payload, uint16_t size, uint8_t flags, void* context);

// Hunts for 0x2D 0xD4; LEN 0 or above k_packet_max_payload is rejected at once. After a CRC failure the
// buffered bytes are rescanned from the byte after the failed 0x2D, so no packet behind it is lost. end and lost
// events rescan the bytes behind a candidate still incomplete (e.g. a corrupted LEN), then reset it.
class PacketReader {
public:
    PacketReader(PacketHandler handler, void* context);

    void push(uint8_t byte, uint8_t flags);
    void on_event(const Event& event);
    void reset();
    uint32_t crc_errors() const;
};

}  // namespace unlimited
```

```cpp
// src/unlimited/audio_io.hpp
#pragma once

#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"

namespace unlimited {

// Driver boundary (D22). Protected non-virtual destructors: the core never deletes through
// these interfaces, so no operator delete is pulled in on MCUs.

class SampleSource {  // produces audio; read() returns 0 at the end
public:
    virtual size_t read(int16_t* out, size_t count) = 0;

protected:
    ~SampleSource() {}
};

class SampleSink {  // consumes audio
public:
    virtual void write(const int16_t* in, size_t count) = 0;

protected:
    ~SampleSink() {}
};

class AudioOutput {  // a device or file that plays a source
public:
    virtual bool start(SampleSource& source, uint32_t sample_rate_hz) = 0;  // driver pulls until read() == 0
    virtual bool wait() = 0;  // blocks until drained; true when all audio was delivered
    virtual void stop() = 0;

protected:
    ~AudioOutput() {}
};

class AudioInput {  // a device or file that captures into a sink
public:
    virtual bool start(SampleSink& sink, uint32_t sample_rate_hz) = 0;  // driver pushes as audio arrives
    virtual bool wait() = 0;  // blocks until the input ends (file) or stop() is called
    virtual void stop() = 0;

protected:
    ~AudioInput() {}
};

// The adapters are inline so that a program using only one of them links only that side of the core.

class EncoderSource : public SampleSource {  // adapts an Encoder; returns 0 once it is idle
public:
    explicit EncoderSource(Encoder& encoder) : encoder_(encoder) {}

    size_t read(int16_t* out, size_t count) override {
        return encoder_.render(out, count);
    }
};

class DecoderSink : public SampleSink {  // adapts a Decoder (input must already be 8 kHz)
public:
    explicit DecoderSink(Decoder& decoder) : decoder_(decoder) {}

    void write(const int16_t* in, size_t count) override {
        decoder_.process(in, count);
    }
};

}  // namespace unlimited
```

```cpp
// src/unlimited/wav_codec.hpp
#pragma once

#include "unlimited/audio_io.hpp"

namespace unlimited {

class ByteSink {
public:
    virtual bool write(const uint8_t* data, size_t size) = 0;
    virtual bool seek(uint32_t position) {  // optional; false when the sink cannot seek
        (void)position;
        return false;
    }

protected:
    ~ByteSink() {}
};

class ByteSource {
public:
    virtual size_t read(uint8_t* data, size_t size) = 0;  // 0 at the end
    virtual bool seek(uint32_t position) {                 // optional; needed only for a data chunk before fmt
        (void)position;
        return false;
    }

protected:
    ~ByteSource() {}
};

enum class WavEncoding : uint8_t { pcm, ieee_float };

struct WavFormat {
    uint32_t sample_rate_hz;
    uint16_t channels;
    uint16_t bits_per_sample;  // pcm 8/16/24/32, float 32
    WavEncoding encoding;
    uint32_t data_bytes;       // k_wav_unknown_size when streaming
};

static const uint16_t k_wav_header_bytes = 44;
static const uint32_t k_wav_unknown_size = 0xFFFFFFFFu;

bool wav_build_header(uint32_t sample_rate_hz, uint32_t total_samples, uint8_t (&header)[k_wav_header_bytes]);

class WavWriter : public SampleSink {  // 16-bit PCM mono
public:
    WavWriter();
    bool begin(ByteSink& sink, uint32_t sample_rate_hz, uint32_t total_samples = k_wav_unknown_size);
    void write(const int16_t* in, size_t count) override;
    bool finish();  // patches the sizes when the sink can seek
    bool ok() const;
    uint32_t samples_written() const;
};

class WavReader : public SampleSource {  // any supported format, downmixed to mono
public:
    static const uint8_t k_max_frame_bytes = 128;  // channels * container bytes

    WavReader();
    bool open(ByteSource& source);  // parses RIFF and fmt (incl. EXTENSIBLE), skips other chunks
    const WavFormat& format() const;
    size_t read(int16_t* out, size_t count) override;
    size_t read(float* out, size_t count);  // full precision, [-1, 1]
};

class WavOutput : public AudioOutput {  // "wav:" driver; runs synchronously inside start()
public:
    explicit WavOutput(ByteSink& sink, uint32_t total_samples = k_wav_unknown_size);
    bool start(SampleSource& source, uint32_t sample_rate_hz) override;
    bool wait() override;
    void stop() override;
};

}  // namespace unlimited
```

`WavReader`/`WavWriter` details: little-endian byte assembly only; malformed or truncated headers are rejected
(including a `bits_per_sample` whose container size would wrap, checked before narrowing); a data chunk longer than
the file is read up to what exists; odd chunk sizes are padded; 8-bit PCM is unsigned; conversion to int16 rounds
and clamps.

### 5.1 Behavioural contract

**Encoder** (queue and threads: §2.5):
- `next_sample()` is ISR-safe and returns 0 when idle; `render(out, count)` stops at the end of the transmission
  and returns the samples written.
- `start()` returns false if the encoder is busy, the queue is empty, or `!config.valid()`; it sets both NCO phases
  to 0.
- `write(byte)` returns false when `k_queue_size` bytes are queued; `write(data, size)` returns the bytes taken.
- `busy()` is true from `start()` until the last tail sample is rendered.
- `queued()` counts the bytes waiting, the frame being sent included; `queue_free()` = `k_queue_size − queued()`.
- `duration_samples(n)` gives the PTT hold time (§2.1 formula, short final frame included); 0 for an invalid
  configuration; saturates at 2³² − 1.
- `abort()` (consumer side) empties the queue and stops at once. It emits no EOT; the receiver reports LOST.
- `status()` (consumer side) is one consistent snapshot: `slot_index` counts slots from `start()` (lead-in included),
  `samples_rendered` the samples since `start()`. In the header and frame segments `slot` is the peak slot (0..7,
  0..d−1) and, during the STOP, 8 (header) or the frame's peak count d (N, or fewer in a short final frame);
  `symbol` and `tone` describe the peak being rendered (0 on a STOP); `byte` is the first byte of the frame being
  sent (frame segment only). All four are 0 in the other segments.
- `config()` returns the configuration the encoder was built with. There is no reconfigure call: build a new
  `Encoder` while idle (H6).

**EncoderConfig:**
- `EncoderConfig()` is the `hf` preset at 8000 Hz; `from_preset()` fills every field from §1.4 (f_ref, T, k,
  N = 8, standard spacing, side below, tune/sync/lead-in, −3 dBFS, tail 100 ms).
- `check()` is integer only and returns the first rule broken, in this order:

| `ConfigError` | Rule |
|---|---|
| `sample_rate` | `k_min_sample_rate_hz`..`k_max_sample_rate_hz` = 8000..192000 Hz |
| `tone` | f_ref in [300, 2700] Hz |
| `slot` | T a whole number of ms, 6000..128000 µs |
| `bits_per_peak` | k = 1..8 |
| `data_slots` | N ∈ {8, 16, 32} |
| `frame_length` | (N+1)·T ≤ 1152 ms |
| `spacing` | a `Spacing` value |
| `dense_slot` | dense spacing needs T ≥ `k_min_dense_slot_us` = 32 ms (G2) |
| `side` | a `GridSide` value |
| `band` | f_ref and every data and header tone in [300, 2700] Hz (exact rational comparison, no float) |
| `queue` | `frame_bytes()` ≤ `Encoder::k_queue_size/2` |
| `sync_markers` | 8..32 |
| `amplitude` | > 0 |

- The ranges alone keep every tone at least 500 Hz below rate/2 and every slot ≥ 32 samples (compile-time
  asserts, no runtime check).
- `valid()` is `check() == ConfigError::none`; `frame_bytes()` = N·k/8. Applications report the rule by mapping
  the enum (the demo does, `demo_cli_config_problem_names_the_rule`), never by copying the rules.

**Decoder:**
- Input is 8000 Hz int16. `process()` calls `process_sample()` for each sample; deterministic and bit-exact
  regardless of chunking (L2). The handler runs synchronously and must not re-enter the decoder. Use one decoder
  from one task, not an ISR.
- An invalid `DecoderConfig` leaves the decoder idle: it emits no event.
- `reset()` emits `lost(reset)` when in PREAMBLE or TRACK, then a `state` event if not in SEARCH; it forgets the
  history, the bans and the mode memory.
- `state()`: carrier detect for applications (DCD) is `state() != DecoderState::search`.
- `tone_hz()` is 0 in SEARCH, else f_ref (the NCO). `slot_ms()` is the exact T once a mode is known, the measured
  train T before that in PREAMBLE, else 0. `snr_db()` is 0 outside PREAMBLE and TRACK.
- `bits_per_peak()` and `data_slots()` are 0 until a mode is known (TRACK entry) and after `end`/`lost`/`reset()`.

**DecoderConfig:**
- `DecoderConfig()` = `for_profile(ssb)` = `{16, 300, 2700, true}`; `for_profile(am)` = `{8, 300, 2700, true}`;
  `for_profile(fm)` = `{4, 1000, 2700, true}` (fields `{min_slot_ms, min_tone_hz, max_tone_hz, impulse_blanker}`).
  The profile itself is not stored.
- `valid()` requires `min_slot_ms` 4..32 and 300 ≤ `min_tone_hz` < `max_tone_hz` ≤ 2700; if `min_slot_ms` < 8,
  `min_tone_hz` ≥ 1000. `max_slot_ms()` = 8·`min_slot_ms`.

**Events** (every event carries `state` (the current one), `frame_index`, `tone_hz`, `slot_ms`, `snr_db`, and the
mode fields while a mode is known):

| Event | Fields filled |
|---|---|
| `state` | state = the new state (emitted on every state change) |
| `locked` | flags (`late_join`, `mode_memory`, `blind_mode`), bits_per_peak, data_slots, spacing, side, frame_index (the first frame released), tone_hz, slot_ms (exact T), snr_db |
| `slot` | value (symbol), index (1..N), tone, level_pct, confidence, soft[0..k−1], flags (`erasure`, `blanked`), mode fields, frame_index of the frame being decided |
| `byte` | value, index, soft[8], flags (`late_join`, `flywheel_start`, `flywheel_stop`, `blanked`, `erasure`, `mode_memory`, `blind_mode`), mode fields, frame_index |
| `end` | mode fields (then cleared) |
| `lost` | reason (`signal_gone`, `alias`, `preamble_timeout`, `reset`, `no_header`, `unsupported_mode`), and the mode fields of the lock it ends |

- `slot` events are telemetry: they are emitted in TRACK as each slot is decided, before the `byte` events of
  their frame, and are never retracted (a frame later discarded by LOST, an unconfirmed lock or a truncation keeps
  its slot events; the slots of a short final frame after its last peak, i.e. its STOP and EOT positions, still give
  slot events until END is declared). Only `locked` and `byte` carry the data contract; false-lock tests count those.
- Byte events of one frame carry index 0..B−1 (B = N·k/8; fewer in a short or truncated frame). `soft[b]` is
  16·q4 (|soft| ≤ 112), its sign the decided bit.

**PacketReader:** `on_event()` forwards `byte` events, ignores `slot` events, and on `end` and `lost` rescans its
incomplete candidate and resets (§2.6).

### 5.2 API changes

**v0.1 → frozen v0.2:**

| Header | Removed | Added or changed |
|---|---|---|
| protocol.hpp | `k_bits_per_frame`, `k_slots_per_frame`, `k_sync_markers`, `k_one_energy`, `k_default_tone_hz`, `k_default_fm_tone_hz`, `k_quarter_sine_q15[65]` | `k_min_slot_us` 4000 → 6000; `k_quarter_table_bits` 6 → 8, `k_quarter_table_size` `uint16_t`, `k_quarter_sine[257]` (`uint16_t`, 65534 scale); added `k_min_sync_markers`, `k_max_sync_markers`, `k_header_slots`, `k_min_data_slots`, `k_max_data_slots`, `k_default_sync_markers`, `k_default_data_slots`, `k_max_bits_per_peak`, `k_bits_per_byte`, `k_grid_guard`, `k_standard_spacing_num/den`, `k_data_ramp`, `k_peak_energy`, `k_slot_quantum_us`, `k_max_frame_us`, `k_min_dense_slot_us`, `k_band_centre_hz`, `k_fm_tone_hz`, `k_header_words`, `k_header_slot_ms_modulo`; `Spacing`, `GridSide`, `HeaderFields`; `header_word`, `header_fields`, `header_symbol`, `gray_encode`, `gray_decode`, `tone_rotation`, `peak_tone`, `peak_symbol` |
| platform.hpp | `rom_read_i16` | `rom_read_u16`; `release_fence()`, `acquire_fence()` |
| encoder.hpp | `SlotKind::one`, `SlotKind::zero`; `EncoderStatus::bit_index` | `k_min_sample_rate_hz`, `k_max_sample_rate_hz`; `ConfigError`, `EncoderConfig::check()`; `EncoderConfig::bits_per_peak`, `data_slots`, `spacing`, `side`, `frame_bytes()`; `EncoderSegment::header`; `SlotKind::peak`; `EncoderStatus::slot`, `symbol`, `tone` (`byte` = first byte of the frame); `Encoder::queued()`; queue capacity exactly `k_queue_size` (asserted power of two 16..128); thread contract; `Preset` modes (§1.4) |
| decoder.hpp | `DecisionMode`; `DecoderConfig::profile`, `decision_mode`, `fixed_ratio`; `event_flag_weak`; `Event::level_pct[8]`, `threshold_pct[8]` | `EventType::slot`; `LostReason::no_header`, `unsupported_mode`; `event_flag_erasure` (0x10, replaces weak), `event_flag_mode_memory`, `event_flag_blind_mode`; `Event` fields `index`, `tone`, `level_pct` (scalar), `confidence`, `bits_per_peak`, `data_slots`, `spacing`, `side` (`soft[8]` kept, 16 per nat); `Decoder::bits_per_peak()`, `data_slots()` |
| packet.hpp | – | `end`/`lost` rescan an incomplete candidate before the reset |
| audio_io.hpp, wav_codec.hpp, unlimited.h | – | – (WavReader rejects a wrapping `bits_per_sample`) |

**First v0.2 headers** (the design listing of the "v0.2: multi-bit peaks" changelog row) **→ frozen v0.2:**

| Header | Removed | Added or changed |
|---|---|---|
| protocol.hpp | `k_sync_markers`; `k_quarter_sine_q15[65]` | `k_min_sync_markers`, `k_max_sync_markers`, `k_min_data_slots`, `k_default_sync_markers`, `k_min_dense_slot_us`; `k_quarter_sine[257]`, `k_quarter_table_bits` 8, `k_quarter_table_size` `uint16_t`; `peak_tone`/`peak_symbol` follow the corrected D33 (on-air change) |
| platform.hpp | `rom_read_i16` | `rom_read_u16`, `release_fence()`, `acquire_fence()` |
| encoder.hpp | – | `k_min_sample_rate_hz`, `k_max_sample_rate_hz`, `ConfigError`, `EncoderConfig::check()`, `Encoder::queued()`; capacity exactly `k_queue_size`; thread contract; `abort()` empties the queue |
| decoder.hpp | `DecoderConfig::profile` | `static_assert` that `Event::soft` holds one peak's LLRs |
| packet.hpp | – | `end`/`lost` rescan before the reset |
| dsp.hpp (internal) | – | see §5.3 |

### 5.3 Internal: `dsp.hpp` (not frozen)

`dsp.hpp` holds the decoder's building blocks. It is public only so that `decoder.hpp` can hold them by value and the
tests can reach them; it is **not** part of the frozen API and may change in any release as long as §8 holds. Only
three of its names are part of the contract: the build-wide caps `UNLIMITED_MAX_BITS_PER_PEAK` and
`UNLIMITED_MAX_FRAME_BYTES`, and `k_decoder_rate_hz` (8000). Changes since the first v0.2 listing:
`SlotBank::open(length, skip)`; `BinBackground::set_mean()`; `HeaderDecision::noise`, `background[8]`;
`HeaderMatch`, `match_header()`; `SlotDecision::noise` (crest now over Σw²); `window_noise()`; `FlipMeasure::steady`,
`energy`; `ToneSearch::interrupt()`, `recent_floor()`, `present()`, `long_run()`, `k_long_run_blocks`,
`train_onset(tone, min_products)`; constants `k_header_side_noise_rank`, `k_header_peak_z`, `k_bg_warmup_slots`,
`k_bg_warmup_factor`, `k_slot_blank_reach`, `k_slot_blank_crest`, `k_slot_blank_delay`. Current listing (private
sections removed):

```cpp
// src/unlimited/dsp.hpp
#pragma once

#include "unlimited/protocol.hpp"

// Compile-time decoder caps (spec 3.15), only ever defined for the whole build (dsp.cpp and decoder.cpp must
// agree). A header announcing more bits per peak or more bytes per frame is refused (lost(unsupported_mode)).
// RAM-tight MCUs (Cortex-M3, ESP8266) build with 7 and 16; spec 3.15 lists sizes and CPU.
#ifndef UNLIMITED_MAX_BITS_PER_PEAK
#define UNLIMITED_MAX_BITS_PER_PEAK 8
#endif
#ifndef UNLIMITED_MAX_FRAME_BYTES
#define UNLIMITED_MAX_FRAME_BYTES 32
#endif

namespace unlimited {

static_assert(UNLIMITED_MAX_BITS_PER_PEAK >= 1 && UNLIMITED_MAX_BITS_PER_PEAK <= k_max_bits_per_peak,
              "UNLIMITED_MAX_BITS_PER_PEAK must be 1..8");
static_assert(UNLIMITED_MAX_FRAME_BYTES >= 1 &&
                  UNLIMITED_MAX_FRAME_BYTES <= k_max_data_slots * k_max_bits_per_peak / k_bits_per_byte,
              "UNLIMITED_MAX_FRAME_BYTES must be 1..32");

static const uint32_t k_decoder_rate_hz = 8000;
static const uint8_t k_blocks_per_min_slot = 8;
static const uint8_t k_history_slots = 12;
static const uint16_t k_history_guard_cells = 32;
static const uint16_t k_history_cells =
    k_history_slots * k_speed_span * k_blocks_per_min_slot + k_history_guard_cells;
static const uint8_t k_mix_shift = 10;
static const uint16_t k_rebase_blocks = 4096;
static const uint8_t k_candidate_scales = 7;
static const uint8_t k_candidate_scale_blocks[k_candidate_scales] = {3, 4, 6, 8, 11, 16, 22};
// Detections of one marker at several scales agree within a block; distinct markers are at least T_min
// (8 blocks) apart. Merging within the T_min half-window keeps every marker of a T_min train.
static const float k_candidate_merge_blocks = 0.35f * k_blocks_per_min_slot;

static const float k_mixer_gain = 32.0f;          // Q15 table >> k_mix_shift
static const float k_cic_overlap = 1.0f / 3.0f;   // n_eff = (M - 1/3) * B for a window of M blocks
static const float k_g_marker = 0.8355f;          // mean |w r| over each 0.35 T marker half
static const float k_min_noise_variance = 1.0f / 12.0f;  // int16 quantization, per sample

// Slot path (spec 3.2, 3.10). Bank bins: 8 header tones x 2 sides in PREAMBLE, max(M, 8) grid tones in TRACK
// (k <= 2 still opens tones M..7, for the noise).
static const uint16_t k_header_bins = 2 * k_header_slots;
static const uint16_t k_max_grid_tones = 1u << UNLIMITED_MAX_BITS_PER_PEAK;
static const uint16_t k_min_grid_bins = 8;
static const uint16_t k_grid_bins = k_max_grid_tones > k_min_grid_bins ? k_max_grid_tones : k_min_grid_bins;
static const float k_peak_window_mean = 0.875f;        // sum of w / L for the Tukey alpha 0.25 window

static const float k_header_bg_scale = 2.732f;        // 1 / E[mean of the 4 smallest of 8 unit exponentials]
static const uint8_t k_header_bg_slots = 4;           // smallest slot energies per side and tone
static const uint8_t k_header_noise_rank = 9;         // N_h = 9th smallest of the 16 tone spreads (both sides)
static const uint8_t k_header_side_noise_rank = 5;    // a side's own noise: 5th smallest of its 8 tone spreads
static const float k_header_margin = 12.0f;           // S1 - S2, noise units
static const uint8_t k_header_agree = 6;              // of k_header_slots
static const float k_header_peak_z = 8.0f;            // a slot holds a peak: noise alone reaches it with p = 3e-3

static const int16_t k_bg_step_up = 22;               // log2 Q8.8; settles on the 25 % quantile
static const int16_t k_bg_step_down = 66;
static const int16_t k_bg_mean_offset = 460;          // 25 % quantile -> mean of an exponential
static const uint8_t k_bg_warmup_slots = 32;          // first slots after reset(): steps x k_bg_warmup_factor, so a
static const int16_t k_bg_warmup_factor = 4;          // carrier 30 dB over the noise is learnt within 32 slots
static const uint8_t k_bg_min_bits = 3;               // no background below k = 3 (noise from tones M..7)

static const float k_presence_factor = 2.0f;          // x H_M
static const float k_amp_floor = 0.05f;               // x N_bin
static const float k_erasure_ratio = 2.0f;            // z_best < 2 z_second (background-subtracted)
static const float k_confidence_step_db = 0.5f;
static const int8_t k_llr_q4_max = 7;                 // stored LLR, 1 nat per step
static const int8_t k_soft_scale = 16;                // Event.soft per nat: |soft| <= 112

static const uint8_t k_ln_i0_points = 33;             // ln I0 table over [0, k_ln_i0_max]
static const float k_ln_i0_max = 16.0f;

static const uint8_t k_slot_blank_ratio = 4;          // |x| > 4 RMS
static const uint8_t k_slot_blank_hold = 4;           // samples zeroed each side of a trigger
static const uint8_t k_slot_blank_reach = 16;         // an impulse's neighbours: 16 samples (2 ms) each side
static const uint8_t k_slot_blank_crest = 5;          // impulse: x^2 > 5 x the mean x^2 of its neighbours
static const uint8_t k_slot_blank_delay = k_slot_blank_reach + k_slot_blank_hold;  // output delay
static const uint16_t k_slot_rms_samples = 800;       // RMS EMA constant

static const uint8_t k_audit_scale = 15;              // audit evidence stored as int8 in 1/15 units

namespace dsp {

struct Complex {
    float re;
    float im;
};

class Nco {
public:
    Nco();
    void set_frequency(float hz);
    void adjust_frequency(float delta_hz);
    float frequency() const;
    void next(int16_t& cos_q15, int16_t& sin_q15);
};

class Cic2 {
public:
    Cic2();
    void reset();
    void push(int32_t re, int32_t im);
    void dump(uint8_t block_samples, int32_t& re, int32_t& im);
};

class QuantileTracker {
public:
    QuantileTracker();
    void reset(float initial);  // initial mean estimate; 0 = take the first input
    void push(float value);
    float mean_estimate() const;
    bool primed() const;
};

// Mean power of noise windows: a running mean for the first inputs, then an exponential average over about
// 64. A value above ten times the estimate counts as ten times, so an impulse the blanker missed moves it
// only a little. Unlike a quantile it stays unbiased when the noise is impulsive (FM clicks).
class NoiseTracker {
public:
    NoiseTracker();
    void reset(float initial);  // initial mean, weighted as a few inputs; 0 = take the first inputs
    void push(float value);
    float mean_estimate() const;
};

class ImpulseBlanker {
public:
    static const uint8_t k_latency = 2;  // push_block() for block k returns the decision for block k - k_latency

    ImpulseBlanker();
    void reset();
    bool push_block(float energy, float in_bin_energy);
};

class PrefixHistory {
public:
    PrefixHistory();
    void reset();  // forgets the contents; the block count continues
    void push(int32_t re, int32_t im, bool blanked);
    uint32_t end_block() const;  // one past the newest block
    bool window(uint32_t origin_block, float from, float to, Complex& sum) const;  // divided by k_mixer_gain
    bool any_blanked(uint32_t origin_block, float from, float to) const;
};

// Tone search (spec 3.6): 49 Goertzel bins 50 Hz apart over 160-sample blocks, with half-bin powers between them
// for the lock, a slow floor and a recent floor (the larger one gates locks), and the block-to-block and half-block
// phases for the tone estimate.
class ToneSearch {
public:
    static const uint8_t k_max_bins = 49;
    static const uint16_t k_block_samples = 160;
    static const uint8_t k_long_run_blocks = 8;  // 160 ms

    ToneSearch();
    void configure(uint16_t min_hz, uint16_t max_hz);  // also clears bans
    void reset();                                       // statistics only; bans and strikes stay
    void interrupt();                                   // samples were skipped: drop the block, end the lock's run
    bool push(int16_t sample);                          // true when a search block ended
    bool candidate(float& tone_hz) const;
    bool steady() const;                                // the lock sees a steady tone (a tune tone)
    float floor() const;                                // noise power per bin, same units as the bin powers
    float recent_floor() const;                         // the same over the last few blocks (follows level steps)
    float onset_floor() const;                          // floor before the tone of the current lock appeared
    bool masked(float tone_hz) const;                   // steady carrier
    bool present(float tone_hz) const;                  // in the fast average now, as strong as a fast lock needs
    void ban(float tone_hz, uint16_t base_blocks);      // base_blocks << min(strikes, 3)
    void exclude(float tone_hz, float sideband_hz);     // no lock near this tone or its train's lines
    void clear_exclusion();
    // A tone steady over at least min_products (>= 3) block products turned into a marker train.
    bool train_onset(float& tone_hz, uint8_t min_products) const;
    bool long_run() const;  // the lock has held k_long_run_blocks blocks: longer than a data peak (T <= 128 ms)
};

class FineAfc {
public:
    static const uint8_t k_bins = 65;

    FineAfc();
    void configure(float input_rate_hz);  // 125 Hz by default
    void reset();
    void push(const Complex& decimated);
    uint16_t inputs() const;
    bool offset(float& tone_offset_hz, bool wide) const;  // wide: +-30 Hz, otherwise +-16.7 Hz
    static float bin_hz(uint8_t bin);  // frequency of a bin on the squared signal
    static float bin_lambda(uint8_t bin);
};

struct Candidate {
    uint32_t block;
    float fraction;
    float q;
    uint8_t scale;
};

class CandidateList {
public:
    static const uint8_t k_size = 16;

    CandidateList();
    void reset();
    bool add(const Candidate& candidate);  // false when merged into a stronger newest entry
    uint8_t count() const;
    const Candidate& newest(uint8_t age) const;
};

// Alias audit (spec 3.11): 2N + 1 positions per frame, evidence clamped by the caller to [-4, 8] and stored as
// int8 in 1/k_audit_scale units. Positions are filled as their windows complete; next_frame() commits them.
class AuditRing {
public:
    static const uint8_t k_max_positions = 2 * k_max_data_slots + 1;
    static const uint8_t k_frames = 4;

    AuditRing();
    void reset(uint8_t positions);                 // clears; frames then have `positions` positions (2N + 1)
    void set(uint8_t position, float evidence);   // position 0..positions-1 of the frame being measured
    void next_frame();                             // commits the frame being measured and starts a clear one
    float max_evidence() const;                    // max over positions of the sum over committed frames
    uint8_t frames() const;                        // committed frames, at most k_frames
    uint8_t positions() const;
};

struct FlipMeasure {
    float q;
    float q_balanced;
    float kappa;
    float amplitude;      // |before - after| on the marker crest scale
    float steady;         // |before + after| on the same scale: a carrier continuous across the centre
    float energy;         // (|before|^2 + |after|^2) / (2 noise_energy): mean half-window energy over the noise
    float phase_step;
    float position;
    bool valid;
};

// Effective sample count of a window of `blocks` CIC-2 blocks (noise variance of its sum / sigma^2).
float noise_samples(float blocks, uint8_t block_samples);

// Flip statistics of the half-window sums before/after a centre (spec 3.4); noise_energy = sigma^2 * n_eff(W).
FlipMeasure flip_measure(const Complex& before, const Complex& after, float noise_energy, float half_samples);

// Vertex offset of the parabola through (-1, left), (0, centre), (1, right), clamped to +-0.5.
float parabolic_offset(float left, float centre, float right);

// Streaming Goertzel bank over one slot window on the raw 8 kHz input (spec 3.2), weighted by the matched
// Tukey alpha 0.25 envelope from a 32-bit slot-phase accumulator. No sample buffer: a window is opened at its
// predicted start and each sample is fed once; one window at a time. With UNLIMITED_BANK_FLOAT the state is
// float, otherwise int32 with Q14 coefficients and int64 products (10 B per bin). No constructor: call reset()
// first. Defined in dsp.cpp for HeaderBank and GridBank (one type when UNLIMITED_MAX_BITS_PER_PEAK is 4).
template <uint16_t Bins>
class SlotBank {
public:
    static const uint16_t k_max_bins = Bins;

    void reset();                                   // no bins, no window
    void set_bins(uint16_t count);                  // at most k_max_bins
    void set_frequency(uint16_t bin, float hz);     // between windows only; clamped to 100..3900 Hz
    uint16_t bins() const;
    // The next push() is the window's first sample, at u = (skip_samples + 0.5) / length. A window opened late
    // skips its first skip_samples (they count as zeros: the state stays 0), so it still ends where it was placed;
    // -0.5..0 places a window starting between two samples.
    void open(float length_samples, float skip_samples = 0.0f);
    bool push(int16_t sample);                      // true when this sample closed the window; idle: ignored
    bool active() const;
    float energy(uint16_t bin) const;               // |sum x w e^-jwn|^2 of the last window, input units
    float window_sum() const;                       // sum of w over the last window
    float window_square_sum() const;                // sum of w^2 over the last window
};

typedef SlotBank<k_header_bins> HeaderBank;  // PREAMBLE
typedef SlotBank<k_grid_bins> GridBank;      // TRACK

// Per-bin background of the grid slot energies (spec 3.10): a log2 Q8.8 tracker per bin, +k_bg_step_up above,
// -k_bg_step_down below, settling on the 25 % quantile. No constructor: call reset() first.
class BinBackground {
public:
    void reset(uint16_t bins, float initial_mean);
    void push(uint16_t bin, float energy);          // every slot pushes bins 0..bins-1 in order
    void set_mean(uint16_t bin, float mean);        // a known background, e.g. a carrier seen in the header
    float mean(uint16_t bin) const;                 // 2^((bg + k_bg_mean_offset) / 256)
    float noise() const;                            // N_bin: median over the bins of mean()
};

// Impulse blanker of the slot path (spec 3.2): r <- r + (min(x^2, 16 r) - r) / 800 over unblanked samples. A
// sample over 4 RMS is loud; a loud sample that is also peaky (x^2 > k_slot_blank_crest x the mean x^2 of the
// k_slot_blank_reach samples on each side) is an impulse. It zeroes itself and k_slot_blank_hold samples on each
// side, and so does every loud sample of its ringing for k_slot_blank_tail samples after it. A tone is not peaky (a
// sine's crest is 2 x its mean; 5 leaves room for the noise on it), so a peak much louder than r (a tilted channel, a
// fade up) is not blanked. Output is delayed by k_slot_blank_delay samples.
class SlotBlanker {
public:
    SlotBlanker();
    void reset();
    int16_t push(int16_t sample);                   // returns the sample k_slot_blank_delay samples earlier
    bool blanked() const;                           // the sample just returned was zeroed
};

// Header ML (spec 3.8). energy[d][j][h]: header slot j, tone h, on side d (0: grid above f_ref as received,
// 1: below). Self-background per side and tone, then S(w, d) over all 512 words and both sides.
struct HeaderDecision {
    uint16_t word;
    int8_t side;          // +1 above f_ref as received, -1 below
    float margin;         // S1 - S2, noise units
    float noise;          // N_h of the decided side, input units: seeds the grid background at TRACK entry
    float background[k_header_slots];  // floor of each tone on the decided side (steady part), input units
    uint8_t agreement;    // slots whose strongest tone is the codeword's
    bool accepted;        // margin >= k_header_margin and agreement >= k_header_agree (one less under a carrier)
};

HeaderDecision decide_header(const float (&energy)[2][k_header_slots][k_header_slots]);

// A known word (the mode memory's) against the header slots on one side (+1 above f_ref as received, -1 below).
struct HeaderMatch {
    uint8_t agreement;    // slots whose strongest tone is the word's
    uint8_t peaks;        // slots whose strongest tone reaches k_header_peak_z: the header window holds peaks
};

HeaderMatch match_header(const float (&energy)[2][k_header_slots][k_header_slots], uint16_t word, int8_t side);

// Slot decision (spec 3.10) from a closed bank window: background-subtracted argmax, max-log LLRs scaled by the
// slot's own amplitude, presence, erasure and telemetry. `slot` is i - 1 (rotation); bins as set for the grid.
struct SlotDecision {
    uint8_t tone;                          // strongest grid tone as received
    uint8_t symbol;                        // its k-bit label
    uint8_t confidence;                    // 10 log10(z_best / z_second) in 0.5 dB steps, <= 255
    int8_t soft[k_max_bits_per_peak];      // q4 LLRs, MSB first, > 0 means 1, |soft| <= k_llr_q4_max
    float crest;                           // 2 sqrt(max(z_best - N_bin, 0)) / sum w^2: the peak's crest, input units
    float noise;                           // N_bin of this decision, input units
    bool confident;                        // z_best / mean of the other z >= k_presence_factor H_M
    bool erasure;                          // z_best < k_erasure_ratio z_second
};

SlotDecision decide_slot(const GridBank& bank, const BinBackground& background, uint8_t bits_per_peak,
                         uint8_t slot);

// Mean noise energy per bin of a bank window that holds no peak (the STOP slot): the (bins / 4)-th smallest of the
// bins' energies over its expectation for exponential noise. A peak's matched window leaks -29..-37 dB into every
// other grid bin; a marker leaks below -40 dB into the grid, so this measures the noise up to per-slot Es/N0 of
// about 40 dB.
float window_noise(const GridBank& bank);

// ln I0(x): k_ln_i0_points table over [0, k_ln_i0_max] with linear interpolation; above, x - ln(2 pi x) / 2.
float ln_i0(float x);

// log2 Q8.8 of a positive value (clamped at the int16 range) and back.
int16_t log2_q8(float value);
float exp2_q8(int32_t log_q8);

}  // namespace dsp
}  // namespace unlimited
```

### 5.4 Freeze rules (v0.2)

- **Frozen:** every declaration listed in §5 outside §5.3 (names, signatures, types, enumerator order and values,
  struct field order, public constant values), the build-wide defines and their defaults (`UNLIMITED_ENCODER_QUEUE`
  64, `UNLIMITED_PACKET_MAX` 1024 / 256 on AVR, `UNLIMITED_MAX_BITS_PER_PEAK` 8, `UNLIMITED_MAX_FRAME_BYTES` 32,
  `UNLIMITED_BANK_FLOAT` off), the behavioural contract §5.1 and the on-air format §1–§2.
- **Free:** comments, private members, `dsp.hpp` and the decoder's internals and tuning (§3), provided every §8 gate
  still passes and the events keep §5.1.
- **Changing a frozen item** needs a changelog row here first, a new version (v0.3) and the `library.properties`
  version.
- **Deferred API** (H6), candidates for v0.3: `DecoderConfig::check()`; an abort request from the producer thread;
  an `Encoder` reconfigure call; moving the dsp constants into `namespace dsp`.

---

## 6. PC-only helpers, drivers, channel simulator and TUI

Location: `pc/`. Standard library only, no third-party code. Namespaces `unlimited::wav`, `unlimited::pc`,
`unlimited::sim`. PC code may use exceptions for unusable configurations (never the core). Not part of the frozen
core API; the declarations below describe the current code.

### 6.1 WAV file adapter (`pc/wav.hpp`)

```cpp
namespace unlimited { namespace wav {
class FileByteSink final : public ByteSink {       // not copyable
public:
    FileByteSink();
    ~FileByteSink();
    bool open(const std::string& path);  // creates or truncates
    bool close();                        // false when the open, a write or the close itself failed
    bool write(const std::uint8_t* data, std::size_t size) override;
    bool seek(std::uint32_t position) override;
};
class FileByteSource final : public ByteSource {   // not copyable
public:
    FileByteSource();
    ~FileByteSource();
    bool open(const std::string& path);
    bool close();                        // false when the open or a read failed
    std::size_t read(std::uint8_t* data, std::size_t size) override;
    bool seek(std::uint32_t position) override;
};
bool read_wav(const std::string& path, std::vector<float>& samples, std::uint32_t& sample_rate, std::string& error);
bool write_wav(const std::string& path, const std::vector<float>& samples, std::uint32_t sample_rate,
               std::string& error);
}}
```
Thin adapters over the core `WavReader` / `WavWriter` through `std::FILE*` (seekable). There is exactly one WAV
parser in the project: the core one.

### 6.2 Resampler (`pc/resampler.hpp`)

```cpp
namespace unlimited { namespace pc {
class Resampler {   // streaming, arbitrary ratio, windowed-sinc (Kaiser), anti-aliased when decimating
public:
    Resampler(double from_hz, double to_hz);   // throws std::invalid_argument unless both rates are finite and > 0
    void process(const float* in, std::size_t count, std::vector<float>& out);   // appends
    void flush(std::vector<float>& out);        // appends the rest, up to the end of the input, then starts over
};
std::vector<float> resample(const std::vector<float>& in, double from_hz, double to_hz);
}}
```
Feeds the 8 kHz decoder from any rate; also used by the channel's TX clock error. Pass-band ripple ≤ 0.1 dB to
0.45·min(rate), stop-band ≥ 60 dB; chunking-invariant; ≥ 500× real time (U19).

### 6.3 Audio drivers (`pc/audio.hpp`, D22)

```cpp
namespace unlimited { namespace pc {
static const std::size_t k_device_chunk_samples = 256;
class OutputDevice : public AudioOutput { public: virtual ~OutputDevice() {} };
class InputDevice : public AudioInput {
public:
    virtual ~InputDevice() {}
    virtual std::uint32_t sample_rate_hz() const = 0;   // the rate the device delivers; pass it to start()
};
std::unique_ptr<OutputDevice> open_output(const std::string& spec, std::string& error);
std::unique_ptr<InputDevice> open_input(const std::string& spec, std::string& error);
// MemoryOutput, MemoryInput, MemorySource (tests); ResamplingSink(SampleSink&, double from_hz,
// double to_hz = k_decoder_rate_hz) with flush()
}}
```
Device specs: `wav:<path>` (a bare path ending in `.wav` means the same) and `null`. The PC devices own a virtual
destructor (the core interfaces keep protected non-virtual ones). The demo wraps the decoder in a `ResamplingSink`
that converts to 8 kHz. Real-time backends (`alsa:`, `coreaudio:`, `wasapi:`) are the §12 modem's work.

### 6.4 Channel simulator (`pc/channel.hpp`, implemented and verified — D26)

The implemented API (`unlimited::sim::Mode {clean, usb, lsb, am, fm}`, `ChannelConfig`, `Channel`,
`fm_cnr_db()`, `FadingPreset`, `apply_preset()`) is normative; see the header comments for the physics:
- **SNR** = key-down tone RF power (usb/lsb) or carrier power (am/fm) over noise in 2500 Hz; `fm_cnr_db()` gives
  the CNR in the FM IF bandwidth.
- **usb/lsb:** analytic signal, fading, frequency offset, LSB inversion around `lsb_pivot_hz`, complex AWGN and
  impulses, one-sided receiver filter (300–2700 Hz default), optional AGC.
- **am/fm:** complex baseband at ≥ 48 kHz; AM envelope with overmodulation clipping and carrier AGC; FM with
  pre-emphasis, deviation limiter, splatter filter, IF filter, limiter-discriminator, de-emphasis.
- **Fading:** Watterson 2-path (Gaussian Doppler), single-path Rayleigh when `path_delay_ms = 0`.
- **Interferers and impairments:** Poisson QRN, steady carrier, keyed CW, QSB, TX clock error (ppm),
  `FadingPreset {none, flat, ccir_good, ccir_moderate, ccir_poor, flutter}` + `apply_preset()`.
- **Chunking invariant and deterministic** per seed and standard library (35 tests, `channel_*`).

### 6.5 Terminal and TUI (`pc/terminal.*`, `pc/tui.*`, D24)

- `terminal.hpp`: `is_tty`, `terminal_size` (80×24 when not a TTY), `enable_vt`, `restore_terminal_on_exit` (the
  only platform-specific PC file besides future audio backends).
- `tui.hpp`: `class Tui(TuiMode::encoder | decoder)` renders into a `std::string` frame buffer (testable without a
  terminal) and writes it with one `fwrite` per refresh; ANSI only; braille scope; refresh ≤ 20 Hz. Calls:
  `set_color`, `set_profile`, `set_tone_hz`, `set_slot_ms`, `set_mode(k, N, spacing, side)` (as sent), `set_field`,
  `push_audio`, `on_encoder_status`, `on_event`, `render(columns, rows)`, `draw`; `struct PeakSlot` (tone, symbol,
  level_pct, confidence, flags, present).
- **Panels:**
  - **Scope:** last ~2 slots of audio.
  - **Peaks:** a tone-by-slot grid of the header or frame, f_ref as the bottom row holding the START/STOP markers
    (or the tune tone); large M is bucketed onto the available rows. Encoder: header h0..h7 and frame slots, a caret
    on the slot being sent, title with symbol, tone index and frequency. Decoder: one column per `slot` event with
    level bars against the 100% START/STOP crest and the 70% line (a weaker glyph below 70%), erasures in yellow,
    tone index and confidence in the footer.
  - **Spectrum strip:** 300–3000 Hz power bar with f_ref marked and **the grid drawn** (tones f_0..f_{M−1} on the
    received side). The decoder view draws no header tones in PREAMBLE: no event carries the mode before `locked`.
  - **Status:** profile, state, f_ref, exact T, mode (k, N, spacing, side, `mode_memory`), bit rate, SNR, bytes,
    text so far, lock/lost/end counts, last lost reason.
- **Encoder contract:** `set_mode()` first, then `on_encoder_status()` at least once per slot: the sent text is
  rebuilt from the symbols of every peak, so short final frames are exact.
- `--realtime` paces file I/O to audio time with a sleep-until-deadline loop (no busy polling).
- Without `--tui` the demos print plain text lines (scriptable).

---

## 7. Layout, build, demos, examples

```
unlimited/
  spec.md  LICENSE (MIT)  library.properties  Makefile  compile_flags.txt  .gitignore
  src/unlimited.h                                   Arduino umbrella include
  src/unlimited/platform.hpp protocol.hpp tables.cpp  (tables.cpp: sine table, GF(8), Gray, header code, mapping)
  src/unlimited/encoder.hpp encoder.cpp
  src/unlimited/dsp.hpp dsp.cpp                       internal: front end, tone search, AFC, SlotBank, BinBackground,
                                                      SlotBlanker, header ML, slot decision, ln I0 table
  src/unlimited/decoder.hpp decoder.cpp
  src/unlimited/packet.hpp packet.cpp
  src/unlimited/audio_io.hpp
  src/unlimited/wav_codec.hpp wav_codec.cpp
  pc/wav.hpp/.cpp  pc/resampler.hpp/.cpp  pc/audio.hpp/.cpp  pc/channel.hpp/.cpp
  pc/terminal.hpp/.cpp  pc/tui.hpp/.cpp
  demo/cli.hpp  demo/unlimited_encode.cpp  demo/unlimited_decode.cpp
  tests/test_harness.hpp  tests/test_main.cpp  tests/test_*.cpp        unit + loopback suite (TEST / CHECK / CHECK_EQ /
                                                                       CHECK_NEAR / REQUIRE / NOTE), 213 tests
  tests/support/loopback.hpp/.cpp                                      shared encode/channel/decode helpers, scoring
  tests/long/regression.hpp  tests/long/regression_*.cpp               long A/C/F/L5 suites (make test_long)
  tests/embedded/heap_trap.cpp                                         heap-trap link test (check_embedded)
  tests/avr/isr_cases.hpp  isr_harness.cpp  isr_cycles.cpp             AVR ISR cycle gate (check_embedded)
  tools/gen_tables.cpp                                                 regenerates k_quarter_sine
  examples/arduino/tx_uno/tx_uno.ino                Timer2 8 kHz ISR → next_sample → Timer1 64 kHz PWM on pin 9 +
                                                    RC filter; PTT on pin 8; Serial lines sent as packets
  examples/arduino/rx_esp32/rx_esp32.ino            ADC DMA 24 kHz → decimate by 3 → Decoder → PacketReader → Serial
  examples/arduino/loopback_esp32/loopback_esp32.ino encoder → decoder in RAM at every preset, CPU load printed
  examples/arduino/wav_sd_esp32/wav_sd_esp32.ino    Encoder → WavWriter → SD card (core WAV codec on an MCU)
  README.md  docs/                                  §12.2 deliverables, not yet written
```

- Arduino compiles only `src/` recursively, so PC code lives in `pc/`.
- Headers are `.hpp`, included as `"unlimited/…"`. Code style: CamelCase types, snake_case functions, variables
  and files, trailing `_` for private members, `k_` named constants, `#pragma once`, 4-space indent, attached
  braces. No doxygen/docstring comments; short comments only where the math is not obvious.
- Build-wide defines (never per file): `UNLIMITED_ENCODER_QUEUE`, `UNLIMITED_PACKET_MAX`,
  `UNLIMITED_MAX_BITS_PER_PEAK`, `UNLIMITED_MAX_FRAME_BYTES`, `UNLIMITED_BANK_FLOAT`.

**library.properties:** name=Unlimited, author/maintainer Gustavo Campos, sentence "Robust tone-peak data modem for
HF, VHF and UHF radio audio.", category=Communication, url=https://github.com/solariun/unlimited, architectures=*,
includes=unlimited.h. `version` is 0.1.0 in the tree and becomes **0.2.0 at the v0.2 release** (§11).

**Makefile targets** (`make help` lists them):

| Target | What it does |
|---|---|
| `all` | lib + demos |
| `lib` | `build/libunlimited.a` (core) |
| `demo` | `bin/unlimited_encode`, `bin/unlimited_decode` |
| `test` | unit + loopback suite `bin/unlimited_tests` (213 tests, ≈ 28 s); `FILTER=` selects tests |
| `test_long` | A/C/F/L5 regressions `bin/unlimited_regression` (sources in `tests/long/`), ≈ 4 min on 10 cores; exit ≠ 0 on any FAIL row |
| `check_embedded` | (1) the core with `-std=c++11 -O2 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror`, then a forbidden-symbol scan (heap, exceptions, RTTI); (2) `tests/embedded/heap_trap.cpp` linked with the core and run (trapping `malloc`/`new`; L1′ cases); (3) the decoder variants float bank, (7, 16), (7, 16) + float, (1, 1), (1, 1) + float (the 12,288 B `static_assert` fires at (7, 16)); (4) `arm-none-eabi-g++` (Cortex-M4) and `xtensa-esp32-elf-g++` builds of the core and the variants when found (PATH, then arduino-cli's bundled toolchains); (5) AVR: the core for atmega328p, the encoder with queues 16 and 128, and the **ISR gate**: 7 cases (the six presets and T128 k8 dense at 2700 Hz, 100 ms lead-in, 40 bytes) built as `tx_uno` builds its ISR (`-Os -flto`), scanned for soft-float symbols, and run on the cycle-counting ATmega328P model of `tests/avr/isr_cycles.cpp`: samples identical to the host encoder, max ≤ 1,600 cycles, mean load ≤ 50%, no lost tick |
| `arduino_check` | `arduino-cli compile --warnings all` of `tx_uno` for `arduino:avr:uno` and every `*_esp32` example for `esp32:esp32:esp32`; fails on any warning from the library or a sketch; prints flash/RAM; fails if the linked `tx_uno` holds a soft-float routine |
| `demo_run` | encode → channel → decode round trips (below); the text must come back exactly |
| `tables` | checks `tables.cpp` against `tools/gen_tables.cpp` |
| `clean` | removes `build/` and `bin/` |

Common flags: `-std=c++11 -O2 -Wall -Wextra -Wpedantic -Werror`.

**`demo_run`** (text "CQ CQ DE UNLIMITED TEST 0123456789"; the decoder learns the mode from the header alone):

| Run | Channel | Receiver profile | Preset | SNR | TX rate | Offset | Framing |
|---|---|---|---|---|---|---|---|
| usb_hf | usb | ssb | hf | 10 dB | 8000 | +80 Hz | – |
| lsb_hf_fast | lsb | ssb | hf_fast | 10 dB | 48000 | −150 Hz (after the inversion) | – |
| am_hf_robust | am | am | hf_robust | 10 dB (carrier) | 8000 | +80 Hz | `--packet` |
| fm_fm | fm | fm | fm | 20 dB | 8000 | +80 Hz | – |
| fm_fm_fast | fm | fm | fm_fast | 20 dB | 8000 | +80 Hz | – |

**Demos:**

```
unlimited_encode (--text STR | --in FILE) [--out SPEC] [--packet]
    [--profile ssb|am|fm] [--preset hf|hf_fast|hf_robust|hf_weak|fm|fm_fast] [--slot-ms X | --baud B]
    [--bits-per-peak|-k K] [--data-slots|-N 8|16|32] [--spacing standard|dense] [--side above|below]
    [--tone HZ] [--rate 8000] [--level-dbfs -3] [--lead-in-ms N] [--tune-ms N] [--sync N]
    [--channel clean|usb|lsb|am|fm [--snr DB] [--offset HZ] [--pivot HZ]
        [--fading none|flat|good|moderate|poor|flutter] [--doppler HZ] [--qsb DEPTH_DB:RATE_HZ]
        [--impulses RATE[:LEVEL_DB]] [--carrier HZ:DB] [--cw HZ:DB:WPM] [--agc]
        [--fm-deviation HZ] [--no-preemphasis] [--no-deemphasis] [--clock-ppm P] [--seed N]
        [--clean-out SPEC]]
    [--tui] [--realtime]

unlimited_decode [--in SPEC] [--profile ssb|am|fm] [--min-slot-ms N] [--tone-range LO:HI]
    [--no-blanker] [--packet] [--events] [--expect FILE] [--tui] [--realtime]
```
- `--profile` on the encoder picks the default preset (ssb → hf, am → hf, fm → fm); `--preset`, `--slot-ms`,
  `--baud`, `--bits-per-peak` (alias `--bits`), `--data-slots` (alias `--slots`), `--spacing`, `--side`, `--tone`
  override. `--slot-ms` (and `--baud`) must give a whole number of ms. An invalid combination is a usage error
  (exit 2) naming the broken rule from `EncoderConfig::check()`.
- **f_ref placement for a changed mode** (no `--tone`): the preset family's rule is kept: FM presets stay at 2650 Hz
  with the grid below; everything else is centred on 1500 Hz (⌈1500 + W/2⌉ below, ⌊1500 − W/2⌋ above).
- The encoder prints the mode, bytes per frame, the tone 0 and tone M−1 frequencies, the band, key-down and
  average-power SNR (FM: the CNR), and a stderr note when the `--profile` receiver cannot decode the mode.
- `unlimited_decode` has **no** mode option (the header gives it); `--rule` and `--ratio` are removed with
  `DecisionMode`. It prints one `rx` line (f_ref, T, k, N, spacing, grid side as received, bit rate, SNR, slot
  count, mean confidence, erasures, flags) and one `text` line per reception. `--events` prints `slot` events too;
  `--expect` places each byte at frame_index·B + index and reports BER, loss, wrong bytes, locks, SNR, exact T and
  the decoded mode.
- `--out` / `--in` take a device spec (§6.3); default `tx.wav` / `rx.wav`. With `--channel`, the encoder writes
  the impaired audio to `--out` and optionally the clean TX audio to `--clean-out`. Exit codes: encoder 0 written,
  2 usage, 3 I/O; decoder 0 decoded (and matches `--expect`), 1 nothing decoded or no match, 2 usage, 3 I/O.

---

## 8. Test plan

**Test conventions:**
- Deterministic seeds (one seed set per long-suite point). SNR is key-down in 2500 Hz unless stated.
- "BER" counts released bits only; "loss" counts bytes not released; "frames" = data frames delivered / sent.
- Every gate reports the measured value. A failing gate is investigated, never relaxed silently; gate changes are
  decisions (§0.6).
- Gates marked *prov.* were set from the genie tables plus the blind margin and are refined only by tightening;
  *derived* gates (N16/N32, grid above, dense) take the gate of the same T plus the genie difference.
- REPORT rows print their value without a gate (statistics, design limits, decisions G1/G4/C12, F6 points).
- BER-only and ratio gates also require ≥ 50% of the frames delivered (a run that releases nothing cannot pass).
- Tests marked ′ replace their v0.1 version; v0.1 tests not listed as changed or removed stand as written.
- **Current status:** `make test` 213/213; `make test_long` 159 PASS / 0 FAIL / 87 REPORT; `check_embedded`,
  `arduino_check`, `demo_run` pass; unit tests clean under ASan + UBSan.

### 8.1 Unit tests (`make test`)

| # | Test functions | Pass criterion |
|---|---|---|
| U1 | `tables_quarter_sine_endpoints`, `tables_sine_within_one_lsb_on_grid`, `tables_sine_within_one_lsb_between_grid_points`, `tables_symmetry_and_cosine` | `k_quarter_sine` endpoints; `sine_q15`/`cosine_q15` ≤ 1 LSB against 32767·sin on and between table points; symmetry |
| U2 | `packet_crc_check_value`, `packet_every_single_bit_flip_is_detected` | "123456789" → 0x29B1; every single-bit flip of a 64-byte packet is detected |
| U3 | `packet_build_layout_and_limits`, `packet_reader_size`, `packet_round_trip_and_back_to_back`, `packet_sync_word_inside_payload`, `packet_crc_failure_resyncs_without_loss`, `packet_end_rescans_behind_a_corrupted_length`, `packet_rejects_bad_length_and_false_sync`, `packet_events_forward_flags_and_reset`, `packet_reader_without_handler`, `packet_modem_round_trip_ax25_and_max` | round trip; back-to-back; sync word inside payload; CRC failure resync without loss; an intact packet behind a corrupted LEN is delivered at `end`/`lost`; LEN = 0 and LEN > max rejected; flags OR'ed; slot events ignored; modem round trip with an AX.25-sized and a maximum packet |
| U4′ | `encoder_total_length_modes_frames_and_rates`, `encoder_total_length_odd_speeds_and_lead_in`, `encoder_slot_drift_over_ten_thousand_slots`, `encoder_duration_saturates` | total length = §2.1 formula ±1 sample for all presets, N = 16/32, dense, grid above and short final frames at 8000, 11025, 44100 and 48000 Hz; slot drift ≤ 1 sample over 10⁴ slots; `duration_samples` saturates |
| U5′ | `encoder_waveform_bounds_edges_and_signs`, `encoder_tune_ramps_and_flat_top` | \|y\| ≤ A; \|y\| ≤ A/256 at slot edges; each marker's half-correlation at f_ref < 0; every peak's > 0 at its grid tone; the marker sign persists across the header and frames; tune ramps and flat top |
| U6 | `encoder_spectrum_marker_not_wider_than_data` | marker −40 dB width ≤ 1.1× the data-peak width at T = 8, 32, 128 ms |
| U7′ | `encoder_energies_and_window_gains` | E_peak 0.84375, E_m 0.5625, g_m 0.8355, each ±0.5% |
| U8 | `dsp_nco_frequency`, `dsp_cic2_response` | NCO within 0.01 Hz; CIC-2 response = analytic sinc² ±0.1 dB; image rejection ≥ 25 dB at 800 Hz for B = 8 and 16 |
| U9 | `dsp_prefix_history_windows`, `dsp_prefix_history_wrap_and_soak`, `dsp_prefix_history_blank_bits` | fractional windows = brute force; correct after 2³² wrap; 24 h-equivalent soak bounded (< 1e-4 relative); blank bits |
| U10 | `dsp_quantile_tracker`, `dsp_noise_tracker` | quantile mean estimate ±5% on exponential noise after 10⁴ inputs; NoiseTracker unbiased on impulsive noise, bounded moves on one impulse, fast start from a far seed |
| U11 | ρ solver | **removed** |
| U12 | `dsp_flip_measure` | balanced flip κ > 0.9; continuous κ < −0.9; one-sided onset q_bal < 0 |
| U13 | `dsp_tone_search_floor`, `dsp_tone_search_between_bins`, `dsp_tone_search_weak_half_bin`, `dsp_tone_search_recent_floor`, `dsp_tone_search_steady_mask`, `dsp_tone_search_ban`, `dsp_tone_search_train_onset`, `dsp_tone_search_onset_floor`, `dsp_tone_search_exclude` | floor within ±10% of true noise; between-bin tones within ±5 Hz; an hf_weak-gate tune halfway between bins found within its 1.5 s on the right side; the recent floor follows a 20 dB AGC step; steady carrier masked after 2.6 s; ban back-off; train onset; onset floor; exclusion |
| U14 | `dsp_fine_afc`, `dsp_fine_afc_pull_in` | pure tone at 0 dB: offset within ±0.2 Hz; pull-in over the wide range |
| U15′ | `decoder_memory_budget`; compile-time asserts built by `check_embedded` | `sizeof(Event)` ≤ 40; `sizeof(Decoder)` ≤ 12288 at caps ≤ (7, 16); AVR `sizeof(Encoder) − k_queue_size` ≤ 96 |
| U16 | `wav_codec_*` (8 tests), `wav_*` (15 tests, through `pc/wav`) | as v0.1: exact RIFF bytes, known/patched/streaming sizes, PCM 8/16/24/32, float 32, EXTENSIBLE, downmix, unknown/odd chunks, malformed/truncated rejected, `bits_per_sample` 2049/4097/2080 rejected before narrowing |
| U17 | `audio_io_*` (5), `pc_audio_*` (8), `demo_io_decoder_sink_equals_direct_process` | `EncoderSource` returns exactly `duration_samples(n)` samples then 0; `DecoderSink` equals direct `process()`; `WavOutput` + memory sink readable; `pc::open_output/open_input` parse and reject specs; resampling sink; stop from a callback |
| U18′ | `decoder_profiles`, `decoder_invalid_config_is_idle` | `for_profile(ssb/am/fm)` values as in §5.1; `valid()` rejects out-of-range fields and `min_slot_ms` < 8 with `min_tone_hz` < 1000; an invalid config emits nothing |
| U19 | `resampler_*` (7) | 8000↔48000, 44100→8000, 11025→8000: gain within 0.1 dB, rejection ≥ 60 dB, chunking-invariant, ≥ 500× real time |
| U20′ | `tui_*` (20) | frames for encoder status and decoder slot events render into a string: fixed width/height respected, no line exceeds the terminal width, the 70% line, tone indices and grid marks where expected; the encoder view rebuilds the sent bytes; `terminal_size` falls back to 80×24 when not a TTY |
| U21 | `protocol_header_vectors`, `protocol_header_code_distance_and_repeats`, `protocol_header_code_misaligned_windows`, `protocol_header_fields_round_trip` | all 512 words: d_min 6, tone repeats ≤ 3, misaligned ±1 ≥ 3/7 and ±2 ≥ 2/6; §2.2 vectors exact; `header_fields` inverts `header_word`; N code 3 gives data_slots 0 |
| U22 | `dsp_header_decision_clean`, `dsp_header_decision_monte_carlo` | every vector word on both sides; Monte Carlo on square-law bins: noise ≤ 1e-4 wrong accepts; carrier +20 dB ≤ 1e-3; chirp ≤ 1e-4; misaligned at 20 dB ≤ 1e-4; detection ≥ 97% at 9 dB and ≥ 99.9% at 12 dB per-slot Es/N0; carrier leaking between header tones (+10 dB, random phase) ≥ 80% detected (*measured*: detection 98.89% at 9 dB and 100% at 12 dB; wrong accepts 1e-5 on noise, 5e-5 under a +20 dB carrier; 83.6% under the carrier leak) |
| U23 | `protocol_gray_code`, `protocol_rotation`, `protocol_mapping_vector`, `protocol_mapping_round_trip_and_neighbours` | round trip k 1..8 × slots 1..32, a bijection per slot; **cyclically neighbouring tones differ in exactly 1 label bit for every k** (hard check); §1.5 vector exact |
| U24 | `protocol_short_frame_packing`, `encoder_packing_all_modes` | N ∈ {8, 16, 32} × k 1..8; short frame gives ⌊⌈8n/k⌉·k/8⌋ = n for every n < B |
| U25 | `encoder_peak_frequencies` | every peak within 0.01 Hz of its exact grid frequency (header on the standard grid in dense modes), f_ref phase continuous, marker sign persists |
| U26 | `dsp_slot_bank` | int/Q14 against double energies within 1e-3; no overflow at full scale at T = 128 ms; window weights exact; late opening |
| U27 | `dsp_bin_background` | a steady carrier in one bin is learnt within 32 slots; N_bin is the median; AWGN errors with background ≤ 1.05 × plain argmax + 5 |
| U28 | `dsp_llr`, `dsp_llr_saturates_at_high_snr`, `dsp_slot_decision` | the LLR sign equals the decision; `ln_i0` error ≤ 0.02 nat absolute and ≤ 1% for x ≥ 3; a full-scale peak at T = 128 ms saturates every bit at ±7 (no float-to-int overflow); decision fields on a clean peak and on noise |
| U29 | `encoder_config_defaults_and_presets`, `encoder_config_valid_rejects_out_of_range`, `encoder_config_check_names_the_rule`, `demo_cli_config_problem_names_the_rule` | every preset valid; rejects non-ms T, T < 6 ms, (N+1)T > 1152 ms, dense T < 32 ms (T31 rejected, T32 accepted), grid or header outside 300–2700 Hz, frame bytes > queue/2, bad k, N, sync, amplitude; `check()` names the first rule; the demo maps every `ConfigError` |
| U30 | `dsp_slot_blanker` | a 20× RMS impulse zeroes exactly 9 samples; clean AWGN unchanged after the 20-sample delay; a tone starting after silence stops blanking within a few ms; Tukey-ramped 48-sample peaks 15 dB over the RMS are never blanked after the first |
| U31 | `encoder_queue_capacity`, `encoder_start_rules`, `encoder_segments_and_status`, `encoder_streaming_and_underrun_eot`, `encoder_short_final_frame_ends_transmission`, `encoder_abort_and_restart`, `encoder_render_chunk_invariance` | the queue holds exactly `k_queue_size` bytes (a producer keeps a whole frame queued behind the one sent); start rules; segment and status sequence (STOP slot = d); underrun → EOT; a short final frame ends the transmission and later bytes wait for `start()`; abort empties the queue and restarts cleanly; chunking-invariant render |
| U32 | `dsp_candidate_list_merge`, `dsp_audit_ring`, `dsp_impulse_blanker`, `dsp_log2_q8`, `demo_cli_numbers_and_names`, `demo_cli_mode_helpers`, `demo_cli_packetize_and_read_file`, `channel_*` (35) | building blocks, demo helpers (every HF preset's f_ref = ⌈1500 + W/2⌉) and the channel simulator's physics (§6.4) |

### 8.2 Loopback and behaviour (`make test`, `tests/test_decoder.cpp` unless stated)

| # | Test function | Pass criterion |
|---|---|---|
| L1′ | `decoder_l1_clean_loopback` | every preset + T ∈ {6, 12, 20, 37, 100, 128} ms × valid (k, N) (both spacings, both sides): 0 errors, exact byte count, `end`, exact T; modes over the build caps give `lost(unsupported_mode)` |
| L2 | `decoder_l2_chunk_invariance` | chunks of 1, 7, 160, 4096: identical event streams (bit-exact), slot events included |
| L3′ | `decoder_l3_mode_agnostic` | one decoder per profile, back-to-back transmissions with every valid mode in its range and f_ref low/centre/high: all decoded, no reconfiguration |
| L4′ | `decoder_l4_sideband_and_offset` | USB/LSB, side above and below, offsets up to each HF preset's tolerance: 0 errors at gate + 3 dB; orientation correct |
| L5 | `decoder_l5_clock_error` (30 s); long suite L5 (10 min) | clock ±1000 ppm on TX, RX, both at T = 16 ms: 0 slips (drift loop), 0 errors at gate + 3 dB |
| L6′ | `decoder_l6_sync_and_header_fades` | zero any 3 of the 8 sync markers, erase up to 2 header slots: all bytes; whole header erased: 0 bytes, or bytes flagged `mode_memory` when memory matches |
| L7 | `decoder_l7_alias_markers` | the STOPs of frames 0, 2, 4, 6 (odd markers) −30 dB: 0 wrong, 0 extra, 0 lost bytes |
| L8′ | `decoder_l8_flywheel` | 1 STOP in 5 zeroed: all bytes, flagged `flywheel_stop`; then 3 of 4 frames fully zeroed: `lost(signal_gone)` with no bytes after |
| L9 | `decoder_l9_end` | `end` ≤ 3T after the final STOP (full and short last frames), no bytes after; PTT cut with no EOT: `lost(signal_gone)` within **5** frames (L9 decision), 0 garbage bytes, no `end` |
| L10′ | `decoder_l10_mid_stream_with_memory` | mid-stream start of a station heard < 60 s before, every HF preset at 10 and 30 dB, 8 trials each: **8/8 late joins** with `late_join` + `mode_memory`, 0 wrong or extra bytes; hf_fast TRACK within 7 frames in ≥ 7 of 8 (L10′ decision); without memory: 0 bytes, no `locked` |
| L11 | `decoder_l11_back_to_back` | back-to-back transmissions with different modes and f_ref, 0.5 s gaps: all decoded |
| L12, L17 | `decoder_l12_profile_coverage`, `decoder_out_of_range_speeds` | each default profile decodes every preset listed for it in §1.4 (clean and gate + 3 dB), the `fm` profile included for hf_fast and hf; senders outside the range: no `locked`, no bytes |
| L13 | `demo_io_l13_rate_independence_hf`, `demo_io_l13_rate_independence_fm` (`test_demo_io.cpp`) | encoder at 8000, 11025, 22050, 44100, 48000 Hz → WAV → resample → decoder: 0 errors clean; the locked mode is the sent one |
| L14 | `make demo_run` | the 5 runs of §7 recover the text exactly; exit code 0 |
| L15 | `decoder_l15_short_final_frame` | hf_fast, T16 k3 N32, T64 k6 N16 and dense T32 k6 N8, n = 1..B−1 and n = B+1..2B−1: exactly n bytes, then `end` |
| L16 | `decoder_l16_unsupported_mode` | a header with N code 3 (or over the caps): `lost(unsupported_mode)`, 0 bytes |
| L18 | `decoder_event_fields` | event fields per §5.1 |
| R1 | `decoder_fade_relock_with_memory` | 5 frames faded out at 20 dB, every HF preset: relock with `late_join` + `mode_memory` within 10 frames, 0 wrong or extra bytes |
| R2 | `decoder_preamble_watch_ignores_train_lines` | 160 sample alignments (T 20 ms / 12 markers and T 8 ms / 32 markers): the watch never leaves a valid preamble for the train's own line |
| R3 | `decoder_saturated_input` | input clipped (×1.5..×4) at hf_robust, hf, hf_fast: 0 lost, wrong or extra bytes |
| R4 | `decoder_retry_after_a_lone_tune` | a tune heard alone, the station's retry 5–12 s later on the same f_ref: every retry byte, 0 wrong |
| R5 | `decoder_short_frame_under_the_next_tune` | a same-f_ref tune keying up 125–200 ms before the end: 0 wrong or extra bytes (at most the short frame dropped) |
| R6 | `decoder_frequency_step_in_track` | steps of +10 Hz (hf_robust), ±5 / −8.93 Hz (hf_weak), −20 Hz (hf), drifts 0.8 Hz/s (hf_robust) and 0.5 Hz/s (hf_weak): wrong bytes ≤ 3 frames' worth, 0 extra, 1 `end`, 0 `lost` |
| R7 | `decoder_weak_marker_sync_t8` | T8 k3 at +0.5 dB, 40 trials: ≥ 38 locked |
| R8 | `decoder_agc_back_to_back_high_snr` | 4 transmissions behind a 1/300 ms AGC at high SNR: 4 locks, 0 lost, wrong or extra bytes |
| R9 | `decoder_qrm_from_start` | a +6 dB steady carrier or 0 dB keyed CW outside the grid from the first sample: 1 lock, no loss; the interferer alone: no lock, no byte |
| R10 | `decoder_carrier_in_grid` | C8′ short: carrier on grid tone 3 at 0 dB, hf at 10 dB: 1 lock, ≥ 90% of bytes, BER ≤ 2e-2 |
| R11 | `decoder_fm_threshold_integrity` | fm and fm_fast near and below the FM threshold: no byte outside a confirmed lock, every lock at the sender's T |
| R12 | `decoder_a4_snr_report`, `decoder_a5_genie_short` | A4 short (±1.5 dB, gate..gate + 20); A5 short at the hf gate − 1: errors ≤ 1.5 × genie + 10 (genie through `peak_symbol`) |

### 8.3 AWGN regression (`make test_long`, ≥ 2·10⁵ bits per point; *measured* values in §4.8)

| # | Suite | Pass criterion |
|---|---|---|
| A1′ | `A1_awgn_integrated` | BER ≤ 1e-3 and ≥ 95% of frames at: hf_fast −1.5 · hf −4.5 · hf_robust −7.0 · hf_weak −9.5 · T8 k3 centred (f_ref 2313, am profile, SSB) +1.5 dB (G1) · FM channel: fm CNR 4.5, fm_fast CNR 7 dB *(prov.)*; derived rows N16/N32, grid above, dense T32 k6 −4.25, T64 k7 −6.95, T128 k8 −9.10. fm preset over SSB: REPORT (G1); dense T16 k5: not run (G2) |
| A2 | Fixed 0.70 | **removed** |
| A3′ | `A3_acquisition` | locked ≥ 99% of 400 transmissions at the A1′ gate, ≥ 90% of 300 at gate − 1 dB (G3), every A1′ row |
| A4 | `A4_snr_report` | mean SNR report within ±1.5 dB over gate..gate + 20 dB (presets on SSB, T8 k3 centred with the fm and am profiles) |
| A5 | `A5_genie_ratio` | integrated / genie BER ≤ 1.5 at gate − 1 dB on the same audio, frames ≥ 50%; hf_fast N32: REPORT (G4). **4 rows FAIL** (hf_weak 1.92, dense T128 k8 2.27, grid above 1.51, hf N32 1.50; §4.8, §11) |

### 8.4 Channels (`make test_long`; *measured* values in §4.8)

| # | Suite | Pass criterion |
|---|---|---|
| C1 | `C1_ccir_good` | hf: BER ≤ 1e-3 at 25 dB; loss ≤ 2% at 10, 15, 20, 28 dB |
| C2′ | `C2_ccir_moderate` | hf: BER ≤ 1e-3 and frames ≥ 97% at 20 dB; hf_robust BER ≤ 5e-4 *(prov.)*; other presets, N, 10/30 dB and the genie comparison: REPORT |
| C3′ | `C3_ccir_poor` | hf: BER ≤ 2e-3 and frames ≥ 93% at 20 dB; alias LOSTs ≤ 1 per 50 tx at 30 dB (250 tx) *(prov.)* |
| C4 | `C4_flat_rayleigh` | hf: BER ≤ 1.5e-2 at 30 dB (v0.1 gate; genie 5e-5; tightening to 5e-4 is open, §11) |
| C5 | `C5_qsb` | QSB 20 dB depth at 0.2 Hz, hf, 15 dB at the crest: ≥ 95% of bytes correct |
| C6′ | `C6_qrn_slot_blanker` | QRN 20/s at +40 dB, T8 k3 centred, am profile, with the slot blanker: BER ≤ 1e-3 at +6 dB; clean AWGN BER ratio blanker on/off ≤ 1.1 |
| C7 | `C7_agc` | AGC (1 ms / 300 ms): BER at the A1′ point ≤ 2× the no-AGC result |
| C8′ | `C8_carrier_in_grid` | steady carrier on grid tone 3 at 0 dB relative to key-down, hf at 10 dB: BER ≤ 2e-2; header detection ≥ 95% with a carrier +10 dB at a random frequency in the header band (C8′ decision; on a header tone: REPORT) |
| C9′ | `C9_cw_in_grid` | keyed CW inside the grid at −6 dB: report only (needs FEC) |
| C10 | `C10_fm` | FM, hf_fast (f_ref 2192) with flat TX + de-emphasis RX and with pre- + de-emphasis: BER ≤ 1e-3 at CNR 6 dB; 0 errors in ≥ 1e4 bits at CNR ≥ 8 dB; at CNR 14 dB 0 bytes lost for hf_fast, fm and fm_fast (blanker-deadlock regression) |
| C11 | `C11_am` | AM, m = 0.8, hf: BER ≤ 1e-3 at CNR 2 dB (6 kHz IF) |
| C12 | `C12_flutter` | flutter (0.5 ms / 10 Hz), hf_fast, 30 dB: BER and wrong bytes REPORT; **unmapped/extra bytes ≤ 1 per 1000 released** (C12 decision) |
| C13 | `C13_agc_fading` | AGC 1/300 ms + CCIR moderate, hf, 20 dB: BER ≤ 1e-3 and ≤ 2× the no-AGC result *(prov.)*; 23 dB REPORT |
| C14 | `C14_sideband_shift_fading` | USB and LSB, CCIR moderate 20 dB, each HF preset shifted to ±its tuning tolerance: BER ≤ 2 × centred + 2e-4, frames ≥ centred − 3 points and ≥ 50% *(prov., derived)*; LSB hf centred: the C2′ gate |
| C15 | `C15_fm_emphasis_mismatch` | FM flat TX / de-emphasis RX, hf_fast: the C10 gates at CNR 6–14 dB; fm and fm_fast rows REPORT |

### 8.5 Integrity and false locks (`make test_long`)

| # | Suite | Pass criterion |
|---|---|---|
| F1 | `F1_noise_false_lock` | 30 min of receiver noise (ssb: USB noise; am/fm: unmodulated carrier, 6 levels), each profile: 0 `locked`, 0 bytes |
| F2 | `F2_carrier_false_lock` | 30 min of noise + steady carrier at +20 dB with slow drift, each profile: 0 `locked` |
| F3 | `F3_cw_false_lock` | 30 min of noise + keyed CW, 12–30 WPM, random tones and levels, each profile: 0 `locked`; 5 more seeds × 30 min REPORT |
| F4 | `F4_speech_false_lock` | 30 min of noise + synthetic speech-shaped bursts (4 Hz syllabic), each profile: 0 `locked`; 5 more seeds × 30 min REPORT |
| F5 | `F5_crc_valid_wrong_packets` | everything in F1–F4 and all C points: 0 CRC-valid wrong packets |
| F6 | `F6_wrong_byte_runs` | all L5, A and C points at ≥ gate + 3 dB: no run of more than 8 consecutive wrong bytes (the alias signature); below gate + 3: REPORT |
| F7 | `F7_header_statistics` | header-accepted TRACK entries in F1–F4 (the decoder does not publish S1 − S2) against the §4.5 Monte Carlo rate × 4 hypotheses per preamble: report, alert above the 95% bound |

### 8.6 Late join v0.2b (`make test_long`, when v0.2b ships)

| # | Test | Pass criterion |
|---|---|---|
| V1 | Band-edge candidates, M ≥ 2N | contain the marker in ≥ 90% at gate + 3..40 dB and CCIR moderate/poor 10–30 dB |
| V2 | T/N fold | correct ≥ 99% |
| V3 | Blind mode | correct ≥ 95% within 4 frames; 0 CRC-valid wrong packets |

### 8.7 Build and embedded (`make check_embedded`, `make arduino_check`)

| # | Test | Pass criterion |
|---|---|---|
| B1 | Warning-clean builds | clean with the flags above on host clang; avr-g++ (whole of `src/`) and xtensa-esp32 (GCC); arm-none-eabi when installed (not installed on the reference machine); decoder variants: both `UNLIMITED_BANK_FLOAT` settings at caps (8, 32), (7, 16), (1, 1); encoder queues 16, 64, 128 on AVR; no heap, exception or RTTI symbol in the core |
| B2 | Heap trap (`tests/embedded/heap_trap.cpp`) | a link with trapping `malloc`/`new` decodes the six presets and T6 k2 N16, T12 k4 N32 above, T20 k1 N32, T20 k5 N16, T37 k6 N16 above, T100 k7 N8 dense, T128 k8 N8 dense above, a packet and a WAV round trip: 0 wrong, 0 extra, 1 lock and 1 end each (cases over the build caps are skipped) |
| B3 | Arduino (`arduino_check`) | `tx_uno` for uno and every `*_esp32` example compile with no library or sketch warning |
| B4′ | Speed | PC decode ≥ 500× real time (*measured* ≈ 4,000×; no automated gate, §11); ESP32 ≤ 6% at k = 7 (float bank) and STM32F4 ≤ 8%: not measured (no hardware); ESP8266 and F1 cycle counts go to the porting guide (§12.2) |
| B5 | AVR encoder | `sizeof(Encoder) − k_queue_size` ≤ 96 B (static_assert); no soft-float symbol in the 7 ISR images nor in the linked `tx_uno`; ISR on the ATmega328P model: output identical to the host encoder, max ≤ 1,600 cycles, mean load ≤ 50%, no lost 8 kHz tick |
| B6 | Sanitizers (manual) | the unit suite under ASan + UBSan: 213/213, no report |

---

## 9. Validation evidence

> The `scratchpad/...` paths below point to the development session's working area and are not part of the
> repository. The reproducible evidence in the repository is the test suite: `make test`, `make test_long`,
> `make check_embedded`, `make arduino_check` and `make demo_run`.

### 9.1 v0.2 integrated and frozen tree (2026-09-26)

- **Long suite:** `make test_long` on the frozen tree; its result lines (246 distinct) are in
  `scratchpad/spec_freeze_results.txt`, identical to the hardening's final run
  (`scratchpad/h_final/matrix/test_long.log`).
- **Full matrix** (clean, all, test, test_long, check_embedded, arduino_check, demo_run, ASan + UBSan):
  `scratchpad/h_final/matrix/`; sizes (`sz.cpp`) and bisection logs in `scratchpad/h_final/`.
- **Reports:** hardening `scratchpad/h_harden_report.md` (24 findings in `h_confirmed.json`); gate decisions and the
  acquisition work `scratchpad/h_resolved.md`; implementation `scratchpad/v02_impl.md`, `v02_rounds.md`,
  `v02_final_run.json`.
- **Last valid A5 ratios** (before H1): `scratchpad/h_rx/res_final3.txt`.

### 9.2 v0.2 design (in `scratchpad/mb_synthesis/`, built on the v0.1b snapshot, `clang++ -std=c++11 -O2`)

- `v01b/`: pristine v0.1b copy with `lib`, `pc/channel.o`, `resampler.o`, `loopback.o` built.
- `proto/`: judge 1's copy of C's prototype with `--guardT`: `bench.cpp` (genie), `blind.cpp` (v0.1b front end),
  `waterfall.cpp` (late-join search); `build.sh ../v01b ../build bench`; `build_nz.sh` builds `blind` against
  `v01b_nz` (zeros_quiet disabled).
- `hdr/`: `hdr_synth.cpp` (header Monte Carlo, `./hdr_synth 50000 0`), `perm_search.cpp` (slot order),
  `vectors.cpp` (§1.5/§2.2 vectors; the §1.5 tones there predate H1).
- Results: `out/ber/*.txt` (§4.1–4.3, `jobs_ber.txt`), `out/blind/*.txt` (§4.4, `jobs_blind.txt`),
  `out/wf4/*.txt` (§4.6, `jobs_wf4.txt`), `out/hdr_selfbg_50k.txt`, `out/hdr_final_100k_t14.txt`,
  `out/hdr_rules2_50k.txt` (§4.5), `out/vectors.txt`, `n_tradeoff.py` (§4.7).
- Proposals and judges: `mb_extend/` (A), `mb_throughput/` (B), `mb_embedded/` (C: streaming SlotBank, GF(8)
  header, AVR/ESP32 builds), `judges_v02.json`.

### 9.3 v0.1 evidence still applying (in `scratchpad/synthesis/`)

- Constants: E_m = 0.5625 (marker), g_m = 0.8355, quantile scale 3.476, uint32 wrap difference exact.
- Flip audit (balanced q_bal, 4-frame evidence, threshold 12; sample-level, real waveform, genie grid):

| Channel | False alarms per 4-frame group | 2T alias caught per group | 3T caught per group |
|---|---|---|---|
| AWGN, T = 32 ms | 0 / 2160 at every SNR from −6 to +20 dB | 0.99 at −2 dB, 1.00 at ≥ 0 dB | 0.93–1.0 at ≥ 0 dB |
| AWGN + 3 Hz offset, T = 16 ms | 0 / 7200 | 0.90 at 0 dB, 0.995 at +2 dB | 0.76–1.0 |
| Rayleigh 1 Hz, T = 32 ms | 0 / 7200 | 0.87 at 0 dB, 0.98 at +4, 1.0 at ≥ 12 dB | 0.75–0.95 |
| Rayleigh 1 Hz, T = 128 ms | 3 / 7200 at 20 dB (Rayleigh-null phase flips), 0 otherwise | 0.86–0.92 | 0.67–0.72 |

- STOP miss rates (q ≥ 4, κ ≥ 0.3): AWGN T = 32: 7.4e-2 at −4 dB, 3.1e-3 at −2, 0 at ≥ 0; Rayleigh 1 Hz T = 128:
  7.4e-2 even at 20 dB (coherence limit, hence Doppler ≤ 0.5 Hz at 128 ms).
- The v0.1 OOK decision-rule evidence (ρ solver, gap-noise estimator, OOK thresholds) is archived in the v0.1
  snapshot (`scratchpad/snapshot_v01b/spec.md` §9) and no longer applies.

---

## 10. Risks

| Risk | Mitigation / test |
|---|---|
| Acquisition is the sensitivity limit: the gates sit 1.4–2.2 dB above the genie 1e-3 points | A3′ met at 99% / 90% (G3); one seed set per point, several rows within one transmission in 400 of their gate (hf N32 exactly 99.00%) |
| The integrated decoder is 1.5–2.3× the genie BER in 4 A5 rows (~0.3–0.5 dB, mostly T = 128 ms) | §11 open item; absolute BER stays < 1e-3 at gate − 1 dB |
| H1 changed the on-air mapping: earlier v0.2 builds do not interoperate | version every release from the freeze on (`library.properties` 0.2.0) |
| AGC + fading at high SNR: C13 at 23 dB delivers 85% of frames (spurious PREAMBLEs on data tones during CCIR fades) | REPORT; decoder work (§13) |
| A frequency step at T < 32 ms (hf_fast ≥ ≈ 0.4 spacing, 28 Hz) loses the lock | H2; operating guide: no RIT/VFO changes during a transmission |
| In-grid interference: keyed CW +3..+6 dB on a grid tone (57–66% wrong bytes) | H3: FEC with erasures (§13); background subtraction handles steady carriers (C8′) |
| The ±50 Hz harmonic-image window of the watch hides a real new station there until the ACQUIRE timeout | rare; set levels with the tune tone (no clipping) |
| RAM headroom: the (7, 16) float bank is 40 B under the 12 KB gate | any decoder growth comes with a size check; ESP32 builds use the default caps |
| Not verified on hardware or on air; no ARM build; no thread-sanitizer stress of the cross-thread queue | §13; the AVR ISR model omits Timer0/USART interrupts (≈ 60–100 cycles each, inside the budget) |
| T = 128 ms needs stable paths | `hf_weak` for stable paths only (§1.4); C2′ hf_weak 20 dB *measured* 99.7% frames |
| T = 8 ms unusable on CCIR poor (25–32% of frames, delay spread) | presets and docs; the `fm` preset is FM-only (G1) |
| Finite tuning tolerance: ±380..590 Hz presets, ≤ ±170 Hz dense modes | band centred on 1500 Hz; C14 passes at the preset tolerances; operating guide |
| 100% duty (average 0.81 of PEP) on duty-limited rigs | −3 dBFS kept; reduce drive for long transmissions (§0.5) |
| Coherence at slow T: the phase signature needs Doppler·T < 0.25 | T_max = 128 ms; §9.3 |
| DC/2f image for low f_ref with small blocks | CIC-2; f_ref ≥ 1000 Hz when `min_slot_ms` < 8; HF presets have f_ref ≥ 2087 |
| ESP8266/F1 CPU at high k; MCU CPU not measured | caps (k ≤ 6 recommended); B4′ |
| ALC or compression flattens crests and reversals | tune tone to set levels; operating guide |
| Strong keyed QRM holds the search | bans only while the tone is present, exponential back-off; F3 |
| `dsp.hpp` names are visible through `decoder.hpp` | not part of the contract (§5.3, §5.4); `namespace dsp` move deferred |

---

## 11. Questions and open items

- **v0.1:** all design-panel questions were answered by Gustavo on 2026-09-25 (§0.3).
- **v0.2:** the seven open questions of the v0.2 design are answered by the defaults of §0.5 (2026-09-26); the gate
  and hardening decisions are in §0.6. Gustavo may override any of them.

**Resolved 2026-09-26 (before the first push):** the long-suite genie uses `peak_symbol`; the stale comments in
`protocol.hpp`, `decoder.hpp`, `encoder.hpp` and `tables.cpp` are corrected; the unused `k_header_bg_scale` is removed;
`library.properties` says `version=0.2.0`.

**Open items:**
1. **A5 implementation loss.** With a valid genie, 4 rows exceed the 1.5 ratio: hf_weak 1.92, dense T128 k8 2.27, grid
   above 1.51, hf N32 1.50 (§4.8). Likely residual timing/AFC error at long slots (§4.1: 0.1/T costs 0.14–0.43 dB).
   The gate is kept; the decoder is to be improved.
2. B4′ "PC decode ≥ 500× real time" has no automated test (only the resampler's speed is gated).
3. `README.md` and `docs/` do not exist yet (§12.2).

**Open decisions:** tighten C4 to BER ≤ 5e-4 (*measured* 6.9e-5; proposed by the long-suite work); the C13 23 dB
row; when to take the deferred API of H6.

---

## 12. Next phase: `unlimited_modem` and full documentation (requested 2026-09-25)

Gustavo's request, scheduled **after the core API is stable** (all §8 gates green), to be done in one go. That point
is reached (2026-09-26): the v0.2 API is frozen (§5) and every §8 gate passes except 4 A5 rows (implementation loss
of ~0.3–0.5 dB against the genie, §11). The
frozen API shapes the rows marked *(v0.2 API)*.

### 12.1 `unlimited_modem` — a real modem on local audio

| Requirement | Decision (to be refined when the phase starts) |
|---|---|
| List every local audio input and output | `unlimited_modem --list`: backend, id, name, direction, default flag, supported rates. |
| Pick devices and everything else from the command line | `--input <id\|name>`, `--output <id\|name>`, `--rate`, `--profile ssb\|am\|fm`, `--preset`, `--slot-ms`/`--baud`, `--bits`, `--slots`, `--spacing`, `--side`, `--tone`, `--level-dbfs`, decoder options as in `unlimited_decode` (no mode option: the receiver reads the header). |
| Encode and decode at the same time | TX: serial bytes → host FIFO → `Encoder` (SPSC queue) → output callback. RX: input callback → lock-free ring → worker thread woken by a condition variable (no polling) → `ResamplingSink` → `Decoder` → `PacketReader` → serial. *(v0.2 API)* The host thread is the encoder's only producer (`write()`, `queued()`, `queue_free()`, `busy()`, `start()` while idle) and the output callback its only consumer (`render()`); the release/acquire fences make this safe across cores (§2.5). `abort()` and `status()` run in the callback, or with it stopped (a producer-side abort request is deferred, H6). |
| Expose a serial port another app can use (a real modem / TNC) | POSIX: a PTY (`posix_openpt`), slave path printed, optional `--link <path>` symlink. Also `--serial <device>`. Windows: an existing COM port (virtual pair via com0com), documented. |
| Modem speed and serial speed are separate | `--preset` / `--slot-ms` / `--bits` set the **modem** speed. `--serial-baud` sets the **serial** speed, **default 115200**. *(v0.2 API)* A speed change builds a new `Encoder` while the current one is idle (no reconfigure call, H6); the configuration is checked with `EncoderConfig::check()` and a refusal names its `ConfigError`. |
| Default modem speed | **Preset `hf`: T = 32 ms, k = 5, N = 8, 139 bit/s net** for `ssb` and `am`; preset `fm`: T = 8 ms, k = 3, 333 bit/s net, for the `fm` profile. |
| **Serial protocol: KISS** | FEND 0xC0 / FESC 0xDB / TFEND 0xDC / TFESC 0xDD; command byte = port << 4 \| cmd; cmd 0 = data frame. One KISS data frame = one Unlimited packet (CRC-16, §2.6); only CRC-valid packets are returned to the host. Compatible with KISS clients such as AX25Toolkit (`ax25tnc`, `bbs`), linbpq and Dire Wolf clients. Conventions follow AX25Toolkit `kiss_modem`: PTY with `--link` symlink (default `/tmp/unlimited`), optional KISS-over-TCP port, `--monitor`, `--loopback` self-test. KISS `TxDelay`/`P`/`SlotTime`/`TxTail`/`FullDuplex` are accepted and stored, but the CLI flags are the timing authority. |
| Packet size | Done during v0.1 stabilization: LEN is 16-bit, `UNLIMITED_PACKET_MAX` PC 1024 (AX.25 frames in KISS reach ~330 bytes). *(v0.2 API)* A KISS frame above `k_packet_max_payload` is refused (`packet_build()` returns 0) and reported. |
| Channel access | CSMA with p-persistence and slot time; DCD = decoder not in SEARCH; post-RX holdoff (DWAIT). *(v0.2 API)* DCD is `Decoder::state() != DecoderState::search`, mirrored from the `state` events into an atomic that the TX side reads (the `Decoder` stays on its worker thread). |
| Rate mismatch (serial ≫ modem) | Bounded host TX buffer, overflow reported; RTS/CTS when a real serial port supports it. |
| Radio control | `--ptt none\|rts\|dtr` on a serial device (VOX works with `none`; the tune tone keys VOX). Half-duplex by default; `--full-duplex` for loopback tests. |
| Audio backends (D22) | New `AudioOutput`/`AudioInput` implementations plus device enumeration: CoreAudio (macOS), ALSA (Linux), WinMM or WASAPI (Windows). One file per backend, platform-guarded. *(v0.2 API)* They derive from `pc::OutputDevice` / `pc::InputDevice` (§6.3) and are returned by `pc::open_output()` / `open_input()`. |

### 12.2 Full documentation

Everything about the project, in `docs/` (English), rendered on GitHub:
- protocol: waveform math, slot kinds, tone grid, header code, frame and transmission format, timing, levels,
  presets, profiles;
- design: why each decision (D1–D49), alternatives rejected and their measurements;
- decoder: every stage, state machine, formulas, thresholds, corner cases (fades, missed/false markers, aliases,
  header loss, mode memory, late join, AGC, QRN, carriers in the grid, FM clicks, clock error, LSB/USB inversion,
  mistuning, end of transmission, lost signal);
- diagrams: Mermaid (state machine, pipelines, layering) and SVG;
- **signal images generated from the real encoder/decoder** (`tools/plot_signals.cpp` → `docs/images/*.svg`,
  `make docs_images`): each slot kind, envelopes, phase reversal, the header, a full transmission, spectra and
  waterfall of the grid, channel impairments, decoder views, BER curves;
- API reference, audio I/O and drivers, channel simulator, TUI, testing and results, porting to MCUs (caps, bank
  variant, cycle counts), operating guide (levels, ALC, VOX, PTT, duty cycle and drive for 100% duty), modem usage.

---

## 13. Roadmap (out of scope for v0.2)

| Item | Notes |
|---|---|
| v0.2b cold late join | §3.13; tests V1–V3. |
| FEC + interleaving across frames | Consumes byte `soft[8]` and `erasure` flags; N code 3 reserved as its header escape (D49). Keyed CW inside the grid (C9′, H3) needs it, with a per-bin CW tracker (≥ 64–128 B RAM). |
| Header repetition | B's two-copy header (99.98% at per-slot Es/N0 7 dB) as an option for weak or coded modes. |
| Full application | Built on the same events and audio interfaces and on `unlimited_modem`. |
| AVR decoder | Fixed-point variant of the block-rate math, if ever needed. |
| Deferred API (H6), v0.3 candidates | `DecoderConfig::check()`; an abort request from the producer thread; an `Encoder` reconfigure call; the dsp constants moved into `namespace dsp`. |
| Frequency step at T < 32 ms | A rotation estimate that a two-path fade cannot mimic (H2). |
| Acquisition near threshold | hf_weak tune locks one search bin off (35–46 Hz, outside the fine AFC range); C13 at 23 dB; T8 at gate − 1. |
| Header under a carrier on a header tone | Coherent carrier cancellation with complex header bins (≈ +300 B RAM). |
| Verification | Hardware and on-air tests; an ARM build; a thread-sanitizer stress test of the cross-thread queue; ESP32/STM32 CPU measurements (B4′); multi-seed long-suite runs. |
