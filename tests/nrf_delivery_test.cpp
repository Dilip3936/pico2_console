#include <stdio.h>
#include <string.h>
#include <pico/stdlib.h>
#include <pico/rand.h>
#include "radio.h"

namespace {
constexpr uint32_t PACKET_COUNT = 1000;
constexpr uint64_t INTERVAL_US = 33333;
const uint8_t SENDER_ADDRESS[5] = {0xa7, 0xd3, 0xc1, 0x5a, 0x01};
const uint8_t RECEIVER_ADDRESS[5] = {0xb6, 0xd3, 0xc1, 0x5a, 0x01};
enum class Role { idle, sender, receiver };
Role role = Role::idle;
bool ack_enabled = true, tx_pending = false, have_session = false;
uint32_t session = 0;
uint64_t started = 0, next_send = 0, last_report = 0;
uint8_t seen[(PACKET_COUNT + 7) / 8];

struct Measurements {
    uint32_t submitted = 0, received = 0, acked = 0, sent_unconfirmed = 0;
    uint32_t ack_failed = 0, stalled = 0, retries = 0;
    uint32_t invalid = 0, foreign = 0, duplicate = 0, reordered = 0, highest = 0;
} measurements;

void put_u32(uint8_t* out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (8 * i));
}
uint32_t get_u32(const uint8_t* in) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)in[i] << (8 * i);
    return value;
}
void make_packet(uint8_t* packet, uint32_t run, uint32_t sequence) {
    packet[0] = 'N'; packet[1] = 'D'; packet[2] = 1; packet[3] = 0;
    put_u32(packet + 4, run);
    put_u32(packet + 8, sequence);
    for (size_t i = 12; i < RADIO_PAYLOAD_SIZE; ++i)
        packet[i] = (uint8_t)((sequence * 37 + i) ^ run);
}
bool was_received(uint32_t sequence) {
    return seen[sequence / 8] & (1u << (sequence % 8));
}
void receive_packet(const uint8_t* packet) {
    uint32_t run = get_u32(packet + 4), sequence = get_u32(packet + 8);
    uint8_t expected[RADIO_PAYLOAD_SIZE];
    make_packet(expected, run, sequence);
    if (sequence >= PACKET_COUNT || memcmp(packet, expected, sizeof(expected))) {
        ++measurements.invalid;
        return;
    }
    if (have_session && run != session) { ++measurements.foreign; return; }
    session = run;
    have_session = true;
    if (was_received(sequence)) { ++measurements.duplicate; return; }
    if (measurements.received && sequence < measurements.highest) ++measurements.reordered;
    if (sequence > measurements.highest) measurements.highest = sequence;
    seen[sequence / 8] |= 1u << (sequence % 8);
    ++measurements.received;
}
void report(bool final) {
    printf("%s role=%s auto_ack=%u elapsed_us=%llu submitted=%lu unique=%lu "
           "tx_acked=%lu sent_unconfirmed=%lu ack_failed=%lu stalled=%lu retries=%lu "
           "invalid=%lu foreign=%lu duplicate=%lu reordered=%lu tx_pending=%u\n",
           final ? "RESULT" : "STATUS", role == Role::sender ? "sender" : "receiver",
           ack_enabled, (unsigned long long)(time_us_64() - started),
           (unsigned long)measurements.submitted, (unsigned long)measurements.received,
           (unsigned long)measurements.acked, (unsigned long)measurements.sent_unconfirmed,
           (unsigned long)measurements.ack_failed, (unsigned long)measurements.stalled,
           (unsigned long)measurements.retries, (unsigned long)measurements.invalid,
           (unsigned long)measurements.foreign, (unsigned long)measurements.duplicate,
           (unsigned long)measurements.reordered, tx_pending);
    if (final && role == Role::receiver) {
        // Include leading and trailing losses; the host must confirm all 1,000 sends completed.
        uint32_t streak = 0, longest = 0;
        for (uint32_t i = 0; i < PACKET_COUNT; ++i) {
            if (was_received(i)) streak = 0;
            else if (++streak > longest) longest = streak;
        }
        printf("DELIVERY expected=%lu unique=%lu missing=%lu missing_pct=%.3f "
               "max_missing_run=%lu session=%lu session_seen=%u\n",
               (unsigned long)PACKET_COUNT, (unsigned long)measurements.received,
               (unsigned long)(PACKET_COUNT - measurements.received),
               100.0 * (PACKET_COUNT - measurements.received) / PACKET_COUNT,
               (unsigned long)longest, (unsigned long)session, have_session);
    }
}
void stop() {
    if (role != Role::idle) report(true);
    radio_shutdown();
    role = Role::idle;
    tx_pending = false;
}
void start(Role requested, bool automatic_ack) {
    stop();
    RadioConfig config = {};
    bool sender = requested == Role::sender;
    memcpy(config.local_address, sender ? SENDER_ADDRESS : RECEIVER_ADDRESS, 5);
    memcpy(config.peer_address, sender ? RECEIVER_ADDRESS : SENDER_ADDRESS, 5);
    config.auto_ack = automatic_ack;
    config.retries = automatic_ack ? 3 : 0;
    if (!radio_init(config)) {
        RadioInitError error = radio_init_error();
        printf("ERROR radio readback reg=0x%02x byte=%u expected=0x%02x actual=0x%02x\n",
               error.reg, error.index, error.expected, error.actual);
        return;
    }
    measurements = {};
    memset(seen, 0, sizeof(seen));
    role = requested;
    ack_enabled = automatic_ack;
    have_session = false;
    session = sender ? get_rand_32() : 0;
    started = next_send = last_report = time_us_64();
    printf("READY role=%s auto_ack=%u packets=%lu interval_us=%llu payload=32 "
           "channel=76 RF_Mbps=1 chip_power_dBm=-18 SPI_Hz=%lu retries=%u session=%lu\n",
           sender ? "sender" : "receiver", ack_enabled, (unsigned long)PACKET_COUNT,
           (unsigned long long)INTERVAL_US, (unsigned long)radio_spi_baud(),
           config.retries, (unsigned long)session);
}
void tick() {
    if (role == Role::idle) return;
    if (role == Role::receiver) {
        uint8_t packet[RADIO_PAYLOAD_SIZE];
        for (unsigned i = 0; i < 3 && radio_receive(packet); ++i) receive_packet(packet);
    } else {
        RadioTxResult result = radio_poll_tx();
        if (result.status != RadioTxStatus::idle && result.status != RadioTxStatus::pending) {
            tx_pending = false;
            measurements.retries += result.retries;
            if (result.status == RadioTxStatus::acked) ++measurements.acked;
            if (result.status == RadioTxStatus::sent) ++measurements.sent_unconfirmed;
            if (result.status == RadioTxStatus::failed) ++measurements.ack_failed;
            if (result.status == RadioTxStatus::timed_out) ++measurements.stalled;
        }
        if (!tx_pending && measurements.submitted == PACKET_COUNT) { stop(); return; }
        uint64_t now = time_us_64();
        if (!tx_pending && now >= next_send) {
            uint8_t packet[RADIO_PAYLOAD_SIZE];
            make_packet(packet, session, measurements.submitted);
            if (radio_send(packet)) {
                ++measurements.submitted;
                tx_pending = true;
                next_send = now + INTERVAL_US;
            }
        }
    }
    if (time_us_64() - last_report >= 1000000) {
        report(false);
        last_report = time_us_64();
    }
}
}

int main() {
    stdio_init_all();
    sleep_ms(2000);
    uint64_t last_help = 0;
    while (true) {
        int c = getchar_timeout_us(0);
        if (c == 'a' || c == 'A') start(Role::sender, true);
        else if (c == 'b' || c == 'B') start(Role::receiver, true);
        else if (c == 'n' || c == 'N') start(Role::sender, false);
        else if (c == 'm' || c == 'M') start(Role::receiver, false);
        else if (c == 's' || c == 'S') stop();
        tick();
        if (role == Role::idle && time_us_64() - last_help >= 5000000) {
            puts("NRF delivery: A=ACK sender; B=ACK receiver; N=no-ACK sender; M=no-ACK receiver; S=stop/report.");
            last_help = time_us_64();
        }
        sleep_us(50);
    }
}
