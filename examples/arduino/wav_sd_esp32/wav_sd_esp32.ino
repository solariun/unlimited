// Unlimited WAV writer on an ESP32: a text is encoded and written to /unlimited.wav on an SD card with the core
// WAV codec (the same RIFF writer the PC tools use). Play the file into a transmitter, or decode it on a PC with
// `unlimited_decode --in unlimited.wav --bps 6`: the decoder is told the speed and finds the pitch. 6 bytes per
// second: one byte per window of 10 slots of 16.667 ms on 1500 Hz, 48 bit/s, 1368-1632 Hz (fits a 2.4 kHz SSB
// filter).
//
// The encoder renders chunks that go straight into a WavWriter; a small ByteSink adapter turns its bytes
// into SD File writes. The exact length is known up front (Encoder::duration_samples), so the header is
// right from the start; the File can also seek, so WavWriter::finish() could patch it otherwise.
//
// Wiring (SD card module on VSPI, 3.3 V):
//   CS -> GPIO5, SCK -> GPIO18, MISO -> GPIO19, MOSI -> GPIO23, VCC -> 3V3, GND -> GND
#include <SD.h>
#include <SPI.h>
#include <unlimited.h>

namespace {

const uint32_t k_serial_baud = 115200;
const uint8_t k_sd_cs_pin = 5;
const char k_path[] = "/unlimited.wav";
const char k_message[] = "CQ CQ DE UNLIMITED WAV ON SD 0123456789";
const uint8_t k_message_size = sizeof(k_message) - 1;
const uint16_t k_centi_bytes_per_second = unlimited::k_default_centi_bytes_per_second;  // 6.00 bytes/s
const uint32_t k_sample_rate_hz = 8000;  // any 8000..192000; the PC decoder resamples
const size_t k_chunk_samples = 512;
const float k_ms_per_s = 1000.0f;

class FileByteSink : public unlimited::ByteSink {
public:
    explicit FileByteSink(File& file) : file_(file) {}

    bool write(const uint8_t* data, size_t size) override { return file_.write(data, size) == size; }

    bool seek(uint32_t position) override { return file_.seek(position); }

private:
    File& file_;
};

int16_t g_chunk[k_chunk_samples];

bool write_wav(File& file) {
    unlimited::EncoderConfig config;
    config.sample_rate_hz = k_sample_rate_hz;
    config.slot_us = unlimited::slot_us_for_centi_speed(k_centi_bytes_per_second);
    unlimited::Encoder encoder(config);
    const uint8_t* message = reinterpret_cast<const uint8_t*>(k_message);
    size_t written = encoder.write(message, k_message_size);
    if (!encoder.start()) return false;

    FileByteSink sink(file);
    unlimited::WavWriter writer;
    if (!writer.begin(sink, k_sample_rate_hz, encoder.duration_samples(k_message_size))) return false;
    while (writer.ok()) {
        written += encoder.write(message + written, k_message_size - written);  // a text may exceed the queue
        const size_t rendered = encoder.render(g_chunk, k_chunk_samples);
        if (rendered == 0) break;
        writer.write(g_chunk, rendered);
    }
    const bool ok = writer.finish() && written == k_message_size;
    const uint32_t samples = writer.samples_written();
    Serial.printf("%s: %u samples, %.1f s at %u Hz, %u bytes\n", k_path, static_cast<unsigned>(samples),
                  samples / static_cast<float>(k_sample_rate_hz), static_cast<unsigned>(k_sample_rate_hz),
                  static_cast<unsigned>(unlimited::k_wav_header_bytes + samples * sizeof(int16_t)));
    return ok;
}

}  // namespace

void setup() {
    Serial.begin(k_serial_baud);
    if (!SD.begin(k_sd_cs_pin)) {
        Serial.println("wav_sd_esp32: no SD card");
        return;
    }
    File file = SD.open(k_path, FILE_WRITE);
    if (!file) {
        Serial.printf("wav_sd_esp32: cannot create %s\n", k_path);
        return;
    }
    const uint32_t start_ms = millis();
    const bool ok = write_wav(file);
    file.close();
    Serial.printf("wav_sd_esp32: %s in %.2f s\n", ok ? "done" : "FAILED", (millis() - start_ms) / k_ms_per_s);
}

void loop() {
    vTaskDelete(nullptr);  // nothing left to do
}
