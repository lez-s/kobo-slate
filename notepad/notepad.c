#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fbink.h"
#include "gesture.h"
#include "notepad.h"

#define SCREEN_WIDTH 1264
#define SCREEN_HEIGHT 1680
#define FB_STRIDE 5056
#define SLOT_COUNT 10
#define PEN_TOOL 1
#define REFRESH_INTERVAL_MS 50
#define PAN_REFRESH_INTERVAL_MS 200
/* This app stays awake, so two presses arrive continuously rather than one
   per suspend/wake cycle as they do in dash.sh. */
#define POWER_PRESS_WINDOW_SECONDS 5
/* Exit status 10 is the contract with dash.sh: exit to Nickel. */
#define EXIT_TO_NICKEL_STATUS 10
#define LIGHT_PATH "/sys/class/backlight/lm3630a_led/brightness"
#define DEFAULT_NOTES_PATH "/mnt/onboard/.adds/kobo-dash/notes/canvas.knp"
#define DEFAULT_WIFI_SCRIPT "/mnt/onboard/.adds/kobo-dash/wifi.sh"
/* Page one sleeps between cycles; page two stays awake the whole time it is
   open, so a notepad left face-up on a desk drains exactly the battery the
   suspend loop exists to protect. Hand the panel back after this long with no
   input of any kind. --idle-seconds overrides it; 0 disables it, and a short
   value makes the behaviour testable, since nobody will sit through ten
   minutes to check a timeout. */
#define DEFAULT_IDLE_SECONDS 600
#define ERASER_RADIUS 12
/* 1, not a batch: erasing something should actually remove it. Below a
   threshold the erased strokes stay in the file -- present to anyone who reads
   it, and to knp2svg.py's replay -- which is not what "erase" means to the
   person who did it. The cost is one rewrite at exit in any session where
   something was erased; a typical notebook is around a megabyte. */
#define COMPACTION_THRESHOLD 1

/* Development aid only: the stylus is an active MPP pen, so a flat one emits
   nothing at all and the ink path cannot be exercised. With this set, a finger
   draws exactly where the pen would. A drawing finger is not also a gesture
   finger, so --finger-draws deliberately disables pan and pinch input. A finger
   tap on a corner control still selects that control instead of drawing ink. */
static int finger_draws;

enum {
    WIFI_OFF = 0,
    WIFI_CONNECTING = 1,
    WIFI_ON = 2
};

enum {
    WIFI_ACTION_STATUS = 0,
    WIFI_ACTION_UP = 1,
    WIFI_ACTION_DOWN = 2
};

/* Palm (2) never draws, whatever the flag says: resting a hand on the panel is
   the case ABS_MT_TOOL_TYPE exists to let us ignore. */
static int tool_draws(int tool_type)
{
    if (tool_type == PEN_TOOL) return 1;
    return finger_draws && tool_type == 0;
}

typedef struct {
    int valid;
    int min_x;
    int min_y;
    int max_x;
    int max_y;
} Rect;

typedef struct {
    int tracking_id;
    int tool_type;
    int x;
    int y;
    int have_point;
    int have_x;
    int have_y;
    int pressure;
    int pending_point;
    int pen_active;
    int reported;
    NPControl control;
    int control_checked;
    Rect ink_rect;
    int last_x;
    int last_y;
    int eraser;
    int erase_done;
    NPStroke model_stroke;
    /* Capacity of model_stroke.points, tracked here rather than in NPStroke:
       growth is a builder's concern, and a stored stroke has no use for it. */
    size_t model_capacity;
} TouchSlot;

typedef struct {
    int fd;
    unsigned char *memory;
    size_t size;
    int fbfd;
    FBInkConfig fbink_cfg;
    int light_previous;
    int light_changed;
    NPCanvas canvas;
    NPHistory history;
    uint32_t next_id;
    NPViewport viewport;
    GestureState gesture;
    int gesture_mode;
    int gesture_pan_pending;
    int suppress_refresh;
    int gesture_active;
    struct timespec last_gesture_refresh;
    const char *notes_path;
    const char *wifi_script;
    pid_t wifi_pid;
    int wifi_state;
    int wifi_action;
    uint8_t current_color;
    uint8_t current_width;
    int side_button;
    struct timespec side_button_pressed;
    TouchSlot *slots;
} App;

static volatile sig_atomic_t stop_requested;

static void repaint_full(App *app);
static void draw_controls(App *app);
static int refresh_full(App *app, WFM_MODE_INDEX_T waveform);

static void stop_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int discover_input(const char *wanted, int *result_fd)
{
    char path[64];
    char name[256];
    int event_number;
    int fd;

    for (event_number = 0; event_number < 32; event_number++) {
        (void)snprintf(path, sizeof(path), "/dev/input/event%d", event_number);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        memset(name, 0, sizeof(name));
        if (ioctl(fd, EVIOCGNAME((int)sizeof(name)), name) >= 0 &&
            strcmp(name, wanted) == 0) {
            *result_fd = fd;
            return 0;
        }
        close(fd);
    }
    fprintf(stderr, "notepad: could not find input device %s\n", wanted);
    return -1;
}

static int validate_axis(int fd, int axis, int expected_min, int expected_max,
                         const char *axis_name)
{
    struct input_absinfo info;
    if (ioctl(fd, EVIOCGABS(axis), &info) != 0) {
        fprintf(stderr, "notepad: EVIOCGABS(%s) failed: %s\n", axis_name,
                strerror(errno));
        return -1;
    }
    if (info.minimum != expected_min || info.maximum != expected_max) {
        fprintf(stderr, "notepad: %s range is %d..%d, expected %d..%d\n",
                axis_name, info.minimum, info.maximum, expected_min, expected_max);
        return -1;
    }
    return 0;
}

static int validate_touch_axes(int fd)
{
    if (validate_axis(fd, ABS_MT_POSITION_X, 0, SCREEN_HEIGHT, "ABS_MT_POSITION_X") != 0 ||
        validate_axis(fd, ABS_MT_POSITION_Y, 0, SCREEN_WIDTH, "ABS_MT_POSITION_Y") != 0) {
        return -1;
    }
    if (validate_axis(fd, ABS_MT_SLOT, 0, SLOT_COUNT - 1, "ABS_MT_SLOT") != 0) return -1;
    if (validate_axis(fd, ABS_MT_PRESSURE, 0, 4095, "ABS_MT_PRESSURE") != 0) return -1;
    if (validate_axis(fd, ABS_MT_DISTANCE, 0, 15, "ABS_MT_DISTANCE") != 0) return -1;
    if (validate_axis(fd, ABS_MT_TOOL_TYPE, 0, 2, "ABS_MT_TOOL_TYPE") != 0) return -1;
    return 0;
}

static void touch_to_screen(int touch_x, int touch_y, int *screen_x, int *screen_y)
{
    *screen_x = touch_y;
    *screen_y = (SCREEN_HEIGHT - 1) - touch_x;
}

static int refresh_region(App *app, WFM_MODE_INDEX_T waveform,
                          int left, int top, int right, int bottom)
{
    FBInkConfig config = app->fbink_cfg;
    uint32_t width;
    uint32_t height;

    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right >= SCREEN_WIDTH) right = SCREEN_WIDTH - 1;
    if (bottom >= SCREEN_HEIGHT) bottom = SCREEN_HEIGHT - 1;
    if (left > right || top > bottom) return 0;
    config.wfm_mode = waveform;
    width = (uint32_t)(right - left + 1);
    height = (uint32_t)(bottom - top + 1);
    return fbink_refresh(app->fbfd, (uint32_t)top, (uint32_t)left,
                         width, height, &config);
}

static int refresh_full(App *app, WFM_MODE_INDEX_T waveform)
{
    return refresh_region(app, waveform, 0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1);
}

static void draw_exit_hint(App *app)
{
    FBInkConfig config = app->fbink_cfg;
    config.row = 1;
    config.is_centered = true;
    config.is_padded = true;
    config.wfm_mode = WFM_GLRC16;
    (void)fbink_print(app->fbfd, "EXIT? press power again", &config);
}

static void set_pixel(App *app, int x, int y, uint32_t pixel)
{
    uint32_t *row;
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT) return;
    row = (uint32_t *)(void *)(app->memory + (size_t)y * FB_STRIDE);
    row[x] = pixel;
}

static void draw_disc(App *app, int x, int y, int radius, uint32_t pixel)
{
    /* Clipped to the panel before iterating, not inside set_pixel. Zoomed in,
       a disc's radius is multiplied by the viewport scale, so most of it can
       lie off screen -- iterating the whole (2r+1) square only to reject each
       pixel individually made off-screen area cost as much as visible area. */
    int r2 = radius * radius;
    int top = y - radius, bottom = y + radius;
    int left = x - radius, right = x + radius;
    int py;

    if (right < 0 || bottom < 0 || left >= SCREEN_WIDTH || top >= SCREEN_HEIGHT) return;
    if (top < 0) top = 0;
    if (left < 0) left = 0;
    if (bottom >= SCREEN_HEIGHT) bottom = SCREEN_HEIGHT - 1;
    if (right >= SCREEN_WIDTH) right = SCREEN_WIDTH - 1;

    for (py = top; py <= bottom; py++) {
        int dy = py - y;
        int px;
        for (px = left; px <= right; px++) {
            int dx = px - x;
            if (dx * dx + dy * dy <= r2) set_pixel(app, px, py, pixel);
        }
    }
}

static void draw_segment(App *app, int x0, int y0, int x1, int y1, int width,
                         uint32_t pixel)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    int radius = width / 2;
    int stride;
    int lo, hi;
    int i;

    /* Whole segment off the panel: reject before stepping along it. A stroke
       whose bounding box clips the viewport is drawn in full, so without this
       one spanning a long way off-canvas costs its entire length rather than
       the part you can see. */
    lo = (x0 < x1 ? x0 : x1) - radius;
    hi = (x0 > x1 ? x0 : x1) + radius;
    if (hi < 0 || lo >= SCREEN_WIDTH) return;
    lo = (y0 < y1 ? y0 : y1) - radius;
    hi = (y0 > y1 ? y0 : y1) + radius;
    if (hi < 0 || lo >= SCREEN_HEIGHT) return;

    if (steps == 0) {
        draw_disc(app, x0, y0, radius, pixel);
        return;
    }

    /* Step by half the radius rather than by one pixel. Discs one pixel apart
       overlap almost entirely, so the old loop did `steps * radius^2` work for
       a stroke whose area is only `steps * 2 * radius`: quadratic in a radius
       that grows with zoom. Measured on device before this, ten thick
       full-screen strokes took 917 ms to draw at 8x, against 20 ms at 1x.
       Half-radius spacing leaves the edge scalloped by about 3% of the radius,
       which is under two pixels even at the widest, and a thin pen (radius 2)
       still steps by one and is bit-for-bit unchanged. */
    {
        /* The stride has to be in path-length units, not steps along the
           dominant axis. `steps` counts the long axis, so on a 45-degree line
           consecutive centres are actually `stride * sqrt(2)` apart -- 41%
           further than intended. Measured on device with that error present, a
           112px-wide diagonal scalloped by up to 7 px against the 1.8 px the
           spacing was supposed to give.

           len ~= max + 0.41 * min is the usual octagonal approximation, within
           a few percent and needing no sqrt. */
        int maxd = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
        int mind = abs(dx) > abs(dy) ? abs(dy) : abs(dx);
        long len = maxd + (mind * 41) / 100;
        long target = radius / 2;
        stride = len > 0 ? (int)(target * maxd / len) : (int)target;
        if (stride < 1) stride = 1;
    }
    for (i = 0; i <= steps; i += stride) {
        draw_disc(app, x0 + (dx * i) / steps, y0 + (dy * i) / steps, radius, pixel);
    }
    /* The stride can overshoot the end; the endpoint is never optional. */
    draw_disc(app, x1, y1, radius, pixel);
}

static void rect_reset(Rect *rect)
{
    rect->valid = 0;
}

static void rect_add(Rect *rect, int x, int y, int radius)
{
    if (!rect->valid) {
        rect->valid = 1;
        rect->min_x = x - radius;
        rect->min_y = y - radius;
        rect->max_x = x + radius;
        rect->max_y = y + radius;
        return;
    }
    if (x - radius < rect->min_x) rect->min_x = x - radius;
    if (y - radius < rect->min_y) rect->min_y = y - radius;
    if (x + radius > rect->max_x) rect->max_x = x + radius;
    if (y + radius > rect->max_y) rect->max_y = y + radius;
}

static void controls_rect(Rect *rect)
{
    NPControlGeometry first;
    NPControlGeometry last;
    (void)np_control_geometry(NP_CONTROL_WIFI, SCREEN_WIDTH, SCREEN_HEIGHT, &first);
    (void)np_control_geometry(NP_CONTROL_COLOR, SCREEN_WIDTH, SCREEN_HEIGHT, &last);
    rect->valid = 1;
    rect->min_x = first.draw.min_x;
    rect->min_y = first.draw.min_y;
    rect->max_x = last.draw.max_x;
    rect->max_y = first.draw.max_y;
}

static int refresh_rect(App *app, WFM_MODE_INDEX_T waveform, const Rect *rect)
{
    if (!rect->valid) return 0;
    return refresh_region(app, waveform, rect->min_x, rect->min_y,
                          rect->max_x, rect->max_y);
}

static void refresh_controls(App *app)
{
    Rect rect;
    controls_rect(&rect);
    (void)refresh_rect(app, WFM_GLRC16, &rect);
}

/* Refresh the controls only when something actually painted over them. Every
   e-ink update costs real time, so do not charge every stroke for this overlay. */
static int rect_overlaps_controls(const Rect *rect)
{
    Rect controls;
    if (!rect->valid) return 0;
    controls_rect(&controls);
    return !(rect->max_x < controls.min_x || rect->min_x > controls.max_x ||
             rect->max_y < controls.min_y || rect->min_y > controls.max_y);
}

static void refresh_controls_if_touched(App *app, const Rect *rect)
{
    if (rect_overlaps_controls(rect)) refresh_controls(app);
}

static void redraw_wifi_control(App *app)
{
    draw_controls(app);
    refresh_controls(app);
}

static void set_wifi_state(App *app, int state)
{
    app->wifi_state = state;
    redraw_wifi_control(app);
}

static int start_wifi_operation(App *app, int action)
{
    const char *command = action == WIFI_ACTION_UP ? "up" :
                          action == WIFI_ACTION_DOWN ? "down" : "status";
    pid_t pid;

    if (app->wifi_pid != -1) return 0;
    app->wifi_action = action;
    set_wifi_state(app, WIFI_CONNECTING);
    pid = fork();
    if (pid == 0) {
        execl(app->wifi_script, app->wifi_script, command, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        fprintf(stderr, "notepad: could not start wifi %s: %s\n",
                command, strerror(errno));
        app->wifi_pid = -1;
        set_wifi_state(app, action == WIFI_ACTION_DOWN ? WIFI_ON : WIFI_OFF);
        return -1;
    }
    app->wifi_pid = pid;
    return 0;
}

static void reap_wifi_operation(App *app)
{
    int status;
    pid_t result;
    int success;
    int state;

    if (app->wifi_pid == -1) return;
    result = waitpid(app->wifi_pid, &status, WNOHANG);
    if (result == 0) return;
    if (result < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "notepad: wait for wifi child: %s\n", strerror(errno));
            app->wifi_pid = -1;
            set_wifi_state(app, app->wifi_action == WIFI_ACTION_DOWN ? WIFI_ON : WIFI_OFF);
        }
        return;
    }
    success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    state = success ? (app->wifi_action == WIFI_ACTION_DOWN ? WIFI_OFF : WIFI_ON) :
                      (app->wifi_action == WIFI_ACTION_DOWN ? WIFI_ON : WIFI_OFF);
    fprintf(stderr, "notepad: wifi operation %s (exit=%d)\n",
            success ? "succeeded" : "failed",
            WIFEXITED(status) ? WEXITSTATUS(status) : 128);
    app->wifi_pid = -1;
    set_wifi_state(app, state);
}

static long elapsed_ms(const struct timespec *since)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - since->tv_sec) * 1000L +
           (now.tv_nsec - since->tv_nsec) / 1000000L;
}

static int report_gesture(App *app, TouchSlot *slots)
{
    GestureContact contacts[2];
    int count = 0;
    int i;
    GestureUpdate update;

    if (finger_draws) {
        /* --finger-draws dedicates finger contacts to ink, so they cannot pan. */
        return 0;
    }
    for (i = 0; i < SLOT_COUNT && count < 2; i++) {
        TouchSlot *slot = &slots[i];
        if (slot->tracking_id >= 0 && slot->control == NP_CONTROL_NONE &&
            slot->tool_type == 0 && slot->pressure > 0 && slot->have_x && slot->have_y) {
            int screen_x;
            int screen_y;
            touch_to_screen(slot->x, slot->y, &screen_x, &screen_y);
            contacts[count].active = 1;
            contacts[count].id = slot->tracking_id;
            contacts[count].x = (double)screen_x;
            contacts[count].y = (double)screen_y;
            count++;
        }
    }
    gesture_report(&app->gesture, contacts, count, &update);
    if (update.kind == GESTURE_PAN) {
        app->suppress_refresh = 0;
        app->viewport.origin_x -= update.dx / app->viewport.scale;
        app->viewport.origin_y -= update.dy / app->viewport.scale;
        app->gesture_mode = GESTURE_PAN;
        app->gesture_active = 1;
        app->gesture_pan_pending = 1;
    } else if (update.kind == GESTURE_ZOOM) {
        np_viewport_zoom_about(&app->viewport, update.anchor_x, update.anchor_y,
                               update.scale);
        app->gesture_mode = GESTURE_ZOOM;
        app->gesture_active = 1;
        app->suppress_refresh = 1;
    } else if (count >= 2) {
        app->gesture_active = 1;
        app->suppress_refresh = 1;
    }
    return update.ended;
}

static void finish_gesture_report(App *app, int ended)
{
    if (ended) {
        repaint_full(app);
        (void)refresh_full(app, WFM_GC16);
        app->gesture_mode = GESTURE_NONE;
        app->gesture_active = 0;
        app->gesture_pan_pending = 0;
        app->suppress_refresh = 0;
    } else if (app->gesture_pan_pending &&
               elapsed_ms(&app->last_gesture_refresh) >= PAN_REFRESH_INTERVAL_MS) {
        repaint_full(app);
        (void)refresh_full(app, WFM_DU);
        clock_gettime(CLOCK_MONOTONIC, &app->last_gesture_refresh);
        app->gesture_pan_pending = 0;
    }
}

static void clear_model(NPStroke *stroke)
{
    free(stroke->points);
    memset(stroke, 0, sizeof(*stroke));
}

static void initialize_slot(TouchSlot *slot)
{
    memset(slot, 0, sizeof(*slot));
    slot->tracking_id = -1;
    slot->control = NP_CONTROL_NONE;
}

/* Grow geometrically. This runs once per input sample -- the panel reports at
   ~83 Hz while the pen is down -- and reallocating to the exact size each time
   asked the allocator to move the whole array on every point of every stroke.
   Capacity lives on the slot because NPStroke is also the stored form, where a
   capacity field would be dead weight in every canvas and every history
   entry. */
static int append_model_point(NPStroke *stroke, size_t *capacity,
                              double canvas_x, double canvas_y)
{
    size_t new_count;

    if (stroke->npts == SIZE_MAX) return -1;
    new_count = stroke->npts + 1;
    if (new_count > *capacity) {
        size_t want = *capacity == 0 ? 64 : *capacity * 2;
        NPPoint *points;
        if (want < *capacity) return -1;
        if (want > SIZE_MAX / sizeof(*points)) return -1;
        points = realloc(stroke->points, want * sizeof(*points));
        if (points == NULL) return -1;
        stroke->points = points;
        *capacity = want;
    }
    stroke->points[stroke->npts].x = (float)canvas_x;
    stroke->points[stroke->npts].y = (float)canvas_y;
    stroke->npts = new_count;
    return 0;
}

static void viewport_point_to_screen(const NPViewport *viewport, double canvas_x,
                                     double canvas_y, int *screen_x, int *screen_y)
{
    double x;
    double y;
    np_canvas_to_screen(viewport, canvas_x, canvas_y, &x, &y);
    *screen_x = (int)lround(x);
    *screen_y = (int)lround(y);
}

static int scaled_stroke_width(const App *app, uint8_t width)
{
    int result = (int)lround((double)width * app->viewport.scale);
    return result < 1 ? 1 : result;
}

/* A multitouch report is only coherent at SYN_REPORT, so all drawing happens
   here and nowhere else.

   X and Y arrive as two separate events and this panel sends both on every
   report. Drawing on each event in turn therefore used the *previous* value of
   the axis that had not arrived yet -- and on the first report of a contact
   that value is the freshly zeroed 0, i.e. the left edge of the screen. The
   result was a straight line reaching in from the left edge to wherever the pen
   actually landed, on the first stroke of every contact. */
static void draw_stroke_pixels(App *app, const NPStroke *stroke, uint32_t pixel)
{
    size_t i;
    int x0;
    int y0;
    int x1;
    int y1;
    int width = scaled_stroke_width(app, stroke->width);
    int radius = width / 2;

    if (stroke->npts == 0) return;
    viewport_point_to_screen(&app->viewport, stroke->points[0].x, stroke->points[0].y,
                             &x0, &y0);
    draw_disc(app, x0, y0, radius, pixel);
    for (i = 1; i < stroke->npts; i++) {
        viewport_point_to_screen(&app->viewport, stroke->points[i].x,
                                 stroke->points[i].y, &x1, &y1);
        draw_segment(app, x0, y0, x1, y1, width, pixel);
        x0 = x1;
        y0 = y1;
    }
}

static void draw_stroke(App *app, const NPStroke *stroke)
{
    uint32_t pixel = stroke->color == NP_COLOR_RED ? NP_PIXEL_RED :
                     stroke->color == NP_COLOR_BLUE ? NP_PIXEL_BLUE :
                     stroke->color == NP_COLOR_GREEN ? NP_PIXEL_GREEN : NP_PIXEL_BLACK;
    draw_stroke_pixels(app, stroke, pixel);
}

static void draw_canvas(App *app)
{
    size_t i;
    for (i = 0; i < app->canvas.count; i++) {
        if (np_canvas_stroke_visible(&app->canvas.strokes[i], &app->viewport,
                                     SCREEN_WIDTH, SCREEN_HEIGHT)) {
            draw_stroke(app, &app->canvas.strokes[i]);
        }
    }
}

static void draw_active_strokes(App *app)
{
    int i;
    for (i = 0; i < SLOT_COUNT; i++) {
        TouchSlot *slot = &app->slots[i];
        if (slot->have_point && slot->model_stroke.npts != 0) {
            draw_stroke_pixels(app, &slot->model_stroke, NP_PIXEL_BLACK);
        }
    }
}

static uint32_t current_pixel(const App *app)
{
    return app->current_color == NP_COLOR_RED ? NP_PIXEL_RED :
           app->current_color == NP_COLOR_BLUE ? NP_PIXEL_BLUE :
           app->current_color == NP_COLOR_GREEN ? NP_PIXEL_GREEN : NP_PIXEL_BLACK;
}

static void draw_control_line(App *app, int x0, int y0, int x1, int y1,
                              int width, uint32_t pixel)
{
    draw_segment(app, x0, y0, x1, y1, width, pixel);
}

static void draw_control_glyph(App *app, NPControl control,
                               const NPControlGeometry *geometry, int enabled)
{
    int cx = (geometry->draw.min_x + geometry->draw.max_x) / 2;
    int cy = (geometry->draw.min_y + geometry->draw.max_y) / 2;
    uint32_t pixel = enabled ? NP_PIXEL_BLACK : 0xffaaaaaau;
    int left = geometry->draw.min_x + 16;
    int right = geometry->draw.max_x - 16;
    int top = geometry->draw.min_y + 16;
    int bottom = geometry->draw.max_y - 16;
    if (control == NP_CONTROL_UNDO || control == NP_CONTROL_REDO) {
        int direction = control == NP_CONTROL_UNDO ? -1 : 1;
        int tip = direction < 0 ? left : right;
        int tail = direction < 0 ? right : left;
        draw_control_line(app, tail, cy, tip, cy, 5, pixel);
        draw_control_line(app, tip, cy, tip + direction * 12, cy - 12, 5, pixel);
        draw_control_line(app, tip, cy, tip + direction * 12, cy + 12, 5, pixel);
    } else if (control == NP_CONTROL_WIFI) {
        /* Off is an X; connecting is blue with a partial radio mark; on is
           green with the complete mark. The background carries the state at
           a glance even for a user who cannot distinguish the small arcs. */
        if (!enabled) {
            draw_control_line(app, left, top, right, bottom, 5, pixel);
            draw_control_line(app, right, top, left, bottom, 5, pixel);
        } else {
            draw_control_line(app, cx - 18, cy + 12, cx, cy - 6, 5, pixel);
            draw_control_line(app, cx, cy - 6, cx + 18, cy + 12, 5, pixel);
            draw_control_line(app, cx - 9, cy + 17, cx, cy + 8, 5, pixel);
            draw_control_line(app, cx, cy + 8, cx + 9, cy + 17, 5, pixel);
            draw_disc(app, cx, cy + 22, 4, pixel);
        }
    } else if (control == NP_CONTROL_CLEAR) {
        /* A letter C, not a cross. The Wi-Fi control draws an X for its off
           state, and two sets of crossed diagonals a few buttons apart are
           indistinguishable at a glance on this panel.

           Built from straight segments rather than an arc so it needs no trig,
           and so nothing here depends on libm being linked into a static ARM
           binary. An octagonal C reads as a C at 72 px. */
        int r = (right - left) / 2 - 6;
        int h = r / 2;
        draw_control_line(app, cx + h, cy - r, cx - h, cy - r, 5, pixel);
        draw_control_line(app, cx - h, cy - r, cx - r, cy - h, 5, pixel);
        draw_control_line(app, cx - r, cy - h, cx - r, cy + h, 5, pixel);
        draw_control_line(app, cx - r, cy + h, cx - h, cy + r, 5, pixel);
        draw_control_line(app, cx - h, cy + r, cx + h, cy + r, 5, pixel);
    } else {
        (void)cx;
    }
}

static void draw_control_box(App *app, const NPControlGeometry *geometry,
                             uint32_t fill)
{
    int y;
    for (y = geometry->draw.min_y; y <= geometry->draw.max_y; y++) {
        int x;
        for (x = geometry->draw.min_x; x <= geometry->draw.max_x; x++) {
            int edge = x == geometry->draw.min_x || x == geometry->draw.max_x ||
                       y == geometry->draw.min_y || y == geometry->draw.max_y;
            set_pixel(app, x, y, edge ? NP_PIXEL_BLACK : fill);
        }
    }
}

static void draw_controls(App *app)
{
    NPControlGeometry color;
    NPControlGeometry width;
    NPControlGeometry undo;
    NPControlGeometry redo;
    NPControlGeometry clear;
    NPControlGeometry wifi;
    NPControlGeometry geometries[6];
    NPControl control;
    int x0;
    int x1;

    (void)np_control_geometry(NP_CONTROL_COLOR, SCREEN_WIDTH, SCREEN_HEIGHT, &color);
    (void)np_control_geometry(NP_CONTROL_WIDTH, SCREEN_WIDTH, SCREEN_HEIGHT, &width);
    (void)np_control_geometry(NP_CONTROL_UNDO, SCREEN_WIDTH, SCREEN_HEIGHT, &undo);
    (void)np_control_geometry(NP_CONTROL_REDO, SCREEN_WIDTH, SCREEN_HEIGHT, &redo);
    (void)np_control_geometry(NP_CONTROL_CLEAR, SCREEN_WIDTH, SCREEN_HEIGHT, &clear);
    (void)np_control_geometry(NP_CONTROL_WIFI, SCREEN_WIDTH, SCREEN_HEIGHT, &wifi);
    geometries[0] = color;
    geometries[1] = width;
    geometries[2] = undo;
    geometries[3] = redo;
    geometries[4] = clear;
    geometries[5] = wifi;
    draw_control_box(app, &color, current_pixel(app));
    draw_control_box(app, &width, 0xffffffffu);
    /* The sample's thickness is the selected model width, not the box size. */
    if (app->current_width > 0) {
        x0 = width.draw.min_x + 12;
        x1 = width.draw.max_x - 12;
        draw_segment(app, x0, width.draw.min_y + NP_CONTROL_SIZE / 2,
                     x1, width.draw.min_y + NP_CONTROL_SIZE / 2,
                     app->current_width, current_pixel(app));
    }
    for (control = NP_CONTROL_UNDO; control <= NP_CONTROL_WIFI; control++) {
        uint32_t fill = 0xffffffffu;
        int enabled = 1;
        if (control == NP_CONTROL_WIFI) {
            fill = app->wifi_state == WIFI_ON ? NP_PIXEL_GREEN :
                   app->wifi_state == WIFI_CONNECTING ? NP_PIXEL_BLUE : 0xffffffffu;
            enabled = app->wifi_state != WIFI_OFF;
        }
        draw_control_box(app, &geometries[control], fill);
        draw_control_glyph(app, control, &geometries[control],
                           control == NP_CONTROL_UNDO ? app->history.undo_count != 0 :
                           control == NP_CONTROL_REDO ? app->history.redo_count != 0 : enabled);
    }
}

static void repaint_full(App *app)
{
    memset(app->memory, 0xff, app->size);
    draw_canvas(app);
    draw_active_strokes(app);
    draw_controls(app);
}

static void clear_rect(App *app, const Rect *rect)
{
    int left = rect->min_x < 0 ? 0 : rect->min_x;
    int right = rect->max_x >= SCREEN_WIDTH ? SCREEN_WIDTH - 1 : rect->max_x;
    int top = rect->min_y < 0 ? 0 : rect->min_y;
    int bottom = rect->max_y >= SCREEN_HEIGHT ? SCREEN_HEIGHT - 1 : rect->max_y;
    int y;
    for (y = top; y <= bottom; y++) {
        int x;
        for (x = left; x <= right; x++) set_pixel(app, x, y, 0xffffffffu);
    }
}

static void stroke_screen_rect(const NPStroke *stroke, const NPViewport *viewport,
                               Rect *rect)
{
    double left;
    double top;
    double right;
    double bottom;
    int radius;
    rect_reset(rect);
    if (stroke == NULL || stroke->npts == 0) return;
    np_canvas_to_screen(viewport, stroke->min_x, stroke->min_y, &left, &top);
    np_canvas_to_screen(viewport, stroke->max_x, stroke->max_y, &right, &bottom);
    if (left > right) { double t = left; left = right; right = t; }
    if (top > bottom) { double t = top; top = bottom; bottom = t; }
    radius = (int)ceil((double)(stroke->width < 1 ? 1 : stroke->width) * viewport->scale / 2.0);
    if (radius < 1) radius = 1;
    rect->valid = 1;
    rect->min_x = (int)floor(left) - radius;
    rect->min_y = (int)floor(top) - radius;
    rect->max_x = (int)ceil(right) + radius;
    rect->max_y = (int)ceil(bottom) + radius;
}

static void repaint_region(App *app, const Rect *region)
{
    size_t i;
    clear_rect(app, region);
    for (i = 0; i < app->canvas.count; i++) {
        Rect stroke_rect;
        stroke_screen_rect(&app->canvas.strokes[i], &app->viewport, &stroke_rect);
        if (stroke_rect.valid && stroke_rect.min_x <= region->max_x &&
            stroke_rect.max_x >= region->min_x && stroke_rect.min_y <= region->max_y &&
            stroke_rect.max_y >= region->min_y &&
            np_canvas_stroke_visible(&app->canvas.strokes[i], &app->viewport,
                                     SCREEN_WIDTH, SCREEN_HEIGHT)) {
            draw_stroke(app, &app->canvas.strokes[i]);
        }
    }
    draw_controls(app);
}

/* Finish on lift and on the pressure-to-zero hover transition. */
static void end_stroke(App *app, TouchSlot *slot)
{
    int saved = 0;
    if (!slot->have_point) return;
    if (slot->eraser) {
        slot->have_point = 0;
        slot->eraser = 0;
        return;
    }
    if (slot->model_stroke.npts != 0) {
        if (np_store_append_stroke(app->notes_path, &slot->model_stroke) != 0) {
            fprintf(stderr, "notepad: could not save stroke: %s\n", strerror(errno));
        } else if (np_canvas_add_stroke(&app->canvas, &slot->model_stroke) != 0) {
            fprintf(stderr, "notepad: could not retain stroke\n");
        } else {
            saved = 1;
            if (np_history_record(&app->history, NP_HISTORY_STROKE,
                                  &slot->model_stroke) != 0) {
                fprintf(stderr, "notepad: could not retain stroke history\n");
            }
        }
    }
    if (!app->gesture_active) {
        if (saved && slot->model_stroke.color != NP_COLOR_BLACK && slot->ink_rect.valid) {
            draw_stroke(app, &slot->model_stroke);
            draw_controls(app);
            (void)refresh_rect(app, WFM_GLRC16, &slot->ink_rect);
        } else if (slot->ink_rect.valid) {
            draw_controls(app);
            (void)refresh_rect(app, WFM_DU, &slot->ink_rect);
        }
        refresh_controls_if_touched(app, &slot->ink_rect);
    }
    rect_reset(&slot->ink_rect);
    clear_model(&slot->model_stroke);
    slot->model_capacity = 0;
    slot->have_point = 0;
    slot->eraser = 0;
}

static void erase_at(App *app, double canvas_x, double canvas_y)
{
    uint32_t id = np_canvas_hit_stroke(&app->canvas, canvas_x, canvas_y, ERASER_RADIUS);
    const NPStroke *hit;
    Rect region;
    int screen_x;
    int screen_y;
    int radius;
    if (id == 0) return;
    hit = np_canvas_find_stroke(&app->canvas, id);
    if (hit == NULL) return;
    stroke_screen_rect(hit, &app->viewport, &region);
    viewport_point_to_screen(&app->viewport, canvas_x, canvas_y, &screen_x, &screen_y);
    radius = (int)ceil(ERASER_RADIUS * app->viewport.scale);
    rect_add(&region, screen_x, screen_y, radius);
    if (np_store_append_erase(app->notes_path, id) != 0) {
        fprintf(stderr, "notepad: could not save erase: %s\n", strerror(errno));
        return;
    }
    if (np_history_record(&app->history, NP_HISTORY_ERASE, hit) != 0 ||
        np_canvas_erase_stroke(&app->canvas, id) != 0) {
        fprintf(stderr, "notepad: could not retain erase history\n");
        return;
    }
    repaint_region(app, &region);
    draw_controls(app);
    (void)refresh_rect(app, WFM_DU, &region);
    refresh_controls_if_touched(app, &region);
}

static void redraw_after_history_change(App *app)
{
    repaint_full(app);
    (void)refresh_full(app, WFM_GC16);
}

static void activate_history_control(App *app, NPControl control)
{
    const NPHistoryOperation *operation;
    int result;
    if (control == NP_CONTROL_UNDO) {
        operation = np_history_peek_undo(&app->history);
        if (operation == NULL) return;
        if (operation->kind == NP_HISTORY_STROKE) {
            if (np_store_append_erase(app->notes_path, operation->stroke.id) != 0) return;
        } else if (np_store_append_stroke(app->notes_path, &operation->stroke) != 0) return;
        result = np_history_undo(&app->history, &app->canvas);
    } else {
        operation = np_history_peek_redo(&app->history);
        if (operation == NULL) return;
        if (operation->kind == NP_HISTORY_STROKE) {
            if (np_store_append_stroke(app->notes_path, &operation->stroke) != 0) return;
        } else if (np_store_append_erase(app->notes_path, operation->stroke.id) != 0) return;
        result = np_history_redo(&app->history, &app->canvas);
    }
    if (result != 0) {
        fprintf(stderr, "notepad: history operation failed\n");
        return;
    }
    redraw_after_history_change(app);
}

static void activate_control(App *app, NPControl control)
{
    if (control == NP_CONTROL_COLOR) {
        app->current_color = (uint8_t)((app->current_color + 1u) % 4u);
        draw_controls(app);
        refresh_controls(app);
    } else if (control == NP_CONTROL_WIDTH) {
        app->current_width = app->current_width == NP_WIDTH_THIN ?
                             NP_WIDTH_THICK : NP_WIDTH_THIN;
        draw_controls(app);
        refresh_controls(app);
    } else if (control == NP_CONTROL_UNDO || control == NP_CONTROL_REDO) {
        activate_history_control(app, control);
    } else if (control == NP_CONTROL_WIFI) {
        /* A tap while connecting is ignored: cancelling wifi.sh would leave
           partially loaded MTK state, while waiting keeps the lifecycle simple. */
        if (app->wifi_state == WIFI_CONNECTING) return;
        (void)start_wifi_operation(app, app->wifi_state == WIFI_ON ?
                                   WIFI_ACTION_DOWN : WIFI_ACTION_UP);
    } else if (control == NP_CONTROL_CLEAR) {
        char archive[PATH_MAX];
        time_t now = time(NULL);
        struct tm local_time;
        if (localtime_r(&now, &local_time) == NULL ||
            strftime(archive, sizeof(archive), "canvas-%Y%m%d-%H%M%S.knp", &local_time) == 0) {
            fprintf(stderr, "notepad: could not name clear archive\n");
            return;
        }
        {
            const char *slash = strrchr(app->notes_path, '/');
            size_t directory_length = slash == NULL ? 0 : (size_t)(slash - app->notes_path + 1);
            char archive_path[PATH_MAX];
            if (directory_length + strlen(archive) + 1 > sizeof(archive_path)) {
                fprintf(stderr, "notepad: clear archive path is too long\n");
                return;
            }
            if (directory_length != 0) memcpy(archive_path, app->notes_path, directory_length);
            memcpy(archive_path + directory_length, archive, strlen(archive) + 1);
            if (access(archive_path, F_OK) == 0) {
                fprintf(stderr, "notepad: clear archive already exists: %s\n", archive_path);
                return;
            }
            if (rename(app->notes_path, archive_path) != 0) {
                fprintf(stderr, "notepad: clear could not archive %s: %s\n",
                        app->notes_path, strerror(errno));
                return;
            }
            {
                NPCanvas empty_canvas;
                np_canvas_init(&empty_canvas);
                if (np_store_compact(app->notes_path, &empty_canvas) != 0) {
                    int saved_errno = errno;
                    np_canvas_free(&empty_canvas);
                    (void)unlink(app->notes_path);
                    if (rename(archive_path, app->notes_path) != 0) {
                        fprintf(stderr, "notepad: clear rollback failed: %s\n", strerror(errno));
                    }
                    fprintf(stderr, "notepad: clear could not create empty notebook: %s\n",
                            strerror(saved_errno));
                    return;
                }
                np_canvas_free(&empty_canvas);
            }
            np_canvas_free(&app->canvas);
            np_canvas_init(&app->canvas);
            np_history_free(&app->history);
            np_history_init(&app->history);
            app->viewport.origin_x = 0.0;
            app->viewport.origin_y = 0.0;
            app->viewport.scale = 1.0;
            redraw_after_history_change(app);
        }
    }
}

static void mark_control_if_needed(App *app, TouchSlot *slot)
{
    int screen_x;
    int screen_y;

    if (slot->control_checked || slot->tracking_id < 0 || slot->pressure <= 0 ||
        slot->tool_type == 2 || !slot->have_x || !slot->have_y) return;
    touch_to_screen(slot->x, slot->y, &screen_x, &screen_y);
    slot->control = np_control_hit_test(screen_x, screen_y, SCREEN_WIDTH, SCREEN_HEIGHT);
    slot->control_checked = 1;
    if (slot->control != NP_CONTROL_NONE) activate_control(app, slot->control);
}

static void mark_control_contacts(App *app, TouchSlot *slots)
{
    int i;
    for (i = 0; i < SLOT_COUNT; i++) mark_control_if_needed(app, &slots[i]);
}

static void commit_report(App *app, TouchSlot *slots, Rect *pending,
                          int eraser_button)
{
    const int pen_width = (int)app->current_width;
    const int render_width = scaled_stroke_width(app, app->current_width);
    const int radius = render_width / 2;
    int i;
    for (i = 0; i < SLOT_COUNT; i++) {
        TouchSlot *slot = &slots[i];
        double canvas_x;
        double canvas_y;
        int screen_x;
        int screen_y;
        if (!slot->pending_point) continue;
        slot->pending_point = 0;
        if (!slot->have_x || !slot->have_y) continue;
        if (slot->control != NP_CONTROL_NONE) {
            /* A pen can hover with the same tracking id; pressure zero ends
               that logical contact, so a later touch-down can select again. */
            if (slot->pressure <= 0) {
                slot->control = NP_CONTROL_NONE;
                slot->control_checked = 0;
            }
            continue;
        }
        if (!slot->reported) {
            slot->reported = 1;
            fprintf(stderr, "notepad: contact tool_type=%d drawing=%d\n",
                    slot->tool_type, slot->pen_active);
        }
        if (!slot->control_checked && slot->pressure > 0 && slot->tool_type != 2) {
            int control_x;
            int control_y;
            touch_to_screen(slot->x, slot->y, &control_x, &control_y);
            slot->control = np_control_hit_test(control_x, control_y,
                                                SCREEN_WIDTH, SCREEN_HEIGHT);
            slot->control_checked = 1;
            if (slot->control != NP_CONTROL_NONE) {
                activate_control(app, slot->control);
                continue;
            }
        }
        if (!slot->pen_active || slot->pressure <= 0) {
            end_stroke(app, slot);
            continue;
        }
        if (slot->eraser && !eraser_button) {
            end_stroke(app, slot);
        }
        /* The touch ranges are 1:1 with the screen, so screen_x/screen_y are
           already exact integer pixels; canvas_x/canvas_y is then the exact
           canvas point and is what gets stored.

           Do NOT round-trip the canvas point back through the screen before
           storing it. That quantises the stored geometry to whatever pixel grid
           happened to be on screen when the stroke was drawn -- invisible at
           scale 1, but it would permanently coarsen every stroke written while
           zoomed out, and no later zoom could recover the detail. Rounding
           belongs on the draw, never on the model. */
        touch_to_screen(slot->x, slot->y, &screen_x, &screen_y);
        np_screen_to_canvas(&app->viewport, screen_x, screen_y, &canvas_x, &canvas_y);
        if (eraser_button) {
            slot->eraser = 1;
            slot->have_point = 1;
            erase_at(app, canvas_x, canvas_y);
            continue;
        }
        if (slot->eraser) end_stroke(app, slot);
        slot->eraser = 0;
        if (!slot->have_point) {
            slot->have_point = 1;
            slot->model_stroke.id = app->next_id++;
            slot->model_stroke.tool = NP_TOOL_PENCIL;
            slot->model_stroke.width = (uint8_t)pen_width;
            slot->model_stroke.color = app->current_color;
            rect_reset(&slot->ink_rect);
            draw_disc(app, screen_x, screen_y, radius, NP_PIXEL_BLACK);
        } else {
            draw_segment(app, slot->last_x, slot->last_y, screen_x, screen_y, render_width,
                         NP_PIXEL_BLACK);
            rect_add(&slot->ink_rect, slot->last_x, slot->last_y, radius);
        }
        if (append_model_point(&slot->model_stroke, &slot->model_capacity,
                           canvas_x, canvas_y) != 0) {
            fprintf(stderr, "notepad: stroke point allocation failed\n");
            stop_requested = 1;
            continue;
        }
        rect_add(&slot->ink_rect, screen_x, screen_y, radius);
        rect_add(pending, screen_x, screen_y, radius);
        slot->last_x = screen_x;
        slot->last_y = screen_y;
    }
    /* Only when the ink actually reached the strip. This ran on every report,
       redrawing six 72x72 boxes -- about 31k bounds-checked writes at the
       panel's ~83 Hz -- for a corner that almost never changes. The call has to
       exist at all because a stroke drawn under the strip would otherwise paint
       over the buttons. */
    if (rect_overlaps_controls(pending)) draw_controls(app);
}

static int handle_touch_event(App *app, TouchSlot *slots, int *current_slot,
                              int *side_button, int *eraser_button,
                              const struct input_event *event, Rect *pending)
{
    TouchSlot *slot;

    if (event->type == EV_SYN && event->code == SYN_REPORT) {
        mark_control_contacts(app, slots);
        int gesture_ended = report_gesture(app, slots);
        commit_report(app, slots, pending, *eraser_button);
        finish_gesture_report(app, gesture_ended);
        return 0;
    }
    if (event->type == EV_KEY && event->code == BTN_STYLUS) {
        *eraser_button = event->value != 0;
        return 0;
    }
    if (event->type == EV_KEY && event->code == BTN_STYLUS2) {
        int pressed = event->value != 0;
        /* app->side_button is the only record of the press: the caller's
           `side_button` is never written back.

           Stamp on *every* press, not only on a 0->1 edge. The action fires on
           release, and a release is only delivered while the pen is within the
           digitizer's field -- press the button in contact, then carry the pen
           out of range before letting go, and that release never arrives. The
           flag would stay stuck at 1, the next press would not re-stamp, and
           the press after that would measure its duration from a timestamp
           minutes old: a tap read as a long press, changing the width when the
           user asked for the next colour. Measured: BTN_STYLUS2 transitions do
           arrive while merely hovering, so this is reachable in normal use. */
        if (pressed) {
            clock_gettime(CLOCK_MONOTONIC, &app->side_button_pressed);
        } else if (app->side_button) {
            long duration = elapsed_ms(&app->side_button_pressed);
            if (duration < 500) {
                app->current_color = (uint8_t)((app->current_color + 1u) % 4u);
            } else {
                app->current_width = app->current_width == NP_WIDTH_THIN ?
                                      NP_WIDTH_THICK : NP_WIDTH_THIN;
            }
            /* Only the controls changed -- the canvas is untouched, so repaint
               and refresh just their combined corner rectangle. */
            draw_controls(app);
            refresh_controls(app);
        }
        app->side_button = pressed;
        return 0;
    }
    if (event->type != EV_ABS) return 0;
    if (event->code == ABS_MT_SLOT) {
        if (event->value >= 0 && event->value < SLOT_COUNT) *current_slot = event->value;
        return 0;
    }
    slot = &slots[*current_slot];
    if (event->code == ABS_MT_TRACKING_ID) {
        if (event->value < 0) {
            end_stroke(app, slot);
            initialize_slot(slot);
        } else if (event->value != slot->tracking_id) {
            if (slot->have_point) end_stroke(app, slot);
            /* A *new* contact -- and only then. This panel re-sends the same
               ABS_MT_TRACKING_ID on every report (measured: 1.02 per
               SYN_REPORT, the same id repeated for the whole contact, -1 only
               at lift), where protocol B nominally sends it once on change.
               Treating every repeat as a new contact reset have_point each
               report, so every point took the pen-down branch: strokes came out
               as a row of isolated discs that never reached draw_segment, and
               the stroke bbox was cleared often enough that the pen-up DU
               covered only the final dot. Comparing against the current id is
               correct for both driver styles. */
            slot->tracking_id = event->value;
            slot->have_point = 0;
            slot->have_x = 0;
            slot->have_y = 0;
            slot->pending_point = 0;
            slot->reported = 0;
            slot->control = NP_CONTROL_NONE;
            slot->control_checked = 0;
            /* tool_type is not yet known here: ABS_MT_TOOL_TYPE follows the
               tracking id within the same report, and initialize_slot has just
               zeroed it. Both pen_active and the diagnostic log are therefore
               settled at the first position event, not here. */
        }
        return 0;
    }
    if (event->code == ABS_MT_TOOL_TYPE) {
        slot->tool_type = event->value;
        if (slot->tracking_id >= 0) slot->pen_active = tool_draws(event->value);
        return 0;
    }
    /* Record only; the draw happens at SYN_REPORT in commit_report(), where
       both axes are known to belong to the same report. */
    if (event->code == ABS_MT_POSITION_X) {
        slot->x = event->value;
        slot->have_x = 1;
        slot->pending_point = 1;
    } else if (event->code == ABS_MT_POSITION_Y) {
        slot->y = event->value;
        slot->have_y = 1;
        slot->pending_point = 1;
    } else if (event->code == ABS_MT_PRESSURE) {
        slot->pressure = event->value;
    }
    (void)side_button;
    (void)eraser_button;
    return 0;
}

static int read_light(int *value)
{
    FILE *file = fopen(LIGHT_PATH, "r");
    int result;
    if (file == NULL) return -1;
    result = fscanf(file, "%d", value);
    fclose(file);
    return result == 1 ? 0 : -1;
}

static int write_light(int value)
{
    FILE *file = fopen(LIGHT_PATH, "w");
    if (file == NULL) return -1;
    if (fprintf(file, "%d\n", value) < 0) {
        fclose(file);
        return -1;
    }
    return fclose(file) == 0 ? 0 : -1;
}

static int ensure_notes_directory(const char *path)
{
    char directory[PATH_MAX];
    char *slash;
    if (path == NULL || strlen(path) >= sizeof(directory)) return -1;
    (void)strcpy(directory, path);
    slash = strrchr(directory, '/');
    if (slash == NULL) return 0;
    if (slash == directory) return 0;
    *slash = '\0';
    if (mkdir(directory, 0755) == 0 || errno == EEXIST) return 0;
    /* The configured default has two levels; create parents for custom paths too. */
    for (slash = directory + 1; *slash != '\0'; slash++) {
        if (*slash != '/') continue;
        *slash = '\0';
        if (mkdir(directory, 0755) != 0 && errno != EEXIST) return -1;
        *slash = '/';
    }
    return mkdir(directory, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static int setup_framebuffer(App *app)
{
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    size_t size;

    app->fbfd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (app->fbfd < 0) {
        fprintf(stderr, "notepad: open /dev/fb0: %s\n", strerror(errno));
        return -1;
    }
    if (ioctl(app->fbfd, FBIOGET_FSCREENINFO, &fix) != 0 ||
        ioctl(app->fbfd, FBIOGET_VSCREENINFO, &var) != 0) {
        fprintf(stderr, "notepad: cannot read framebuffer info: %s\n", strerror(errno));
        return -1;
    }
    if (fix.line_length != FB_STRIDE || var.xres != SCREEN_WIDTH ||
        var.yres != SCREEN_HEIGHT || var.bits_per_pixel != 32) {
        fprintf(stderr, "notepad: unexpected framebuffer geometry (%ux%u, %u bpp, stride %u)\n",
                var.xres, var.yres, var.bits_per_pixel, fix.line_length);
        return -1;
    }
    size = (size_t)FB_STRIDE * SCREEN_HEIGHT;
    app->memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, app->fbfd, 0);
    if (app->memory == MAP_FAILED) {
        app->memory = NULL;
        fprintf(stderr, "notepad: mmap framebuffer: %s\n", strerror(errno));
        return -1;
    }
    app->size = size;
    memset(&app->fbink_cfg, 0, sizeof(app->fbink_cfg));
    app->fbink_cfg.is_quiet = true;
    if (fbink_init(app->fbfd, &app->fbink_cfg) != 0) {
        fprintf(stderr, "notepad: fbink_init failed\n");
        return -1;
    }
    /* Same white-then-canvas-then-swatch sequence every full redraw uses. */
    repaint_full(app);
    if (refresh_full(app, WFM_GC16) != 0) {
        fprintf(stderr, "notepad: initial GC16 refresh failed\n");
        return -1;
    }
    return 0;
}

static int setup_light(App *app)
{
    int current;
    if (read_light(&current) != 0) {
        fprintf(stderr, "notepad: cannot read %s: %s\n", LIGHT_PATH, strerror(errno));
        return -1;
    }
    app->light_previous = current;
    if (write_light(current < 30 ? 30 : current) != 0) {
        fprintf(stderr, "notepad: cannot raise frontlight: %s\n", strerror(errno));
        return -1;
    }
    app->light_changed = 1;
    return 0;
}

static void cleanup(App *app, int touch_fd, int power_fd, int page_fd)
{
    /* Do not force Wi-Fi down on exit; dash.sh owns its next-cycle lifecycle,
       and tearing down a connection the user requested would be surprising. */
    reap_wifi_operation(app);
    if (app->slots != NULL) {
        size_t i;
        for (i = 0; i < SLOT_COUNT; i++) end_stroke(app, &app->slots[i]);
    }
    if (np_store_should_compact(&app->canvas, COMPACTION_THRESHOLD)) {
        if (np_store_compact(app->notes_path, &app->canvas) != 0) {
            fprintf(stderr, "notepad: could not compact notes: %s\n", strerror(errno));
        }
    }
    np_history_free(&app->history);
    np_canvas_free(&app->canvas);
    if (app->light_changed) (void)write_light(app->light_previous);
    if (app->memory != NULL) (void)munmap(app->memory, app->size);
    if (app->fbfd >= 0) close(app->fbfd);
    if (touch_fd >= 0) close(touch_fd);
    if (power_fd >= 0) close(power_fd);
    if (page_fd >= 0) close(page_fd);
}

int main(int argc, char **argv)
{
    App app;
    TouchSlot slots[SLOT_COUNT];
    struct pollfd poll_fds[3];
    struct input_event events[32];
    int touch_fd = -1;
    int power_fd = -1;
    int page_fd = -1;
    int current_slot = 0;
    int side_button = 0;
    int eraser_button = 0;
    int refresh_failures = 0;
    int exit_status = EXIT_SUCCESS;
    int power_press_pending = 0;
    struct timespec power_press_time;
    struct timespec last_refresh;
    Rect pending;
    const char *notes_path = DEFAULT_NOTES_PATH;
    const char *wifi_script = DEFAULT_WIFI_SCRIPT;
    int idle_seconds = DEFAULT_IDLE_SECONDS;
    struct timespec last_activity;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--finger-draws") == 0) {
            finger_draws = 1;
        } else if (strcmp(argv[i], "--notes") == 0 && i + 1 < argc) {
            notes_path = argv[++i];
        } else if (strcmp(argv[i], "--wifi-script") == 0 && i + 1 < argc) {
            wifi_script = argv[++i];
        } else if (strcmp(argv[i], "--idle-seconds") == 0 && i + 1 < argc) {
            idle_seconds = atoi(argv[++i]);
            if (idle_seconds < 0) idle_seconds = 0;
        } else {
            fprintf(stderr, "notepad: unknown option %s\n", argv[i]);
            return EXIT_FAILURE;
        }
    }
    rect_reset(&pending);
    memset(&app, 0, sizeof(app));
    app.fbfd = -1;
    app.notes_path = notes_path;
    app.wifi_script = wifi_script;
    app.wifi_pid = -1;
    app.wifi_state = WIFI_OFF;
    app.wifi_action = WIFI_ACTION_STATUS;
    app.current_color = NP_COLOR_BLACK;
    app.current_width = NP_WIDTH_THIN;
    app.slots = slots;
    app.viewport.origin_x = 0.0;
    app.viewport.origin_y = 0.0;
    app.viewport.scale = 1.0;
    app.next_id = 1;
    np_canvas_init(&app.canvas);
    np_history_init(&app.history);
    for (i = 0; i < SLOT_COUNT; i++) initialize_slot(&slots[i]);
    if (ensure_notes_directory(app.notes_path) != 0) {
        fprintf(stderr, "notepad: could not create notes directory for %s\n",
                app.notes_path);
        cleanup(&app, touch_fd, power_fd, page_fd);
        return EXIT_FAILURE;
    }
    if (np_store_load(app.notes_path, &app.canvas, NULL) != 0) {
        /* A truncated tail is already tolerated by the replay, so reaching here
           means the file is genuinely unreadable -- bad magic, an unknown
           version, a corrupt opcode. Refusing to start would make the notepad
           permanently unreachable on a device with no UI to diagnose it, and
           overwriting it would destroy whatever is still in there. Move it
           aside instead: the notepad comes up empty and usable, and the old
           file is still on the card to recover from a real computer. */
        char aside[512];
        (void)snprintf(aside, sizeof(aside), "%s.bad", app.notes_path);
        if (rename(app.notes_path, aside) != 0) {
            fprintf(stderr, "notepad: %s is unreadable and cannot be moved aside: %s\n",
                    app.notes_path, strerror(errno));
            cleanup(&app, touch_fd, power_fd, page_fd);
            return EXIT_FAILURE;
        }
        fprintf(stderr, "notepad: %s was unreadable; moved to %s, starting empty\n",
                app.notes_path, aside);
        np_canvas_free(&app.canvas);
        np_canvas_init(&app.canvas);
    }
    for (i = 0; i < (int)app.canvas.count; i++) {
        if (app.canvas.strokes[i].id >= app.next_id) app.next_id = app.canvas.strokes[i].id + 1;
    }
    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    if (discover_input("Elan Touchscreen", &touch_fd) != 0 ||
        discover_input("bd71828-pwrkey", &power_fd) != 0 ||
        discover_input("gpio-keys", &page_fd) != 0 ||
        validate_touch_axes(touch_fd) != 0 || setup_framebuffer(&app) != 0 ||
        setup_light(&app) != 0) {
        cleanup(&app, touch_fd, power_fd, page_fd);
        return EXIT_FAILURE;
    }
    /* Probe once without assuming dash.sh left the radio down. The child is
       reaped from the poll loop, and its result becomes the initial state. */
    (void)start_wifi_operation(&app, WIFI_ACTION_STATUS);
    poll_fds[0].fd = touch_fd;
    poll_fds[0].events = POLLIN;
    poll_fds[1].fd = power_fd;
    poll_fds[1].events = POLLIN;
    poll_fds[2].fd = page_fd;
    poll_fds[2].events = POLLIN;
    fprintf(stderr, "notepad: ready; finger_draws=%d; idle_seconds=%d; "
                    "page key switches app; power double press exits\n",
            finger_draws, idle_seconds);
    gesture_init(&app.gesture);
    clock_gettime(CLOCK_MONOTONIC, &app.last_gesture_refresh);
    clock_gettime(CLOCK_MONOTONIC, &last_refresh);
    clock_gettime(CLOCK_MONOTONIC, &last_activity);
    while (!stop_requested) {
        int poll_result = poll(poll_fds, 3, REFRESH_INTERVAL_MS);
        reap_wifi_operation(&app);
        /* Any traffic on any of the three devices counts, including a hovering
           pen: the point is to catch a notepad nobody is near, not to punish
           someone who is thinking. Sampled before the fds are read, so it costs
           nothing per event. */
        if (poll_result > 0) clock_gettime(CLOCK_MONOTONIC, &last_activity);
        else if (idle_seconds > 0 &&
                 elapsed_ms(&last_activity) >= (long)idle_seconds * 1000L) {
            fprintf(stderr, "notepad: idle for %ds; returning to the dashboard\n",
                    idle_seconds);
            /* Status 0: hand the panel back to page one, the same as a page
               key. Never 10 -- an unattended notepad must not reboot the
               device out from under whatever the dashboard was doing. */
            stop_requested = 1;
            break;
        }
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "notepad: poll: %s\n", strerror(errno));
            break;
        }
        /* A timeout is not a reason to skip the flush below: it is exactly when
           ink that arrived just under the interval finally goes out. */
        if (poll_result > 0 && (poll_fds[1].revents & POLLIN) != 0) {
            ssize_t bytes = read(power_fd, events, sizeof(events));
            size_t count = bytes > 0 ? (size_t)bytes / sizeof(events[0]) : 0;
            size_t event_index;
            for (event_index = 0; event_index < count; event_index++) {
                if (events[event_index].type == EV_KEY && events[event_index].code == KEY_POWER &&
                    events[event_index].value == 0) {
                    if (power_press_pending &&
                        elapsed_ms(&power_press_time) <= POWER_PRESS_WINDOW_SECONDS * 1000L) {
                        /* dash.sh owns the reboot; status 10 means exit to Nickel. */
                        exit_status = EXIT_TO_NICKEL_STATUS;
                        stop_requested = 1;
                    } else {
                        power_press_pending = 1;
                        clock_gettime(CLOCK_MONOTONIC, &power_press_time);
                        draw_exit_hint(&app);
                    }
                }
            }
        }
        if (poll_result > 0 && (poll_fds[2].revents & POLLIN) != 0) {
            ssize_t bytes = read(page_fd, events, sizeof(events));
            size_t count = bytes > 0 ? (size_t)bytes / sizeof(events[0]) : 0;
            size_t event_index;
            for (event_index = 0; event_index < count; event_index++) {
                if (events[event_index].type == EV_KEY &&
                    (events[event_index].code == KEY_F23 ||
                     events[event_index].code == KEY_F24) && events[event_index].value == 0) {
                    /* Contract with dash.sh: 0 returns to the dashboard. */
                    if (!stop_requested) {
                        exit_status = EXIT_SUCCESS;
                        stop_requested = 1;
                    }
                }
            }
        }
        if (poll_result > 0 && (poll_fds[0].revents & POLLIN) != 0 && !stop_requested) {
            ssize_t bytes = read(touch_fd, events, sizeof(events));
            size_t count = bytes > 0 ? (size_t)bytes / sizeof(events[0]) : 0;
            size_t event_index;
            for (event_index = 0; event_index < count && !stop_requested; event_index++) {
                if (handle_touch_event(&app, slots, &current_slot, &side_button,
                                       &eraser_button, &events[event_index], &pending) != 0) {
                    stop_requested = 1;
                }
            }
        }
        if (power_press_pending &&
            elapsed_ms(&power_press_time) > POWER_PRESS_WINDOW_SECONDS * 1000L) {
            power_press_pending = 0;
            repaint_full(&app);
            (void)refresh_full(&app, WFM_GLRC16);
        }
        /* Rate-limited, and driven by the clock rather than by SYN_REPORT: this
           panel reports at ~83 Hz, so refreshing per report would issue four
           times the ioctls the driver needs and swamp the ink it is drawing.
           `pending` survives across iterations, so ink that arrives split over
           two reads is never dropped -- it just goes out with the next flush. */
        if (pending.valid && !app.gesture_active && !app.suppress_refresh &&
            elapsed_ms(&last_refresh) >= REFRESH_INTERVAL_MS) {
            if (refresh_rect(&app, WFM_A2, &pending) != 0) {
                refresh_failures++;
                fprintf(stderr, "notepad: A2 refresh failed (%d consecutive)\n",
                        refresh_failures);
                /* One failure is cosmetic -- the ink is already in the
                   framebuffer and the next pass covers it. A run of them means
                   the driver is gone, and then there is nothing left to draw on. */
                if (refresh_failures >= 10) {
                    fprintf(stderr, "notepad: giving up on the eInk driver\n");
                    stop_requested = 1;
                }
            } else {
                refresh_failures = 0;
            }
            rect_reset(&pending);
            clock_gettime(CLOCK_MONOTONIC, &last_refresh);
        }
    }
    cleanup(&app, touch_fd, power_fd, page_fd);
    return exit_status;
}
