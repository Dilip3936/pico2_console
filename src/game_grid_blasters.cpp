#include "game.h"
#include "graphics.h"
#include "console.h"
#include "multiplayer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
// Speeds are pixels/second; timings are 30 Hz simulation ticks.
constexpr int STARTING_HEALTH = 3;
constexpr float MOVE_SPEED = 70.0f;
constexpr float BULLET_SPEED = 180.0f;
constexpr float DASH_SPEED = 210.0f;
constexpr int DASH_DURATION_TICKS = 4;
constexpr int DASH_COOLDOWN_TICKS = 36;
constexpr int FIRE_INTERVAL_TICKS = 6;
constexpr int BULLET_LIFETIME_TICKS = 90;
constexpr int HIT_FLASH_TICKS = 5;
constexpr int MAX_BULLETS = 64;
constexpr float PLAYER_HALF_SIZE = 5.0f;
constexpr float BULLET_HALF_SIZE = 1.5f;
constexpr float JOYSTICK_DEADZONE = 0.2f;
constexpr int ARENA_LEFT = 8, ARENA_RIGHT = 312;
constexpr int ARENA_TOP = 28, ARENA_BOTTOM = 224;
constexpr uint16_t BACKGROUND = 0x0841, GRID = 0x1082, WHITE = 0xFFFF, MUTED = 0x8410;
constexpr uint16_t PLAYER_COLORS[] = {0x07FF, 0xFD20};
static_assert(STARTING_HEALTH > 0 && MOVE_SPEED > 0 && BULLET_SPEED > 0 && DASH_SPEED > 0,
              "Invalid Grid Blasters health/speeds");
static_assert(DASH_DURATION_TICKS > 0 && DASH_COOLDOWN_TICKS >= DASH_DURATION_TICKS &&
              FIRE_INTERVAL_TICKS > 0 && BULLET_LIFETIME_TICKS > 0 && MAX_BULLETS > 0,
              "Invalid Grid Blasters timing/capacity");

struct Vec { float x = 0, y = 0; };
struct Wall { int x, y, w, h; };
constexpr Wall WALLS[] = {{150, 92, 20, 68}, {74, 58, 40, 12}, {206, 182, 40, 12}};
struct Player {
    Vec position, aim, dash_direction;
    int health = STARTING_HEALTH;
    int fire_ticks = 0, dash_ticks = 0, cooldown_ticks = 0, flash_ticks = 0;
};
struct Bullet {
    Vec position, velocity;
    int owner = 0, life_ticks = 0;
    uint16_t id = 0;
};
enum class Phase { title, playing, result, ready };
Phase phase;
Player players[2];
Bullet bullets[MAX_BULLETS];

constexpr uint16_t GAME_ID = 0x4742;
constexpr int EVENT_CAPACITY = 128, COUNTDOWN_TICKS = 90;
constexpr uint64_t INPUT_TIMEOUT_US = 100000;
static_assert(ARENA_RIGHT < 512 && ARENA_BOTTOM < 256 && STARTING_HEALTH < 4 &&
              DASH_COOLDOWN_TICKS < 64 && DASH_DURATION_TICKS < 8 &&
              BULLET_LIFETIME_TICKS <= 90 && GAME_FPS == 30 && MULTIPLAYER_BODY_SIZE == 24,
              "Grid Blasters wire fields no longer fit");
static_assert(EVENT_CAPACITY >= MAX_BULLETS + 2, "Event queue cannot reserve a physics tick");
struct Command {
    uint8_t id = 0, kind = 0, desired = 0, round = 0;
    Phase phase = Phase::title;
    uint16_t tick = 0;
    int8_t x = 0, y = 0;
};
struct Event { uint16_t sequence = 0, bullet = 0; bool removal = false; };
struct Network {
    uint32_t session = 0;
    MultiplayerRole role = MultiplayerRole::none;
    uint16_t tick = 0, input_tick = 0, event_sequence = 0, bullet_sequence = 0;
    uint16_t event_ack = 0, received_bullet = 0;
    uint8_t received_flags = 0;
    bool received_event = false, received_removal = false;
    unsigned phase_age = 0;
    uint64_t input_time = 0;
    uint8_t round = 0, ready = 0, command_ack = 0;
    bool initialized = false, have_state = false, have_input = false;
    bool paused = false, flow_paused = false, pending = false, head_published = false;
    Command command;
    int8_t local_x = 0, local_y = 0, remote_x = 0, remote_y = 0;
    uint8_t local_fire = 0, remote_fire = 0;
    int countdown = 0, head = 0, count = 0;
};
Network network;
Event events[EVENT_CAPACITY];
bool receive_packet(MultiplayerPacketType, uint16_t, const uint8_t*, uint64_t);
void publish_input();
bool wireless() { return network.session && network.role != MultiplayerRole::none; }
void enqueue_event(const Bullet& bullet, bool removal) {
    if (!wireless() || network.role != MultiplayerRole::host) return;
    // A whole tick is reserved before physics; no producer runs outside that tick.
    events[(network.head + network.count) % EVENT_CAPACITY] =
        {++network.event_sequence, bullet.id, removal};
    ++network.count;
}
void retire(Bullet& bullet) {
    if (!bullet.life_ticks) return;
    enqueue_event(bullet, true);
    bullet.life_ticks = 0;
}

Vec unit(Vec value) {
    const float length = std::hypot(value.x, value.y);
    return length > 0 ? Vec{value.x / length, value.y / length} : Vec{};
}

float axis(uint16_t raw) {
    const float value = (int(raw) - 2048) / 2048.0f;
    return std::fabs(value) <= JOYSTICK_DEADZONE ? 0 : value;
}

Vec movement(const ControlsState& controls) {
    Vec value = {axis(controls.x), axis(controls.y)};
    return std::hypot(value.x, value.y) > 1 ? unit(value) : value;
}

bool overlaps(Vec position, float half, const Wall& wall) {
    return position.x + half > wall.x && position.x - half < wall.x + wall.w &&
           position.y + half > wall.y && position.y - half < wall.y + wall.h;
}

bool blocked(Vec position, float half) {
    if (position.x - half < ARENA_LEFT || position.x + half > ARENA_RIGHT ||
        position.y - half < ARENA_TOP || position.y + half > ARENA_BOTTOM) return true;
    for (const Wall& wall : WALLS) if (overlaps(position, half, wall)) return true;
    return false;
}

bool player_blocked(Vec position, int player) {
    const Vec other = players[1 - player].position;
    return blocked(position, PLAYER_HALF_SIZE) ||
        (std::fabs(position.x - other.x) < PLAYER_HALF_SIZE * 2 &&
         std::fabs(position.y - other.y) < PLAYER_HALF_SIZE * 2);
}

void centered_text(int y, const char* text, uint16_t color) {
    draw_text((DISPLAY_WIDTH - int(std::strlen(text)) * 6) / 2, y, text, color);
}

void reset_match() {
    players[0] = {};
    players[1] = {};
    players[0].position = {40, 126};
    players[1].position = {280, 126};
    players[0].aim = {1, 0};
    players[1].aim = {-1, 0};
    for (Bullet& bullet : bullets) bullet = {};
}

void init() {
    reset_match();
    phase = Phase::title;
    network = {};
#ifdef PICO_BUILD
    multiplayer_open(GAME_ID);
    multiplayer_set_receiver(receive_packet);
#endif
}

void fire(int player) {
    const Player& ship = players[player];
    for (Bullet& bullet : bullets) {
        if (bullet.life_ticks) continue;
        // Start outside the owner's hull; cover still removes a blocked shot.
        const float muzzle = PLAYER_HALF_SIZE + BULLET_HALF_SIZE + 1;
        bullet.position = {ship.position.x + ship.aim.x * muzzle,
                           ship.position.y + ship.aim.y * muzzle};
        bullet.velocity = {ship.aim.x * BULLET_SPEED, ship.aim.y * BULLET_SPEED};
        bullet.owner = player;
        bullet.life_ticks = BULLET_LIFETIME_TICKS;
        if (wireless()) bullet.id = ++network.bullet_sequence;
        enqueue_event(bullet, false);
        return;
    }
}

Vec prepare_player(int player, const ControlsState& controls, const Vec* requested_dash = nullptr) {
    Player& ship = players[player];
    if (ship.fire_ticks > 0) --ship.fire_ticks;
    if (ship.cooldown_ticks > 0) --ship.cooldown_ticks;
    if (ship.flash_ticks > 0) --ship.flash_ticks;
    const Vec move = movement(controls);
    const Vec dash = requested_dash ? *requested_dash : move;
    if (((controls.pressed & CONTROL_K) || requested_dash) && ship.cooldown_ticks == 0 &&
        (dash.x != 0 || dash.y != 0)) {
        ship.dash_direction = unit(dash);
        ship.dash_ticks = DASH_DURATION_TICKS;
        ship.cooldown_ticks = DASH_COOLDOWN_TICKS;
    }
    const Vec aim = {float(bool(controls.held & CONTROL_B)) - float(bool(controls.held & CONTROL_D)),
                     float(bool(controls.held & CONTROL_C)) - float(bool(controls.held & CONTROL_A))};
    if (aim.x != 0 || aim.y != 0) {
        ship.aim = unit(aim);
        if (!ship.fire_ticks) {
            fire(player);
            ship.fire_ticks = FIRE_INTERVAL_TICKS;
        }
    }
    if (ship.dash_ticks > 0) {
        --ship.dash_ticks;
        return {ship.dash_direction.x * DASH_SPEED, ship.dash_direction.y * DASH_SPEED};
    }
    return {move.x * MOVE_SPEED, move.y * MOVE_SPEED};
}

void move_player(int player, Vec velocity, float dt) {
    Player& ship = players[player];
    Vec next = {ship.position.x + velocity.x * dt, ship.position.y};
    if (!player_blocked(next, player)) ship.position.x = next.x;
    next = {ship.position.x, ship.position.y + velocity.y * dt};
    if (!player_blocked(next, player)) ship.position.y = next.y;
}

void move_bullet(Bullet& bullet, float dt) {
    bullet.position.x += bullet.velocity.x * dt;
    bullet.position.y += bullet.velocity.y * dt;
    if (blocked(bullet.position, BULLET_HALF_SIZE)) {
        retire(bullet);
        return;
    }
    Player& target = players[1 - bullet.owner];
    const float contact = PLAYER_HALF_SIZE + BULLET_HALF_SIZE;
    if (std::fabs(bullet.position.x - target.position.x) < contact &&
        std::fabs(bullet.position.y - target.position.y) < contact) {
        target.health = std::max(0, target.health - 1);
        target.flash_ticks = HIT_FLASH_TICKS;
        retire(bullet);
    }
}

void advance_playing(const InputState& input, const Vec* remote_dash = nullptr) {
    const Vec velocities[] = {prepare_player(0, input.controls),
                              prepare_player(1, input.second_player_controls, remote_dash)};
    // Bound each displacement to one pixel, including dash and opposing motion.
    // Collisions remain reliable without expensive swept geometry or allocations.
    const float fastest = std::max(BULLET_SPEED, std::max(MOVE_SPEED, DASH_SPEED));
    const int steps = std::max(1, int(std::ceil(fastest / GAME_FPS)));
    const float dt = 1.0f / (GAME_FPS * steps);
    for (int step = 0; step < steps; ++step) {
        for (int player = 0; player < 2; ++player) move_player(player, velocities[player], dt);
        for (Bullet& bullet : bullets) if (bullet.life_ticks) move_bullet(bullet, dt);
    }
    for (Bullet& bullet : bullets) if (bullet.life_ticks > 0) {
        if (bullet.life_ticks == 1) retire(bullet);
        else --bullet.life_ticks;
    }
    // Both players' shots finish the tick; simultaneous knockouts are a draw.
    if (!players[0].health || !players[1].health) phase = Phase::result;
}

void update(const InputState& input) {
#ifdef PICO_BUILD
    if (multiplayer_state() == MultiplayerState::closed) console_return_to_selection();
    return;
#endif
    if (phase == Phase::title || phase == Phase::result) {
        if (input.menu.back) {
            if (phase == Phase::title) console_return_to_selection();
            else phase = Phase::title;
        } else if (input.menu.select) {
            reset_match();
            phase = Phase::playing;
        }
        return;
    }
    advance_playing(input);
}

// STATE24: two LE32 players (x9,y8,aim3,hp2,cooldown6,dash3,flash1).
// Byte 8: phase2,ready2,user-pause,flow-pause,event,zero; 9 round,
// 10 command ACK, 11 countdown. Tail: seq16,id16,dir3/owner1,life8,
// x16/y16 in 1/16 pixels,zero16. Without an event only seq baseline remains.
// INPUT24: signed axes8x2,fire4,eventACK16,commandID8,kind8,desired8,
// captured dash axes8x2,origin tick16,round8,phase8,ten zero bytes.
// Event and bullet IDs wrap independently. Only an accepted newer INPUT ACK
// retires a published queue head; RF ACKs never advance this stream.
uint16_t read16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
void write16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
bool newer(uint16_t a, uint16_t b) { return a != b && uint16_t(a - b) < 0x8000; }
int signed8(uint8_t v) { return v < 128 ? v : int(v) - 256; }
bool zeros(const uint8_t* p, int count) {
    for (int i = 0; i < count; ++i) if (p[i]) return false;
    return true;
}
Network session_state() {
    if (network.session == multiplayer_session_id() && network.role == multiplayer_role()) return network;
    Network fresh;
    fresh.session = multiplayer_session_id();
    fresh.role = multiplayer_role();
    return fresh;
}
Vec direction(int index) {
    constexpr float diagonal = 0.70710678118f;
    const Vec directions[] = {{1,0}, {diagonal,diagonal}, {0,1}, {-diagonal,diagonal},
                              {-1,0}, {-diagonal,-diagonal}, {0,-1}, {diagonal,-diagonal}};
    return directions[index];
}
int direction_index(Vec v) {
    int index = int(std::lround(std::atan2(v.y, v.x) * 4 / 3.14159265358979323846));
    return (index + 8) % 8;
}
int8_t quantize(float value) { return int8_t(std::lround(value * 127)); }
int8_t quantize_axis(uint16_t raw) {
    const float value = axis(raw);
    const int quantized = quantize(value);
    // Keep a physically active axis outside the deadzone after reconstruction.
    const int minimum = int(std::floor(JOYSTICK_DEADZONE * 127)) + 1;
    return int8_t(value > 0 ? std::max(minimum, quantized) :
                  value < 0 ? std::min(-minimum, quantized) : 0);
}
ControlsState held_controls(int8_t x, int8_t y, uint8_t fire) {
    ControlsState result = {};
    result.x = uint16_t(std::min(4095L, 2048 + std::lround(x * (2048.0f / 127))));
    result.y = uint16_t(std::min(4095L, 2048 + std::lround(y * (2048.0f / 127))));
    result.held = fire;
    return result;
}
uint32_t pack_player(const Player& p) {
    return uint32_t(std::lround(p.position.x)) | uint32_t(std::lround(p.position.y)) << 9 |
        uint32_t(direction_index(p.aim)) << 17 | uint32_t(p.health) << 20 |
        uint32_t(p.cooldown_ticks) << 22 | uint32_t(p.dash_ticks) << 28 |
        uint32_t(p.flash_ticks != 0) << 31;
}
void publish_state() {
    uint8_t body[24] = {};
    for (int i = 0; i < 2; ++i) {
        const uint32_t packed = pack_player(players[i]);
        for (int j = 0; j < 4; ++j) body[i * 4 + j] = uint8_t(packed >> (j * 8));
    }
    body[8] = uint8_t(int(phase) | network.ready << 2 | (network.paused ? 16 : 0) |
                      (network.flow_paused ? 32 : 0) | (network.count ? 64 : 0));
    body[9] = network.round;
    body[10] = network.command_ack;
    body[11] = uint8_t(network.countdown);
    write16(body + 12, network.event_sequence);
    if (network.count) {
        const Event& event = events[network.head];
        write16(body + 12, event.sequence);
        write16(body + 14, event.bullet);
        // Retries describe the live bullet, not its obsolete muzzle position.
        if (!event.removal) for (const Bullet& bullet : bullets) {
            if (!bullet.life_ticks || bullet.id != event.bullet) continue;
            body[16] = uint8_t(direction_index(bullet.velocity) | bullet.owner << 3);
            body[17] = uint8_t(bullet.life_ticks);
            write16(body + 18, uint16_t(std::lround(bullet.position.x * 16)));
            write16(body + 20, uint16_t(std::lround(bullet.position.y * 16)));
            break;
        }
        network.head_published = true;
    }
    multiplayer_publish(MultiplayerPacketType::state, network.tick, body);
}
void publish_input() {
    uint8_t body[24] = {};
    body[0] = uint8_t(network.local_x); body[1] = uint8_t(network.local_y);
    body[2] = network.local_fire;
    write16(body + 3, network.event_ack);
    if (network.pending) {
        const Command& c = network.command;
        body[5] = c.id; body[6] = c.kind; body[7] = c.desired;
        body[8] = uint8_t(c.x); body[9] = uint8_t(c.y);
        write16(body + 10, c.tick); body[12] = c.round; body[13] = uint8_t(c.phase);
    }
    multiplayer_publish(MultiplayerPacketType::input, network.tick, body);
}
bool same_command(const Command& a, const Command& b) {
    return a.id == b.id && a.kind == b.kind && a.desired == b.desired &&
        a.x == b.x && a.y == b.y && a.tick == b.tick && a.round == b.round && a.phase == b.phase;
}
bool receive_input(uint16_t tick, const uint8_t* body, uint64_t now, Network next) {
    if (!next.initialized || newer(tick, next.tick) ||
        (next.have_input && !newer(tick, next.input_tick)) || body[0] == 128 || body[1] == 128 ||
        body[2] > 15 || !zeros(body + 14, 10)) return false;
    Command c;
    c.id = body[5]; c.kind = body[6]; c.desired = body[7];
    c.x = int8_t(signed8(body[8])); c.y = int8_t(signed8(body[9]));
    c.tick = read16(body + 10); c.round = body[12]; c.phase = Phase(body[13]);
    if (c.kind > 3 || c.desired > 1 || body[8] == 128 || body[9] == 128 || body[13] > 3 ||
        (!c.kind && !zeros(body + 5, 9)) ||
        (c.kind && newer(c.tick, tick)) ||
        (c.kind == 1 && (c.desired != 1 || (c.phase != Phase::ready && c.phase != Phase::result))) ||
        (c.kind >= 2 && c.phase != Phase::playing) ||
        (c.kind != 3 && (c.x || c.y)) || (c.kind == 3 && c.desired)) return false;
    const uint16_t ack = read16(body + 3);
    const bool pop = next.count && next.head_published && ack == events[next.head].sequence;
    if (ack != next.event_ack && !pop) return false;
    if (c.kind && c.id != next.command_ack) {
        if (c.id != uint8_t(next.command_ack + 1) ||
            (next.pending && !same_command(next.command, c))) return false;
        next.pending = true;
        next.command = c;
    }
    if (pop) {
        next.event_ack = ack;
        next.head = (next.head + 1) % EVENT_CAPACITY;
        --next.count;
        next.head_published = false;
    }
    next.remote_x = int8_t(signed8(body[0])); next.remote_y = int8_t(signed8(body[1]));
    next.remote_fire = body[2]; next.input_tick = tick; next.input_time = now; next.have_input = true;
    network = next;
    return true;
}
bool receive_state(uint16_t tick, const uint8_t* body, Network next) {
    if (next.have_state && !newer(tick, next.tick)) return false;
    const Phase incoming = Phase(body[8] & 3);
    const int ready = (body[8] >> 2) & 3, countdown = body[11];
    const bool paused = body[8] & 16, flow = body[8] & 32, present = body[8] & 64;
    const bool reset = !next.have_state || body[9] != next.round;
    if ((body[8] & 128) || !zeros(body + 22, 2) || countdown > COUNTDOWN_TICKS ||
        incoming == Phase::title || ((paused || flow) && incoming != Phase::playing) ||
        (countdown && (incoming != Phase::ready || ready != 3)) ||
        (!countdown && incoming == Phase::ready && ready == 3) ||
        (incoming == Phase::playing && ready) ||
        (reset && next.have_state && body[9] != uint8_t(next.round + 1))) return false;
    Player received[2];
    for (int i = 0; i < 2; ++i) {
        uint32_t p = 0;
        for (int j = 0; j < 4; ++j) p |= uint32_t(body[i * 4 + j]) << (j * 8);
        Player& ship = received[i];
        ship.position = {float(p & 511), float((p >> 9) & 255)};
        ship.aim = direction((p >> 17) & 7); ship.health = (p >> 20) & 3;
        ship.cooldown_ticks = (p >> 22) & 63; ship.dash_ticks = (p >> 28) & 7;
        ship.flash_ticks = p >> 31;
        if (ship.position.x < ARENA_LEFT + PLAYER_HALF_SIZE ||
            ship.position.x > ARENA_RIGHT - PLAYER_HALF_SIZE ||
            ship.position.y < ARENA_TOP + PLAYER_HALF_SIZE ||
            ship.position.y > ARENA_BOTTOM - PLAYER_HALF_SIZE ||
            ship.cooldown_ticks > DASH_COOLDOWN_TICKS || ship.dash_ticks > DASH_DURATION_TICKS ||
            (incoming != Phase::result && !ship.health)) return false;
    }
    if (incoming == Phase::result && received[0].health && received[1].health) return false;
    const uint8_t ack = body[10];
    if (next.pending && ack == next.command.id) next.pending = false;
    else if (ack != next.command_ack) return false;
    const uint16_t sequence = read16(body + 12), id = read16(body + 14);
    const int life = body[17];
    if ((!present && !zeros(body + 14, 10)) ||
        (present && (body[16] > 15 || life > BULLET_LIFETIME_TICKS ||
            (!life && (!zeros(body + 16, 1) || !zeros(body + 18, 4))) ||
            (life && (read16(body + 18) < ARENA_LEFT * 16 || read16(body + 18) > ARENA_RIGHT * 16 ||
                      read16(body + 20) < ARENA_TOP * 16 || read16(body + 20) > ARENA_BOTTOM * 16))))) return false;
    if (!reset && sequence != next.event_ack &&
        (!present || sequence != uint16_t(next.event_ack + 1))) return false;
    if (!reset && present && sequence == next.event_ack &&
        (!next.received_event || id != next.received_bullet ||
         (life && (next.received_removal || body[16] != next.received_flags)))) return false;
    // Determine storage before committing anything, including command/event ACKs.
    int slot = -1;
    if (present && life) {
        for (int i = 0; i < MAX_BULLETS; ++i)
            if (!reset && bullets[i].life_ticks && bullets[i].id == id) { slot = i; break; }
        if (slot < 0) for (int i = 0; i < MAX_BULLETS; ++i)
            if (reset || !bullets[i].life_ticks) { slot = i; break; }
        if (slot < 0) return false;
    }
    if (reset) for (Bullet& bullet : bullets) bullet = {};
    if (present) {
        if (life) {
            Bullet& bullet = bullets[slot];
            bullet.id = id; bullet.owner = (body[16] >> 3) & 1; bullet.life_ticks = life;
            bullet.position = {read16(body + 18) / 16.0f, read16(body + 20) / 16.0f};
            const Vec v = direction(body[16] & 7);
            bullet.velocity = {v.x * BULLET_SPEED, v.y * BULLET_SPEED};
        } else for (Bullet& bullet : bullets) if (bullet.id == id) bullet.life_ticks = 0;
    }
    std::copy(received, received + 2, players);
    phase = incoming; next.round = body[9]; next.ready = uint8_t(ready);
    next.countdown = countdown; next.paused = paused; next.flow_paused = flow;
    next.command_ack = ack; next.event_ack = sequence; next.tick = tick;
    next.received_event = present; next.received_bullet = id;
    next.received_flags = body[16]; next.received_removal = life == 0;
    next.have_state = next.initialized = true;
    network = next;
    // The response slot must be ready before the transport's 5 ms deadline.
    // Do not replace the freshest sampled held axes/fire with command capture.
    publish_input();
    return true;
}
bool receive_packet(MultiplayerPacketType type, uint16_t tick, const uint8_t* body, uint64_t now) {
    const Network next = session_state();
    if (!next.session || !body) return false;
    if (next.role == MultiplayerRole::host && type == MultiplayerPacketType::input)
        return receive_input(tick, body, now, next);
    if (next.role == MultiplayerRole::joiner && type == MultiplayerPacketType::state)
        return receive_state(tick, body, next);
    return false;
}
void queue_command(uint8_t kind, bool desired, Vec dash = {}) {
    if (network.pending) return;
    network.pending = true;
    network.command = {uint8_t(network.command_ack + 1), kind, uint8_t(desired), network.round,
                       phase, network.tick, quantize(dash.x), quantize(dash.y)};
}

void advance_visuals() {
    // Visual shots can hit static cover, never ships; only the host deals damage.
    const int steps = std::max(1, int(std::ceil(BULLET_SPEED / GAME_FPS)));
    for (Bullet& bullet : bullets) if (bullet.life_ticks) {
        for (int i = 0; i < steps && bullet.life_ticks; ++i) {
            bullet.position.x += bullet.velocity.x / (GAME_FPS * steps);
            bullet.position.y += bullet.velocity.y / (GAME_FPS * steps);
            if (blocked(bullet.position, BULLET_HALF_SIZE)) bullet.life_ticks = 0;
        }
        if (bullet.life_ticks) --bullet.life_ticks;
    }
}
void multiplayer_tick(const InputState& input) {
    if (multiplayer_state() != MultiplayerState::connected) {
        network = {}; reset_match(); phase = Phase::title;
        return;
    }
    network = session_state();
    if (!network.session) return;
    if (!network.initialized && network.role == MultiplayerRole::host) {
        reset_match(); phase = Phase::ready; network.initialized = true;
    }
    const uint8_t pressed = input.controls.pressed;
    const bool select = pressed & (CONTROL_B | CONTROL_K);
    const bool pause_press = (pressed & CONTROL_E) || (network.paused && (pressed & (CONTROL_B | CONTROL_K)));
    if ((pressed & CONTROL_D) && (phase != Phase::playing || network.paused || network.flow_paused)) {
        network = {}; reset_match(); phase = Phase::title;
        console_return_to_selection();
        return;
    }
    const Vec move = movement(input.controls);
    if (network.role == MultiplayerRole::joiner) {
        network.local_x = quantize_axis(input.controls.x);
        network.local_y = quantize_axis(input.controls.y);
        network.local_fire = input.controls.held & 15;
        if (!network.have_state) return;
        if ((phase == Phase::ready || phase == Phase::result) && !network.countdown && select && !(network.ready & 2))
            queue_command(1, true);
        else if (phase == Phase::playing && pause_press) queue_command(2, !network.paused);
        else if (phase == Phase::playing && !network.paused && !network.flow_paused && (pressed & CONTROL_K))
            queue_command(3, false, move);
        if (phase == Phase::playing && !network.paused && !network.flow_paused) advance_visuals();
        publish_input();
        return;
    }
    ++network.tick;
    if (network.phase_age < 32767) ++network.phase_age;
    const bool was_paused = network.paused;
    Vec remote_dash = {};
    bool have_dash = false;
    if (network.pending) {
        const Command c = network.command;
        const uint16_t age = uint16_t(network.tick - c.tick);
        if (age < 0x8000 && age <= network.phase_age && c.phase == phase && c.round == network.round) {
            if (c.kind == 1 && (phase == Phase::ready || phase == Phase::result) && !network.countdown)
                network.ready |= 2;
            if (c.kind == 2 && phase == Phase::playing) network.paused = c.desired;
            if (c.kind == 3 && phase == Phase::playing && !was_paused &&
                !network.paused && !network.flow_paused) {
                // Apply after the same cooldown decrement as the local dash.
                remote_dash = {c.x / 127.0f, c.y / 127.0f};
                have_dash = true;
            }
        }
        // Obsolete/blocked commands are consumed exactly once, never carried into a new round.
        network.command_ack = c.id; network.pending = false;
    }
    if (phase == Phase::ready || phase == Phase::result) {
        if (network.countdown) {
            if (--network.countdown == 0) {
                phase = Phase::playing; network.ready = 0; network.phase_age = 0;
                network.remote_x = network.remote_y = 0; network.remote_fire = 0;
            }
        } else {
            if (select) network.ready |= 1;
            if (network.ready == 3) {
                reset_match(); ++network.round; phase = Phase::ready;
                network.countdown = COUNTDOWN_TICKS; network.phase_age = 0;
                // Only a new round discards old events. The monotonic baseline survives.
                network.head = network.count = 0; network.head_published = false;
                network.event_ack = network.event_sequence;
                network.paused = network.flow_paused = false;
            }
        }
    } else if (phase == Phase::playing) {
        // Simultaneous local/remote presses request the same desired state, not two toggles.
        if (pause_press) network.paused = !was_paused;
        if (network.paused != was_paused) {
            network.phase_age = 0;
            network.remote_x = network.remote_y = 0; network.remote_fire = 0;
        }
        if (network.flow_paused && !network.count) network.flow_paused = false;
        if (!network.flow_paused && EVENT_CAPACITY - network.count < MAX_BULLETS + 2)
            network.flow_paused = true;
        if (!was_paused && !network.paused && !network.flow_paused) {
            const uint64_t now = multiplayer_now_us();
            const bool fresh = network.have_input && now >= network.input_time && now - network.input_time < INPUT_TIMEOUT_US;
            InputState controls = {};
            controls.controls = input.controls;
            controls.controls.pressed &= uint8_t(~CONTROL_E);
            controls.second_player_controls = fresh ? held_controls(network.remote_x, network.remote_y, network.remote_fire) :
                                                       held_controls(0, 0, 0);
            advance_playing(controls, have_dash ? &remote_dash : nullptr);
            if (phase == Phase::result) { network.ready = 0; network.phase_age = 0; network.flow_paused = false; }
        }
    }
    publish_state();
}

void draw_player(int player) {
    const Player& ship = players[player];
    const uint16_t color = ship.flash_ticks ? WHITE : PLAYER_COLORS[player];
    const int x = int(ship.position.x), y = int(ship.position.y), half = int(PLAYER_HALF_SIZE);
    if (ship.dash_ticks) draw_rect(x - half - 2, y - half - 2, half * 2 + 4, half * 2 + 4, color);
    draw_filled_rect(x - half, y - half, half * 2, half * 2, color);
    draw_filled_rect(x - 2, y - 2, 4, 4, BACKGROUND);
    for (int i = half; i <= half + 3; ++i)
        draw_filled_rect(x + int(ship.aim.x * i) - 1, y + int(ship.aim.y * i) - 1, 2, 2, color);
}

void draw(const char*) {
    clear_screen(BACKGROUND);
    if (phase == Phase::title) {
#ifdef PICO_BUILD
        centered_text(40, "GRID BLASTERS", WHITE);
        centered_text(80, "HOST P1 / JOIN P2", MUTED);
        centered_text(112, "JOYSTICK: MOVE   K: DASH", WHITE);
        centered_text(136, "A/B/C/D: DIRECTIONAL FIRE", WHITE);
        centered_text(168, "E: PAUSE   F: MENU", MUTED);
#else
        centered_text(18, "GRID BLASTERS", WHITE);
        centered_text(36, "LOCAL TWO PLAYER DUEL", MUTED);
        centered_text(64, "PLAYER 1", PLAYER_COLORS[0]);
        centered_text(80, "MOVE: WASD   FIRE: ARROWS", WHITE);
        centered_text(96, "DASH: LEFT SHIFT", WHITE);
        centered_text(122, "PLAYER 2", PLAYER_COLORS[1]);
        centered_text(138, "MOVE: IJKL   FIRE: T/F/G/H", WHITE);
        centered_text(152, "T UP / F LEFT / G DOWN / H RIGHT", MUTED);
        centered_text(168, "DASH: RIGHT SHIFT", WHITE);
        centered_text(190, "3 HEALTH EACH - FIRST KNOCKOUT WINS", WHITE);
        centered_text(210, "ENTER: START   P: PAUSE", WHITE);
        centered_text(226, "ESC: MENU", MUTED);
#endif
        return;
    }
    for (int x = ARENA_LEFT; x < ARENA_RIGHT; x += 16)
        draw_filled_rect(x, ARENA_TOP, 1, ARENA_BOTTOM - ARENA_TOP, GRID);
    for (int y = ARENA_TOP; y < ARENA_BOTTOM; y += 16)
        draw_filled_rect(ARENA_LEFT, y, ARENA_RIGHT - ARENA_LEFT, 1, GRID);
    draw_rect(ARENA_LEFT - 1, ARENA_TOP - 1, ARENA_RIGHT - ARENA_LEFT + 2,
              ARENA_BOTTOM - ARENA_TOP + 2, MUTED);
    for (const Wall& wall : WALLS) {
        draw_filled_rect(wall.x, wall.y, wall.w, wall.h, 0x3186);
        draw_rect(wall.x, wall.y, wall.w, wall.h, MUTED);
    }
    for (int player = 0; player < 2; ++player) {
        const Player& ship = players[player];
        const int x = player == 0 ? 12 : 184;
        char label[24];
        std::snprintf(label, sizeof(label), "P%d HP %d", player + 1, ship.health);
        draw_text(x, 16, label, PLAYER_COLORS[player]);
        draw_text(x + 52, 16, "DASH", MUTED);
        const int ready = 36 * (DASH_COOLDOWN_TICKS - ship.cooldown_ticks) / DASH_COOLDOWN_TICKS;
        draw_rect(x + 80, 15, 38, 8, MUTED);
        if (ready > 0) draw_filled_rect(x + 81, 16, ready, 6, PLAYER_COLORS[player]);
        draw_player(player);
    }
    for (const Bullet& bullet : bullets) if (bullet.life_ticks)
        draw_filled_rect(int(bullet.position.x) - 1, int(bullet.position.y) - 1, 3, 3,
                         PLAYER_COLORS[bullet.owner]);
#ifdef PICO_BUILD
    centered_text(232, "K: DASH   E: PAUSE   F: MENU", MUTED);
#else
    centered_text(232, "P: PAUSE   ESC: MENU", MUTED);
#endif
    if (phase == Phase::ready) {
        draw_filled_rect(36, 82, 248, 90, BACKGROUND);
        draw_rect(36, 82, 248, 90, WHITE);
        if (network.countdown) {
            char label[24];
            std::snprintf(label, sizeof(label), "START IN %d", (network.countdown + GAME_FPS - 1) / GAME_FPS);
            centered_text(112, label, WHITE);
        } else {
            centered_text(94, "WIRELESS DUEL - READY?", WHITE);
            centered_text(114, network.ready & 1 ? "HOST P1: READY" : "HOST P1: WAITING", PLAYER_COLORS[0]);
            centered_text(132, network.ready & 2 ? "JOIN P2: READY" : "JOIN P2: WAITING", PLAYER_COLORS[1]);
            centered_text(154, "B/K: READY   D: EXIT", MUTED);
        }
    }
    if (phase == Phase::playing && (network.paused || network.flow_paused)) {
        draw_filled_rect(36, 92, 248, 68, BACKGROUND);
        draw_rect(36, 92, 248, 68, WHITE);
        centered_text(106, network.paused ? "PAUSED" : "SYNCING BULLET EVENTS", WHITE);
        centered_text(136, network.paused ? "B/K/E: RESUME   D: EXIT" : "PLEASE WAIT   D: EXIT", MUTED);
    }
    if (phase == Phase::result) {
        draw_filled_rect(42, 86, 236, 76, BACKGROUND);
        draw_rect(42, 86, 236, 76, WHITE);
        centered_text(100, !players[0].health && !players[1].health ? "DRAW" :
                      players[0].health ? "PLAYER 1 WINS" : "PLAYER 2 WINS", WHITE);
#ifdef PICO_BUILD
        centered_text(120, network.ready & 1 ? "HOST: READY" : "HOST: WAITING", PLAYER_COLORS[0]);
        centered_text(132, network.ready & 2 ? "JOIN: READY" : "JOIN: WAITING", PLAYER_COLORS[1]);
        centered_text(148, "B/K: REMATCH   D: EXIT", MUTED);
#else
        centered_text(124, "ENTER: REMATCH", WHITE);
        centered_text(146, "LEFT ARROW: BACK", MUTED);
#endif
    }
}
}

extern const Game game_grid_blasters = {"Grid Blasters", init, update, draw, 0, nullptr, multiplayer_tick};
