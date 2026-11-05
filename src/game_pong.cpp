#include "game.h"
#include "graphics.h"
#include "console.h"
#include "multiplayer.h"
#include "pong_assets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#ifdef PICO_BUILD
#include <pico/stdlib.h>
#else
#include <chrono>
#endif

namespace {
// Speeds are pixels/second and delays are seconds. Level 1 reaches its cap
// in two paddle hits; levels 2 and 4 accelerate more gradually.
constexpr int WINNING_SCORE = 7;
constexpr int MAX_BALLS = 3;
constexpr float INITIAL_BALL_SPEED = 120.0f;
constexpr float LEVEL_1_MAX_SPEED = 240.0f;
constexpr float LEVEL_1_SPEED_PER_HIT = 60.0f;
constexpr float SPEED_PER_HIT = 8.0f;
constexpr float MAX_BALL_SPEED = 480.0f;
constexpr float PADDLE_SPEED = 180.0f;
constexpr int PADDLE_HEIGHT = 36;
constexpr int PADDLE_WIDTH = 5;
constexpr int PADDLE_INSET = 12;
constexpr int BALL_RADIUS = 2;
constexpr float EXTRA_BALL_DELAY = 5.0f;
constexpr int START_COUNTDOWN_SECONDS = 3;
constexpr float SPAWN_CUE_DURATION = 0.5f;
constexpr float SPAWN_CUE_FLASH_PERIOD = 0.1f;
constexpr float PADDLE_SPIN_STRENGTH = 0.20f;

constexpr uint16_t PONG_GAME_ID = 0x5047;
constexpr uint64_t INPUT_TIMEOUT_US = 100000;
constexpr int JOYSTICK_UP = 1024;
constexpr int JOYSTICK_DOWN = 3072;
constexpr float MAX_REBOUND_VERTICAL = 0.8f;
constexpr float MIN_REBOUND_VERTICAL = 0.18f;
// Slope is vertical/horizontal speed; these bounds avoid flat or near-vertical serves.
constexpr float MIN_SERVE_SLOPE = 0.25f;
constexpr float MAX_SERVE_SLOPE = 0.70f;

constexpr int ARENA_TOP = 34;
constexpr int ARENA_BOTTOM = 224;
constexpr float CENTER_X = DISPLAY_WIDTH / 2.0f;
constexpr float CENTER_Y = (ARENA_TOP + ARENA_BOTTOM) / 2.0f;
constexpr float LEFT_FACE = PADDLE_INSET + PADDLE_WIDTH;
constexpr float RIGHT_FACE = DISPLAY_WIDTH - PADDLE_INSET - PADDLE_WIDTH;
constexpr int EXTRA_BALL_TICKS = int(EXTRA_BALL_DELAY * GAME_FPS + 0.5f);
constexpr int START_COUNTDOWN_TICKS = START_COUNTDOWN_SECONDS * GAME_FPS;
constexpr int SPAWN_CUE_TICKS = int(SPAWN_CUE_DURATION * GAME_FPS + 0.5f);
constexpr int SPAWN_CUE_FLASH_TICKS = int(SPAWN_CUE_FLASH_PERIOD * GAME_FPS + 0.5f);

static_assert(MAX_BALLS > 0 && WINNING_SCORE > 0, "Pong needs balls and a winning score");
static_assert(INITIAL_BALL_SPEED > 0 && LEVEL_1_MAX_SPEED >= INITIAL_BALL_SPEED &&
              LEVEL_1_SPEED_PER_HIT >= 0 &&
              MAX_BALL_SPEED >= INITIAL_BALL_SPEED &&
              SPEED_PER_HIT >= 0 && PADDLE_SPEED > 0, "Invalid Pong speeds");
static_assert(EXTRA_BALL_TICKS > 0 && START_COUNTDOWN_TICKS > 0, "Invalid Pong timing");
static_assert(SPAWN_CUE_TICKS >= 0 && SPAWN_CUE_TICKS <= EXTRA_BALL_TICKS &&
              SPAWN_CUE_FLASH_TICKS > 0, "Invalid Pong spawn cue timing");
static_assert(MIN_SERVE_SLOPE > 0 && MAX_SERVE_SLOPE >= MIN_SERVE_SLOPE &&
              MAX_SERVE_SLOPE <= 1, "Invalid Pong serve slopes");
static_assert(PADDLE_SPIN_STRENGTH >= 0 && PADDLE_SPIN_STRENGTH <= 1,
              "Invalid Pong paddle influence");
static_assert(BALL_RADIUS > 0 && PADDLE_HEIGHT > 0 &&
              PADDLE_HEIGHT < ARENA_BOTTOM - ARENA_TOP && PADDLE_WIDTH > 0,
              "Invalid Pong dimensions");
static_assert(MIN_REBOUND_VERTICAL > 0 && MIN_REBOUND_VERTICAL <= MAX_REBOUND_VERTICAL &&
              MAX_REBOUND_VERTICAL < 1, "Invalid Pong rebound angles");

constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t MUTED = 0x8410;
constexpr uint16_t PLAYER_COLORS[] = {0x07FF, 0xFD20};
struct Mode { const char* name; bool multiball; bool accelerates; float initial_speed; };
constexpr Mode MODES[] = {
    {"1  CLASSIC", false, false, INITIAL_BALL_SPEED},
    {"2  ACCELERATING", false, true, INITIAL_BALL_SPEED},
    {"3  MULTIBALL", true, false, INITIAL_BALL_SPEED},
    {"4  MULTIBALL + SPEED", true, true, INITIAL_BALL_SPEED},
};
constexpr int MODE_COUNT = sizeof(MODES) / sizeof(MODES[0]);
enum class Phase { choosing, playing, result, ready };
struct Ball {
    float x = CENTER_X, y = CENTER_Y;
    float vx = 0, vy = 0;
    bool active = false;
    bool serve_right = false;

};
Phase phase;
bool choosing_players = true;
bool single_player = true;
int selected_mode;
Ball balls[MAX_BALLS];
float paddle_y[2];
float paddle_velocity[2];
int score[2];
int last_scorer;

int extra_ball_ticks;
int countdown_ticks = 0;
unsigned serve_index;
bool next_match_serve_right = false;
std::minstd_rand serve_random;
struct PongNetworkState {
    uint32_t session = 0;
    MultiplayerRole role = MultiplayerRole::none;
    uint16_t tick = 0;
    unsigned phase_age = 65535;
    bool have_state = false, have_input = false, initialized = false, paused = false;
    uint16_t input_tick = 0;
    uint8_t ready = 0, acknowledged = 0;
    int remote_direction = 0, local_direction = 0;
    uint64_t input_time = 0;
    bool pending = false, control_value = false, cue_right = false;
    uint8_t control_id = 0, control_kind = 0;
    Phase control_phase = Phase::choosing;
    uint16_t control_tick = 0;
};
PongNetworkState network;
bool receive_network_packet(MultiplayerPacketType, uint16_t, const uint8_t*, uint64_t);
void publish_input(int direction);
bool network_game() { return network.session != 0; }
int paddle_height(int player) {
    return single_player && player == 1 ? ARENA_BOTTOM - ARENA_TOP : PADDLE_HEIGHT;
}

// The compact STATE body has fixed field widths; reject incompatible tuning at build time.
static_assert(MAX_BALLS == 3 && WINNING_SCORE <= 7 && MODE_COUNT == 4 &&
              EXTRA_BALL_TICKS <= 255 && START_COUNTDOWN_TICKS <= 255 && MAX_BALL_SPEED <= 508 &&
              LEVEL_1_MAX_SPEED <= 508, "Pong tuning exceeds packet field widths");

void centered_text(int y, const char* text, uint16_t color) {
    draw_text((DISPLAY_WIDTH - int(std::strlen(text)) * 6) / 2, y, text, color);
}

bool spawn_direction(const Ball* replacing, bool fallback_right) {
    bool right = last_scorer >= 0 ? last_scorer == 1 : fallback_right;
    if (MODES[selected_mode].multiball) {
        int heading_left = 0, heading_right = 0;
        float left_distance = -1, right_distance = -1;
        for (const Ball& other : balls) {
            if (!other.active || &other == replacing) continue;
            const float distance = std::fabs(other.x - CENTER_X);
            if (other.vx < 0) {
                ++heading_left;
                left_distance = std::max(left_distance, distance);
            } else {
                ++heading_right;
                right_distance = std::max(right_distance, distance);
            }
        }
        // Balance the destinations first. With one ball heading each way, send
        // the new ball after the one farther from center; exact ties keep the serve rule.
        if (heading_left != heading_right) right = heading_left > heading_right;
        else if (left_distance != right_distance) right = right_distance > left_distance;
    }
    return right;
}

void spawn_ball(Ball& ball) {
    ball.serve_right = spawn_direction(&ball, ball.serve_right);
    float slope = std::uniform_real_distribution<float>(MIN_SERVE_SLOPE, MAX_SERVE_SLOPE)(serve_random);
    if (std::uniform_int_distribution<int>(0, 1)(serve_random)) slope = -slope;
    ++serve_index;
    const float horizontal = MODES[selected_mode].initial_speed / std::sqrt(1.0f + slope * slope);
    ball.x = CENTER_X;
    ball.y = CENTER_Y;
    ball.vx = ball.serve_right ? horizontal : -horizontal;
    ball.vy = horizontal * slope;
    ball.active = true;
    extra_ball_ticks = EXTRA_BALL_TICKS;
}

void clear_match() {
    speaker_stop();
    countdown_ticks = 0;
    score[0] = single_player ? WINNING_SCORE : 0;
    score[1] = 0;
    last_scorer = -1;
    paddle_y[0] = paddle_y[1] = CENTER_Y;
    paddle_velocity[0] = paddle_velocity[1] = 0;
    for (Ball& ball : balls) ball = {};

    serve_index = 0;
}

void start_match() {
    clear_match();
    balls[0].serve_right = next_match_serve_right;
    next_match_serve_right = !next_match_serve_right;
    spawn_ball(balls[0]);
    phase = Phase::playing;
}

void begin_countdown() {
    clear_match();
    countdown_ticks = START_COUNTDOWN_TICKS;
    phase = Phase::ready;
}

void init() {
    // Keep serve randomness local to Pong, without changing Flappy's random sequence.
#ifdef PICO_BUILD
    serve_random.seed(static_cast<unsigned>(time_us_64()));
#else
    serve_random.seed(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
#endif
    network = {};
    choosing_players = single_player = true;
    selected_mode = 0;
    phase = Phase::choosing;
    clear_match();
}

int paddle_direction(const ControlsState& controls, bool up = false, bool down = false) {
    up = up || (controls.held & CONTROL_A) || controls.y < JOYSTICK_UP;
    down = down || (controls.held & CONTROL_C) || controls.y > JOYSTICK_DOWN;
    return int(down) - int(up);
}

void rebound(Ball& ball, int player) {
    float speed = std::sqrt(ball.vx * ball.vx + ball.vy * ball.vy);
    if (selected_mode == 0) speed = std::min(speed + LEVEL_1_SPEED_PER_HIT, LEVEL_1_MAX_SPEED);
        else if (MODES[selected_mode].accelerates) speed = std::min(speed + SPEED_PER_HIT, MAX_BALL_SPEED);
    float vertical = (ball.y - paddle_y[player]) / (paddle_height(player) / 2.0f);
    vertical = std::max(-1.0f, std::min(vertical, 1.0f)) * MAX_REBOUND_VERTICAL;
    vertical += paddle_velocity[player] / PADDLE_SPEED * PADDLE_SPIN_STRENGTH;
    vertical = std::max(-MAX_REBOUND_VERTICAL, std::min(vertical, MAX_REBOUND_VERTICAL));
    // Keep a useful slope after adding paddle motion, without changing total speed.
    if (std::fabs(vertical) < MIN_REBOUND_VERTICAL) {
        const bool upward = vertical < 0 || (vertical == 0 && ball.vy < 0);
        vertical = upward ? -MIN_REBOUND_VERTICAL : MIN_REBOUND_VERTICAL;
    }
    ball.vy = speed * vertical;
    ball.vx = speed * std::sqrt(1.0f - vertical * vertical) * (player == 0 ? 1 : -1);
    speaker_play(pong_sounds::paddle);
}

void move_ball(Ball& ball, float dt) {
    const float previous_x = ball.x;
    ball.x += ball.vx * dt;
    ball.y += ball.vy * dt;
    const float top = ARENA_TOP + BALL_RADIUS;
    const float bottom = ARENA_BOTTOM - BALL_RADIUS;
    if (ball.y < top) {
        ball.y = 2 * top - ball.y;
        ball.vy = std::fabs(ball.vy);
        speaker_play(pong_sounds::wall);
    } else if (ball.y > bottom) {
        ball.y = 2 * bottom - ball.y;
        ball.vy = -std::fabs(ball.vy);
        speaker_play(pong_sounds::wall);
    }

    const int player = ball.vx < 0 ? 0 : 1;
    const float contact = player == 0 ? LEFT_FACE + BALL_RADIUS : RIGHT_FACE - BALL_RADIUS;
    const bool crossed = player == 0 ? previous_x >= contact && ball.x <= contact
                                     : previous_x <= contact && ball.x >= contact;
    if (crossed && std::fabs(ball.y - paddle_y[player]) <= paddle_height(player) / 2.0f + BALL_RADIUS) {
        ball.x = contact;
        rebound(ball, player);
    }
    if (ball.x <= -BALL_RADIUS || ball.x >= DISPLAY_WIDTH + BALL_RADIUS) {
        const int missed = ball.x < 0 ? 0 : 1;
        if (single_player) score[0] = std::max(0, score[0] - 1);
        else score[1 - missed] = std::min(score[1 - missed] + 1, WINNING_SCORE);
        ball.active = false;
        last_scorer = 1 - missed;
        speaker_play(pong_sounds::point);
    }
}

void advance_playing(int first_direction, int second_direction) {
    if (extra_ball_ticks > 0) --extra_ball_ticks;
    const int direction[] = {first_direction, second_direction};

    // Substeps keep both paddles and balls moving by at most one pixel, including
    // at tuned speeds above one paddle-width per simulation tick.
    float fastest = PADDLE_SPEED;
    for (const Ball& ball : balls) {
        if (ball.active) fastest = std::max(fastest, std::sqrt(ball.vx * ball.vx + ball.vy * ball.vy));
    }
    if (MODES[selected_mode].accelerates) fastest = std::max(fastest, MAX_BALL_SPEED);
    const int steps = std::max(1, int(std::ceil(fastest / GAME_FPS)));
    const float dt = 1.0f / (GAME_FPS * steps);
    for (int step = 0; step < steps; ++step) {
        for (int player = 0; player < (single_player ? 1 : 2); ++player) {
            const float previous_y = paddle_y[player];
            paddle_y[player] += direction[player] * PADDLE_SPEED * dt;
            paddle_y[player] = std::max(ARENA_TOP + PADDLE_HEIGHT / 2.0f,
                std::min(paddle_y[player], ARENA_BOTTOM - PADDLE_HEIGHT / 2.0f));
            paddle_velocity[player] = (paddle_y[player] - previous_y) / dt;
        }
        for (Ball& ball : balls) if (ball.active) move_ball(ball, dt);
    }
    // Resolve the whole tick before declaring a winner, independent of ball order.
    if (single_player ? score[0] == 0 :
                        score[0] >= WINNING_SCORE || score[1] >= WINNING_SCORE) {
        phase = Phase::result;
        speaker_play(pong_sounds::match_end);
        return;
    }

    bool any_active = false;
    Ball* available = nullptr;
    for (Ball& ball : balls) {
        if (ball.active) any_active = true;
        else if (!available) available = &ball;
    }
    // All spawns share one cooldown, including replacements. An empty arena
    // bypasses the wait, but that immediate serve also restarts the cooldown.
    if (available && (!any_active || (MODES[selected_mode].multiball && extra_ball_ticks == 0))) {
        available->serve_right = serve_index % 2 != 0;
        spawn_ball(*available);
    }
}

void update(const InputState& input) {
    if (choosing_players) {
        if (input.menu.back) console_return_to_selection();
        else if (input.menu.up || input.menu.down) single_player = !single_player;
        else if (input.menu.select) {
            choosing_players = false;
            clear_match();
#ifdef PICO_BUILD
            if (!single_player) {
                multiplayer_open(PONG_GAME_ID);
                multiplayer_set_receiver(receive_network_packet);
            }
#endif
        }
        return;
    }
#ifdef PICO_BUILD
    if (!single_player) {
        // Closing Host/Join returns to the player-count selection.
        if (multiplayer_state() == MultiplayerState::closed) {
            network = {};
            clear_match();
            phase = Phase::choosing;
            choosing_players = true;
        }
        return;
    }
#endif
    if (phase == Phase::choosing) {
        if (input.menu.back) { choosing_players = true; speaker_stop(); }
        else if (input.menu.up) selected_mode = (selected_mode + MODE_COUNT - 1) % MODE_COUNT;
        else if (input.menu.down) selected_mode = (selected_mode + 1) % MODE_COUNT;
        else if (input.menu.select) begin_countdown();
    } else if (phase == Phase::result) {
        if (input.menu.back) { phase = Phase::choosing; speaker_stop(); }
        else if (input.menu.select) begin_countdown();
    } else if (phase == Phase::ready) {
        if (input.menu.back) { countdown_ticks = 0; phase = Phase::choosing; }
        else if (countdown_ticks > 0 && --countdown_ticks == 0) start_match();
    } else if (phase == Phase::playing) {
        if (single_player) advance_playing(paddle_direction(input.controls, input.up, input.down), 0);
        else advance_playing(paddle_direction(input.second_player_controls),
                             paddle_direction(input.controls, input.up, input.down));
    }
}

uint16_t read_u16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
int signed_value(unsigned value, unsigned sign) {
    return value < sign ? int(value) : int(value) - int(sign * 2);
}
void write_u16(uint8_t* p, uint16_t value) { p[0] = uint8_t(value); p[1] = uint8_t(value >> 8); }
bool newer_tick(uint16_t a, uint16_t b) { return a != b && uint16_t(a - b) < 0x8000; }
PongNetworkState session_state() {
    const uint32_t session = multiplayer_session_id();
    const MultiplayerRole role = multiplayer_role();
    if (network.session == session && network.role == role) return network;
    PongNetworkState fresh;
    fresh.session = session;
    fresh.role = role;
    return fresh;
}

void publish_state() {
    uint8_t body[MULTIPLAYER_BODY_SIZE] = {};
    for (int i = 0; i < MAX_BALLS; ++i) if (balls[i].active) {
        write_u16(body + i * 6, uint16_t(std::lround(balls[i].x * 16)));
        write_u16(body + i * 6 + 2, uint16_t(std::lround(balls[i].y * 16)));
        body[i * 6 + 4] = uint8_t(std::lround(balls[i].vx / 4));
        body[i * 6 + 5] = uint8_t(std::lround(balls[i].vy / 4));
        body[21] |= uint8_t(1 << i);
    }
    body[18] = uint8_t(std::lround(paddle_y[0]));
    body[19] = uint8_t(std::lround(paddle_y[1]));
    body[20] = uint8_t(score[0] | (score[1] << 3) | (network.ready << 6));
    body[21] |= uint8_t((selected_mode << 3) | (int(phase) << 5) | (network.paused ? 0x80 : 0));
    body[22] = uint8_t(countdown_ticks > 0 ? countdown_ticks : extra_ball_ticks);
    body[23] = network.acknowledged | (spawn_direction(nullptr, serve_index % 2 != 0) ? 0x80 : 0);
    multiplayer_publish(MultiplayerPacketType::state, network.tick, body);
}

bool receive_state(uint16_t tick, const uint8_t* body, PongNetworkState next) {
    if (next.have_state && !newer_tick(tick, next.tick)) return false;
    const int mode = (body[21] >> 3) & 3, mask = body[21] & 7;
    const Phase incoming_phase = Phase((body[21] >> 5) & 3);
    const bool paused = (body[21] & 0x80) != 0;
    const int scores[] = {body[20] & 7, (body[20] >> 3) & 7};
    const bool counting = incoming_phase == Phase::ready && (body[20] >> 6) == 3;
    if (scores[0] > WINNING_SCORE || scores[1] > WINNING_SCORE ||
        (counting ? (!body[22] || body[22] > START_COUNTDOWN_TICKS || mask || scores[0] || scores[1]) :
                    body[22] > EXTRA_BALL_TICKS) ||
        (paused && incoming_phase != Phase::playing) ||
        (incoming_phase == Phase::playing && (!mask || scores[0] == WINNING_SCORE || scores[1] == WINNING_SCORE)) ||
        (incoming_phase == Phase::result && scores[0] != WINNING_SCORE && scores[1] != WINNING_SCORE)) return false;
    for (int i = 0; i < 2; ++i)
        if (body[18 + i] < ARENA_TOP + PADDLE_HEIGHT / 2.0f ||
            body[18 + i] > ARENA_BOTTOM - PADDLE_HEIGHT / 2.0f) return false;
    Ball received[MAX_BALLS];
    int active = 0;
    for (int i = 0; i < MAX_BALLS; ++i) {
        const uint8_t* p = body + i * 6;
        if (!(mask & (1 << i))) {
            for (int j = 0; j < 6; ++j) if (p[j]) return false;
            continue;
        }
        Ball& ball = received[i];
        ball.active = true;
        ball.x = signed_value(read_u16(p), 0x8000) / 16.0f;
        ball.y = signed_value(read_u16(p + 2), 0x8000) / 16.0f;
        ball.vx = signed_value(p[4], 0x80) * 4.0f;
        ball.vy = signed_value(p[5], 0x80) * 4.0f;
        const float limit = mode == 0 ? LEVEL_1_MAX_SPEED :
                                    MODES[mode].accelerates ? MAX_BALL_SPEED : MODES[mode].initial_speed;
        if (ball.x < -BALL_RADIUS || ball.x > DISPLAY_WIDTH + BALL_RADIUS ||
            ball.y < ARENA_TOP + BALL_RADIUS || ball.y > ARENA_BOTTOM - BALL_RADIUS ||
            !ball.vx || std::hypot(ball.vx, ball.vy) > limit + 3) return false;
        ++active;
    }
    if (!MODES[mode].multiball && active > 1) return false;
    const uint8_t ack = body[23] & 0x7f;
    if (next.pending && ack == next.control_id) next.pending = false;
    else if (ack != next.acknowledged) return false;
    // Snapshot cues need no extra radio fields. Only adjacent playing snapshots
    // identify bounces: a gap can hide a miss/respawn or multiple direction changes.
    if (!next.have_state || paused || incoming_phase == Phase::choosing ||
        incoming_phase == Phase::ready ||
        (incoming_phase == Phase::playing && phase != Phase::playing)) {
        speaker_stop();
    } else if (incoming_phase == Phase::result && phase == Phase::playing) {
        speaker_play(pong_sounds::match_end);
    } else if (incoming_phase == Phase::playing &&
               (scores[0] > score[0] || scores[1] > score[1])) {
        speaker_play(pong_sounds::point);
    } else if (incoming_phase == Phase::playing && !network.paused &&
               uint16_t(tick - next.tick) == 1 &&
               scores[0] == score[0] && scores[1] == score[1]) {
        bool paddle_hit = false, wall_hit = false;
        for (int i = 0; i < MAX_BALLS; ++i) {
            if (!balls[i].active || !received[i].active) continue;
            if (balls[i].vx * received[i].vx < 0) paddle_hit = true;
            else if (balls[i].vy * received[i].vy < 0) wall_hit = true;
        }
        if (paddle_hit) speaker_play(pong_sounds::paddle);
        else if (wall_hit) speaker_play(pong_sounds::wall);
    }
    next.acknowledged = ack;
    next.tick = tick;
    next.have_state = next.initialized = true;
    next.ready = body[20] >> 6;
    next.paused = paused;
    next.cue_right = (body[23] & 0x80) != 0;
    // Apply only after every field passes validation. The joiner never advances these snapshots.
    std::copy(received, received + MAX_BALLS, balls);
    for (int i = 0; i < 2; ++i) { paddle_y[i] = body[18 + i]; paddle_velocity[i] = 0; score[i] = scores[i]; }
    selected_mode = mode;
    phase = incoming_phase;
    countdown_ticks = counting ? body[22] : 0;
    if (!counting) extra_ball_ticks = body[22];
    network = next;
    // Refresh the response slot before the manager's 5 ms reply deadline, even
    // when the joiner's next 30 Hz update has not sampled another held direction.
    publish_input(network.local_direction);
    return true;
}

bool receive_input(uint16_t tick, const uint8_t* body, uint64_t now, PongNetworkState next) {
    if (!next.initialized || newer_tick(tick, next.tick) ||
        (next.have_input && !newer_tick(tick, next.input_tick)) ||
        body[0] > 2 || body[1] > 127 || body[2] > 2 || body[3] > 1 || body[4] > 3) return false;
    for (int i = 8; i < int(MULTIPLAYER_BODY_SIZE); ++i) if (body[i]) return false;
    if (body[5] || (!body[2] && (body[1] || body[3] || body[4] || body[6] || body[7])) ||
        (body[2] == 1 && (!body[3] || (Phase(body[4]) != Phase::ready && Phase(body[4]) != Phase::result))) ||
        (body[2] == 2 && Phase(body[4]) != Phase::playing) ||
        (body[2] && newer_tick(read_u16(body + 6), tick))) return false;
    if (body[2]) {
        const uint8_t distance = (body[1] - next.acknowledged) & 127;
        if (distance && distance < 64) {
            if (distance != 1 || (next.pending &&
                (next.control_id != body[1] || next.control_kind != body[2] ||
                 next.control_value != bool(body[3]) || next.control_phase != Phase(body[4]) ||
                 next.control_tick != read_u16(body + 6)))) return false;
            next.pending = true;
            next.control_id = body[1];
            next.control_kind = body[2];
            next.control_value = body[3];
            next.control_phase = Phase(body[4]);
            next.control_tick = read_u16(body + 6);
        }
    }
    next.remote_direction = body[0] == 1 ? -1 : body[0] == 2 ? 1 : 0;
    next.input_time = now;
    next.input_tick = tick;
    next.have_input = true;
    network = next;
    return true;
}

bool receive_network_packet(MultiplayerPacketType type, uint16_t tick, const uint8_t* body, uint64_t now) {
    const PongNetworkState next = session_state();
    if (!next.session || !body) return false;
    if (next.role == MultiplayerRole::joiner && type == MultiplayerPacketType::state)
        return receive_state(tick, body, next);
    if (next.role == MultiplayerRole::host && type == MultiplayerPacketType::input)
        return receive_input(tick, body, now, next);
    return false;
}

void publish_input(int direction) {
    uint8_t body[MULTIPLAYER_BODY_SIZE] = {};
    body[0] = direction < 0 ? 1 : direction > 0 ? 2 : 0;
    if (network.pending) {
        body[1] = network.control_id;
        body[2] = network.control_kind;
        body[3] = network.control_value;
        body[4] = uint8_t(network.control_phase);
        write_u16(body + 6, network.control_tick);
    }
    multiplayer_publish(MultiplayerPacketType::input, network.tick, body);
}

void queue_control(uint8_t kind, bool value) {
    if (network.pending) return;
    network.pending = true;
    network.control_id = (network.acknowledged + 1) & 127;
    network.control_kind = kind;
    network.control_value = value;
    network.control_phase = phase;
    network.control_tick = network.tick;
}

void pong_multiplayer_update(const InputState& input) {
    if (multiplayer_state() != MultiplayerState::connected) {
        network = {};
        clear_match();
        phase = Phase::choosing;
        return;
    }
    network = session_state();
    if (!network.session) return;
    if (!network.initialized && network.role == MultiplayerRole::host) {
        clear_match();
        phase = Phase::choosing;
        network.initialized = true;
    }
    if (input.menu.back && (phase != Phase::playing || network.paused)) {
        if (network.role == MultiplayerRole::host && (phase == Phase::ready || phase == Phase::result)) {
            speaker_stop();
            phase = Phase::choosing;
            countdown_ticks = 0;
            network.ready = 0;
            network.phase_age = 0;
        } else {
            console_return_to_selection();
            network = {};
            return;
        }
    }
    const bool pause_press = (input.controls.pressed & CONTROL_E) || (network.paused && input.menu.select);
    const int direction = paddle_direction(input.controls, input.up, input.down);
    if (network.role == MultiplayerRole::joiner) {
        network.local_direction = direction;
        if (!network.have_state) return;
        if ((phase == Phase::ready || phase == Phase::result) && input.menu.select && !(network.ready & 2))
            queue_control(1, true);
        else if (phase == Phase::playing && pause_press) queue_control(2, !network.paused);
        publish_input(direction);
        return;
    }
    ++network.tick;
    if (network.phase_age < 65535) ++network.phase_age;
    const bool was_paused = network.paused;
    if (network.pending) {
        const uint16_t age = uint16_t(network.tick - network.control_tick);
        if (age < 0x8000 && age <= network.phase_age && network.control_phase == phase) {
            if (network.control_kind == 1 && (phase == Phase::ready || phase == Phase::result)) network.ready |= 2;
            if (network.control_kind == 2 && phase == Phase::playing) network.paused = network.control_value;
        }
        network.acknowledged = network.control_id;
        network.pending = false;
    }
    if (phase == Phase::choosing) {
        if (input.menu.up) selected_mode = (selected_mode + MODE_COUNT - 1) % MODE_COUNT;
        else if (input.menu.down) selected_mode = (selected_mode + 1) % MODE_COUNT;
        else if (input.menu.select) { clear_match(); phase = Phase::ready; network.ready = 0; network.phase_age = 0; }
    } else if (phase == Phase::ready || phase == Phase::result) {
        if (countdown_ticks > 0) {
            if (--countdown_ticks == 0) {
                start_match();
                network.ready = 0;
                network.remote_direction = 0;
                network.phase_age = 0;
            }
        } else {
            if (input.menu.select) network.ready |= 1;
            if (network.ready == 3) {
                begin_countdown();
                network.phase_age = 0;
            }
        }
    } else {
        if (pause_press) network.paused = !was_paused;
        if (network.paused != was_paused) {
            if (network.paused) speaker_stop();
            network.phase_age = 0;
            network.remote_direction = 0;
        }
        if (!network.paused && !was_paused) {
            const uint64_t now = multiplayer_now_us();
            const int remote = network.have_input && now >= network.input_time &&
                now - network.input_time < INPUT_TIMEOUT_US ? network.remote_direction : 0;
            advance_playing(direction, remote);
            if (phase == Phase::result) { network.ready = 0; network.phase_age = 0; }
        }
    }
    publish_state();
}

bool spawn_cue_visible() {
    if (phase != Phase::playing || !MODES[selected_mode].multiball ||
        extra_ball_ticks <= 0 || extra_ball_ticks > SPAWN_CUE_TICKS) return false;
    int active = 0;
    for (const Ball& ball : balls) active += ball.active;
    return active > 0 && active < MAX_BALLS;
}

void draw_spawn_cue() {
    if (!spawn_cue_visible()) return;
    const bool right = network_game() && network.role == MultiplayerRole::joiner ?
        network.cue_right : spawn_direction(nullptr, serve_index % 2 != 0);
    const uint16_t color = PLAYER_COLORS[right ? 1 : 0];
    const int x = int(CENTER_X), y = int(CENTER_Y);
    draw_rect(x - 5, y - 5, 11, 11,
              (extra_ball_ticks / SPAWN_CUE_FLASH_TICKS) % 2 ? WHITE : color);
    draw_filled_rect(right ? x + 9 : x - 21, y - 1, 13, 3, color);
    for (int i = 0; i <= 4; ++i) {
        const int tip_x = x + (right ? 1 : -1) * (21 - i);
        draw_pixel(tip_x, y - i, color);
        draw_pixel(tip_x, y + i, color);
    }
}

void draw(const char*) {
    clear_screen(0x0841);
    if (choosing_players) {
        centered_text(48, "PONG", WHITE);
        const char* choices[] = {"SINGLE PLAYER", "MULTIPLAYER"};
        for (int i = 0; i < 2; ++i) {
            const bool selected = i == (single_player ? 0 : 1);
            const int y = 100 + i * 28;
            if (selected) draw_filled_rect(56, y - 5, 208, 18, PLAYER_COLORS[0]);
            centered_text(y, choices[i], selected ? 0x0000 : WHITE);
        }
        return;
    }
    if (network_game() && network.role == MultiplayerRole::joiner && !network.have_state) {
        centered_text(100, "WAITING FOR HOST STATE", WHITE);
        return;
    }
    if (phase == Phase::choosing) {
        centered_text(28, "PONG", WHITE);
        char rules[48];
        std::snprintf(rules, sizeof(rules), single_player ? "SINGLE PLAYER - %d LIVES" :
                      network_game() ? "WIRELESS - FIRST TO %d" : "LOCAL TWO PLAYER - FIRST TO %d",
                      WINNING_SCORE);
        centered_text(46, rules, MUTED);
        for (int i = 0; i < MODE_COUNT; ++i) {
            const int y = 72 + i * 22;
            if (i == selected_mode) draw_filled_rect(36, y - 5, 248, 18, PLAYER_COLORS[0]);
            centered_text(y, MODES[i].name, i == selected_mode ? 0x0000 : WHITE);
        }
        if (network_game()) centered_text(180, network.role == MultiplayerRole::host ?
                                          "HOST: SELECT LEVEL" : "HOST IS SELECTING LEVEL", WHITE);
#ifndef PICO_BUILD
        else {
            if (single_player) centered_text(180, "UP/DOWN OR SPACE/C (A/C): MOVE", PLAYER_COLORS[0]);
            else {
                centered_text(170, "P1: W/S", PLAYER_COLORS[0]);
                centered_text(186, "P2: UP/DOWN OR SPACE/C (A/C)", PLAYER_COLORS[1]);
            }
            centered_text(208, "UP/DOWN: MODE   X/ENTER: START", WHITE);
            centered_text(224, "D: BACK   P: PAUSE   ESC: MENU", MUTED);
        }
#endif
        return;
    }

    char text[48];
    if (single_player) {
        std::snprintf(text, sizeof(text), "LIVES %d", score[0]);
        centered_text(3, text, PLAYER_COLORS[0]);
    } else {
        std::snprintf(text, sizeof(text), "P1 %d", score[0]);
        draw_text(80, 3, text, PLAYER_COLORS[0]);
        std::snprintf(text, sizeof(text), "P2 %d", score[1]);
        draw_text(210, 3, text, PLAYER_COLORS[1]);
    }
    centered_text(20, MODES[selected_mode].name, MUTED);
    draw_filled_rect(0, ARENA_TOP - 2, DISPLAY_WIDTH, 2, MUTED);
    draw_filled_rect(0, ARENA_BOTTOM, DISPLAY_WIDTH, 2, MUTED);
    for (int y = ARENA_TOP + 4; y < ARENA_BOTTOM; y += 12) draw_filled_rect(159, y, 2, 5, 0x3186);
    draw_spawn_cue();
    draw_filled_rect(PADDLE_INSET, int(paddle_y[0] - PADDLE_HEIGHT / 2.0f),
                     PADDLE_WIDTH, PADDLE_HEIGHT, PLAYER_COLORS[0]);
    draw_filled_rect(DISPLAY_WIDTH - PADDLE_INSET - PADDLE_WIDTH,
                     int(paddle_y[1] - paddle_height(1) / 2.0f),
                     PADDLE_WIDTH, paddle_height(1), PLAYER_COLORS[1]);
    for (const Ball& ball : balls) {
        if (ball.active) draw_filled_rect(int(ball.x) - BALL_RADIUS, int(ball.y) - BALL_RADIUS,
                                          BALL_RADIUS * 2, BALL_RADIUS * 2, WHITE);
    }
#ifndef PICO_BUILD
    if (!network_game()) centered_text(232, single_player ? "UP/DOWN: MOVE   P: PAUSE" :
                                      "P1: W/S   P2: ARROWS/SPACE/C   P: PAUSE", MUTED);
#endif
    if (countdown_ticks > 0) {
        draw_filled_rect(65, 88, 190, 64, 0x1082);
        draw_rect(65, 88, 190, 64, WHITE);
        centered_text(100, "GET READY", WHITE);
        std::snprintf(text, sizeof(text), "%d", (countdown_ticks + GAME_FPS - 1) / GAME_FPS);
        centered_text(124, text, WHITE);
        return;
    }
    if (network_game() && (phase == Phase::ready || phase == Phase::result || network.paused)) {
        draw_filled_rect(35, 78, 250, 100, 0x1082);
        draw_rect(35, 78, 250, 100, WHITE);
        centered_text(90, network.paused ? "PAUSED" : phase == Phase::ready ? "READY TO PLAY" :
                      score[0] == score[1] ? "DRAW" : score[0] > score[1] ? "PLAYER 1 WINS" : "PLAYER 2 WINS", WHITE);
        if (network.paused) centered_text(116, "B/K OR E: RESUME", WHITE);
        else {
            centered_text(112, network.ready & 1 ? "P1: READY" : "P1: WAITING", PLAYER_COLORS[0]);
            centered_text(128, network.ready & 2 ? "P2: READY" : "P2: WAITING", PLAYER_COLORS[1]);
            centered_text(150, "B/K: READY", WHITE);
        }
        centered_text(166, network.role == MultiplayerRole::host && !network.paused ?
                      "D: LEVELS" : "D: BACK", MUTED);
        return;
    }
    if (phase == Phase::result) {
        draw_filled_rect(35, 88, 250, 72, 0x1082);
        draw_rect(35, 88, 250, 72, WHITE);
        centered_text(100, single_player ? "GAME OVER" :
                      score[0] == score[1] ? "DRAW" : score[0] > score[1] ? "PLAYER 1 WINS" : "PLAYER 2 WINS", WHITE);
#ifdef PICO_BUILD
        centered_text(122, "B/K: PLAY AGAIN", WHITE);
#else
        centered_text(122, "X/ENTER: PLAY AGAIN", WHITE);
#endif
        centered_text(142, "D: CHOOSE LEVEL", MUTED);
    }
}
}

extern const Game game_pong = {"Pong", init, update, draw, 0, nullptr, pong_multiplayer_update};
