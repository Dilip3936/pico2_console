#include "game.h"
#include "graphics.h"
#include "console.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <random>

namespace {
// Speeds are pixels/second and delays are seconds. Level 1 has its own constant
// speed; the other levels share a starting speed and accelerating levels use the cap.
constexpr int WINNING_SCORE = 7;
constexpr int MAX_BALLS = 3;
constexpr float INITIAL_BALL_SPEED = 120.0f;
constexpr float LEVEL_1_BALL_SPEED = 240.0f;
constexpr float SPEED_PER_HIT = 8.0f;
constexpr float MAX_BALL_SPEED = 480.0f;
constexpr float PADDLE_SPEED = 180.0f;
constexpr int PADDLE_HEIGHT = 36;
constexpr int PADDLE_WIDTH = 5;
constexpr int PADDLE_INSET = 12;
constexpr int BALL_RADIUS = 2;
constexpr float EXTRA_BALL_DELAY = 5.0f;
constexpr float SPAWN_CUE_DURATION = 0.5f;
constexpr float SPAWN_CUE_FLASH_PERIOD = 0.1f;
constexpr float PADDLE_SPIN_STRENGTH = 0.20f;

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
constexpr int SPAWN_CUE_TICKS = int(SPAWN_CUE_DURATION * GAME_FPS + 0.5f);
constexpr int SPAWN_CUE_FLASH_TICKS = int(SPAWN_CUE_FLASH_PERIOD * GAME_FPS + 0.5f);

static_assert(MAX_BALLS > 0 && WINNING_SCORE > 0, "Pong needs balls and a winning score");
static_assert(INITIAL_BALL_SPEED > 0 && LEVEL_1_BALL_SPEED > 0 &&
              MAX_BALL_SPEED >= INITIAL_BALL_SPEED &&
              SPEED_PER_HIT >= 0 && PADDLE_SPEED > 0, "Invalid Pong speeds");
static_assert(EXTRA_BALL_TICKS > 0, "Invalid Pong spawn delay");
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
    {"1  CLASSIC", false, false, LEVEL_1_BALL_SPEED},
    {"2  ACCELERATING", false, true, INITIAL_BALL_SPEED},
    {"3  MULTIBALL", true, false, INITIAL_BALL_SPEED},
    {"4  MULTIBALL + SPEED", true, true, INITIAL_BALL_SPEED},
};
constexpr int MODE_COUNT = sizeof(MODES) / sizeof(MODES[0]);
enum class Phase { choosing, playing, result };
struct Ball {
    float x = CENTER_X, y = CENTER_Y;
    float vx = 0, vy = 0;
    bool active = false;
    bool serve_right = false;

};
Phase phase;
int selected_mode;
Ball balls[MAX_BALLS];
float paddle_y[2];
float paddle_velocity[2];
int score[2];
int last_scorer;

int extra_ball_ticks;
unsigned serve_index;
bool next_match_serve_right = false;
std::minstd_rand serve_random;

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

void start_match() {
    score[0] = score[1] = 0;
    last_scorer = -1;
    paddle_y[0] = paddle_y[1] = CENTER_Y;
    paddle_velocity[0] = paddle_velocity[1] = 0;
    for (Ball& ball : balls) ball = {};

    serve_index = 0;
    balls[0].serve_right = next_match_serve_right;
    next_match_serve_right = !next_match_serve_right;
    spawn_ball(balls[0]);
    phase = Phase::playing;
}

void init() {
    // Keep serve randomness local to Pong, without changing Flappy's random sequence.
    serve_random.seed(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
    selected_mode = 0;
    phase = Phase::choosing;
}

int paddle_direction(const ControlsState& controls, bool up = false, bool down = false) {
    up = up || (controls.held & CONTROL_A) || controls.y < JOYSTICK_UP;
    down = down || (controls.held & CONTROL_C) || controls.y > JOYSTICK_DOWN;
    return int(down) - int(up);
}

void rebound(Ball& ball, int player) {
    float speed = std::sqrt(ball.vx * ball.vx + ball.vy * ball.vy);
    if (MODES[selected_mode].accelerates) speed = std::min(speed + SPEED_PER_HIT, MAX_BALL_SPEED);
    float vertical = (ball.y - paddle_y[player]) / (PADDLE_HEIGHT / 2.0f);
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
    } else if (ball.y > bottom) {
        ball.y = 2 * bottom - ball.y;
        ball.vy = -std::fabs(ball.vy);
    }

    const int player = ball.vx < 0 ? 0 : 1;
    const float contact = player == 0 ? LEFT_FACE + BALL_RADIUS : RIGHT_FACE - BALL_RADIUS;
    const bool crossed = player == 0 ? previous_x >= contact && ball.x <= contact
                                     : previous_x <= contact && ball.x >= contact;
    if (crossed && std::fabs(ball.y - paddle_y[player]) <= PADDLE_HEIGHT / 2.0f + BALL_RADIUS) {
        ball.x = contact;
        rebound(ball, player);
    }
    if (ball.x <= -BALL_RADIUS || ball.x >= DISPLAY_WIDTH + BALL_RADIUS) {
        const int missed = ball.x < 0 ? 0 : 1;
        score[1 - missed] = std::min(score[1 - missed] + 1, WINNING_SCORE);
        ball.active = false;
        last_scorer = 1 - missed;

    }
}

void update(const InputState& input) {
    if (phase == Phase::choosing) {
        if (input.menu.back) console_return_to_selection();
        else if (input.menu.up) selected_mode = (selected_mode + MODE_COUNT - 1) % MODE_COUNT;
        else if (input.menu.down) selected_mode = (selected_mode + 1) % MODE_COUNT;
        else if (input.menu.select) start_match();
        return;
    }
    if (phase == Phase::result) {
        if (input.menu.back) phase = Phase::choosing;
        else if (input.menu.select) start_match();
        return;
    }

    if (extra_ball_ticks > 0) --extra_ball_ticks;
    const int direction[] = {paddle_direction(input.second_player_controls),
                             paddle_direction(input.controls, input.up, input.down)};
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
        for (int player = 0; player < 2; ++player) {
            const float previous_y = paddle_y[player];
            paddle_y[player] += direction[player] * PADDLE_SPEED * dt;
            paddle_y[player] = std::max(ARENA_TOP + PADDLE_HEIGHT / 2.0f,
                std::min(paddle_y[player], ARENA_BOTTOM - PADDLE_HEIGHT / 2.0f));
            paddle_velocity[player] = (paddle_y[player] - previous_y) / dt;
        }
        for (Ball& ball : balls) if (ball.active) move_ball(ball, dt);
    }
    // Resolve the whole tick before declaring a winner, independent of ball order.
    if (score[0] >= WINNING_SCORE || score[1] >= WINNING_SCORE) {
        phase = Phase::result;
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

bool spawn_cue_visible() {
    if (phase != Phase::playing || !MODES[selected_mode].multiball ||
        extra_ball_ticks <= 0 || extra_ball_ticks > SPAWN_CUE_TICKS) return false;
    int active = 0;
    for (const Ball& ball : balls) active += ball.active;
    return active > 0 && active < MAX_BALLS;
}

void draw_spawn_cue() {
    if (!spawn_cue_visible()) return;
    const bool right = spawn_direction(nullptr, serve_index % 2 != 0);
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
    if (phase == Phase::choosing) {
        centered_text(28, "PONG", WHITE);
        char rules[48];
        std::snprintf(rules, sizeof(rules), "LOCAL TWO PLAYER - FIRST TO %d", WINNING_SCORE);
        centered_text(46, rules, MUTED);
        for (int i = 0; i < MODE_COUNT; ++i) {
            const int y = 72 + i * 22;
            if (i == selected_mode) draw_filled_rect(36, y - 5, 248, 18, PLAYER_COLORS[0]);
            centered_text(y, MODES[i].name, i == selected_mode ? 0x0000 : WHITE);
        }
        centered_text(170, "P1: W/S", PLAYER_COLORS[0]);
        centered_text(186, "P2: UP/DOWN OR SPACE/C (A/C)", PLAYER_COLORS[1]);
        centered_text(208, "UP/DOWN: MODE   X/ENTER: START", WHITE);
        centered_text(224, "D: BACK   P: PAUSE   ESC: MENU", MUTED);
        return;
    }

    char text[48];
    std::snprintf(text, sizeof(text), "P1 %d", score[0]);
    draw_text(80, 3, text, PLAYER_COLORS[0]);
    std::snprintf(text, sizeof(text), "P2 %d", score[1]);
    draw_text(210, 3, text, PLAYER_COLORS[1]);
    centered_text(20, MODES[selected_mode].name, MUTED);
    draw_filled_rect(0, ARENA_TOP - 2, DISPLAY_WIDTH, 2, MUTED);
    draw_filled_rect(0, ARENA_BOTTOM, DISPLAY_WIDTH, 2, MUTED);
    for (int y = ARENA_TOP + 4; y < ARENA_BOTTOM; y += 12) draw_filled_rect(159, y, 2, 5, 0x3186);
    draw_spawn_cue();
    draw_filled_rect(PADDLE_INSET, int(paddle_y[0] - PADDLE_HEIGHT / 2.0f),
                     PADDLE_WIDTH, PADDLE_HEIGHT, PLAYER_COLORS[0]);
    draw_filled_rect(DISPLAY_WIDTH - PADDLE_INSET - PADDLE_WIDTH,
                     int(paddle_y[1] - PADDLE_HEIGHT / 2.0f),
                     PADDLE_WIDTH, PADDLE_HEIGHT, PLAYER_COLORS[1]);
    for (const Ball& ball : balls) {
        if (ball.active) draw_filled_rect(int(ball.x) - BALL_RADIUS, int(ball.y) - BALL_RADIUS,
                                          BALL_RADIUS * 2, BALL_RADIUS * 2, WHITE);
    }
    centered_text(232, "P1: W/S   P2: ARROWS/SPACE/C   P: PAUSE", MUTED);
    if (phase == Phase::result) {
        draw_filled_rect(35, 88, 250, 72, 0x1082);
        draw_rect(35, 88, 250, 72, WHITE);
        centered_text(100, score[0] == score[1] ? "DRAW" : score[0] > score[1] ? "PLAYER 1 WINS" : "PLAYER 2 WINS", WHITE);
        centered_text(122, "X/ENTER: REMATCH", WHITE);
        centered_text(142, "D: CHOOSE LEVEL", MUTED);
    }
}
}

extern const Game game_pong = {"Pong", init, update, draw};
