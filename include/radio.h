#pragma once
#include <stddef.h>
#include <stdint.h>

constexpr size_t RADIO_PAYLOAD_SIZE = 32;

struct RadioConfig {
    // Addresses are LSByte first; pipes 0 and 1 need different first bytes.
    uint8_t local_address[5];
    uint8_t peer_address[5];
    uint8_t channel = 76;
    uint8_t retries = 3;
    bool auto_ack = true;
};

enum class RadioTxStatus { idle, pending, acked, sent, failed, timed_out };
struct RadioTxResult {
    RadioTxStatus status;
    uint8_t retries;
};

struct RadioInitError {
    uint8_t reg, index, expected, actual;
};

// Fixed 32-byte payloads, 1 Mbit/s, 2-byte CRC, optional automatic ACK, no IRQ/DMA.
// A successful init checks register/address readback, not communication with a peer.
bool radio_init(const RadioConfig& config);
RadioInitError radio_init_error();
void radio_shutdown();
uint32_t radio_spi_baud();
bool radio_send(const uint8_t* payload);
// Poll frequently. A terminal result is returned once, then reception resumes.
// Without automatic ACK, sent means local completion, not delivery confirmation.
RadioTxResult radio_poll_tx();
bool radio_receive(uint8_t* payload);
