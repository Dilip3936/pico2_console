#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <pico/stdlib.h>
#include <pico/rand.h>
#include "radio.h"

namespace {
constexpr uint32_t PROBE_COUNT = 1000;
constexpr uint64_t PROBE_INTERVAL_US = 33333, REPLY_TIMEOUT_US = 20000;
constexpr uint64_t ECHO_DELAY_US = 500, LONG_ECHO_DELAY_US = 5000;
uint64_t echo_delay_us = ECHO_DELAY_US;
const uint8_t ADDRESS_A[5] = {0xa7, 0xd3, 0xc1, 0x5a, 0x01};
const uint8_t ADDRESS_B[5] = {0xb6, 0xd3, 0xc1, 0x5a, 0x01};
enum class Role { idle, initiator, responder };
Role role = Role::idle;
bool fast = false, waiting = false, tx_pending = false, echo_pending = false;
uint32_t session = 0, sequence = 0;
uint64_t started = 0, probe_started = 0, next_probe = 0, echo_due = 0, last_report = 0;
uint8_t echo_packet[RADIO_PAYLOAD_SIZE];

uint32_t rtt_samples[PROBE_COUNT];

struct Measurements {
    uint32_t sent = 0, received = 0, timeouts = 0, cancelled = 0;
    uint32_t acked = 0, ack_failed = 0, tx_stalled = 0, retries = 0, retried = 0;
    uint32_t invalid = 0, foreign = 0, duplicate = 0, late = 0, busy_drop = 0;
    uint32_t last_reply = UINT32_MAX, last_run = 0, last_probe = UINT32_MAX;
    uint32_t count = 0, minimum = UINT32_MAX, maximum = 0;
    uint64_t sum = 0, jitter_sum = 0;
    uint32_t previous = 0, jitter_max = 0;

    void add_rtt(uint32_t us) {
        if (count) {
            uint32_t delta = us > previous ? us - previous : previous - us;
            jitter_sum += delta;
            jitter_max = std::max(jitter_max, delta);
        }
        previous = us;
        if (count < PROBE_COUNT) rtt_samples[count++] = us;
        sum += us;
        minimum = std::min(minimum, us);
        maximum = std::max(maximum, us);
    }
} measurements;

void put_u32(uint8_t* out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (8 * i));
}
uint32_t get_u32(const uint8_t* in) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)in[i] << (8 * i);
    return value;
}
void make_probe(uint8_t* packet, uint32_t run, uint32_t seq) {
    packet[0] = 'N'; packet[1] = 'R'; packet[2] = 1; packet[3] = 0;
    put_u32(packet + 4, run);
    put_u32(packet + 8, seq);
    for (size_t i = 12; i < RADIO_PAYLOAD_SIZE; ++i)
        packet[i] = (uint8_t)((seq * 37 + i) ^ run);
}
bool valid_packet(const uint8_t* packet) {
    if (packet[0] != 'N' || packet[1] != 'R' || packet[2] != 1 || packet[3] > 1) return false;
    uint8_t expected[RADIO_PAYLOAD_SIZE];
    make_probe(expected, get_u32(packet + 4), get_u32(packet + 8));
    return memcmp(packet + 4, expected + 4, RADIO_PAYLOAD_SIZE - 4) == 0;
}

void report(bool final) {
    uint64_t elapsed = time_us_64() - started;
    printf("%s role=%s elapsed_us=%llu sent=%lu received=%lu timeout=%lu cancelled=%lu "
           "tx_acked=%lu ack_failed=%lu tx_stalled=%lu retries=%lu retried=%lu "
           "invalid=%lu foreign=%lu duplicate=%lu late=%lu busy_drop=%lu\n",
           final ? "RESULT" : "STATUS", role == Role::initiator ? "initiator" : "responder",
           (unsigned long long)elapsed, (unsigned long)measurements.sent,
           (unsigned long)measurements.received, (unsigned long)measurements.timeouts,
           (unsigned long)measurements.cancelled, (unsigned long)measurements.acked,
           (unsigned long)measurements.ack_failed, (unsigned long)measurements.tx_stalled,
           (unsigned long)measurements.retries, (unsigned long)measurements.retried,
           (unsigned long)measurements.invalid, (unsigned long)measurements.foreign,
           (unsigned long)measurements.duplicate, (unsigned long)measurements.late,
           (unsigned long)measurements.busy_drop);
    if (role == Role::initiator && final) {
        uint32_t completed = measurements.received + measurements.timeouts;
        printf("LOSS completed=%lu missing_reply_pct=%.3f goodput_Bps=%.1f\n",
               (unsigned long)completed, completed ? 100.0 * measurements.timeouts / completed : 0.0,
               elapsed ? measurements.received * (double)RADIO_PAYLOAD_SIZE * 1000000 / elapsed : 0.0);
        if (measurements.count) {
            std::sort(rtt_samples, rtt_samples + measurements.count);
            auto percentile = [](uint32_t percent) {
                return rtt_samples[(measurements.count * percent + 99) / 100 - 1];
            };
            printf("RTT_US count=%lu min=%lu mean=%.2f p50=%lu p95=%lu p99=%lu max=%lu "
                   "jitter_mean=%.2f jitter_max=%lu\n",
                   (unsigned long)measurements.count, (unsigned long)measurements.minimum,
                   (double)measurements.sum / measurements.count,
                   (unsigned long)percentile(50), (unsigned long)percentile(95),
                   (unsigned long)percentile(99), (unsigned long)measurements.maximum,
                   measurements.count > 1 ? (double)measurements.jitter_sum / (measurements.count - 1) : 0.0,
                   (unsigned long)measurements.jitter_max);
        } else puts("RTT_US count=0 (no valid replies; latency/jitter unavailable)");
    }
}

void stop() {
    if (role != Role::idle) {
        if (waiting) ++measurements.cancelled;
        report(true);
    }
    radio_shutdown();
    role = Role::idle;
    waiting = tx_pending = echo_pending = false;
}

void start(Role requested, bool saturated, uint64_t reply_delay_us = ECHO_DELAY_US) {
    stop();
    RadioConfig config = {};
    bool initiator = requested == Role::initiator;
    memcpy(config.local_address, initiator ? ADDRESS_A : ADDRESS_B, 5);
    memcpy(config.peer_address, initiator ? ADDRESS_B : ADDRESS_A, 5);
    if (!radio_init(config)) {
        RadioInitError error = radio_init_error();
        printf("ERROR radio readback reg=0x%02x byte=%u expected=0x%02x actual=0x%02x; "
               "check power and CE/CSN/SPI wiring.\n",
               error.reg, error.index, error.expected, error.actual);
        return;
    }
    measurements = {};
    role = requested;
    fast = saturated;
    echo_delay_us = reply_delay_us;
    session = get_rand_32();
    started = next_probe = last_report = time_us_64();
    printf("READY role=%s mode=%s probes=%lu channel=%u RF_Mbps=1 payload=32 "
           "SPI_Hz=%lu retries=%u retry_delay_us=500 reply_timeout_us=20000 echo_delay_us=%llu "
           "chip_power_dBm=-18 session=%lu\n",
           initiator ? "initiator" : "responder", fast ? "saturated" : "30Hz",
           (unsigned long)PROBE_COUNT, config.channel, (unsigned long)radio_spi_baud(),
           config.retries, (unsigned long long)echo_delay_us, (unsigned long)session);
}

void poll_tx() {
    RadioTxResult tx = radio_poll_tx();
    if (tx.status == RadioTxStatus::idle || tx.status == RadioTxStatus::pending) return;
    tx_pending = false;
    measurements.retries += tx.retries;
    if (tx.retries) ++measurements.retried;
    if (tx.status == RadioTxStatus::acked) ++measurements.acked;
    if (tx.status == RadioTxStatus::failed) ++measurements.ack_failed;
    if (tx.status == RadioTxStatus::timed_out) ++measurements.tx_stalled;
}

void receive_packets() {
    uint8_t packet[RADIO_PAYLOAD_SIZE];
    // Bound work per pass even if unrelated traffic keeps the FIFO occupied.
    for (unsigned i = 0; i < 3 && radio_receive(packet); ++i) {
        if (!valid_packet(packet)) { ++measurements.invalid; continue; }
        uint32_t run = get_u32(packet + 4), seq = get_u32(packet + 8);
        if (role == Role::initiator) {
            if (packet[3] != 1 || run != session) { ++measurements.foreign; continue; }
            uint64_t now = time_us_64();
            if (waiting && seq == sequence && now - probe_started < REPLY_TIMEOUT_US) {
                measurements.add_rtt((uint32_t)(now - probe_started));
                ++measurements.received;
                measurements.last_reply = seq;
                waiting = false;
            } else if (seq == measurements.last_reply) ++measurements.duplicate;
            else ++measurements.late;
        } else {
            if (packet[3] != 0) { ++measurements.foreign; continue; }
            if (run == measurements.last_run && seq == measurements.last_probe) {
                ++measurements.duplicate;
                continue;
            }
            if (echo_pending || tx_pending) { ++measurements.busy_drop; continue; }
            measurements.last_run = run;
            measurements.last_probe = seq;
            ++measurements.received;
            memcpy(echo_packet, packet, RADIO_PAYLOAD_SIZE);
            echo_packet[3] = 1;
            echo_pending = true;
            // Give the initiator time to observe its ACK and switch back to RX.
            echo_due = time_us_64() + echo_delay_us;
        }
    }
}

void tick() {
    if (role == Role::idle) return;
    poll_tx();
    receive_packets();
    uint64_t now = time_us_64();
    if (role == Role::initiator) {
        if (waiting && now - probe_started >= REPLY_TIMEOUT_US) {
            ++measurements.timeouts;
            waiting = false;
        }
        if (!waiting && !tx_pending && measurements.sent == PROBE_COUNT) { stop(); return; }
        if (!waiting && !tx_pending && now >= next_probe) {
            uint8_t packet[RADIO_PAYLOAD_SIZE];
            sequence = measurements.sent;
            make_probe(packet, session, sequence);
            probe_started = time_us_64();
            if (radio_send(packet)) {
                ++measurements.sent;
                waiting = tx_pending = true;
                // No burst catch-up after a delay: one request is outstanding at a time.
                next_probe = probe_started + (fast ? 0 : PROBE_INTERVAL_US);
            }
        }
    } else if (echo_pending && !tx_pending && now >= echo_due && radio_send(echo_packet)) {
        echo_pending = false;
        tx_pending = true;
        ++measurements.sent;
    }
    if (now - last_report >= 1000000) {
        report(false);
        last_report = now;
    }
}
}

int main() {
    stdio_init_all();
    sleep_ms(2000);
    uint64_t last_help = 0;
    while (true) {
        int c = getchar_timeout_us(0);
        if (c == 'r' || c == 'R') start(Role::responder, false);
        else if (c == 'l' || c == 'L') start(Role::responder, false, LONG_ECHO_DELAY_US);
        else if (c == 'i' || c == 'I') start(Role::initiator, false);
        else if (c == 'f' || c == 'F') start(Role::initiator, true);
        else if (c == 's' || c == 'S') stop();
        tick();
        if (role == Role::idle && time_us_64() - last_help >= 5000000) {
            puts("NRF test: R=0.5ms responder; L=5ms responder; I=1000 probes at 30Hz; F=1000 saturated probes; S=stop/report.");
            last_help = time_us_64();
        }
        sleep_us(50);
    }
}
