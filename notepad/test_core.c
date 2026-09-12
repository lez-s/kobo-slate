#include "notepad.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static const NPPoint first_points[] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {20.0f, 5.0f}};
static const NPPoint second_points[] = {{-4.5f, 8.25f}, {2.0f, 12.0f}};

static NPStroke make_stroke(uint32_t id, uint8_t width, uint8_t color, const NPPoint *points, size_t npts)
{
    NPStroke stroke;
    memset(&stroke, 0, sizeof(stroke));
    stroke.id = id;
    stroke.tool = NP_TOOL_PENCIL;
    stroke.width = width;
    stroke.color = color;
    stroke.points = (NPPoint *)points;
    stroke.npts = npts;
    return stroke;
}

static int same_stroke(const NPStroke *a, const NPStroke *b)
{
    size_t i;
    if (a->id != b->id || a->tool != b->tool || a->width != b->width || a->color != b->color || a->npts != b->npts) return 0;
    for (i = 0; i < a->npts; i++) {
        if (a->points[i].x != b->points[i].x || a->points[i].y != b->points[i].y) return 0;
    }
    return 1;
}

static long file_size(const char *path)
{
    struct stat st;
    CHECK(stat(path, &st) == 0);
    return (long)st.st_size;
}

static int copy_prefix(const char *source, const char *destination, long length)
{
    FILE *in = fopen(source, "rb");
    FILE *out = fopen(destination, "wb");
    unsigned char buffer[128];
    long remaining = length;
    if (in == NULL || out == NULL) {
        if (in != NULL) fclose(in);
        if (out != NULL) fclose(out);
        return -1;
    }
    while (remaining > 0) {
        size_t want = (size_t)(remaining < (long)sizeof(buffer) ? remaining : (long)sizeof(buffer));
        size_t got = fread(buffer, 1, want, in);
        if (got != want || fwrite(buffer, 1, got, out) != got) {
            fclose(in);
            fclose(out);
            return -1;
        }
        remaining -= (long)got;
    }
    if (fclose(in) != 0 || fclose(out) != 0) return -1;
    return 0;
}

static int test_round_trip_and_erase(const char *path)
{
    NPCanvas canvas;
    NPCanvas loaded;
    NPStoreStats stats;
    NPStroke first = make_stroke(11, NP_WIDTH_THIN, NP_COLOR_RED, first_points, 3);
    NPStroke second = make_stroke(22, NP_WIDTH_THICK, NP_COLOR_BLUE, second_points, 2);

    unlink(path);
    CHECK(np_store_append_stroke(path, &first) == 0);
    CHECK(np_store_append_stroke(path, &second) == 0);
    np_canvas_init(&canvas);
    CHECK(np_canvas_add_stroke(&canvas, &first) == 0);
    CHECK(np_canvas_add_stroke(&canvas, &second) == 0);
    CHECK(np_store_append_erase(path, first.id) == 0);
    CHECK(np_store_load(path, &loaded, &stats) == 0);
    CHECK(loaded.count == 1);
    CHECK(same_stroke(&loaded.strokes[0], &second));
    CHECK(stats.stroke_records == 2 && stats.erase_records == 1);
    CHECK(loaded.dead_records == 1);
    np_canvas_free(&canvas);
    np_canvas_free(&loaded);
    return 0;
}

static int test_all_colors_and_widths(const char *path)
{
    static const uint8_t colors[] = {
        NP_COLOR_BLACK, NP_COLOR_RED, NP_COLOR_BLUE, NP_COLOR_GREEN
    };
    static const uint8_t widths[] = {NP_WIDTH_THIN, NP_WIDTH_THICK};
    NPCanvas loaded;
    size_t color_index;
    size_t width_index;
    uint32_t id = 100;

    unlink(path);
    for (color_index = 0; color_index < sizeof(colors); color_index++) {
        for (width_index = 0; width_index < sizeof(widths); width_index++) {
            NPStroke stroke = make_stroke(id++, widths[width_index], colors[color_index],
                                          first_points, 3);
            CHECK(np_store_append_stroke(path, &stroke) == 0);
        }
    }
    CHECK(np_store_load(path, &loaded, NULL) == 0);
    CHECK(loaded.count == sizeof(colors) * sizeof(widths));
    for (color_index = 0; color_index < sizeof(colors); color_index++) {
        for (width_index = 0; width_index < sizeof(widths); width_index++) {
            const NPStroke *stroke = &loaded.strokes[color_index * sizeof(widths) + width_index];
            CHECK(stroke->color == colors[color_index]);
            CHECK(stroke->width == widths[width_index]);
        }
    }
    np_canvas_free(&loaded);
    return 0;
}

static int test_v1_upgrade(const char *path)
{
    FILE *file;
    NPStroke loaded_stroke;
    NPCanvas loaded;
    NPPoint points[] = {{1.5f, 2.5f}, {3.5f, 4.5f}};
    NPStoreStats stats;
    uint32_t i;

    unlink(path);
    file = fopen(path, "wb");
    CHECK(file != NULL);
    CHECK(fwrite("KNP1\001\000", 1, 6, file) == 6);
    CHECK(fputc(0x01, file) != EOF);
    CHECK(fputc(NP_TOOL_PENCIL, file) != EOF);
    CHECK(fputc(7, file) != EOF);
    CHECK(fwrite("{", 0, 0, file) == 0);
    for (i = 0; i < 4; i++) CHECK(fputc((int)((42u >> (8u * i)) & 0xffu), file) != EOF);
    for (i = 0; i < 4; i++) CHECK(fputc((int)((2u >> (8u * i)) & 0xffu), file) != EOF);
    for (i = 0; i < 2; i++) {
        uint32_t bits;
        memcpy(&bits, &points[i].x, sizeof(bits));
        CHECK(fputc((int)(bits & 0xffu), file) != EOF);
        CHECK(fputc((int)((bits >> 8) & 0xffu), file) != EOF);
        CHECK(fputc((int)((bits >> 16) & 0xffu), file) != EOF);
        CHECK(fputc((int)(bits >> 24), file) != EOF);
        memcpy(&bits, &points[i].y, sizeof(bits));
        CHECK(fputc((int)(bits & 0xffu), file) != EOF);
        CHECK(fputc((int)((bits >> 8) & 0xffu), file) != EOF);
        CHECK(fputc((int)((bits >> 16) & 0xffu), file) != EOF);
        CHECK(fputc((int)(bits >> 24), file) != EOF);
    }
    CHECK(fclose(file) == 0);
    CHECK(np_store_load(path, &loaded, &stats) == 0);
    CHECK(loaded.count == 1 && loaded.strokes[0].color == NP_COLOR_BLACK);
    loaded_stroke = loaded.strokes[0];
    CHECK(loaded_stroke.width == 7 && loaded_stroke.id == 42 && loaded_stroke.npts == 2);
    file = fopen(path, "rb");
    CHECK(file != NULL);
    CHECK(fgetc(file) == 'K' && fgetc(file) == 'N' && fgetc(file) == 'P' && fgetc(file) == '1');
    CHECK(fgetc(file) == 2 && fgetc(file) == 0);
    CHECK(fclose(file) == 0);
    np_canvas_free(&loaded);
    return 0;
}

static int test_truncated_tail(const char *path, const char *truncated)
{
    NPStroke first = make_stroke(11, 3, NP_COLOR_RED, first_points, 3);
    NPStroke second = make_stroke(22, 5, NP_COLOR_BLUE, second_points, 2);
    long complete_size;
    long full_size;
    long k;

    unlink(path);
    unlink(truncated);
    CHECK(np_store_append_stroke(path, &first) == 0);
    complete_size = file_size(path);
    CHECK(np_store_append_stroke(path, &second) == 0);
    full_size = file_size(path);
    CHECK(full_size > complete_size);
    for (k = 1; k < full_size - complete_size; k++) {
        NPCanvas loaded;
        NPStoreStats stats;
        CHECK(copy_prefix(path, truncated, full_size - k) == 0);
        CHECK(np_store_load(truncated, &loaded, &stats) == 0);
        CHECK(loaded.count == 1);
        CHECK(same_stroke(&loaded.strokes[0], &first));
        CHECK(stats.discarded_tail_records == 1);
        np_canvas_free(&loaded);
    }
    return 0;
}

static int test_short_files(const char *directory)
{
    char empty[256];
    char header[256];
    char bad[256];
    FILE *file;
    NPCanvas canvas;
    NPStoreStats stats;

    (void)snprintf(empty, sizeof(empty), "%s/empty.knp", directory);
    (void)snprintf(header, sizeof(header), "%s/header.knp", directory);
    (void)snprintf(bad, sizeof(bad), "%s/bad.knp", directory);
    file = fopen(empty, "wb");
    CHECK(file != NULL && fclose(file) == 0);
    CHECK(np_store_load(empty, &canvas, &stats) == 0 && canvas.count == 0);
    np_canvas_free(&canvas);
    file = fopen(header, "wb");
    CHECK(file != NULL);
    CHECK(fwrite("KNP1\001\000", 1, 6, file) == 6);
    CHECK(fclose(file) == 0);
    CHECK(np_store_load(header, &canvas, &stats) == 0 && canvas.count == 0);
    np_canvas_free(&canvas);
    file = fopen(bad, "wb");
    CHECK(file != NULL);
    CHECK(fwrite("nope", 1, 4, file) == 4);
    CHECK(fclose(file) == 0);
    CHECK(np_store_load(bad, &canvas, &stats) != 0);
    np_canvas_free(&canvas);
    return 0;
}

static int test_compaction(const char *path)
{
    NPCanvas canvas;
    NPCanvas loaded;
    NPStroke first = make_stroke(11, 3, NP_COLOR_RED, first_points, 3);
    NPStroke second = make_stroke(22, 5, NP_COLOR_BLUE, second_points, 2);

    unlink(path);
    CHECK(np_store_append_stroke(path, &first) == 0);
    CHECK(np_store_append_stroke(path, &second) == 0);
    CHECK(np_store_append_erase(path, first.id) == 0);
    CHECK(np_store_load(path, &canvas, NULL) == 0);
    CHECK(np_store_should_compact(&canvas, 1));
    CHECK(np_store_compact(path, &canvas) == 0);
    CHECK(file_size(path) == 6 + 1 + 1 + 1 + 1 + 4 + 4 + (long)(2 * sizeof(float) * 2));
    CHECK(np_store_load(path, &loaded, NULL) == 0);
    CHECK(loaded.count == 1 && same_stroke(&loaded.strokes[0], &second));
    np_canvas_free(&canvas);
    np_canvas_free(&loaded);
    return 0;
}

/* The two properties the user actually cares about: erasing removes the ink
   from the file rather than only hiding it, and erasing does not shuffle the
   drawing order of what remains. */
static int test_erase_leaves_no_trace_and_keeps_order(const char *path)
{
    NPCanvas canvas;
    NPCanvas loaded;
    NPStoreStats stats;
    NPStroke a = make_stroke(1, 3, NP_COLOR_BLACK, first_points, 3);
    NPStroke b = make_stroke(2, 5, NP_COLOR_RED, second_points, 2);
    NPStroke c = make_stroke(3, 7, NP_COLOR_BLUE, first_points, 3);
    NPStroke d = make_stroke(4, 9, NP_COLOR_GREEN, second_points, 2);
    long before, after;

    unlink(path);
    CHECK(np_store_append_stroke(path, &a) == 0);
    CHECK(np_store_append_stroke(path, &b) == 0);
    CHECK(np_store_append_stroke(path, &c) == 0);
    CHECK(np_store_append_stroke(path, &d) == 0);
    before = file_size(path);
    /* Erase the FIRST of four, deliberately. Removing the middle of three
       cannot tell the two implementations apart -- the last stroke is exactly
       the one a shift would have moved there, so both give the same answer.
       Erasing index 0 of four does: a swap-with-last yields {d, b, c} while a
       shift yields {b, c, d}, and since strokes draw in array order that is
       the difference between d being under b and over it. */
    CHECK(np_store_append_erase(path, a.id) == 0);
    CHECK(np_store_load(path, &canvas, NULL) == 0);

    CHECK(canvas.count == 3);
    CHECK(canvas.strokes[0].id == b.id);
    CHECK(canvas.strokes[1].id == c.id);
    CHECK(canvas.strokes[2].id == d.id);

    /* Before compaction the erased stroke is still in the file. */
    CHECK(file_size(path) > before);

    CHECK(np_store_compact(path, &canvas) == 0);
    after = file_size(path);
    CHECK(after < before);

    /* And afterwards there is no erase record left to replay, and no trace of
       the erased stroke to recover. */
    CHECK(np_store_load(path, &loaded, &stats) == 0);
    CHECK(stats.erase_records == 0);
    CHECK(stats.stroke_records == 3);
    CHECK(loaded.count == 3);
    CHECK(loaded.strokes[0].id == b.id && loaded.strokes[1].id == c.id &&
          loaded.strokes[2].id == d.id);
    CHECK(np_canvas_find_stroke(&loaded, a.id) == NULL);

    /* Compaction must not leave its scratch file behind. access(), not
       file_size(): that helper CHECKs its own stat and so cannot express
       "absent". */
    {
        char temp[1024];
        (void)snprintf(temp, sizeof(temp), "%s.compact", path);
        CHECK(access(temp, F_OK) != 0);
    }

    np_canvas_free(&canvas);
    np_canvas_free(&loaded);
    return 0;
}

static int test_control_geometry(void)
{
    NPControlGeometry color;
    NPControlGeometry width;
    NPControlGeometry undo;
    NPControlGeometry redo;
    NPControlGeometry clear;
    NPControlGeometry wifi;
    NPControlGeometry *controls[] = {&color, &width, &undo, &redo, &clear, &wifi};
    NPControl control;
    int color_x;
    int width_x;
    int y;

    CHECK(np_control_geometry(NP_CONTROL_COLOR, NP_SCREEN_WIDTH, NP_SCREEN_HEIGHT,
                              &color) == 0);
    CHECK(np_control_geometry(NP_CONTROL_WIDTH, NP_SCREEN_WIDTH, NP_SCREEN_HEIGHT,
                              &width) == 0);
    CHECK(np_control_geometry(NP_CONTROL_UNDO, NP_SCREEN_WIDTH, NP_SCREEN_HEIGHT,
                              &undo) == 0);
    CHECK(np_control_geometry(NP_CONTROL_REDO, NP_SCREEN_WIDTH, NP_SCREEN_HEIGHT,
                              &redo) == 0);
    CHECK(np_control_geometry(NP_CONTROL_CLEAR, NP_SCREEN_WIDTH, NP_SCREEN_HEIGHT,
                              &clear) == 0);
    CHECK(np_control_geometry(NP_CONTROL_WIFI, NP_SCREEN_WIDTH, NP_SCREEN_HEIGHT,
                              &wifi) == 0);
    color_x = (color.draw.min_x + color.draw.max_x) / 2;
    width_x = (width.draw.min_x + width.draw.max_x) / 2;
    y = (color.draw.min_y + color.draw.max_y) / 2;
    CHECK(np_control_hit_test(color_x, y, NP_SCREEN_WIDTH, NP_SCREEN_HEIGHT) == NP_CONTROL_COLOR);
    CHECK(np_control_hit_test(width_x, y, NP_SCREEN_WIDTH, NP_SCREEN_HEIGHT) == NP_CONTROL_WIDTH);
    for (control = NP_CONTROL_UNDO; control <= NP_CONTROL_WIFI; control++) {
        int x = (controls[control]->draw.min_x + controls[control]->draw.max_x) / 2;
        CHECK(np_control_hit_test(x, y, NP_SCREEN_WIDTH, NP_SCREEN_HEIGHT) == control);
    }
    CHECK(np_control_hit_test((color.draw.min_x + width.draw.max_x) / 2, y,
                              NP_SCREEN_WIDTH, NP_SCREEN_HEIGHT) == NP_CONTROL_NONE);
    CHECK(np_control_hit_test(color.hit.min_x - 1, color.hit.min_y - 1,
                              NP_SCREEN_WIDTH, NP_SCREEN_HEIGHT) == NP_CONTROL_NONE);

    /* Left-to-right order is pinned so reordering the strip remains a visible
       test failure rather than merely moving each button's hit target. */
    CHECK(wifi.draw.max_x < clear.draw.min_x);
    CHECK(clear.draw.max_x < undo.draw.min_x);
    CHECK(undo.draw.max_x < redo.draw.min_x);
    CHECK(redo.draw.max_x < width.draw.min_x);
    CHECK(width.draw.max_x < color.draw.min_x);
    /* And every hit rect stays clear of its neighbour despite the padding. */
    CHECK(wifi.hit.max_x < clear.hit.min_x);
    CHECK(clear.hit.max_x < undo.hit.min_x);
    CHECK(undo.hit.max_x < redo.hit.min_x);
    CHECK(redo.hit.max_x < width.hit.min_x);
    CHECK(width.hit.max_x < color.hit.min_x);
    /* The whole strip has to fit on the panel. */
    CHECK(wifi.hit.min_x >= 0);
    CHECK(color.hit.max_x < NP_SCREEN_WIDTH);
    CHECK(np_control_hit_test(color.hit.min_x, color.hit.min_y,
                              NP_SCREEN_WIDTH, NP_SCREEN_HEIGHT) == NP_CONTROL_COLOR);
    CHECK(np_control_hit_test(color.hit.max_x, color.hit.max_y,
                              NP_SCREEN_WIDTH, NP_SCREEN_HEIGHT) == NP_CONTROL_COLOR);
    CHECK(np_control_hit_test(width.hit.min_x, width.hit.max_y,
                              NP_SCREEN_WIDTH, NP_SCREEN_HEIGHT) == NP_CONTROL_WIDTH);
    CHECK(wifi.draw.min_x == 758 && wifi.draw.max_x == 829);
    CHECK(wifi.draw.min_y == 14 && wifi.draw.max_y == 85);
    CHECK(wifi.hit.min_x == 754 && wifi.hit.max_x == 833);
    CHECK(np_control_hit_test(wifi.hit.min_x, wifi.hit.max_y,
                              NP_SCREEN_WIDTH, NP_SCREEN_HEIGHT) == NP_CONTROL_WIFI);
    CHECK(np_control_hit_test(width.hit.max_x + 1, width.hit.max_y,
                              NP_SCREEN_WIDTH, NP_SCREEN_HEIGHT) == NP_CONTROL_NONE);
    return 0;
}

static int test_history(void)
{
    NPCanvas canvas;
    NPHistory history;
    NPPoint points[] = {{1.0f, 2.0f}, {3.0f, 4.0f}};
    NPStroke first = make_stroke(1, NP_WIDTH_THIN, NP_COLOR_RED, points, 2);
    NPStroke second = make_stroke(2, NP_WIDTH_THICK, NP_COLOR_BLUE, points, 2);

    np_canvas_init(&canvas);
    np_history_init(&history);
    CHECK(np_canvas_add_stroke(&canvas, &first) == 0);
    CHECK(np_history_record(&history, NP_HISTORY_STROKE, &first) == 0);
    CHECK(np_canvas_add_stroke(&canvas, &second) == 0);
    CHECK(np_history_record(&history, NP_HISTORY_STROKE, &second) == 0);
    CHECK(np_history_undo(&history, &canvas) == 0 && canvas.count == 1);
    CHECK(np_history_redo(&history, &canvas) == 0 && canvas.count == 2);
    CHECK(np_history_undo(&history, &canvas) == 0);
    CHECK(np_history_record(&history, NP_HISTORY_ERASE, &first) == 0);
    CHECK(np_canvas_erase_stroke(&canvas, first.id) == 0);
    CHECK(np_history_undo(&history, &canvas) == 0 && np_canvas_find_stroke(&canvas, first.id) != NULL);
    CHECK(np_history_undo(&history, &canvas) == 0 && np_canvas_find_stroke(&canvas, first.id) == NULL);
    CHECK(np_history_undo(&history, &canvas) == 0);
    CHECK(np_history_redo(&history, &canvas) == 0);
    CHECK(np_history_redo(&history, &canvas) == 0);
    CHECK(np_history_redo(&history, &canvas) == 0);
    np_canvas_free(&canvas);
    np_canvas_init(&canvas);
    CHECK(np_canvas_add_stroke(&canvas, &first) == 0);
    CHECK(np_canvas_add_stroke(&canvas, &first) == 0 && canvas.count == 1);
    np_history_free(&history);
    np_canvas_free(&canvas);
    return 0;
}

static int test_transform_and_hit(void)
{
    NPViewport viewport = {10.0, -4.0, 2.0};
    NPViewport before = viewport;
    double anchor_x = 632.0;
    double anchor_y = 840.0;
    double canvas_x;
    double canvas_y;
    double screen_x;
    double screen_y;
    NPStroke stroke = make_stroke(7, 2, NP_COLOR_GREEN, first_points, 3);
    NPCanvas canvas;

    np_screen_to_canvas(&viewport, anchor_x, anchor_y, &canvas_x, &canvas_y);
    np_viewport_zoom_about(&viewport, anchor_x, anchor_y, 1.5);
    np_canvas_to_screen(&viewport, canvas_x, canvas_y, &screen_x, &screen_y);
    CHECK(fabs(screen_x - anchor_x) < 1e-9 && fabs(screen_y - anchor_y) < 1e-9);
    np_canvas_to_screen(&before, canvas_x, canvas_y, &screen_x, &screen_y);
    np_screen_to_canvas(&before, screen_x, screen_y, &canvas_x, &canvas_y);
    CHECK(fabs(canvas_x - (before.origin_x + screen_x / before.scale)) < 1e-9);
    CHECK(fabs(viewport.scale - 3.0) < 1e-12);

    np_canvas_init(&canvas);
    CHECK(np_canvas_add_stroke(&canvas, &stroke) == 0);
    CHECK(np_canvas_hit_stroke(&canvas, 15.0, 2.5, 1.1) == stroke.id);
    CHECK(np_canvas_hit_stroke(&canvas, 15.0, 4.5, 1.1) == 0);
    CHECK(np_canvas_stroke_visible(&canvas.strokes[0], &before, NP_SCREEN_WIDTH,
                                   NP_SCREEN_HEIGHT));
    np_canvas_free(&canvas);
    return 0;
}

int main(void)
{
    char directory[] = "/tmp/kno-test-XXXXXX";
    char path[256];
    char truncated[256];
    CHECK(mkdtemp(directory) != NULL);
    (void)snprintf(path, sizeof(path), "%s/notebook.knp", directory);
    (void)snprintf(truncated, sizeof(truncated), "%s/truncated.knp", directory);
    CHECK(test_round_trip_and_erase(path) == 0);
    CHECK(test_all_colors_and_widths(path) == 0);
    CHECK(test_truncated_tail(path, truncated) == 0);
    CHECK(test_v1_upgrade(path) == 0);
    CHECK(test_short_files(directory) == 0);
    CHECK(test_compaction(path) == 0);
    CHECK(test_erase_leaves_no_trace_and_keeps_order(path) == 0);
    CHECK(test_history() == 0);
    CHECK(test_control_geometry() == 0);
    CHECK(test_transform_and_hit() == 0);
    printf("all core tests passed\n");
    (void)unlink(path);
    (void)unlink(truncated);
    return 0;
}
