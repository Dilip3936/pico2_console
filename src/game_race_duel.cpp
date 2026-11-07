// Landscape port of assets/racer_duo, using the console's shared drivers and NRF transport.
#include "game.h"
#include "graphics.h"
#include "console.h"
#include "font.h"
#include "racer_assets.h"
#include "multiplayer.h"
#ifdef PICO_BUILD
#include <pico/stdlib.h>
#endif
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>

namespace {
constexpr int SCREEN_W = DISPLAY_WIDTH, SCREEN_H = DISPLAY_HEIGHT;
constexpr uint16_t RGB565(int r, int g, int b) {
    return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
std::minstd_rand random_engine;
uint32_t RAND32() { return random_engine(); }

#define FPS            GAME_FPS
#define FP_SHIFT       8
#define FP(x)          ((int32_t)((x) * (1 << FP_SHIFT)))

#define LANES          4
#define LANE_W         40
#define ROAD_L         80
#define ROAD_R         (ROAD_L + LANES * LANE_W)
#define CAR_W          18
#define CAR_H          30
#define CAR_Y          (SCREEN_H - 70)
#define MAX_STEER      FP(5)      /* pixels per frame at full deflection */

#define MAX_TRAFFIC    8
#define SPAWN_SPACING  110        /* vertical pixels between traffic rows */
#define START_LIVES    3
#define INVULN_FRAMES  60
#define OVER_DELAY     45

/* Speeds are px/frame (fixed point). Each car has its own speed. */
#define SPEED_START    FP(3)
#define SPEED_MIN      FP(1.5)    /* a braking car never stops completely */
#define SPEED_MAX      FP(8)      /* top speed without boost */
#define SPEED_BOOST_MAX FP(12)    /* top speed while boosting */
#define ACCEL          FP(0.06)   /* per frame at full throttle (~3 s to top speed) */
#define BRAKE          FP(0.25)   /* per frame at full brake */
#define DRAG           FP(0.02)   /* per frame when the stick is centred */
#define BOOST_ACCEL    FP(0.4)    /* extra push per frame while boosting */
#define BOOST_DECAY    FP(0.1)    /* per frame back to SPEED_MAX after boost */
#define BOOST_FRAMES   90         /* 3 s of boost per bottle */
#define KNOCK_VX       7          /* px/frame a knocked-out traffic car flies sideways */
#define KMH_PER_PX     20         /* display only: px/frame -> km/h */

/* A faster car pulls ahead on screen, a slower one drops back. The road
   scrolls at the average speed of the two cars. */
#define OFF_MIN        FP(-35)    /* furthest back (px below CAR_Y) */
#define OFF_MAX        FP(90)     /* furthest ahead (px above CAR_Y) */

/* Boost bottles sit in the gap between two traffic rows */
#define MAX_BOTTLES    3
#define BOTTLE_W       10
#define BOTTLE_H       18
#define BOTTLE_CHANCE  3          /* 1 in N traffic rows gets a bottle */

typedef enum { R_LOBBY, R_COUNTDOWN, R_RACING, R_OVER } RaceState;

typedef struct {
    int32_t x;          /* fixed point 24.8, left edge of the car */
    int16_t y;          /* top edge in pixels (moves with speed vs. the road) */
    uint8_t lives;
    uint8_t invuln;     /* frames of protection left after a crash */
    bool    ready;
    uint8_t boost;      /* frames of boost left */
    uint8_t kmh;        /* display only */
    int32_t speed_fp, off_fp;   /* px/frame, offset from CAR_Y */
} Car;

typedef struct {
    int16_t x, y;
    bool    active;
    bool    knocked;    /* hit by a boosting car: flies off the road, harmless */
    int8_t  vx;         /* sideways speed while knocked */
} Traffic;
typedef struct { int16_t x, y; bool active; } Bottle;

/* steer -127..127; throttle -127 (full brake) .. +127 (full throttle) */
typedef struct { int8_t steer, throttle; bool button; } PlayerInput;

typedef struct {
    RaceState state;
    uint8_t   countdown;       /* 3,2,1 */
    int8_t    winner;          /* -1 none, 0 = P1, 1 = P2, 2 = draw */
    uint8_t   road_off;        /* 0..39 road marking scroll */
    uint8_t   speed;           /* road scroll, px/frame (display only) */
    uint16_t  meters;
    Car       car[2];
    Traffic   traffic[MAX_TRAFFIC];
    Bottle    bottle[MAX_BOTTLES];

    /* Simulation-only accumulators. */
    uint32_t  speed_fp, scroll_fp, dist_px, frames, spawn_px;
    int       timer, last_lane;
    bool      prev_btn[2];
} Race;

Race race;
constexpr uint16_t RACE_GAME_ID = 4;
constexpr uint64_t INPUT_TIMEOUT_US = 100000;
struct Command {
    uint8_t id = 0, kind = 0, value = 0, round = 0;
    RaceState phase = R_LOBBY;
    uint16_t tick = 0;
};
struct Network {
    uint32_t session = 0;
    MultiplayerRole role = MultiplayerRole::none;
    uint16_t tick = 0, input_tick = 0;
    unsigned phase_age = 0;
    uint64_t input_time = 0, state_time = 0;
    uint8_t round = 0, acknowledged = 0;
    bool initialized = false, have_input = false, paused = false, pending = false;
    PlayerInput local = {}, remote = {};
    Command command;
};
Network network;
bool receive_packet(MultiplayerPacketType, uint16_t, const uint8_t*, uint64_t);

#define X_MIN FP(ROAD_L + 3)
#define X_MAX FP(ROAD_R - CAR_W - 3)
#define LANE_X(l) (ROAD_L + (l) * LANE_W + (LANE_W - CAR_W) / 2)

static void place_cars(Race *r) {
    r->car[0].x = FP(LANE_X(1));
    r->car[1].x = FP(LANE_X(2));
    for (int i = 0; i < 2; i++) {
        r->car[i].y = CAR_Y;
        r->car[i].speed_fp = SPEED_START;
        r->car[i].off_fp = 0;
        r->car[i].boost = 0;
        r->car[i].kmh = (uint8_t)((SPEED_START * KMH_PER_PX) >> FP_SHIFT);
    }
}

void race_init(Race *r) {
    memset(r, 0, sizeof *r);
    r->state = R_LOBBY;
    r->winner = -1;
    r->last_lane = 1;
    r->speed_fp = SPEED_START;
    r->car[0].lives = r->car[1].lives = START_LIVES;
    place_cars(r);
}

static void start_countdown(Race *r) {
    memset(r->traffic, 0, sizeof r->traffic);
    memset(r->bottle, 0, sizeof r->bottle);
    for (int i = 0; i < 2; i++) {
        r->car[i].lives = START_LIVES;
        r->car[i].invuln = 0;
        r->car[i].ready = false;
    }
    place_cars(r);
    r->frames = 0;
    r->speed_fp = SPEED_START;
    r->scroll_fp = r->dist_px = r->spawn_px = 0;
    r->meters = 0;
    r->winner = -1;
    r->timer = 3 * FPS;
    r->countdown = 3;
    r->state = R_COUNTDOWN;
}

static void move_car(Car *c, int8_t steer) {
    c->x += ((int32_t)steer * MAX_STEER) / 127;
    if (c->x < X_MIN) c->x = X_MIN;
    if (c->x > X_MAX) c->x = X_MAX;
}

static void spawn_traffic(Race *r) {
    int lane = r->last_lane + (int)(RAND32() % 5) - 2;
    if (lane < 0) lane = 0;
    if (lane > LANES - 1) lane = LANES - 1;
    r->last_lane = lane;
    for (int i = 0; i < MAX_TRAFFIC; i++) {
        if (!r->traffic[i].active) {
            r->traffic[i].active = true;
            r->traffic[i].knocked = false;
            r->traffic[i].vx = 0;
            r->traffic[i].x = (int16_t)LANE_X(lane);
            r->traffic[i].y = -CAR_H;
            return;
        }
    }
}

static void spawn_bottle(Race *r) {
    for (int i = 0; i < MAX_BOTTLES; i++) {
        if (!r->bottle[i].active) {
            int lane = (int)(RAND32() % LANES);
            r->bottle[i].active = true;
            r->bottle[i].x = (int16_t)(ROAD_L + lane * LANE_W + (LANE_W - BOTTLE_W) / 2);
            /* centred half a row above the car that was just spawned, so the
               bottle never overlaps a traffic car */
            r->bottle[i].y = (int16_t)(-(CAR_H / 2) - SPAWN_SPACING / 2 - BOTTLE_H / 2);
            return;
        }
    }
}

static bool overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

/* throttle > 0 accelerates, < 0 brakes, 0 coasts. A boost overrides it. */
static void update_speed(Car *c, int8_t throttle) {
    int32_t s = c->speed_fp;
    if (c->boost) {
        c->boost--;
        s += BOOST_ACCEL;
        if (s > SPEED_BOOST_MAX) s = SPEED_BOOST_MAX;
    } else {
        if (s > SPEED_MAX) {
            s -= BOOST_DECAY;                       /* boost wore off */
            if (s < SPEED_MAX) s = SPEED_MAX;
        }
        if (throttle > 0) {
            if (s < SPEED_MAX) {
                s += ((int32_t)throttle * ACCEL) / 127;
                if (s > SPEED_MAX) s = SPEED_MAX;
            }
        } else if (throttle < 0) {
            s += ((int32_t)throttle * BRAKE) / 127;
        } else {
            s -= DRAG;
        }
        if (s < SPEED_MIN) s = SPEED_MIN;
    }
    c->speed_fp = s;
    c->kmh = (uint8_t)((s * KMH_PER_PX) >> FP_SHIFT);
}

static bool hit(int ax, int ay, int bx, int by) {
    const int m = 3;                       /* forgiving hitbox */
    return ax + m < bx + CAR_W - m && ax + CAR_W - m > bx + m &&
           ay + m < by + CAR_H - m && ay + CAR_H - m > by + m;
}

static void update_racing(Race *r, const PlayerInput in[2]) {
    r->frames++;
    update_speed(&r->car[0], in[0].throttle);
    update_speed(&r->car[1], in[1].throttle);
    r->speed_fp = (uint32_t)((r->car[0].speed_fp + r->car[1].speed_fp) / 2);
    r->speed = (uint8_t)(r->speed_fp >> FP_SHIFT);

    r->scroll_fp += r->speed_fp;
    int step = (int)(r->scroll_fp >> FP_SHIFT);
    r->scroll_fp &= 0xFF;

    r->road_off = (uint8_t)((r->road_off + step) % 40);
    r->dist_px += (uint32_t)step;
    uint32_t m = r->dist_px / 10;
    r->meters = m > 65535 ? 65535 : (uint16_t)m;

    for (int i = 0; i < MAX_TRAFFIC; i++) {
        if (!r->traffic[i].active) continue;
        r->traffic[i].y = (int16_t)(r->traffic[i].y + step);
        if (r->traffic[i].knocked) {
            r->traffic[i].x = (int16_t)(r->traffic[i].x + r->traffic[i].vx);
            if (r->traffic[i].x < -CAR_W || r->traffic[i].x > SCREEN_W) r->traffic[i].active = false;
        }
        if (r->traffic[i].y > SCREEN_H) r->traffic[i].active = false;
    }
    r->spawn_px += (uint32_t)step;
    if (r->spawn_px >= SPAWN_SPACING) {
        r->spawn_px -= SPAWN_SPACING;
        spawn_traffic(r);
        if (RAND32() % BOTTLE_CHANCE == 0) spawn_bottle(r);
    }
    for (int i = 0; i < MAX_BOTTLES; i++) {
        if (!r->bottle[i].active) continue;
        r->bottle[i].y = (int16_t)(r->bottle[i].y + step);
        if (r->bottle[i].y > SCREEN_H) r->bottle[i].active = false;
    }

    for (int p = 0; p < 2; p++) {
        Car *c = &r->car[p];
        move_car(c, in[p].steer);

        /* faster than the road scroll = creep forward on screen */
        c->off_fp += c->speed_fp - (int32_t)r->speed_fp;
        if (c->off_fp < OFF_MIN) c->off_fp = OFF_MIN;
        if (c->off_fp > OFF_MAX) c->off_fp = OFF_MAX;
        c->y = (int16_t)(CAR_Y - (c->off_fp >> FP_SHIFT));
        int cx = (int)(c->x >> FP_SHIFT);

        for (int i = 0; i < MAX_BOTTLES; i++) {      /* works even while invulnerable */
            Bottle *b = &r->bottle[i];
            if (b->active && overlap(cx, c->y, CAR_W, CAR_H, b->x, b->y, BOTTLE_W, BOTTLE_H)) {
                b->active = false;
                c->boost = BOOST_FRAMES;
            }
        }

        bool protect = c->invuln > 0;
        if (protect) c->invuln--;
        for (int i = 0; i < MAX_TRAFFIC; i++) {
            Traffic *t = &r->traffic[i];
            if (!t->active || t->knocked || !hit(cx, c->y, t->x, t->y)) continue;
            if (c->boost) {                          /* boosting: smash it aside, no damage */
                t->knocked = true;
                t->vx = (int8_t)(t->x >= cx ? KNOCK_VX : -KNOCK_VX);
            } else if (!protect) {
                if (c->lives > 0) c->lives--;
                c->invuln = INVULN_FRAMES;
                t->active = false;
                break;
            }
        }
    }

    bool a0 = r->car[0].lives > 0, a1 = r->car[1].lives > 0;

    if (!a0 || !a1) {
        r->state = R_OVER;
        r->winner = (a0 && !a1) ? 0 : (a1 && !a0) ? 1 : 2;
        r->timer = OVER_DELAY;
        r->car[0].ready = r->car[1].ready = false;
    }
}

void race_update(Race *r, const PlayerInput in_[2]) {
    PlayerInput in[2] = { in_[0], in_[1] };

    bool edge[2];
    for (int i = 0; i < 2; i++) {
        edge[i] = in[i].button && !r->prev_btn[i];
        r->prev_btn[i] = in[i].button;
    }

    switch (r->state) {
    case R_LOBBY:
    case R_OVER:
        if (r->state == R_OVER && r->timer > 0) { r->timer--; break; }
        if (edge[0]) r->car[0].ready = true;
        if (edge[1]) r->car[1].ready = true;
        if (r->car[0].ready && r->car[1].ready) start_countdown(r);
        else if (r->state == R_LOBBY) r->road_off = (uint8_t)((r->road_off + 2) % 40);
        break;

    case R_COUNTDOWN:
        move_car(&r->car[0], in[0].steer);
        move_car(&r->car[1], in[1].steer);
        r->timer--;
        r->countdown = (uint8_t)((r->timer + FPS - 1) / FPS);
        if (r->timer <= 0) r->state = R_RACING;
        break;

    case R_RACING:
        update_racing(r, in);
        break;
    }
}

static uint16_t GRASS, GRASS_DARK, ROAD, LINE, EDGE, WHITE, YELLOW, BLACK,
                PANEL, RED, BLUE, WINDOW, TYRE, EMPTY, BOTTLE, FLAME;
static uint16_t SHADOW, RIM, HEADL, TAIL, GLASS_HI, SPARK;

typedef struct { uint16_t body, dark, light; } Pal;     /* paint + shade + highlight */
static Pal PAL_PLAYER[2], PAL_TRAFFIC[LANES], PAL_WRECK;

static int lighten(int v) { return v + 70 > 255 ? 255 : v + 70; }
static Pal make_pal(int r, int g, int b) {
    Pal p = { RGB565(r, g, b),
              RGB565(r * 6 / 10, g * 6 / 10, b * 6 / 10),
              RGB565(lighten(r), lighten(g), lighten(b)) };
    return p;
}

void render_init(void) {
    GRASS      = RGB565(40, 130, 50);
    GRASS_DARK = RGB565(28, 100, 38);
    ROAD       = RGB565(70, 70, 76);
    LINE       = RGB565(230, 230, 230);
    EDGE       = RGB565(240, 240, 240);
    WHITE      = RGB565(255, 255, 255);
    YELLOW     = RGB565(255, 220, 40);
    BLACK      = RGB565(0, 0, 0);
    PANEL      = RGB565(20, 20, 30);
    RED        = RGB565(220, 40, 40);
    BLUE       = RGB565(50, 100, 235);
    WINDOW     = RGB565(170, 220, 240);
    TYRE       = RGB565(15, 15, 15);
    EMPTY      = RGB565(90, 90, 100);
    BOTTLE     = RGB565(0, 200, 255);
    FLAME      = RGB565(255, 120, 20);
    SHADOW     = RGB565(48, 48, 54);
    RIM        = RGB565(170, 170, 180);
    HEADL      = RGB565(255, 250, 200);
    TAIL       = RGB565(235, 30, 30);
    GLASS_HI   = RGB565(235, 248, 255);
    SPARK      = RGB565(255, 235, 120);
    PAL_PLAYER[0]  = make_pal(215, 35, 35);
    PAL_PLAYER[1]  = make_pal(45, 95, 230);
    PAL_TRAFFIC[0] = make_pal(60, 165, 90);
    PAL_TRAFFIC[1] = make_pal(235, 145, 40);
    PAL_TRAFFIC[2] = make_pal(160, 80, 195);
    PAL_TRAFFIC[3] = make_pal(185, 190, 200);
    PAL_WRECK      = make_pal(75, 65, 62);
}

/* Top-down car, nose up. All parts are small rectangles so it stays cheap. */
static void draw_car(int x, int y, const Pal *c, bool sporty, bool rack) {
    const int W = CAR_W, H = CAR_H;
    draw_filled_rect(x - 1, y + 3, W + 3, H, SHADOW);                 /* ground shadow */

    for (int i = 0; i < 4; i++) {                                  /* wheels + rims */
        int wx = (i & 1) ? x + W - 1 : x - 2;
        int wy = (i & 2) ? y + 20 : y + 4;
        draw_filled_rect(wx, wy, 3, 7, TYRE);
        draw_filled_rect(wx + 1, wy + 2, 1, 3, RIM);
    }

    draw_filled_rect(x + 4, y,         W - 8, 1, c->body);            /* rounded nose */
    draw_filled_rect(x + 2, y + 1,     W - 4, 1, c->body);
    draw_filled_rect(x,     y + 2,     W, H - 5, c->body);            /* main body */
    draw_filled_rect(x + 1, y + H - 3, W - 2, 2, c->body);            /* rounded tail */
    draw_filled_rect(x + 2, y + H - 1, W - 4, 1, c->dark);            /* rear bumper */
    draw_filled_rect(x,         y + 4, 1, H - 8, c->dark);            /* side shading */
    draw_filled_rect(x + W - 1, y + 4, 1, H - 8, c->dark);

    draw_filled_rect(x + 5, y + 2, W - 10, 3, c->light);              /* hood highlight */
    draw_filled_rect(x + 4, y + 6, W - 8, 1, WINDOW);                 /* windscreen (tapered) */
    draw_filled_rect(x + 3, y + 7, W - 6, 4, WINDOW);
    draw_filled_rect(x + 4, y + 7, 3, 1, GLASS_HI);                   /* glare */
    draw_filled_rect(x + 3, y + 11, W - 6, 8, c->light);              /* roof */
    draw_filled_rect(x + 3, y + 19, W - 6, 2, WINDOW);                /* rear window */
    draw_filled_rect(x + 4, y + 21, W - 8, 1, WINDOW);

    draw_filled_rect(x - 1,     y + 11, 2, 2, c->dark);               /* mirrors */
    draw_filled_rect(x + W - 1, y + 11, 2, 2, c->dark);

    if (sporty) {                                                  /* racing stripes + spoiler */
        draw_filled_rect(x + 7,     y + 2,  1, 3, WHITE);
        draw_filled_rect(x + W - 8, y + 2,  1, 3, WHITE);
        draw_filled_rect(x + 7,     y + 12, 1, 6, WHITE);
        draw_filled_rect(x + W - 8, y + 12, 1, 6, WHITE);
        draw_filled_rect(x + 1,     y + H - 7, W - 2, 2, c->dark);
    }
    if (rack) {                                                    /* roof rack */
        draw_filled_rect(x + 3, y + 13, W - 6, 1, c->dark);
        draw_filled_rect(x + 3, y + 17, W - 6, 1, c->dark);
    }

    draw_filled_rect(x + 2,     y + 1,     3, 2, HEADL);             /* headlights */
    draw_filled_rect(x + W - 5, y + 1,     3, 2, HEADL);
    draw_filled_rect(x + 2,     y + H - 4, 4, 2, TAIL);               /* tail lights */
    draw_filled_rect(x + W - 6, y + H - 4, 4, 2, TAIL);
}

/* a traffic car that was smashed aside by a boosting car */
static void draw_wreck(int x, int y) {
    draw_car(x, y, &PAL_WRECK, false, false);
    draw_filled_rect(x + 3, y + 2, CAR_W - 6, 4, BLACK);                 /* crumpled, scorched hood */
    draw_filled_rect(x + (y & 7),       y - 3,          2, 2, SPARK);
    draw_filled_rect(x + CAR_W - 4 - (y & 3), y + 12,   2, 2, FLAME);
    draw_filled_rect(x + 2 + (y & 5),   y + CAR_H - 2,  2, 2, SPARK);
}

static void draw_bottle(int x, int y) {
    draw_filled_rect(x + 3, y, 4, 2, YELLOW);                   /* cap */
    draw_filled_rect(x + 3, y + 2, 4, 4, BOTTLE);               /* neck */
    draw_filled_rect(x, y + 6, BOTTLE_W, BOTTLE_H - 6, BOTTLE); /* body */
    draw_filled_rect(x + 1, y + 10, BOTTLE_W - 2, 4, WHITE);    /* label */
}

static void draw_flame(int x, int y, int flicker) {
    int len = 8 + flicker * 4;
    draw_filled_rect(x + 3, y + CAR_H, CAR_W - 6, len, FLAME);
    draw_filled_rect(x + 6, y + CAR_H, CAR_W - 12, len - 4, YELLOW);
}

static void draw_lives(int x, int y, int lives) {
    for (int i = 0; i < START_LIVES; i++)
        draw_filled_rect(x + i * 12, y, 8, 8, i < lives ? YELLOW : EMPTY);
}

void text_center(int cx, int y, const char* text, int scale, uint16_t color) {
    int x = cx - (int(std::strlen(text)) * 6 - 1) * scale / 2;
    for (; *text; ++text, x += 6 * scale) {
        const unsigned c = static_cast<unsigned char>(*text);
        if (c < 32 || c > 127) continue;
        for (int col = 0; col < 5; ++col)
            for (int row = 0; row < 7; ++row)
                if (font5x7[c - 32][col] & (1 << row))
                    draw_filled_rect(x + col * scale, y + row * scale, scale, scale, color);
    }
}
void render(const Race* r) {
    char buf[24];

    clear_screen(GRASS);
    for (int y = -40 + r->road_off; y < SCREEN_H; y += 40) {
        draw_filled_rect(ROAD_L - 7, y, 3, 16, GRASS_DARK);
        draw_filled_rect(ROAD_R + 4, y, 3, 16, GRASS_DARK);
    }
    draw_filled_rect(ROAD_L, 0, ROAD_R - ROAD_L, SCREEN_H, ROAD);
    draw_filled_rect(ROAD_L - 3, 0, 3, SCREEN_H, EDGE);
    draw_filled_rect(ROAD_R, 0, 3, SCREEN_H, EDGE);
    for (int k = 1; k < LANES; k++)
        for (int y = -40 + r->road_off; y < SCREEN_H; y += 40)
            draw_filled_rect(ROAD_L + k * LANE_W - 1, y, 3, 20, LINE);

    for (int i = 0; i < MAX_TRAFFIC; i++) {
        if (!r->traffic[i].active) continue;
        if (r->traffic[i].knocked) { draw_wreck(r->traffic[i].x, r->traffic[i].y); continue; }
        int lane = (r->traffic[i].x - ROAD_L) / LANE_W;
        if (lane < 0) lane = 0;
        if (lane > LANES - 1) lane = LANES - 1;
        draw_car(r->traffic[i].x, r->traffic[i].y, &PAL_TRAFFIC[lane], false, lane & 1);
    }

    for (int i = 0; i < MAX_BOTTLES; i++)
        if (r->bottle[i].active) draw_bottle(r->bottle[i].x, r->bottle[i].y);

    /* opponent first, my car on top; blink while protected after a crash */
    for (int pass = 0; pass < 2; pass++) {
        const int me = network.role == MultiplayerRole::joiner ? 1 : 0;
        int p = pass == 0 ? 1 - me : me;
        if (r->car[p].invuln && ((r->car[p].invuln / 3) & 1)) continue;
        int cx = (int)(r->car[p].x >> FP_SHIFT);
        if (r->car[p].boost) draw_flame(cx, r->car[p].y, r->road_off & 1);
        draw_car(cx, r->car[p].y, &PAL_PLAYER[p], true, false);
    }

    // Side panels leave the complete road visible, including incoming traffic.
    for (int player = 0; player < 2; ++player) {
        const int x = player == 0 ? 0 : ROAD_R + 8;
        draw_filled_rect(x, 0, 72, SCREEN_H, PANEL);
        const int cx = x + 36;
        text_center(cx, 16, player == 0 ? "P1" : "P2", 2, player == 0 ? RED : BLUE);
        draw_lives(x + 20, 44, r->car[player].lives);
        snprintf(buf, sizeof buf, "%u", unsigned(r->car[player].kmh));
        text_center(cx, 68, buf, 2, r->car[player].boost ? FLAME : WHITE);
        text_center(cx, 89, "KM/H", 1, WHITE);
        if (r->car[player].boost) text_center(cx, 112, "BOOST!", 1, FLAME);
        snprintf(buf, sizeof buf, "%uM", unsigned(r->meters));
        text_center(cx, 142, buf, 1, YELLOW);
        if (network.session) {
            if (player == (network.role == MultiplayerRole::joiner ? 1 : 0))
                text_center(cx, 192, "YOU", 1, YELLOW);
        } else {
            text_center(cx, 192, player == 0 ? "ARROWS" : "WASD", 1, WHITE);
            text_center(cx, 208, player == 0 ? "ENTER" : "V", 1, WHITE);
        }
        if (r->state == R_LOBBY || r->state == R_OVER)
            text_center(cx, 170, r->car[player].ready ? "READY" : "WAIT", 1, YELLOW);
    }
    switch (r->state) {
    case R_LOBBY:
        draw_filled_rect(77, 54, 166, 106, PANEL);
        text_center(160, 66, "RACE DUEL", 2, YELLOW);
        text_center(160, 98, "BOTH PLAYERS", 1, WHITE);
        text_center(160, 114, "PRESS READY", 1, WHITE);
        text_center(160, 142, "3 LIVES EACH", 1, WHITE);
        break;
    case R_COUNTDOWN:
        snprintf(buf, sizeof buf, "%u", unsigned(r->countdown));
        text_center(160, 74, buf, 7, YELLOW);
        break;
    case R_RACING: break;
    case R_OVER:
        draw_filled_rect(77, 54, 166, 106, PANEL);
        text_center(160, 66, "GAME OVER", 2, WHITE);
        text_center(160, 98, r->winner == 2 ? "DRAW" : r->winner == 0 ? "P1 WINS!" : "P2 WINS!", 2, YELLOW);
        if (r->timer <= 0) text_center(160, 142, "READY TO REMATCH", 1, WHITE);
        break;
    }
}

int8_t axis(uint16_t value) {
    const int delta = std::max(-2048, std::min(2047, int(value) - 2048));
    constexpr int deadzone = 250;
    if (delta > deadzone) return int8_t((delta - deadzone) * 127 / (2047 - deadzone));
    if (delta < -deadzone) return int8_t((delta + deadzone) * 127 / (2048 - deadzone));
    return 0;
}
PlayerInput player_input(const ControlsState& controls) {
    PlayerInput result = {axis(controls.x), int8_t(-axis(controls.y)),
                          bool(controls.pressed & (CONTROL_B | CONTROL_K))};
    if (controls.held & (CONTROL_B | CONTROL_D))
        result.steer = int8_t(127 * (int(bool(controls.held & CONTROL_B)) - int(bool(controls.held & CONTROL_D))));
    if (controls.held & (CONTROL_A | CONTROL_C))
        result.throttle = int8_t(127 * (int(bool(controls.held & CONTROL_A)) - int(bool(controls.held & CONTROL_C))));
    return result;
}
// Snapshots use whole pixels; only the host retains subpixel physics state.
static_assert(MAX_TRAFFIC == 8 && MAX_BOTTLES == 3 && LANES == 4 && LANE_W == 40 &&
              ROAD_L == 80 && CAR_W == 18 && CAR_H == 30 && SCREEN_H == 240 &&
              BOOST_FRAMES <= 127 && INVULN_FRAMES <= 63 && START_LIVES == 3 &&
              CAR_Y - (OFF_MIN >> FP_SHIFT) < 256 && CAR_Y - (OFF_MAX >> FP_SHIFT) >= 0 &&
              (SPEED_BOOST_MAX * KMH_PER_PX >> FP_SHIFT) <= 255 &&
              SCREEN_W + 32 < 512 && SCREEN_H + 80 < 512 &&
                            CAR_H / 2 + SPAWN_SPACING / 2 + BOTTLE_H / 2 <= 80 &&
              GAME_FPS == 30 && MULTIPLAYER_PAIR_BODY_SIZE == 48,
              "Race tuning requires a matching packet layout update");
bool newer16(uint16_t a, uint16_t b) { return uint16_t(a - b) && uint16_t(a - b) < 0x8000; }
uint16_t read16(const uint8_t* p) { return uint16_t(p[0] | (unsigned(p[1]) << 8)); }
void write16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
int signed8(uint8_t value) { return value < 128 ? value : int(value) - 256; }
bool zeros(const uint8_t* p, int count) {
    for (int i = 0; i < count; ++i) if (p[i]) return false;
    return true;
}
void put_bits(uint8_t* bytes, int offset, unsigned value, int width) {
    for (int bit = 0; bit < width; ++bit)
        if (value & (1u << bit)) bytes[(offset + bit) / 8] |= uint8_t(1u << ((offset + bit) % 8));
}
unsigned get_bits(const uint8_t* bytes, int offset, int width) {
    unsigned value = 0;
    for (int bit = 0; bit < width; ++bit)
        if (bytes[(offset + bit) / 8] & (1u << ((offset + bit) % 8))) value |= 1u << bit;
    return value;
}
Network session_state() {
    const uint32_t session = multiplayer_session_id();
    const MultiplayerRole role = multiplayer_role();
    if (network.session == session && network.role == role) return network;
    Network fresh;
    fresh.session = session;
    fresh.role = role;
    return fresh;
}
void play_changes(const Race& before, const Race& after) {
    if (after.state == R_OVER && before.state != R_OVER) speaker_play(racer_sounds::result);
    else if (after.car[0].lives < before.car[0].lives || after.car[1].lives < before.car[1].lives)
        speaker_play(racer_sounds::crash);
    else if (after.car[0].boost > before.car[0].boost || after.car[1].boost > before.car[1].boost)
        speaker_play(racer_sounds::boost);
    else if (after.state == R_RACING && before.state == R_COUNTDOWN) speaker_play(racer_sounds::start);
    else if (after.state == R_COUNTDOWN && (before.state != R_COUNTDOWN || before.countdown != after.countdown))
        speaker_play(racer_sounds::countdown);
}
void publish_input() {
    uint8_t body[24] = {};
    body[0] = 1;
    body[1] = uint8_t(network.local.steer);
    body[2] = uint8_t(network.local.throttle);
    body[3] = network.round;
    body[4] = uint8_t(race.state);
    if (network.pending) {
        const Command& c = network.command;
        body[5] = c.id; body[6] = c.kind; body[7] = c.value;
        body[8] = c.round; body[9] = uint8_t(c.phase); write16(body + 10, c.tick);
    }
    multiplayer_publish(MultiplayerPacketType::input, network.tick, body);
}
void pack_snapshot(uint8_t* body) {
    std::memset(body, 0, 48);
    body[0] = 1;
    body[1] = uint8_t(race.state) | (network.paused ? 4 : 0);
    body[2] = (race.state == R_COUNTDOWN || race.state == R_OVER) ? uint8_t(race.timer) : 0;
    body[3] = uint8_t(race.winner + 1);
    body[4] = race.road_off;
    write16(body + 5, race.meters);
    body[7] = network.acknowledged;
    body[8] = network.round;
    for (int i = 0; i < 2; ++i) {
        const Car& c = race.car[i];
        uint8_t* p = body + 10 + 5 * i;
        p[0] = uint8_t(c.x >> FP_SHIFT); p[1] = uint8_t(c.y); p[2] = c.kmh;
        p[3] = c.lives | (c.invuln << 2); p[4] = c.boost | (c.ready ? 128 : 0);
    }
    for (int i = 0; i < MAX_TRAFFIC; ++i) {
        const Traffic& t = race.traffic[i];
        if (!t.active) continue;
        const unsigned packed = unsigned(t.x + 32) | (unsigned(t.y + 32) << 9) |
                                (1u << 18) | (unsigned(t.knocked) << 19);
        put_bits(body + 20, i * 20, packed, 20);
    }
    for (int i = 0; i < MAX_BOTTLES; ++i) {
        const Bottle& b = race.bottle[i];
        if (!b.active) continue;
        const unsigned lane = unsigned((b.x - ROAD_L) / LANE_W);
        put_bits(body + 40, i * 12, lane | (unsigned(b.y + 80) << 2) | (1u << 11), 12);
    }
}
bool unpack_snapshot(const uint8_t* b, Race& view, bool& paused) {
    const unsigned phase = b[1] & 3;
    paused = bool(b[1] & 4);
    if (b[0] != 1 || b[1] > 7 || b[3] > 3 || b[4] >= 40 || b[9] ||
        (b[44] & 0xf0) || !zeros(b + 45, 3) ||
        (paused && phase != R_RACING && phase != R_COUNTDOWN)) return false;
    if ((phase == R_COUNTDOWN && (!b[2] || b[2] > 3 * FPS)) ||
        (phase == R_OVER && b[2] > OVER_DELAY) ||
        ((phase == R_LOBBY || phase == R_RACING) && b[2]) ||
        (phase == R_OVER ? b[3] == 0 : b[3] != 0)) return false;
    view = {};
    view.state = RaceState(phase); view.timer = b[2]; view.winner = int8_t(int(b[3]) - 1);
    view.countdown = phase == R_COUNTDOWN ? uint8_t((view.timer + FPS - 1) / FPS) : 0;
    view.road_off = b[4]; view.meters = read16(b + 5);
    for (int i = 0; i < 2; ++i) {
        const uint8_t* p = b + 10 + i * 5;
        Car& c = view.car[i];
        c.x = FP(p[0]); c.y = p[1]; c.kmh = p[2];
        c.lives = p[3] & 3; c.invuln = p[3] >> 2; c.boost = p[4] & 127; c.ready = bool(p[4] & 128);
        if (c.x < X_MIN || c.x > X_MAX || c.y < CAR_Y - (OFF_MAX >> FP_SHIFT) ||
            c.y > CAR_Y - (OFF_MIN >> FP_SHIFT) || c.kmh < (SPEED_MIN * KMH_PER_PX >> FP_SHIFT) ||
                        c.kmh > (SPEED_BOOST_MAX * KMH_PER_PX >> FP_SHIFT) ||
            c.invuln > INVULN_FRAMES || c.boost > BOOST_FRAMES ||
            (c.ready && phase != R_LOBBY && phase != R_OVER)) return false;
    }
    if (phase != R_OVER && (!view.car[0].lives || !view.car[1].lives)) return false;
    if (phase == R_OVER) {
        const int winner = view.car[0].lives ? 0 : view.car[1].lives ? 1 : 2;
        if ((view.car[0].lives && view.car[1].lives) || view.winner != winner) return false;
    }
    for (int i = 0; i < MAX_TRAFFIC; ++i) {
        const unsigned packed = get_bits(b + 20, i * 20, 20);
        Traffic& t = view.traffic[i];
        t.active = bool(packed & (1u << 18)); t.knocked = bool(packed & (1u << 19));
        if (!t.active) { if (packed) return false; continue; }
        t.x = int16_t(int(packed & 511) - 32); t.y = int16_t(int((packed >> 9) & 511) - 32);
        if (t.x < -CAR_W || t.x > SCREEN_W || t.y < -CAR_H || t.y > SCREEN_H) return false;
        if (!t.knocked && (t.x < LANE_X(0) || t.x > LANE_X(3) || (t.x - LANE_X(0)) % LANE_W)) return false;
    }
    for (int i = 0; i < MAX_BOTTLES; ++i) {
        const unsigned packed = get_bits(b + 40, i * 12, 12);
        Bottle& bottle = view.bottle[i];
        bottle.active = bool(packed & (1u << 11));
        if (!bottle.active) { if (packed) return false; continue; }
        bottle.x = int16_t(ROAD_L + int(packed & 3) * LANE_W + (LANE_W - BOTTLE_W) / 2);
        bottle.y = int16_t(int((packed >> 2) & 511) - 80);
        if (bottle.y < -(CAR_H / 2) - SPAWN_SPACING / 2 - BOTTLE_H / 2 || bottle.y > SCREEN_H) return false;
    }
    return true;
}
bool same_command(const Command& a, const Command& b) {
    return a.id == b.id && a.kind == b.kind && a.value == b.value && a.round == b.round &&
           a.phase == b.phase && a.tick == b.tick;
}
bool receive_packet(MultiplayerPacketType type, uint16_t tick, const uint8_t* b, uint64_t now) {
    Network next = session_state();
    if (!b || !next.session) return false;
    if (next.role == MultiplayerRole::joiner && type == MultiplayerPacketType::state_pair) {
        if (next.initialized && !newer16(tick, next.tick)) return false;
        Race view; bool paused;
        if (!unpack_snapshot(b, view, paused) ||
            (b[7] != next.acknowledged && (!next.pending || b[7] != next.command.id))) return false;
        if (next.initialized) {
            if (paused && !next.paused) speaker_stop();
            if (!paused) play_changes(race, view);
        }
        if (next.pending && b[7] == next.command.id) next.pending = false;
        next.acknowledged = b[7]; next.round = b[8]; next.paused = paused;
        next.tick = tick; next.state_time = now; next.initialized = true;
        race = view; network = next;
        // Reply from this callback, not the next logic tick, to meet the 5 ms deadline.
        publish_input();
        return true;
    }
    if (next.role != MultiplayerRole::host || type != MultiplayerPacketType::input || !next.initialized ||
        b[0] != 1 || b[1] == 128 || b[2] == 128 || b[4] > R_OVER || b[6] > 2 || b[7] > 1 ||
        b[9] > R_OVER || !zeros(b + 12, 12) || newer16(tick, next.tick) ||
        uint16_t(next.tick - tick) > 3 || (next.have_input && !newer16(tick, next.input_tick))) return false;
    if (!b[6] && !zeros(b + 5, 7)) return false;
    if (b[6]) {
        Command c;
        c.id = b[5]; c.kind = b[6]; c.value = b[7]; c.round = b[8]; c.phase = RaceState(b[9]); c.tick = read16(b + 10);
        if (newer16(c.tick, tick) ||
            (c.kind == 1 && (!c.value || (c.phase != R_LOBBY && c.phase != R_OVER))) ||
            (c.kind == 2 && c.phase != R_RACING && c.phase != R_COUNTDOWN)) return false;
        const uint8_t distance = uint8_t(c.id - next.acknowledged);
        if (distance) {
            if (distance != 1 || (next.pending && !same_command(c, next.command))) return false;
            next.pending = true; next.command = c;
        }
    }
    next.remote = {};
    if (b[3] == next.round && b[4] == race.state && !next.paused && uint16_t(next.tick - tick) <= next.phase_age) {
        next.remote.steer = int8_t(signed8(b[1])); next.remote.throttle = int8_t(signed8(b[2]));
    }
    next.have_input = true; next.input_tick = tick; next.input_time = now;
    network = next;
    return true;
}
void queue_command(uint8_t kind, bool value) {
    if (network.pending) return;
    network.pending = true;
    network.command.id = uint8_t(network.acknowledged + 1);
    network.command.kind = kind; network.command.value = value; network.command.round = network.round;
    network.command.phase = race.state; network.command.tick = network.tick;
}
void multiplayer_update_game(const InputState& input) {
    if (multiplayer_state() != MultiplayerState::connected) {
        speaker_stop(); network = {}; race_init(&race); return;
    }
    network = session_state();
    if (!network.session) return;
    if (network.role == MultiplayerRole::host && !network.initialized) {
        race_init(&race); network.initialized = true;
    }
    if (input.menu.back && (race.state == R_LOBBY || race.state == R_OVER || network.paused)) {
        console_return_to_selection(); network = {}; return;
    }
    network.local = player_input(input.controls);
    const bool pause_press = (input.controls.pressed & CONTROL_E) || (network.paused && input.menu.select);
    const bool ready_press = input.menu.select && (race.state == R_LOBBY || (race.state == R_OVER && race.timer == 0));
    if (network.role == MultiplayerRole::joiner) {
        const uint64_t now = multiplayer_now_us();
        if (!network.initialized) return;
        if (now >= network.state_time && now - network.state_time < INPUT_TIMEOUT_US) {
            if (ready_press && !race.car[1].ready) queue_command(1, true);
            else if (pause_press && (race.state == R_COUNTDOWN || race.state == R_RACING)) queue_command(2, !network.paused);
        }
        publish_input();
        return;
    }
    ++network.tick;
    if (network.phase_age < 65535) ++network.phase_age;
    const Race before = race;
    const bool was_paused = network.paused;
    bool remote_ready = false;
    if (network.pending) {
        const Command& c = network.command;
        // Acknowledging obsolete requests releases the joiner's one-command slot.
        // Bound their age as well as phase/round, so delayed pause cannot surprise a later scene.
        const unsigned age = uint16_t(network.tick - c.tick);
        if (c.round == network.round && c.phase == race.state && age <= network.phase_age && age <= GAME_FPS) {
            if (c.kind == 1 && (race.state == R_LOBBY || (race.state == R_OVER && !race.timer))) remote_ready = true;
            if (c.kind == 2 && (race.state == R_COUNTDOWN || race.state == R_RACING)) network.paused = c.value;
        }
        network.acknowledged = c.id; network.pending = false;
    }
    if (pause_press && (race.state == R_COUNTDOWN || race.state == R_RACING)) network.paused = !was_paused;
    if (network.paused != was_paused) {
        network.phase_age = 0; network.have_input = false; network.remote = {};
        if (network.paused) speaker_stop();
    }
    if (!network.paused && !was_paused) {
        const uint64_t now = multiplayer_now_us();
        const bool fresh = network.have_input && now >= network.input_time &&
            now - network.input_time < INPUT_TIMEOUT_US && uint16_t(network.tick - network.input_tick) <= 3;
        PlayerInput players[] = {network.local, fresh ? network.remote : PlayerInput{}};
        players[0].button = ready_press; players[1].button = remote_ready;
        race_update(&race, players);
        if (race.state != before.state) {
            network.phase_age = 0; network.have_input = false; network.remote = {};
            if (race.state == R_COUNTDOWN) ++network.round;
        }
        play_changes(before, race);
    }
    uint8_t body[48]; pack_snapshot(body);
    multiplayer_publish_pair(network.tick, body);
}
void open_connection() {
    multiplayer_open(RACE_GAME_ID);
    multiplayer_set_receiver(receive_packet);
}
void init() {
#ifndef PICO_BUILD
    random_engine.seed(std::random_device{}());
#else
    random_engine.seed(uint32_t(time_us_64()));
#endif
    network = {};
    render_init();
    race_init(&race);
#ifdef PICO_BUILD
    open_connection();
#endif
}
void update(const InputState& input) {
#ifdef PICO_BUILD
    if (input.menu.back) console_return_to_selection();
    else if (input.menu.select) open_connection();
#else
    if (input.menu.back && (race.state == R_LOBBY || race.state == R_OVER)) {
        console_return_to_selection();
        return;
    }
    const PlayerInput players[] = {player_input(input.controls), player_input(input.second_player_controls)};
    const Race before = race;
    race_update(&race, players);
    play_changes(before, race);
#endif
}
void draw(const char*) {
#ifdef PICO_BUILD
    if (!network.session) {
        clear_screen(0x1082);
        text_center(160, 64, "RACE DUEL", 3, WHITE);
        text_center(160, 112, "B/K: HOST OR JOIN", 1, WHITE);
        text_center(160, 184, "D: BACK", 1, WHITE);
        return;
    }
#endif
    render(&race);
    if (network.paused) {
        draw_filled_rect(58, 76, 204, 76, PANEL);
        text_center(160, 88, "PAUSED", 2, YELLOW);
        text_center(160, 116, "B/K OR E: RESUME", 1, WHITE);
        text_center(160, 136, "D: LEAVE GAME", 1, WHITE);
    } else if (network.session && network.role == MultiplayerRole::joiner &&
               (!network.initialized || multiplayer_now_us() - network.state_time >= INPUT_TIMEOUT_US)) {
        draw_filled_rect(80, 112, 160, 18, PANEL);
        text_center(160, 118, "WAITING FOR HOST", 1, YELLOW);
    }
}
} // namespace

extern const Game game_race_duel = {"Race Duel", init, update, draw, 0, nullptr, multiplayer_update_game};
