#ifndef KNO_NOTEPAD_H
#define KNO_NOTEPAD_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define NP_SCREEN_WIDTH 1264
#define NP_SCREEN_HEIGHT 1680
#define NP_FORMAT_VERSION 2u

typedef enum {
    NP_COLOR_BLACK = 0,
    NP_COLOR_RED = 1,
    NP_COLOR_BLUE = 2,
    NP_COLOR_GREEN = 3
} NPColor;

/* The framebuffer is RGBA, so little-endian uint32 pixels store R,G,B,A.
   Keep these literals beside the known-good black: 0xff000000 is bytes
   00 00 00 ff, then only the corresponding channel byte changes. */
#define NP_PIXEL_BLACK 0xff000000u
#define NP_PIXEL_RED   0xff0000ffu /* bytes ff 00 00 ff */
#define NP_PIXEL_BLUE  0xffff0000u /* bytes 00 00 ff ff */
#define NP_PIXEL_GREEN 0xff00ff00u /* bytes 00 ff 00 ff */

#define NP_WIDTH_THIN 5u
#define NP_WIDTH_THICK 14u

#define NP_CONTROL_SIZE 72
#define NP_CONTROL_GAP 12
#define NP_CONTROL_MARGIN 14
#define NP_CONTROL_HIT_PADDING 4

typedef struct {
    int min_x;
    int min_y;
    int max_x;
    int max_y;
} NPRect;

typedef enum {
    NP_CONTROL_NONE = -1,
    NP_CONTROL_COLOR = 0,
    NP_CONTROL_WIDTH = 1,
    NP_CONTROL_UNDO = 2,
    NP_CONTROL_REDO = 3,
    NP_CONTROL_CLEAR = 4,
    NP_CONTROL_WIFI = 5
} NPControl;

typedef struct {
    NPRect draw;
    NPRect hit;
} NPControlGeometry;

/* Screen-fixed overlay geometry, shared with the input hit-test. */
/* Where a control sits, counting from the right edge. Deliberately separate
   from the NPControl values: the enum says what a button *is* and is used for
   dispatch and for contiguous loops, while this says where it sits. Conflating
   the two means reordering two buttons silently renumbers others -- swapping
   undo and redo in the enum would have made `for (c = UNDO; c <= CLEAR; c++)`
   skip one of them and stop drawing it. */
int np_control_slot(NPControl control);

int np_control_geometry(NPControl control, int screen_width, int screen_height,
                        NPControlGeometry *geometry);
NPControl np_control_hit_test(int screen_x, int screen_y, int screen_width,
                              int screen_height);

typedef struct {
    float x;
    float y;
} NPPoint;

typedef enum {
    NP_TOOL_PENCIL = 0,
    NP_TOOL_ERASER_CREATED = 1
} NPTool;

typedef struct {
    uint32_t id;
    uint8_t tool;
    uint8_t width;
    uint8_t color;
    size_t npts;
    NPPoint *points;
    double min_x;
    double min_y;
    double max_x;
    double max_y;
} NPStroke;

typedef struct {
    NPStroke *strokes;
    size_t count;
    size_t capacity;
    size_t dead_records;
    /* Highest id ever added. An id above it cannot collide with anything
       present, which is what keeps loading from being quadratic -- see
       np_canvas_add_stroke. Zeroed by np_canvas_init's memset. */
    uint32_t max_id;
} NPCanvas;

typedef enum {
    NP_HISTORY_STROKE = 0,
    NP_HISTORY_ERASE = 1
} NPHistoryKind;

typedef struct {
    NPHistoryKind kind;
    NPStroke stroke;
} NPHistoryOperation;

typedef struct {
    NPHistoryOperation *undo;
    NPHistoryOperation *redo;
    size_t undo_count;
    size_t undo_capacity;
    size_t redo_count;
    size_t redo_capacity;
} NPHistory;

typedef struct {
    double origin_x;
    double origin_y;
    double scale;
} NPViewport;

typedef struct {
    size_t stroke_records;
    size_t erase_records;
    size_t discarded_tail_records;
} NPStoreStats;

void np_canvas_init(NPCanvas *canvas);
void np_canvas_free(NPCanvas *canvas);
int np_canvas_add_stroke(NPCanvas *canvas, const NPStroke *stroke);
int np_canvas_erase_stroke(NPCanvas *canvas, uint32_t id);
const NPStroke *np_canvas_find_stroke(const NPCanvas *canvas, uint32_t id);

void np_history_init(NPHistory *history);
void np_history_free(NPHistory *history);
int np_history_record(NPHistory *history, NPHistoryKind kind, const NPStroke *stroke);
const NPHistoryOperation *np_history_peek_undo(const NPHistory *history);
const NPHistoryOperation *np_history_peek_redo(const NPHistory *history);
int np_history_undo(NPHistory *history, NPCanvas *canvas);
int np_history_redo(NPHistory *history, NPCanvas *canvas);
int np_canvas_stroke_visible(const NPStroke *stroke, const NPViewport *viewport,
                             int screen_width, int screen_height);
uint32_t np_canvas_hit_stroke(const NPCanvas *canvas, double canvas_x,
                              double canvas_y, double radius);

void np_canvas_to_screen(const NPViewport *viewport, double canvas_x,
                        double canvas_y, double *screen_x, double *screen_y);
void np_screen_to_canvas(const NPViewport *viewport, double screen_x,
                         double screen_y, double *canvas_x, double *canvas_y);
void np_viewport_zoom_about(NPViewport *viewport, double screen_x,
                            double screen_y, double factor);

/* Append one exact-format record. The file header is created if needed. */
int np_store_append_stroke(const char *path, const NPStroke *stroke);
int np_store_append_erase(const char *path, uint32_t id);

/* Replay a complete file, retaining records before a truncated tail. */
int np_store_replay(FILE *file, NPCanvas *canvas, NPStoreStats *stats);
int np_store_load(const char *path, NPCanvas *canvas, NPStoreStats *stats);
int np_store_compact(const char *path, const NPCanvas *canvas);
int np_store_should_compact(const NPCanvas *canvas, size_t threshold);

#endif
