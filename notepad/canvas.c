#include "notepad.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Right to left across the top edge. Undo sits to the left of redo so the pair
   reads in the same direction as the arrows on them. */
static const NPControl control_order[] = {
    NP_CONTROL_COLOR,
    NP_CONTROL_WIDTH,
    NP_CONTROL_REDO,
    NP_CONTROL_UNDO,
    NP_CONTROL_CLEAR,
    NP_CONTROL_WIFI
};

int np_control_slot(NPControl control)
{
    size_t i;
    for (i = 0; i < sizeof(control_order) / sizeof(control_order[0]); i++) {
        if (control_order[i] == control) return (int)i;
    }
    return -1;
}

int np_control_geometry(NPControl control, int screen_width, int screen_height,
                        NPControlGeometry *geometry)
{
    int slot;

    int color_x;
    int x;

    if (geometry == NULL || screen_width <= 0 || screen_height <= 0 ||
        control < NP_CONTROL_COLOR || control > NP_CONTROL_WIFI) return -1;
    slot = np_control_slot(control);
    if (slot < 0) return -1;
    color_x = screen_width - NP_CONTROL_MARGIN - NP_CONTROL_SIZE;
    x = color_x - (slot * (NP_CONTROL_GAP + NP_CONTROL_SIZE));
    geometry->draw.min_x = x;
    geometry->draw.min_y = NP_CONTROL_MARGIN;
    geometry->draw.max_x = x + NP_CONTROL_SIZE - 1;
    geometry->draw.max_y = NP_CONTROL_MARGIN + NP_CONTROL_SIZE - 1;
    geometry->hit.min_x = x - NP_CONTROL_HIT_PADDING;
    geometry->hit.min_y = NP_CONTROL_MARGIN - NP_CONTROL_HIT_PADDING;
    geometry->hit.max_x = x + NP_CONTROL_SIZE - 1 + NP_CONTROL_HIT_PADDING;
    geometry->hit.max_y = NP_CONTROL_MARGIN + NP_CONTROL_SIZE - 1 + NP_CONTROL_HIT_PADDING;
    return 0;
}

NPControl np_control_hit_test(int screen_x, int screen_y, int screen_width,
                              int screen_height)
{
    NPControl control;
    for (control = NP_CONTROL_COLOR; control <= NP_CONTROL_WIFI; control++) {
        NPControlGeometry geometry;
        if (np_control_geometry(control, screen_width, screen_height, &geometry) != 0) {
            return NP_CONTROL_NONE;
        }
        if (screen_x >= geometry.hit.min_x && screen_x <= geometry.hit.max_x &&
            screen_y >= geometry.hit.min_y && screen_y <= geometry.hit.max_y) {
            return control;
        }
    }
    return NP_CONTROL_NONE;
}

static void stroke_bounds(NPStroke *stroke)
{
    size_t i;

    if (stroke->npts == 0 || stroke->points == NULL) {
        stroke->min_x = stroke->min_y = 0.0;
        stroke->max_x = stroke->max_y = 0.0;
        return;
    }
    stroke->min_x = stroke->max_x = stroke->points[0].x;
    stroke->min_y = stroke->max_y = stroke->points[0].y;
    for (i = 1; i < stroke->npts; i++) {
        if (stroke->points[i].x < stroke->min_x) stroke->min_x = stroke->points[i].x;
        if (stroke->points[i].x > stroke->max_x) stroke->max_x = stroke->points[i].x;
        if (stroke->points[i].y < stroke->min_y) stroke->min_y = stroke->points[i].y;
        if (stroke->points[i].y > stroke->max_y) stroke->max_y = stroke->points[i].y;
    }
}

static int stroke_copy(NPStroke *dst, const NPStroke *src)
{
    memset(dst, 0, sizeof(*dst));
    *dst = *src;
    if (src->npts != 0) {
        if (src->points == NULL || src->npts > SIZE_MAX / sizeof(*src->points)) return -1;
        dst->points = malloc(src->npts * sizeof(*src->points));
        if (dst->points == NULL) return -1;
        memcpy(dst->points, src->points, src->npts * sizeof(*src->points));
    }
    stroke_bounds(dst);
    return 0;
}

void np_canvas_init(NPCanvas *canvas)
{
    memset(canvas, 0, sizeof(*canvas));
}

void np_canvas_free(NPCanvas *canvas)
{
    size_t i;
    for (i = 0; i < canvas->count; i++) free(canvas->strokes[i].points);
    free(canvas->strokes);
    memset(canvas, 0, sizeof(*canvas));
}

int np_canvas_add_stroke(NPCanvas *canvas, const NPStroke *stroke)
{
    size_t i;
    NPStroke copy;

    if (canvas == NULL) return -1;

    if (stroke == NULL || (stroke->npts != 0 && stroke->points == NULL)) return -1;

    /* Skip the duplicate scan when the id is above every id seen. Ids are
       handed out monotonically, so replay -- every stroke in the file, in
       order -- never collides, and only redo re-adds an existing one.
       Measured on device before this: 20000 strokes took 7.7s to load against
       164ms for 5000 -- four times the strokes for forty-seven times the work,
       because every insert rescanned the whole list. The guard holds for any
       insertion order, not only ascending: if an id exceeds the maximum
       present, nothing present can carry it. */
    i = (canvas->count != 0 && stroke->id > canvas->max_id) ? canvas->count : 0;
    for (; i < canvas->count; i++) {
        if (canvas->strokes[i].id == stroke->id) {
            if (stroke_copy(&copy, stroke) != 0) return -1;
            free(canvas->strokes[i].points);
            canvas->strokes[i] = copy;
            canvas->dead_records++;
            return 0;
        }
    }
    if (canvas->count == canvas->capacity) {
        size_t new_capacity = canvas->capacity == 0 ? 16 : canvas->capacity * 2;
        NPStroke *new_strokes;
        if (new_capacity < canvas->capacity || new_capacity > SIZE_MAX / sizeof(*new_strokes)) return -1;
        new_strokes = realloc(canvas->strokes, new_capacity * sizeof(*new_strokes));
        if (new_strokes == NULL) return -1;
        canvas->strokes = new_strokes;
        canvas->capacity = new_capacity;
    }
    if (stroke_copy(&copy, stroke) != 0) return -1;
    canvas->strokes[canvas->count++] = copy;
    if (stroke->id > canvas->max_id) canvas->max_id = stroke->id;
    return 0;
}

int np_canvas_erase_stroke(NPCanvas *canvas, uint32_t id)
{
    size_t i;
    for (i = 0; i < canvas->count; i++) {
        if (canvas->strokes[i].id == id) {
            free(canvas->strokes[i].points);
            /* Close the gap in order rather than swapping the last stroke into
               it. Strokes are drawn in array order, so a swap silently changes
               which of two overlapping strokes is on top -- a red line crossing
               a black one would swap over after an unrelated erase elsewhere.
               The memmove is O(n) where the swap was O(1), but n here is the
               stroke count and an erase happens at human speed. */
            if (i + 1 < canvas->count) {
                memmove(&canvas->strokes[i], &canvas->strokes[i + 1],
                        (canvas->count - i - 1) * sizeof(canvas->strokes[0]));
            }
            canvas->count--;
            canvas->dead_records++;
            return 0;
        }
    }
    canvas->dead_records++;
    return 1;
}

const NPStroke *np_canvas_find_stroke(const NPCanvas *canvas, uint32_t id)
{
    size_t i;
    for (i = 0; i < canvas->count; i++) {
        if (canvas->strokes[i].id == id) return &canvas->strokes[i];
    }
    return NULL;
}

int np_canvas_stroke_visible(const NPStroke *stroke, const NPViewport *viewport,
                             int screen_width, int screen_height)
{
    double left, right, top, bottom;
    if (stroke == NULL || viewport == NULL || viewport->scale <= 0.0 ||
        screen_width <= 0 || screen_height <= 0) return 0;
    np_screen_to_canvas(viewport, 0.0, 0.0, &left, &top);
    np_screen_to_canvas(viewport, (double)screen_width, (double)screen_height,
                        &right, &bottom);
    if (left > right) { double t = left; left = right; right = t; }
    if (top > bottom) { double t = top; top = bottom; bottom = t; }
    return stroke->max_x >= left && stroke->min_x <= right &&
           stroke->max_y >= top && stroke->min_y <= bottom;
}

static double point_segment_distance(double px, double py, double ax, double ay,
                                     double bx, double by)
{
    double dx = bx - ax;
    double dy = by - ay;
    double length2 = dx * dx + dy * dy;
    double t;
    double qx, qy;
    if (length2 == 0.0) return hypot(px - ax, py - ay);
    t = ((px - ax) * dx + (py - ay) * dy) / length2;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    qx = ax + t * dx;
    qy = ay + t * dy;
    return hypot(px - qx, py - qy);
}

uint32_t np_canvas_hit_stroke(const NPCanvas *canvas, double canvas_x,
                              double canvas_y, double radius)
{
    size_t i, j;
    double radius2;
    if (canvas == NULL || radius < 0.0) return 0;
    radius2 = radius * radius;
    for (i = canvas->count; i > 0; i--) {
        const NPStroke *stroke = &canvas->strokes[i - 1];
        if (canvas_x < stroke->min_x - radius || canvas_x > stroke->max_x + radius ||
            canvas_y < stroke->min_y - radius || canvas_y > stroke->max_y + radius) continue;
        if (stroke->npts == 1) {
            double dx = canvas_x - stroke->points[0].x;
            double dy = canvas_y - stroke->points[0].y;
            if (dx * dx + dy * dy <= radius2) return stroke->id;
        } else {
            for (j = 1; j < stroke->npts; j++) {
                double d = point_segment_distance(canvas_x, canvas_y,
                                                  stroke->points[j - 1].x,
                                                  stroke->points[j - 1].y,
                                                  stroke->points[j].x,
                                                  stroke->points[j].y);
                if (d * d <= radius2) return stroke->id;
            }
        }
    }
    return 0;
}

void np_canvas_to_screen(const NPViewport *viewport, double canvas_x,
                         double canvas_y, double *screen_x, double *screen_y)
{
    *screen_x = (canvas_x - viewport->origin_x) * viewport->scale;
    *screen_y = (canvas_y - viewport->origin_y) * viewport->scale;
}

void np_screen_to_canvas(const NPViewport *viewport, double screen_x,
                         double screen_y, double *canvas_x, double *canvas_y)
{
    *canvas_x = viewport->origin_x + screen_x / viewport->scale;
    *canvas_y = viewport->origin_y + screen_y / viewport->scale;
}

void np_viewport_zoom_about(NPViewport *viewport, double screen_x,
                            double screen_y, double factor)
{
    const double min_scale = 0.1;
    const double max_scale = 8.0;
    double anchor_x, anchor_y, new_scale;
    if (viewport == NULL || viewport->scale <= 0.0 || factor <= 0.0) return;
    anchor_x = viewport->origin_x + screen_x / viewport->scale;
    anchor_y = viewport->origin_y + screen_y / viewport->scale;
    new_scale = viewport->scale * factor;
    if (new_scale < min_scale) new_scale = min_scale;
    if (new_scale > max_scale) new_scale = max_scale;
    viewport->scale = new_scale;
    viewport->origin_x = anchor_x - screen_x / new_scale;
    viewport->origin_y = anchor_y - screen_y / new_scale;
}
