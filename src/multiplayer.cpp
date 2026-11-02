#include "multiplayer.h"
#include "graphics.h"
#include <string.h>
#ifdef PICO_BUILD
#include <stdio.h>
#include "radio.h"
#include <pico/stdlib.h>
#endif

namespace {
MultiplayerState state = MultiplayerState::closed;
MultiplayerRole role = MultiplayerRole::none;
uint16_t game_id = 0;
int selected = 0;
bool local_test = false, game_active = false;
MultiplayerReceiver receiver = nullptr;
uint64_t serviced_now = 0;
#ifdef PICO_BUILD
constexpr int CHOICE_COUNT = 3;
constexpr uint64_t PERIOD_US = 100000, RESPONSE_US = 5000, TIMEOUT_US = 5000000;
enum class PacketType : uint8_t { beacon = 1, request, accept, confirm, ping, pong };
struct Packet {
    PacketType type;
    uint64_t host, join;
    uint32_t sequence;
};
bool transport = false, tx_pending = false, response_pending = false;
bool response_game = false, outgoing_pending = false, incoming_seen = false;
uint16_t incoming_tick = 0;
uint8_t outgoing[RADIO_PAYLOAD_SIZE] = {};
uint64_t host_token = 0, join_token = 0, token_counter = 0;
uint64_t last_peer = 0, next_host = 0, response_at = 0;
uint32_t sequence = 0;
Packet response = {};
RadioInitError init_error = {};

uint64_t new_token() {
    // These are freshness tokens, not security credentials. The counter also
    // separates consecutive opens that happen within the same clock tick.
    uint64_t value = time_us_64() + (++token_counter * UINT64_C(0x9e3779b97f4a7c15));
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value ? value : token_counter;
}

void put_integer(uint8_t* bytes, uint64_t value, int count) {
    for (int i = 0; i < count; ++i) { bytes[i] = (uint8_t)value; value >>= 8; }
}
uint64_t get_integer(const uint8_t* bytes, int count) {
    uint64_t value = 0;
    for (int i = count - 1; i >= 0; --i) value = (value << 8) | bytes[i];
    return value;
}

bool decode(const uint8_t* bytes, Packet& packet) {
    // Fixed little-endian wire layout; no struct padding or native enum encoding.
    if (memcmp(bytes, "P2MP", 4) || bytes[4] != 1 ||
        get_integer(bytes + 6, 2) != game_id) return false;
    if (bytes[5] < (uint8_t)PacketType::beacon || bytes[5] > (uint8_t)PacketType::pong)
        return false;
    for (int i = 28; i < 32; ++i) if (bytes[i]) return false;
    packet = {static_cast<PacketType>(bytes[5]), get_integer(bytes + 8, 8),
              get_integer(bytes + 16, 8), (uint32_t)get_integer(bytes + 24, 4)};
    if (!packet.host || !packet.sequence) return false;
    return packet.type == PacketType::beacon ? packet.join == 0 : packet.join != 0;
}

bool send_packet(const Packet& packet) {
    uint8_t bytes[RADIO_PAYLOAD_SIZE] = {};
    static_assert(RADIO_PAYLOAD_SIZE == 32, "Multiplayer requires fixed 32-byte packets");
    memcpy(bytes, "P2MP", 4);
    bytes[4] = 1;
    bytes[5] = (uint8_t)packet.type;
    put_integer(bytes + 6, game_id, 2);
    put_integer(bytes + 8, packet.host, 8);
    put_integer(bytes + 16, packet.join, 8);
    put_integer(bytes + 24, packet.sequence, 4);
    tx_pending = radio_send(bytes);
    return tx_pending;
}

bool newer(uint32_t incoming, uint32_t previous) {
    uint32_t difference = incoming - previous;
    return difference != 0 && difference < UINT32_C(0x80000000);
}

void receive_game_packet(const uint8_t* bytes, uint64_t now) {
    const uint32_t session = multiplayer_session_id();
    if (bytes[0] != 0xd7 || !session || get_integer(bytes + 2, 4) != session) return;
    const MultiplayerPacketType type = role == MultiplayerRole::host ?
        MultiplayerPacketType::input : MultiplayerPacketType::state;
    if (bytes[1] != (0x10 | (uint8_t)type)) return;
    if (state != MultiplayerState::connected &&
        !(role == MultiplayerRole::joiner && state == MultiplayerState::searching)) return;
    const uint16_t tick = (uint16_t)get_integer(bytes + 6, 2);
    const uint16_t difference = (uint16_t)(tick - incoming_tick);
    if (incoming_seen && (!difference || difference >= 0x8000)) return;
    // Validate before connecting: the first STATE proves receipt of CONFIRM,
    // even if the host starts gameplay before its first pairing PING.
    if (!receiver || !receiver(type, tick, bytes + 8, now)) return;
    incoming_tick = tick;
    incoming_seen = true;
    last_peer = now;
    if (role == MultiplayerRole::joiner) {
        state = MultiplayerState::connected;
        response_pending = response_game = true;
        response_at = now + RESPONSE_US;
    }
}

void receive_packet(const Packet& packet, uint64_t now) {
    // Once gameplay owns the exchange, old control replies must not replace its
    // response slot or keep an otherwise silent game alive.
    if (game_active || (response_pending && response_game)) return;
    if (role == MultiplayerRole::host) {
        if (packet.host != host_token || packet.sequence != sequence) return;
        if (packet.type == PacketType::request && state == MultiplayerState::hosting) {
            if (join_token && packet.join != join_token) return;
            join_token = packet.join;
            last_peer = now;
            // Claim exactly one nonce; the next periodic packet will be ACCEPT.
        } else if (packet.join == join_token && join_token &&
                   ((packet.type == PacketType::confirm && state == MultiplayerState::hosting) ||
                    (packet.type == PacketType::pong && state == MultiplayerState::connected))) {
            state = MultiplayerState::connected;
            last_peer = now;
        }
        return;
    }

    if (packet.type == PacketType::beacon && state == MultiplayerState::searching) {
        if (host_token && packet.host != host_token) return;
        if (host_token && !newer(packet.sequence, sequence)) return;
        host_token = packet.host;
        sequence = packet.sequence;
        // Beacons alone do not prove that the host has received our request.
        response = {PacketType::request, host_token, join_token, sequence};
    } else if (packet.host == host_token && host_token && packet.join == join_token &&
               newer(packet.sequence, sequence) &&
               (packet.type == PacketType::accept || packet.type == PacketType::ping)) {
        if (packet.type == PacketType::accept && state == MultiplayerState::connected) return;
        sequence = packet.sequence;
        last_peer = now;
        if (packet.type == PacketType::ping) state = MultiplayerState::connected;
        response = {packet.type == PacketType::accept ? PacketType::confirm : PacketType::pong,
                    host_token, join_token, sequence};
    } else {
        return;
    }
    // Only one response slot exists. A newer valid host packet supersedes it;
    // duplicates cannot postpone the response or extend the peer timeout.
    response_pending = true;
    response_game = false;
    response_at = now + RESPONSE_US;
}
#endif
#ifndef PICO_BUILD
constexpr int CHOICE_COUNT = 4;
#endif

void stop_transport() {
#ifdef PICO_BUILD
    // Cancellation never waits for a leave exchange. The peer times out instead.
    if (transport) radio_shutdown();
    transport = tx_pending = response_pending = false;
    outgoing_pending = response_game = false;
#endif
}

void choose() {
    stop_transport();
    game_active = false;
#ifdef PICO_BUILD
    host_token = join_token = 0;
    incoming_seen = false;
#endif
    role = MultiplayerRole::none;
    state = MultiplayerState::choosing;
}

void start(MultiplayerRole requested_role) {
    stop_transport();
    game_active = false;
    role = requested_role;
#ifdef PICO_BUILD
    host_token = role == MultiplayerRole::host ? new_token() : 0;
    join_token = role == MultiplayerRole::joiner ? new_token() : 0;
    sequence = 0;
    incoming_seen = false;
    init_error = {};
    RadioConfig config = {};
    const uint8_t host_address[5] = {0xa7, 0xd3, 0xc1, 0x5a, 0x02};
    const uint8_t join_address[5] = {0xb6, 0xd3, 0xc1, 0x5a, 0x02};
    memcpy(config.local_address, role == MultiplayerRole::host ? host_address : join_address, 5);
    memcpy(config.peer_address, role == MultiplayerRole::host ? join_address : host_address, 5);
    if (!radio_init(config)) {
        init_error = radio_init_error();
        radio_shutdown();
        state = MultiplayerState::error;
        return;
    }
    transport = true;
    last_peer = next_host = time_us_64();
    state = role == MultiplayerRole::host ? MultiplayerState::hosting : MultiplayerState::searching;
#else
    state = MultiplayerState::unavailable;
#endif
}

void centered_text(int y, const char* text, uint16_t color = 0xffff) {
    draw_text((DISPLAY_WIDTH - (int)strlen(text) * 6) / 2, y, text, color);
}
}

void multiplayer_open(uint16_t requested_game_id) {
    multiplayer_close();
    game_id = requested_game_id;
    selected = 0;
    state = MultiplayerState::choosing;
}

void multiplayer_close() {
    stop_transport();
    state = MultiplayerState::closed;
    role = MultiplayerRole::none;
    local_test = false;
    game_active = false;
    receiver = nullptr;
#ifdef PICO_BUILD
    host_token = join_token = 0;
    incoming_seen = false;
#endif
}

void multiplayer_set_receiver(MultiplayerReceiver callback) { receiver = callback; }

uint32_t multiplayer_session_id() {
#ifdef PICO_BUILD
    if (!host_token || !join_token) return 0;
    uint32_t hash = UINT32_C(2166136261);
    // Hash the wire-order tokens and game ID, independently of native layout.
    for (int field = 0; field < 3; ++field) {
        uint64_t value = field == 0 ? host_token : field == 1 ? join_token : game_id;
        const int count = field == 2 ? 2 : 8;
        for (int i = 0; i < count; ++i) {
            hash = (hash ^ (uint8_t)value) * UINT32_C(16777619);
            value >>= 8;
        }
    }
    return hash ? hash : 1;
#else
    return 0;
#endif
}

void multiplayer_publish(MultiplayerPacketType type, uint16_t tick, const uint8_t* body) {
#ifdef PICO_BUILD
    const uint32_t session = multiplayer_session_id();
    if (!transport || !session || !body ||
        (role == MultiplayerRole::host &&
         (type != MultiplayerPacketType::state || state != MultiplayerState::connected)) ||
        (role == MultiplayerRole::joiner &&
         (type != MultiplayerPacketType::input ||
          (state != MultiplayerState::searching && state != MultiplayerState::connected))) ||
        role == MultiplayerRole::none) return;
    outgoing[0] = 0xd7;
    outgoing[1] = 0x10 | (uint8_t)type;
    put_integer(outgoing + 2, session, 4);
    put_integer(outgoing + 6, tick, 2);
    memcpy(outgoing + 8, body, MULTIPLAYER_BODY_SIZE);
    outgoing_pending = true;
#else
    (void)type;
    (void)tick;
    (void)body;
#endif
}

void multiplayer_enter_game() {
    if (state == MultiplayerState::connected) {
        game_active = true;
#ifdef PICO_BUILD
        if (!response_game) response_pending = false;
#endif
    }
}
bool multiplayer_game_active() { return game_active; }
uint64_t multiplayer_now_us() { return serviced_now; }

bool multiplayer_is_open() {
    return state != MultiplayerState::closed &&
           (!game_active || state != MultiplayerState::connected);
}
MultiplayerState multiplayer_state() { return state; }
MultiplayerRole multiplayer_role() { return role; }

void multiplayer_service(uint64_t now_us) {
    if (now_us > serviced_now) serviced_now = now_us;
#ifdef PICO_BUILD
    if (!transport) return;
    // The caller may have sampled this tick before synchronous radio_init finished.
    if (now_us < last_peer) return;
    // Poll even when there are no packets, so TX always returns to RX and
    // connected-peer silence still expires. ACK failure is not evidence of application loss.
    if (tx_pending) {
        RadioTxResult result = radio_poll_tx();
        tx_pending = result.status == RadioTxStatus::pending;
    }
    if (state == MultiplayerState::connected && now_us - last_peer >= TIMEOUT_US) {
        stop_transport();
        state = MultiplayerState::disconnected;
        return;
    }
    if (tx_pending) return;
    uint8_t bytes[RADIO_PAYLOAD_SIZE];
    for (int i = 0; i < 3 && radio_receive(bytes); ++i) {
        if (bytes[0] == 0xd7) {
            receive_game_packet(bytes, now_us);
        } else {
            Packet packet;
            if (decode(bytes, packet)) receive_packet(packet, now_us);
        }
    }
    if (role == MultiplayerRole::host && game_active && state == MultiplayerState::connected) {
        if (outgoing_pending) {
            tx_pending = radio_send(outgoing);
            if (tx_pending) outgoing_pending = false;
        }
    } else if (role == MultiplayerRole::host && now_us >= next_host) {
        if (++sequence == 0) ++sequence;
        PacketType type = state == MultiplayerState::connected ? PacketType::ping :
                          join_token ? PacketType::accept : PacketType::beacon;
        send_packet({type, host_token, join_token, sequence});
        next_host = now_us + PERIOD_US;
    } else if (role == MultiplayerRole::joiner && response_pending && now_us >= response_at) {
        if (response_game) {
            if (outgoing_pending) {
                tx_pending = radio_send(outgoing);
                if (tx_pending) outgoing_pending = response_pending = false;
            }
        } else if (send_packet(response)) {
            response_pending = false;
        }
    }
#else
    (void)now_us;
#endif
}

void multiplayer_update(const InputState& input) {
    if (!multiplayer_is_open()) return;
    uint8_t pressed = input.controls.pressed;
    if (pressed & CONTROL_B) {
        if (state == MultiplayerState::choosing) multiplayer_close();
        else choose();
        return;
    }
    if (state != MultiplayerState::choosing) return;
    if (pressed & CONTROL_C) selected = (selected + CHOICE_COUNT - 1) % CHOICE_COUNT;
    else if (pressed & CONTROL_D) selected = (selected + 1) % CHOICE_COUNT;
    else if (pressed & CONTROL_A) {
        if (selected == 0) start(MultiplayerRole::host);
        else if (selected == 1) start(MultiplayerRole::joiner);
#ifndef PICO_BUILD
        else if (selected == 2) {
            multiplayer_close();
            local_test = true;
        }
#endif
        else multiplayer_close();
    }
}

void multiplayer_draw() {
    if (!multiplayer_is_open()) return;
    clear_screen(0x1082);
    centered_text(20, "MULTIPLAYER");
    if (state == MultiplayerState::choosing) {
        const char* choices[] = {"HOST", "JOIN",
#ifndef PICO_BUILD
                                 "LOCAL TEST",
#endif
                                 "BACK"};
        for (int i = 0; i < CHOICE_COUNT; ++i) {
            int y = 66 + i * 26;
            if (i == selected) draw_filled_rect(44, y - 4, 232, 18, 0xffff);
            centered_text(y, choices[i], i == selected ? 0x0000 : 0xffff);
        }
        centered_text(200, "C/D: CHOOSE   A: CONFIRM");
        centered_text(220, "B: BACK");
        return;
    }
    centered_text(54, role == MultiplayerRole::host ? "ROLE: HOST" : "ROLE: JOINER");
    switch (state) {
    case MultiplayerState::hosting:
        centered_text(92, "HOSTING - WAITING FOR JOINER");
        break;
    case MultiplayerState::searching:
        centered_text(92, "SEARCHING - CONNECTING TO HOST");
        break;
    case MultiplayerState::connected:
        centered_text(92, "CONNECTED", 0x07e0);
        centered_text(128, "OPENING GAME");
        break;
    case MultiplayerState::disconnected:
        centered_text(92, "DISCONNECTED", 0xffe0);
        centered_text(128, "PEER TIMED OUT");
        break;
    case MultiplayerState::error:
        centered_text(92, "RADIO INIT FAILED", 0xffe0);
#ifdef PICO_BUILD
        {
            char detail[48];
            snprintf(detail, sizeof(detail), "REG %02X[%u] EXPECT %02X GOT %02X",
                     (unsigned)init_error.reg, (unsigned)init_error.index,
                     (unsigned)init_error.expected, (unsigned)init_error.actual);
            centered_text(128, detail);
        }
#endif
        break;
    case MultiplayerState::unavailable:
        centered_text(92, "RADIO UNAVAILABLE", 0xffe0);
        centered_text(128, "SIMULATOR: USE LOCAL TEST");
        break;
    default:
        break;
    }
    centered_text(220, "B: RETURN TO HOST/JOIN");
}

bool multiplayer_take_local_test() {
    bool result = local_test;
    local_test = false;
    return result;
}
