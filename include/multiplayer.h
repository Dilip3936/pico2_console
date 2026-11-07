#pragma once
#include "game.h"
#include <stdint.h>
#include <cstddef>

enum class MultiplayerState {
    closed, choosing, hosting, searching, connected, disconnected, error, unavailable
};
enum class MultiplayerRole { none, host, joiner };

enum class MultiplayerPacketType : uint8_t { state = 1, input = 2, state_pair = 3 };
constexpr size_t MULTIPLAYER_BODY_SIZE = 24;
constexpr size_t MULTIPLAYER_PAIR_BODY_SIZE = 48;
// The body is 48 bytes ONLY for state_pair; state/input remain 24 bytes. Wire
// fragments never reach the callback. Storage is temporary; do not retain it.
// Return true only for valid game data; rejection does not refresh peer timeout.
using MultiplayerReceiver = bool (*)(MultiplayerPacketType, uint16_t, const uint8_t*, uint64_t);

// Open/close clear the receiver; choosing or starting a role preserves it.
void multiplayer_set_receiver(MultiplayerReceiver receiver);
// FNV-1a freshness ID, not authentication; zero until both pairing tokens exist.
uint32_t multiplayer_session_id();
// Accepts only state/input and copies 24 bytes (host STATE/joiner INPUT).
// INPUT is sent only after valid STATE or complete state_pair, delayed by 5 ms.
void multiplayer_publish(MultiplayerPacketType type, uint16_t tick, const uint8_t* body);
// Host only: copies contiguous 48 bytes. Both 32-byte packets use the existing
// D7/version-1/session/LE-tick header; low-nibble types 3/4 carry bytes 0–23/24–47.
// Parts may arrive in either order. Unsigned 16-bit half-range freshness rejects
// stale/duplicate parts; a newer pair discards an incomplete one. Even a rejected
// complete callback consumes that pair tick, without connecting or refreshing life.
// Only the queued snapshot is replaceable; an active pair stays intact. After
// part two completes, host reserves 10 ms RX (5 ms reply delay + 5 ms TX/retries).
// This guard applies only to pairs; normal state/input wire layout/timing is unchanged.
void multiplayer_publish_pair(uint16_t tick, const uint8_t* bodies);
// Only a connected session can enter gameplay and hide the connection screen.
void multiplayer_enter_game();
// Remains true on timeout so the console can notify the game before returning.
bool multiplayer_game_active();
// Last serviced monotonic clock on either platform, in microseconds.
uint64_t multiplayer_now_us();

void multiplayer_open(uint16_t game_id);
void multiplayer_close();
// Screen visibility, not transport lifetime; a timeout makes the screen visible.
bool multiplayer_is_open();
MultiplayerState multiplayer_state();
MultiplayerRole multiplayer_role();
// Call frequently with the monotonic microsecond clock, independently of game ticks.
void multiplayer_service(uint64_t now_us);
void multiplayer_update(const InputState& input);
void multiplayer_draw();
// Consumes the simulator-only selection once; open/close discard an unconsumed selection.
bool multiplayer_take_local_test();
