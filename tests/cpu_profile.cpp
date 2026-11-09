// Separate profiling firmware: reuse the real main loop without adding work to
// normal builds. Wrappers below affect only calls made directly by that loop.
#include <stdio.h>
#include <stdarg.h>
#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <hardware/spi.h>
#include "console.h"
#include "graphics.h"
#include "display.h"
#include "multiplayer.h"
#include "speaker.h"

namespace {

enum Region { CONTROLS, RADIO, AUDIO, LOGIC, DRAW, DISPLAY, LOGGING, SLEEP, OTHER, REGION_COUNT };
static const char* const region_names[] = {
    "controls", "radio_service", "audio_service", "game_update", "draw", "display",
    "logging", "sleep", "other"
};

struct Bucket {
    uint64_t total_us = 0;
    uint64_t max_us = 0;
    uint32_t calls = 0;
};

struct Profile {
    Bucket buckets[REGION_COUNT];
    uint32_t loops = 0;
    uint32_t updates = 0;
    uint32_t draws = 0;
    uint32_t frames = 0;
    uint64_t poll_gap_max_us = 0;
    Bucket transfers;
    uint32_t rows_min = DISPLAY_HEIGHT;
    uint32_t rows_max = 0;
};

static Profile profile;
static bool active = false;
static bool frame_in_flight = false;
static uint64_t window_start_us = 0;
static uint64_t cursor_us = 0;
static uint64_t last_poll_us = 0;
static uint64_t frame_start_us = 0;

static void add_time(Region region, uint64_t elapsed_us) {
    Bucket& bucket = profile.buckets[region];
    bucket.total_us += elapsed_us;
    if (elapsed_us > bucket.max_us) bucket.max_us = elapsed_us;
    ++bucket.calls;
}

// Regions never nest. Gaps include loop bookkeeping and profiler overhead;
// interrupts and blocking waits remain charged to their enclosing region.
struct Scope {
    bool enabled;
    Region region;
    uint64_t start_us;

    explicit Scope(Region value) : enabled(active), region(value), start_us(0) {
        if (!enabled) return;
        start_us = time_us_64();
        add_time(OTHER, start_us - cursor_us);
    }

    ~Scope() {
        if (!enabled) return;
        const uint64_t end_us = time_us_64();
        add_time(region, end_us - start_us);
        cursor_us = end_us;
    }
};

static void report_if_due() {
    if (!active) return;
    const uint64_t end_us = time_us_64();
    if (end_us - window_start_us < 1000000) return;

    add_time(OTHER, end_us - cursor_us);
    const Profile saved = profile;
    const uint64_t elapsed_us = end_us - window_start_us;
    const uint64_t start_us = window_start_us;
    profile = {};
    window_start_us = end_us;
    cursor_us = end_us;

    // Reset/copy overhead and this report belong to the next window, avoiding
    // an unmeasured hole or counting the report against its own denominator.
    Scope logging(LOGGING);
    uint64_t accounted_us = 0;
    for (const Bucket& bucket : saved.buckets) accounted_us += bucket.total_us;
    printf("[PROFILE] start_us=%llu elapsed_us=%llu accounted_us=%llu"
           " loops=%u polls=%u poll_gap_max_us=%llu updates=%u draws=%u frames=%u"
           " role=%u session=%lu managerstate=%u gameactive=%u\n",
           (unsigned long long)start_us, (unsigned long long)elapsed_us,
           (unsigned long long)accounted_us, (unsigned)saved.loops,
           (unsigned)saved.buckets[CONTROLS].calls,
           (unsigned long long)saved.poll_gap_max_us, (unsigned)saved.updates,
           (unsigned)saved.draws, (unsigned)saved.frames,
           (unsigned)multiplayer_role(), (unsigned long)multiplayer_session_id(),
           (unsigned)multiplayer_state(), (unsigned)multiplayer_game_active());
    for (unsigned i = 0; i < REGION_COUNT; ++i) {
        const Bucket& bucket = saved.buckets[i];
        const unsigned share = unsigned(bucket.total_us * 10000 / elapsed_us);
        const uint64_t mean_us = bucket.calls ? bucket.total_us / bucket.calls : 0;
        printf("[PROFILE_TASK] %s total_us=%llu share=%u.%02u%% calls=%u"
               " mean_us=%llu max_us=%llu\n", region_names[i],
               (unsigned long long)bucket.total_us, share / 100, share % 100,
               (unsigned)bucket.calls, (unsigned long long)mean_us,
               (unsigned long long)bucket.max_us);
    }
    // DMA duration overlaps loop work; report it separately, never as CPU time.
    printf("[PROFILE_DISPLAY] completed=%u total_us=%llu max_us=%llu"
           " rows_min=%u rows_max=%u game=\"%s\"\n",
           (unsigned)saved.transfers.calls,
           (unsigned long long)saved.transfers.total_us,
           (unsigned long long)saved.transfers.max_us,
           (unsigned)saved.rows_min, (unsigned)saved.rows_max,
           console_active_game() ? console_active_game()->name : "MENU");
    printf("[PROFILE_END]\n");
}

static ControlsState profile_controls_poll() {
    if (!active) {
        printf("[PROFILE_INFO] core0 elapsed wall time, NOT active CPU cycles;"
               " clock_hz=%lu peri_hz=%lu display_spi_hz=%lu"
               " game_target_hz=%u profiler_bytes=%u\n",
               (unsigned long)clock_get_hz(clk_sys),
               (unsigned long)clock_get_hz(clk_peri),
               (unsigned long)spi_get_baudrate(spi0), (unsigned)GAME_FPS,
               (unsigned)sizeof(profile));
        window_start_us = cursor_us = time_us_64();
        active = true;
    }
    ++profile.loops;
    Scope scope(CONTROLS);
    if (last_poll_us) {
        const uint64_t gap = scope.start_us - last_poll_us;
        if (gap > profile.poll_gap_max_us) profile.poll_gap_max_us = gap;
    }
    last_poll_us = scope.start_us;
    return controls_poll();
}

static void profile_multiplayer_service(uint64_t now_us) {
    Scope scope(RADIO);
    multiplayer_service(now_us);
}

static void profile_speaker_service() {
    Scope scope(AUDIO);
    speaker_service();
}

static void profile_console_update(const InputState& input) {
    Scope scope(LOGIC);
    console_update(input);
    if (active) ++profile.updates;
}

static int profile_console_draw() {
    Scope scope(DRAW);
    const int rows = console_draw();
    if (active) ++profile.draws;
    return rows;
}

static bool profile_display_busy() {
    Scope scope(DISPLAY);
    const bool busy = display_busy();
    if (active && frame_in_flight && !busy) {
        frame_in_flight = false;
        ++profile.frames;
        const uint64_t elapsed_us = time_us_64() - frame_start_us;
        profile.transfers.total_us += elapsed_us;
        if (elapsed_us > profile.transfers.max_us) profile.transfers.max_us = elapsed_us;
        ++profile.transfers.calls;
    }
    return busy;
}

static bool profile_display_blit_async(int x, int y, int w, int h,
                                       const uint16_t* pixels) {
    Scope scope(DISPLAY);
    const uint64_t start_us = active ? time_us_64() : 0;
    const bool submitted = display_blit_async(x, y, w, h, pixels);
    if (active && submitted) {
        frame_in_flight = true;
        frame_start_us = start_us;
        if (unsigned(h) < profile.rows_min) profile.rows_min = unsigned(h);
        if (unsigned(h) > profile.rows_max) profile.rows_max = unsigned(h);
    }
    return submitted;
}

static void profile_console_debug() {
    Scope scope(LOGGING);
    console_debug();
}

static int profile_printf(const char* format, ...) __attribute__((format(printf, 1, 2)));
static int profile_printf(const char* format, ...) {
    Scope scope(LOGGING);
    va_list args;
    va_start(args, format);
    const int result = vprintf(format, args);
    va_end(args);
    return result;
}

static void profile_sleep_ms(uint32_t milliseconds) {
    {
        Scope scope(SLEEP);
        sleep_ms(milliseconds);
    }
    report_if_due();
}

} // namespace

// Headers are already included above, so these aliases only instrument the
// current main-loop call sites, not SDK declarations or other source files.
// A renamed/new main-loop call must be reviewed here to keep attribution useful.
#define controls_poll profile_controls_poll
#define multiplayer_service profile_multiplayer_service
#define speaker_service profile_speaker_service
#define console_update profile_console_update
#define console_draw profile_console_draw
#define display_busy profile_display_busy
#define display_blit_async profile_display_blit_async
#define console_debug profile_console_debug
#define printf profile_printf
#define sleep_ms profile_sleep_ms
#include "../src/main_pico.cpp"
#undef controls_poll
#undef multiplayer_service
#undef speaker_service
#undef console_update
#undef console_draw
#undef display_busy
#undef display_blit_async
#undef console_debug
#undef printf
#undef sleep_ms
