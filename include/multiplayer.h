#pragma once
#include "game.h"
#include <stdint.h>
#include <cstddef>

enum class MultiplayerState {
    closed, choosing, hosting, searching, connected, disconnected, error, unavailable
};
enum class MultiplayerRole { none, host, joiner };

enum class MultiplayerPacketType : uint8_t { state = 1, input = 2 };
constexpr size_t MULTIPLAYER_BODY_SIZE = 24;
// The body is temporary callback storage; do not retain its pointer. Return true
// only for valid game data; rejected packets do not refresh the peer timeout.
using MultiplayerReceiver = bool (*)(MultiplayerPacketType, uint16_t, const uint8_t*, uint64_t);

// Open/close clear the receiver; choosing or starting a role preserves it.
void multiplayer_set_receiver(MultiplayerReceiver receiver);
// FNV-1a freshness ID, not authentication; zero until both pairing tokens exist.
uint32_t multiplayer_session_id();
// Copies all 24 body bytes into the latest outgoing slot (host STATE/joiner INPUT).
// INPUT is sent only in response to a valid STATE, after the existing 5 ms delay.
void multiplayer_publish(MultiplayerPacketType type, uint16_t tick, const uint8_t* body);
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
