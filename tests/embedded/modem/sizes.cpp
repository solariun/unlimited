// The KISS modem core's sizes as a target builds it (spec 3.9, 12.2): 'make check_embedded' compiles this for the
// ESP32 and prints the size of each object below with the toolchain's nm (the send queue is the target's default).
#include "unlimited/modem.hpp"

char unlimited_size_modem[sizeof(unlimited::Modem)];
char unlimited_size_transmitter[sizeof(unlimited::ModemTransmitter)];
char unlimited_size_decoder[sizeof(unlimited::Decoder)];
char unlimited_size_kiss_decoder[sizeof(unlimited::KissDecoder)];
char unlimited_size_encoder[sizeof(unlimited::Encoder)];
