#include "game.h"
#include "multiplayer.h"
#include "graphics.h"
#include "flappy_assets.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MAX_PIPES 3
#define SPRITE_SCALE 2
#define SPRITE_SIZE (16 * SPRITE_SCALE)
#define PIPE_SPACING 160
#define PIPE_WIDTH 32
#define PIPE_GAP 72
#define BIRD_X 72
#define BIRD_SIZE (14 * SPRITE_SCALE)
// Both modes keep the bottom 24 rows static between transitions.
#define GROUND_Y (DISPLAY_HEIGHT - 24)
#define BIRD_GROUND_Y (GROUND_Y - SPRITE_SIZE)

namespace {

// Network game identity stays stable even if the generated game catalogue changes order.
constexpr uint16_t FLAPPY_GAME_ID = 1;

enum GameState { TITLE, PLAYING, GAME_OVER };

struct Pipe {
    float x;
    float gap_y;
    bool passed;
};

static GameState current_state = TITLE;
struct Bird {
    float y;
    float velocity;
};

static Bird birds[2] = {};
static bool racing = false;
static int race_winner = -1;
static const float GRAVITY = 1.6f;
static const float JUMP_STRENGTH = -10.8f;
static int score = 0;
static int highscore = 0;
static Pipe pipes[MAX_PIPES];
static int anim_frame = 0;
static int anim_timer = 0;

static GameState last_debug_state = TITLE;
static unsigned debug_frames = 0;

struct FlapEvent {
    uint16_t id;
    uint16_t tick;
    bool allowed = true;
};

struct FlappyNetworkState {
    uint32_t session = 0;
    MultiplayerRole role = MultiplayerRole::none;
    uint16_t tick = 0;
    bool have_state = false;
    uint64_t state_time = 0;
    bool scene_initialized = false;
    bool paused = false;
    bool ready[2] = {};
    uint16_t handled_flap = 0;
    uint16_t next_flap = 0;
    uint16_t flap_floor = 0;
    bool flap_floor_active = false;
    // Saturation keeps phase checks valid even when the 16-bit network_state tick wraps.
    unsigned phase_age = 65535;
    FlapEvent flaps[4] = {};
    unsigned flap_count = 0;
    uint8_t handled_control = 0;
    uint8_t next_control = 0;
    bool control_pending = false;
    uint8_t control_id = 0;
    uint8_t control_kind = 0;
    bool control_value = false;
    GameState control_phase = TITLE;
    bool control_allowed = false;
};

static FlappyNetworkState network_state;
static bool receive_network_packet(MultiplayerPacketType type, uint16_t tick,
                         const uint8_t* body, uint64_t now_us);

static bool network_game_active() {
    return network_state.session != 0;
}

static uint16_t read_u16(const uint8_t* bytes) {
    return uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8);
}

static int read_i16(const uint8_t* bytes) {
    unsigned value = read_u16(bytes);
    return value < 0x8000u ? int(value) : int(value) - 0x10000;
}

static void write_u16(uint8_t* bytes, uint16_t value) {
    bytes[0] = uint8_t(value);
    bytes[1] = uint8_t(value >> 8);
}

// IDs include zero on wrap; unsigned half-range comparisons keep wrap defined.
static bool newer16(uint16_t a, uint16_t b) {
    uint16_t distance = uint16_t(a - b);
    return distance != 0 && distance < 0x8000u;
}

static bool newer8(uint8_t a, uint8_t b) {
    uint8_t distance = uint8_t(a - b);
    return distance != 0 && distance < 0x80u;
}

static FlappyNetworkState network_state_for_session() {
    uint32_t session = multiplayer_session_id();
    MultiplayerRole role = multiplayer_role();
    if (network_state.session == session && network_state.role == role) return network_state;
    FlappyNetworkState fresh;
    fresh.session = session;
    fresh.role = role;
    return fresh;
}

static void publish_input() {
    uint8_t body[MULTIPLAYER_BODY_SIZE] = {};
    body[0] = uint8_t(network_state.flap_count);
    if (network_state.control_pending) {
        body[1] = network_state.control_id;
        body[2] = network_state.control_kind;
        body[3] = network_state.control_value;
    }
    for (unsigned i = 0; i < network_state.flap_count; ++i) {
        write_u16(body + 4 + i * 4, network_state.flaps[i].id);
        write_u16(body + 6 + i * 4, network_state.flaps[i].tick);
    }
    multiplayer_publish(MultiplayerPacketType::input, network_state.tick, body);
}

static void discard_flaps() {
    if (network_state.flap_count) network_state.handled_flap = network_state.flaps[network_state.flap_count - 1].id;
    network_state.flap_count = 0;
}

static void abort_network_game() {
    network_state = FlappyNetworkState{};
    current_state = TITLE;
    race_winner = -1;
    score = 0;
    anim_frame = 0;
    anim_timer = 0;
}

static void load_highscore() {
#ifndef PICO_BUILD
    FILE* f = fopen("highscore.txt", "r");
    if (f) {
        fscanf(f, "%d", &highscore);
        fclose(f);
    }
#endif
}

static void save_highscore() {
#ifndef PICO_BUILD
    FILE* f = fopen("highscore.txt", "w");
    if (f) {
        fprintf(f, "%d", highscore);
        fclose(f);
    }
#endif
}

static void spawn_pipe(int index, float x_pos) {
    pipes[index].x = x_pos;
    const int margin = PIPE_GAP / 2 + 20;
    pipes[index].gap_y = margin + (rand() % (GROUND_Y - 2 * margin + 1));
    pipes[index].passed = false;
}

static void reset_game() {
    for (Bird& bird : birds) bird = {DISPLAY_HEIGHT / 2.0f, 0};
    race_winner = -1;
    score = 0;
    for (int i = 0; i < MAX_PIPES; i++) {
        spawn_pipe(i, DISPLAY_WIDTH + i * PIPE_SPACING);
    }
}

void flappy_init() {
    network_state = FlappyNetworkState{};
    racing = false;
    anim_frame = 0;
    anim_timer = 0;
    last_debug_state = TITLE;
    debug_frames = 0;
    load_highscore();
    reset_game();
    current_state = TITLE;
}

static bool check_collision(const Bird& bird) {
    if (bird.y >= BIRD_GROUND_Y) return true;
    if (bird.y <= 0) return true;

    for (int i = 0; i < MAX_PIPES; i++) {
        float px = pipes[i].x;
        float py = pipes[i].gap_y;

        // We inset the hitbox so touching the sprite edges is a little more forgiving.
        float bx1 = BIRD_X + 2 * SPRITE_SCALE;
        float bx2 = BIRD_X + BIRD_SIZE - 2 * SPRITE_SCALE;
        float by1 = bird.y + 2 * SPRITE_SCALE;
        float by2 = bird.y + BIRD_SIZE - 2 * SPRITE_SCALE;

        float upx1 = px;
        float upx2 = px + PIPE_WIDTH;
        float upy1 = 0;
        float upy2 = py - PIPE_GAP / 2;

        float lpx1 = px;
        float lpx2 = px + PIPE_WIDTH;
        float lpy1 = py + PIPE_GAP / 2;
        float lpy2 = GROUND_Y;

        if (bx1 < upx2 && bx2 > upx1 && by1 < upy2 && by2 > upy1) return true;
        if (bx1 < lpx2 && bx2 > lpx1 && by1 < lpy2 && by2 > lpy1) return true;
    }

    return false;
}

static void update_bird(Bird& bird) {
    bird.velocity += GRAVITY;
    bird.y += bird.velocity;
    if (bird.y >= BIRD_GROUND_Y) {
        bird.y = BIRD_GROUND_Y;
        bird.velocity = 0;
    }
}

static void advance_playing(const bool flap[2]) {
    for (int player = 0; player < (racing ? 2 : 1); ++player) {
        if (flap[player]) birds[player].velocity = JUMP_STRENGTH;
        update_bird(birds[player]);
    }

    for (int i = 0; i < MAX_PIPES; i++) {
        pipes[i].x -= 5.0f;

        if (!pipes[i].passed && pipes[i].x + PIPE_WIDTH < BIRD_X) {
            pipes[i].passed = true;
            score++;
        }

        if (pipes[i].x < -PIPE_WIDTH) {
            float max_x = 0;
            for (int j = 0; j < MAX_PIPES; j++) {
                if (pipes[j].x > max_x) max_x = pipes[j].x;
            }
            spawn_pipe(i, max_x + PIPE_SPACING);
        }
    }

    anim_timer++;
    if (anim_timer >= 3) {
        anim_timer = 0;
        anim_frame = (anim_frame + 1) % 3;
    }

    bool first_crashed = check_collision(birds[0]);
    bool second_crashed = racing && check_collision(birds[1]);
    if (first_crashed || second_crashed) {
        // Both collisions use the same completed tick, so simultaneous crashes are a draw.
        if (racing) race_winner = first_crashed == second_crashed ? -1 : (first_crashed ? 1 : 0);
        if (!racing && score > highscore) {
            highscore = score;
            save_highscore();
        }
        current_state = GAME_OVER;
    }
}

void flappy_update(const InputState& input) {
    if (network_game_active() && multiplayer_state() == MultiplayerState::closed) abort_network_game();
    if (multiplayer_take_local_test() && current_state == TITLE && racing) {
        reset_game();
        current_state = PLAYING;
        return;
    }
    const uint8_t actions = CONTROL_A | CONTROL_B | CONTROL_C | CONTROL_D;
    bool flap[2] = {
        (input.controls.pressed & actions) != 0,
        (input.second_player_controls.pressed & actions) != 0
    };
    bool btn_a_pressed = flap[0] || (racing && flap[1]);
    Bird& bird = birds[0];

    if (current_state == TITLE) {
        if (input.controls.pressed & (CONTROL_C | CONTROL_D)) {
            racing = !racing;
        } else if (input.controls.pressed & (CONTROL_A | CONTROL_B)) {
            if (racing) {
                multiplayer_open(FLAPPY_GAME_ID);
                multiplayer_set_receiver(receive_network_packet);
            } else {
                reset_game();
                current_state = PLAYING;
            }
        }
    } else if (current_state == PLAYING) {
        advance_playing(flap);
    } else if (current_state == GAME_OVER) {
        if (!racing && bird.y < BIRD_GROUND_Y) {
            update_bird(bird);
        }
        if (btn_a_pressed) {
            current_state = TITLE;
        }
    }
}

static void publish_state() {
    uint8_t body[MULTIPLAYER_BODY_SIZE] = {};
    for (unsigned i = 0; i < 2; ++i) {
        // Truncate magnitude so quantization cannot round across a rendered pixel.
        write_u16(body + i * 4, uint16_t(int(birds[i].y * 64.0f)));
        float velocity = birds[i].velocity * 256.0f;
        write_u16(body + i * 4 + 2,
                  uint16_t(int(velocity + (velocity < 0 ? -0.5f : 0.5f))));
    }
    for (unsigned i = 0; i < MAX_PIPES; ++i) {
        write_u16(body + 8 + i * 3, uint16_t(int(pipes[i].x)));
        body[10 + i * 3] = uint8_t(pipes[i].gap_y);
    }
    write_u16(body + 17, uint16_t(score));
    body[19] = uint8_t(current_state) | (network_state.paused ? 4 : 0) |
               (network_state.ready[0] ? 8 : 0) | (network_state.ready[1] ? 16 : 0) |
               uint8_t((race_winner + 1) << 5);
    body[20] = uint8_t(anim_frame);
    write_u16(body + 21, network_state.handled_flap);
    body[23] = network_state.handled_control;
    multiplayer_publish(MultiplayerPacketType::state, network_state.tick, body);
}

static bool receive_state(uint16_t tick, const uint8_t* body, uint64_t now_us,
                          FlappyNetworkState next) {
    if (next.have_state && !newer16(tick, next.tick)) return false;
    const unsigned flags = body[19];
    const unsigned phase = flags & 3;
    const unsigned winner = (flags >> 5) & 3;
    if ((flags & 128) || phase > GAME_OVER || winner > 2 || body[20] > 2) return false;
    if ((phase != GAME_OVER && winner) || (phase != PLAYING && (flags & 4)) ||
        (phase == PLAYING && (flags & 24))) return false;

    Bird next_birds[2];
    Pipe next_pipes[MAX_PIPES];
    for (unsigned i = 0; i < 2; ++i) {
        next_birds[i] = {read_i16(body + i * 4) / 64.0f,
                         read_i16(body + i * 4 + 2) / 256.0f};
        if (next_birds[i].y < -SPRITE_SIZE || next_birds[i].y > BIRD_GROUND_Y ||
            next_birds[i].velocity < -11.0f || next_birds[i].velocity > 64.0f) return false;
    }
    const int margin = PIPE_GAP / 2 + 20;
    for (unsigned i = 0; i < MAX_PIPES; ++i) {
        int x = read_i16(body + 8 + i * 3);
        int gap = body[10 + i * 3];
        if (x < -PIPE_WIDTH || x > DISPLAY_WIDTH + (MAX_PIPES - 1) * PIPE_SPACING ||
            gap < margin || gap > GROUND_Y - margin) return false;
        next_pipes[i] = {float(x), float(gap), false};
    }

    uint16_t flap_ack = read_u16(body + 21);
    unsigned consumed = uint16_t(flap_ack - next.handled_flap);
    if (consumed > next.flap_count) return false;
    if (consumed && next.flaps[consumed - 1].id != flap_ack) return false;
    for (unsigned i = consumed; i < next.flap_count; ++i)
        next.flaps[i - consumed] = next.flaps[i];
    next.flap_count -= consumed;
    next.handled_flap = flap_ack;
    uint8_t control_ack = body[23];
    if (control_ack != next.handled_control) {
        if (!next.control_pending || control_ack != next.control_id ||
            !newer8(control_ack, next.handled_control)) return false;
        next.control_pending = false;
        next.handled_control = control_ack;
    }

    next.tick = tick;
    next.have_state = true;
    next.state_time = now_us;
    next.scene_initialized = true;
    next.paused = (flags & 4) != 0;
    next.ready[0] = (flags & 8) != 0;
    next.ready[1] = (flags & 16) != 0;
    network_state = next;
    for (unsigned i = 0; i < 2; ++i) birds[i] = next_birds[i];
    for (unsigned i = 0; i < MAX_PIPES; ++i) pipes[i] = next_pipes[i];
    current_state = GameState(phase);
    racing = true;
    race_winner = int(winner) - 1;
    score = read_u16(body + 17);
    anim_frame = body[20];
    publish_input();
    return true;
}

static bool receive_input(uint16_t tick, const uint8_t* body, FlappyNetworkState next) {
    unsigned count = body[0];
    unsigned kind = body[2];
    if (count > 4 || kind > 2 || body[3] > 1 || newer16(tick, next.tick)) return false;
    if ((!kind && (body[1] || body[3])) || (kind == 1 && body[3] != 1)) return false;
    for (unsigned i = 4 + count * 4; i < MULTIPLAYER_BODY_SIZE; ++i)
        if (body[i]) return false;

    uint16_t previous_id = 0;
    for (unsigned i = 0; i < count; ++i) {
        uint16_t id = read_u16(body + 4 + i * 4);
        uint16_t origin = read_u16(body + 6 + i * 4);
        if ((i && id != uint16_t(previous_id + 1)) || newer16(origin, tick)) return false;
        previous_id = id;
        if (id == next.handled_flap || newer16(next.handled_flap, id)) continue;
        unsigned offset = uint16_t(id - next.handled_flap - 1);
        if (offset < next.flap_count) {
            if (next.flaps[offset].id != id || next.flaps[offset].tick != origin) return false;
        } else {
            if (offset != next.flap_count || next.flap_count == 4) return false;
            next.flaps[next.flap_count++] = {
                id, origin, current_state == PLAYING && !next.paused
            };
        }
    }
    if (kind) {
        uint8_t id = body[1];
        if (id != next.handled_control && !newer8(next.handled_control, id)) {
            if (id != uint8_t(next.handled_control + 1)) return false;
            if (next.control_pending) {
                if (next.control_id != id || next.control_kind != kind ||
                    next.control_value != bool(body[3])) return false;
            } else {
                next.control_pending = true;
                next.control_id = id;
                next.control_kind = uint8_t(kind);
                next.control_value = body[3] != 0;
                next.control_phase = current_state;
                next.control_allowed = uint16_t(next.tick - tick) <= next.phase_age;
            }
        }
    }
    network_state = next;
    return true;
}

static bool receive_network_packet(MultiplayerPacketType type, uint16_t tick,
                         const uint8_t* body, uint64_t now_us) {
    FlappyNetworkState next = network_state_for_session();
    if (!next.session) return false;
    if (next.role == MultiplayerRole::joiner && type == MultiplayerPacketType::state)
        return receive_state(tick, body, now_us, next);
    if (next.role == MultiplayerRole::host && type == MultiplayerPacketType::input)
        return receive_input(tick, body, next);
    return false;
}

static void queue_control(uint8_t kind, bool value) {
    if (network_state.control_pending) return;
    network_state.control_pending = true;
    network_state.control_id = uint8_t(network_state.next_control + 1);
    network_state.next_control = network_state.control_id;
    network_state.control_kind = kind;
    network_state.control_value = value;
}

void flappy_multiplayer_update(const InputState& input) {
    if (multiplayer_state() != MultiplayerState::connected) {
        abort_network_game();
        return;
    }
    network_state = network_state_for_session();
    if (!network_state.session) return;
    racing = true;
    if (!network_state.scene_initialized) {
        reset_game();
        current_state = TITLE;
        anim_frame = 0;
        anim_timer = 0;
        network_state.scene_initialized = true;
    }
    const uint8_t pressed = input.controls.pressed;
    const bool ready_press = (pressed & (CONTROL_A | CONTROL_B)) != 0;
    const bool pause_press = (pressed & CONTROL_E) != 0;
    const bool flap_press = (pressed & (CONTROL_A | CONTROL_B | CONTROL_C | CONTROL_D)) != 0;
    if (network_state.role == MultiplayerRole::joiner) {
        if (!network_state.have_state) return;
        if (current_state != PLAYING) {
            if (ready_press && !network_state.ready[1]) queue_control(1, true);
        } else if (pause_press) {
            queue_control(2, !network_state.paused);
        } else if (!network_state.paused && flap_press && network_state.flap_count < 4 &&
                   multiplayer_now_us() - network_state.state_time <= 100000) {
            uint16_t id = uint16_t(network_state.next_flap + 1);
            network_state.next_flap = id;
            network_state.flaps[network_state.flap_count++] = {id, network_state.tick};
        }
        publish_input();
        return;
    }
    if (network_state.role != MultiplayerRole::host) return;

    network_state.tick = uint16_t(network_state.tick + 1);
    if (network_state.phase_age < 65535) ++network_state.phase_age;
    if (network_state.flap_floor_active && uint16_t(network_state.tick - network_state.flap_floor) > 3)
        network_state.flap_floor_active = false;
    const bool was_paused = network_state.paused;
    if (network_state.control_pending) {
        if (network_state.control_allowed && network_state.control_phase == current_state) {
            if (network_state.control_kind == 1 && current_state != PLAYING) network_state.ready[1] = true;
            if (network_state.control_kind == 2 && current_state == PLAYING)
                network_state.paused = network_state.control_value;
        }
        network_state.handled_control = network_state.control_id;
        network_state.control_pending = false;
    }
    if (current_state != PLAYING) {
        discard_flaps();
        if (ready_press) network_state.ready[0] = true;
        if (network_state.ready[0] && network_state.ready[1]) {
            reset_game();
            current_state = PLAYING;
            network_state.ready[0] = network_state.ready[1] = false;
            network_state.paused = false;
            network_state.flap_floor = network_state.tick;
            network_state.flap_floor_active = true;
            network_state.phase_age = 0;
        }
        publish_state();
        return;
    }
    if (pause_press) network_state.paused = !was_paused;
    // A fresh round or resume must not replay flaps from the preceding frozen state.
    if (network_state.paused || was_paused) {
        discard_flaps();
        network_state.flap_floor = network_state.tick;
        network_state.flap_floor_active = true;
        if (network_state.paused) {
            publish_state();
            return;
        }
    }

    bool flap[2] = {flap_press && !pause_press && !was_paused, false};
    // Discards are acknowledged in order; only one live event advances a bird per tick.
    unsigned consumed = 0;
    while (consumed < network_state.flap_count) {
        const FlapEvent& event = network_state.flaps[consumed++];
        network_state.handled_flap = event.id;
        if (event.allowed && uint16_t(network_state.tick - event.tick) <= 3 &&
            !(network_state.flap_floor_active && newer16(network_state.flap_floor, event.tick))) {
            flap[1] = true;
            break;
        }
    }
    for (unsigned i = consumed; i < network_state.flap_count; ++i)
        network_state.flaps[i - consumed] = network_state.flaps[i];
    network_state.flap_count -= consumed;
    advance_playing(flap);
    if (current_state == GAME_OVER) {
        discard_flaps();
        network_state.phase_age = 0;
    }
    publish_state();
}

void draw_playfield() {
    clear_screen(0x5CBF); // Sky blue background

    if (current_state == PLAYING || current_state == GAME_OVER) {
        for (int i = 0; i < MAX_PIPES; i++) {
            int px = (int)pipes[i].x;
            int py = (int)pipes[i].gap_y;

            int lower_y = py + PIPE_GAP / 2;
            draw_sprite(px, lower_y, pipe_top, 16, 16, SPRITE_SCALE);
            for (int y = lower_y + SPRITE_SIZE; y < GROUND_Y; y += SPRITE_SIZE) {
                draw_sprite(px, y, pipe_middle, 16, 16, SPRITE_SCALE);
            }
            
            int upper_y = py - PIPE_GAP / 2;
            draw_sprite(px, upper_y - SPRITE_SIZE, pipe_bottom, 16, 16, SPRITE_SCALE);
            for (int y = upper_y - 2 * SPRITE_SIZE; y >= -SPRITE_SIZE; y -= SPRITE_SIZE) {
                draw_sprite(px, y, pipe_middle, 16, 16, SPRITE_SCALE);
            }
        }
    }

    draw_filled_rect(0, GROUND_Y, DISPLAY_WIDTH, DISPLAY_HEIGHT - GROUND_Y, 0xDE8B); // Ground
    draw_rect(0, GROUND_Y, DISPLAY_WIDTH, 1, 0x0000);

    const uint16_t* bird_sprites[] = {bird1, bird2, bird3};
    // Draw the opponent first so the normal bird remains visible when both overlap.
    const int local_player = network_game_active() && network_state.role == MultiplayerRole::joiner ? 1 : 0;
    for (int layer = racing ? 1 : 0; layer >= 0; --layer) {
        const int player = network_game_active() ? (layer == 0 ? local_player : 1 - local_player) : layer;
        const Bird& bird = birds[player];
        int frame = current_state == TITLE || bird.velocity >= 0 ? 1 : anim_frame;
        int y = current_state == TITLE ? DISPLAY_HEIGHT / 2 + player * 40 : (int)bird.y;
        draw_sprite(BIRD_X, y, bird_sprites[frame], 16, 16, SPRITE_SCALE, layer == 1);
    }
}

static void draw_centered_text(int y, const char* text, uint16_t color) {
    draw_text((DISPLAY_WIDTH - (int)strlen(text) * 6) / 2, y, text, color);
}

void flappy_draw(const char*) {
    draw_playfield();
    char buffer[32];
    if (current_state == TITLE && network_game_active()) {
        draw_filled_rect(56, 24, 208, 88, 0xFFFF);
        draw_rect(56, 24, 208, 88, 0x0000);
        draw_centered_text(36, "FLAPPY PICO", 0x0000);
        draw_centered_text(54, network_state.ready[0] ? "P1: READY" : "P1: WAITING", 0x0000);
        draw_centered_text(70, network_state.ready[1] ? "P2: READY" : "P2: WAITING", 0x0000);
        draw_centered_text(94, "A/B: READY", 0x0000);
        draw_centered_text(190, network_state.role == MultiplayerRole::host ?
                           "YOU: P1  WAITING FOR PEER" : "YOU: P2  WAITING FOR HOST", 0xFFFF);
    } else if (current_state == TITLE) {
        draw_filled_rect(56, 24, 208, 88, 0xFFFF);
        draw_rect(56, 24, 208, 88, 0x0000);
        draw_centered_text(40, "FLAPPY PICO", 0x0000);
        draw_centered_text(58, racing ? "  SINGLE PLAYER" : "> SINGLE PLAYER", 0x0000);
        draw_centered_text(74, racing ? "> MULTIPLAYER" : "  MULTIPLAYER", 0x0000);
        draw_centered_text(96, "C/D: MODE  A/B: OPEN", 0x0000);
        if (racing) draw_centered_text(174, "HOST/JOIN ON NEXT SCREEN", 0xFFFF);
    } else if (current_state == GAME_OVER) {
        draw_filled_rect(90, 48, 140, 88, 0xFFFF);
        draw_rect(90, 48, 140, 88, 0x0000);
        const char* result = !racing ? "GAME OVER" :
            race_winner < 0 ? "DRAW" : race_winner == 0 ? "PLAYER 1 WINS" : "PLAYER 2 WINS";
        draw_centered_text(64, result, 0x0000);
        snprintf(buffer, sizeof(buffer), "SCORE:%d", score);
        draw_centered_text(88, buffer, 0x0000);
        if (network_game_active()) snprintf(buffer, sizeof(buffer), "A/B: REMATCH READY");
        else if (racing) snprintf(buffer, sizeof(buffer), "SPACE / V: TITLE");
        else snprintf(buffer, sizeof(buffer), "BEST:%d", highscore);
        draw_centered_text(112, buffer, 0x0000);
        if (network_game_active()) {
            snprintf(buffer, sizeof(buffer), "P1:%s  P2:%s",
                     network_state.ready[0] ? "READY" : "WAIT", network_state.ready[1] ? "READY" : "WAIT");
            draw_centered_text(152, buffer, 0xFFFF);
        }
    } else {
        snprintf(buffer, sizeof(buffer), "%d", score);
        draw_centered_text(12, buffer, 0xFFFF);
        if (network_game_active()) {
            draw_text(4, 16, network_state.role == MultiplayerRole::host ? "YOU: P1" : "YOU: P2", 0xFFFF);
            draw_centered_text(198, "A/B/C/D: FLAP  E: PAUSE", 0xFFFF);
            if (network_state.paused) {
                draw_filled_rect(90, 80, 140, 48, 0xFFFF);
                draw_rect(90, 80, 140, 48, 0x0000);
                draw_centered_text(92, "PAUSED", 0x0000);
                draw_centered_text(112, "E: RESUME", 0x0000);
            }
        } else if (racing) {
            draw_text(4, 16, "P1: SPACE", 0xFFFF);
            draw_text(242, 16, "P2: V", 0xFFFF);
        }
    }
}

void flappy_debug() {
    const Bird& bird = birds[0];
    if (current_state != last_debug_state) {
        if (current_state == PLAYING) printf("[STATE] GAME STARTED!\n");
        else if (current_state == GAME_OVER) {
            printf("[STATE] GAME OVER! Score: %d, Highscore: %d\n", score, highscore);
        } else printf("[STATE] TITLE SCREEN\n");
        last_debug_state = current_state;
    }
    if (++debug_frames % 10 == 0 && current_state == PLAYING) {
        printf("[FLAPPY] Y:%.2f VY:%.2f SCORE:%d\n", bird.y, bird.velocity, score);
    }
}
}

extern const Game game_flappy = {
    "Flappy Bird", flappy_init, flappy_update, flappy_draw,
    GROUND_Y, flappy_debug, flappy_multiplayer_update
};
