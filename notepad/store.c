#define _POSIX_C_SOURCE 200809L
#include "notepad.h"

#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static int write_u8(FILE *file, uint8_t value)
{
    return fputc(value, file) == EOF ? -1 : 0;
}

static int write_u16(FILE *file, uint16_t value)
{
    return write_u8(file, (uint8_t)(value & 0xffu)) ||
           write_u8(file, (uint8_t)(value >> 8));
}

static int write_u32(FILE *file, uint32_t value)
{
    return write_u8(file, (uint8_t)(value & 0xffu)) ||
           write_u8(file, (uint8_t)((value >> 8) & 0xffu)) ||
           write_u8(file, (uint8_t)((value >> 16) & 0xffu)) ||
           write_u8(file, (uint8_t)(value >> 24));
}

static int read_u8(FILE *file, uint8_t *value)
{
    int c = fgetc(file);
    if (c == EOF) return feof(file) ? 0 : -1;
    *value = (uint8_t)c;
    return 1;
}

static int read_u16(FILE *file, uint16_t *value)
{
    uint8_t a, b;
    int ra = read_u8(file, &a);
    int rb;
    if (ra <= 0) return ra;
    rb = read_u8(file, &b);
    if (rb <= 0) return -1;
    *value = (uint16_t)a | ((uint16_t)b << 8);
    return 1;
}

static int read_u32(FILE *file, uint32_t *value)
{
    uint8_t bytes[4];
    size_t i;
    int result;
    for (i = 0; i < sizeof(bytes); i++) {
        result = read_u8(file, &bytes[i]);
        if (result <= 0) return -1;
    }
    *value = (uint32_t)bytes[0] |
             ((uint32_t)bytes[1] << 8) |
             ((uint32_t)bytes[2] << 16) |
             ((uint32_t)bytes[3] << 24);
    return 1;
}

static int write_f32(FILE *file, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return write_u32(file, bits);
}

static int read_f32(FILE *file, float *value)
{
    uint32_t bits;
    if (read_u32(file, &bits) != 1) return -1;
    memcpy(value, &bits, sizeof(*value));
    return 0;
}

static int write_header(FILE *file)
{
    static const unsigned char magic[] = { 'K', 'N', 'P', '1' };
    if (fwrite(magic, 1, sizeof(magic), file) != sizeof(magic)) return -1;
    return write_u16(file, NP_FORMAT_VERSION);
}

static int check_or_create_header(FILE *file, int allow_empty)
{
    unsigned char magic[4];
    uint16_t version;
    long end;

    if (fseek(file, 0, SEEK_END) != 0) return -1;
    end = ftell(file);
    if (end < 0) return -1;
    if (end == 0 && allow_empty) {
        if (write_header(file) != 0) return -1;
        return fflush(file) == 0 ? 0 : -1;
    }
    if (fseek(file, 0, SEEK_SET) != 0 ||
        fread(magic, 1, sizeof(magic), file) != sizeof(magic) ||
        memcmp(magic, "KNP1", sizeof(magic)) != 0 ||
        read_u16(file, &version) != 1 || version != NP_FORMAT_VERSION) return -1;
    if (fseek(file, 0, SEEK_END) != 0) return -1;
    return 0;
}

static int write_stroke_record(FILE *file, const NPStroke *stroke)
{
    size_t i;
    if (stroke == NULL || stroke->npts > UINT32_MAX ||
        stroke->color > NP_COLOR_GREEN ||
        (stroke->npts != 0 && stroke->points == NULL)) return -1;
    if (write_u8(file, 0x01) != 0 || write_u8(file, stroke->tool) != 0 ||
        write_u8(file, stroke->width) != 0 || write_u8(file, stroke->color) != 0 ||
        write_u32(file, stroke->id) != 0 ||
        write_u32(file, (uint32_t)stroke->npts) != 0) return -1;
    for (i = 0; i < stroke->npts; i++) {
        if (write_f32(file, stroke->points[i].x) != 0 ||
            write_f32(file, stroke->points[i].y) != 0) return -1;
    }
    return 0;
}

/* fflush only hands the bytes to the kernel. The whole reason this format is
   append-only is to survive a battery-out mid-session, and a page cache that
   never reached the card does not survive one -- so every append is fsync'd.
   The cost is one sync per stroke, a few hundred bytes; the dashboard loop next
   door avoids writing 8 MB per refresh for flash-wear reasons, and this is three
   orders of magnitude below that. Losing notes is the worse failure. */
static int flush_durable(FILE *file)
{
    if (fflush(file) != 0) return -1;
    if (fsync(fileno(file)) != 0) return -1;
    return 0;
}

int np_store_append_stroke(const char *path, const NPStroke *stroke)
{
    FILE *file;
    int result;
    if (path == NULL || stroke == NULL) return -1;
    file = fopen(path, "r+b");
    if (file == NULL && errno == ENOENT) file = fopen(path, "w+b");
    if (file == NULL) return -1;
    result = check_or_create_header(file, 1);
    if (result == 0) result = write_stroke_record(file, stroke);
    if (result == 0 && flush_durable(file) != 0) result = -1;
    fclose(file);
    return result;
}

int np_store_append_erase(const char *path, uint32_t id)
{
    FILE *file;
    int result;
    if (path == NULL) return -1;
    file = fopen(path, "r+b");
    if (file == NULL && errno == ENOENT) file = fopen(path, "w+b");
    if (file == NULL) return -1;
    result = check_or_create_header(file, 1);
    if (result == 0 && (write_u8(file, 0x02) != 0 || write_u32(file, id) != 0)) result = -1;
    if (result == 0 && flush_durable(file) != 0) result = -1;
    fclose(file);
    return result;
}

int np_store_replay(FILE *file, NPCanvas *canvas, NPStoreStats *stats)
{
    unsigned char magic[4];
    uint16_t version;
    uint8_t opcode;
    int result;

    if (file == NULL || canvas == NULL) return -1;
    if (stats != NULL) memset(stats, 0, sizeof(*stats));
    np_canvas_init(canvas);
    if (fseek(file, 0, SEEK_SET) != 0) return -1;
    result = (int)fread(magic, 1, sizeof(magic), file);
    if (result == 0 || result < (int)sizeof(magic)) return 0;
    if (memcmp(magic, "KNP1", sizeof(magic)) != 0) return -1;
    result = read_u16(file, &version);
    if (result < 0) return 0;
    if (result != 1 || (version != 1u && version != NP_FORMAT_VERSION)) return -1;

    for (;;) {
        result = read_u8(file, &opcode);
        if (result == 0) break;
        if (result < 0) return -1;
        if (opcode == 0x01) {
            NPStroke stroke;
            uint32_t npts;
            size_t i;
            memset(&stroke, 0, sizeof(stroke));
            if (read_u8(file, &stroke.tool) != 1 || read_u8(file, &stroke.width) != 1 ||
                (version == 2u && read_u8(file, &stroke.color) != 1) ||
                read_u32(file, &stroke.id) != 1 || read_u32(file, &npts) != 1) {
                if (stats != NULL) stats->discarded_tail_records++;
                break;
            }
            if (stroke.color > NP_COLOR_GREEN) return -1;
            if ((uint64_t)npts > (uint64_t)SIZE_MAX / sizeof(*stroke.points)) return -1;
            stroke.npts = npts;
            if (npts != 0) {
                stroke.points = malloc((size_t)npts * sizeof(*stroke.points));
                if (stroke.points == NULL) return -1;
            }
            for (i = 0; i < stroke.npts; i++) {
                if (read_f32(file, &stroke.points[i].x) != 0 ||
                    read_f32(file, &stroke.points[i].y) != 0) {
                    free(stroke.points);
                    if (stats != NULL) stats->discarded_tail_records++;
                    return 0;
                }
            }
            if (np_canvas_add_stroke(canvas, &stroke) != 0) {
                free(stroke.points);
                return -1;
            }
            free(stroke.points);
            if (stats != NULL) stats->stroke_records++;
        } else if (opcode == 0x02) {
            uint32_t id;
            if (read_u32(file, &id) != 1) {
                if (stats != NULL) stats->discarded_tail_records++;
                break;
            }
            (void)np_canvas_erase_stroke(canvas, id);
            if (stats != NULL) stats->erase_records++;
        } else {
            return -1;
        }
    }
    return ferror(file) ? -1 : 0;
}

int np_store_load(const char *path, NPCanvas *canvas, NPStoreStats *stats)
{
    FILE *file;
    int result;
    int legacy = 0;
    if (path == NULL || canvas == NULL) return -1;
    file = fopen(path, "rb");
    if (file == NULL) {
        np_canvas_init(canvas);
        if (stats != NULL) memset(stats, 0, sizeof(*stats));
        return errno == ENOENT ? 0 : -1;
    }
    if (fseek(file, 4, SEEK_SET) == 0) {
        uint16_t version;
        if (read_u16(file, &version) == 1) legacy = version == 1u;
    }
    result = np_store_replay(file, canvas, stats);
    fclose(file);
    if (result == 0 && legacy && np_store_compact(path, canvas) != 0) return -1;
    return result;
}

int np_store_compact(const char *path, const NPCanvas *canvas)
{
    FILE *file;
    size_t i;
    int result = -1;
    char temp[1024];
    if (path == NULL || canvas == NULL) return -1;
    if ((size_t)snprintf(temp, sizeof(temp), "%s.compact", path) >= sizeof(temp)) return -1;

    /* Write a new file and move it into place, rather than truncating the
       notebook and refilling it. Compaction is the one operation here that
       rewrites, and truncating in place puts the entire notebook at risk for
       as long as the rewrite takes -- a power loss halfway through would lose
       everything, which is exactly what the append-only format exists to
       prevent. rename() is not atomic on FAT32, so this narrows the window to
       a directory-entry update rather than closing it, but that is orders of
       magnitude better than a multi-megabyte rewrite. */
    file = fopen(temp, "wb");
    if (file == NULL) return -1;
    if (write_header(file) == 0) {
        result = 0;
        for (i = 0; i < canvas->count; i++) {
            if (write_stroke_record(file, &canvas->strokes[i]) != 0) {
                result = -1;
                break;
            }
        }
        if (result == 0 && flush_durable(file) != 0) result = -1;
    }
    if (fclose(file) != 0) result = -1;
    if (result == 0 && rename(temp, path) != 0) result = -1;
    if (result != 0) remove(temp);
    return result;
}

int np_store_should_compact(const NPCanvas *canvas, size_t threshold)
{
    return canvas != NULL && threshold != 0 && canvas->dead_records >= threshold;
}

static void history_operation_free(NPHistoryOperation *operation)
{
    if (operation == NULL) return;
    free(operation->stroke.points);
    memset(operation, 0, sizeof(*operation));
}

static void history_stack_clear(NPHistoryOperation *operations, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) history_operation_free(&operations[i]);
}

static int history_operation_copy(NPHistoryOperation *destination,
                                  NPHistoryKind kind, const NPStroke *stroke)
{
    memset(destination, 0, sizeof(*destination));
    destination->kind = kind;
    if (stroke == NULL) return -1;
    destination->stroke = *stroke;
    if (stroke->npts != 0) {
        if (stroke->points == NULL || stroke->npts > SIZE_MAX / sizeof(*stroke->points)) return -1;
        destination->stroke.points = malloc(stroke->npts * sizeof(*stroke->points));
        if (destination->stroke.points == NULL) return -1;
        memcpy(destination->stroke.points, stroke->points,
               stroke->npts * sizeof(*stroke->points));
    }
    return 0;
}

static int history_stack_push(NPHistoryOperation **stack, size_t *count,
                               size_t *capacity, NPHistoryKind kind,
                               const NPStroke *stroke)
{
    NPHistoryOperation operation;
    NPHistoryOperation *new_stack;
    size_t new_capacity;
    if (history_operation_copy(&operation, kind, stroke) != 0) return -1;
    if (*count == *capacity) {
        new_capacity = *capacity == 0 ? 16 : *capacity * 2;
        if (new_capacity < *capacity || new_capacity > SIZE_MAX / sizeof(*new_stack)) {
            history_operation_free(&operation);
            return -1;
        }
        new_stack = realloc(*stack, new_capacity * sizeof(*new_stack));
        if (new_stack == NULL) {
            history_operation_free(&operation);
            return -1;
        }
        *stack = new_stack;
        *capacity = new_capacity;
    }
    (*stack)[(*count)++] = operation;
    return 0;
}

static void history_stack_pop(NPHistoryOperation *stack, size_t *count,
                              NPHistoryOperation *operation)
{
    *operation = stack[--(*count)];
    memset(&stack[*count], 0, sizeof(stack[*count]));
}

void np_history_init(NPHistory *history)
{
    if (history != NULL) memset(history, 0, sizeof(*history));
}

void np_history_free(NPHistory *history)
{
    if (history == NULL) return;
    history_stack_clear(history->undo, history->undo_count);
    history_stack_clear(history->redo, history->redo_count);
    free(history->undo);
    free(history->redo);
    memset(history, 0, sizeof(*history));
}

int np_history_record(NPHistory *history, NPHistoryKind kind, const NPStroke *stroke)
{
    if (history == NULL || stroke == NULL) return -1;
    history_stack_clear(history->redo, history->redo_count);
    history->redo_count = 0;
    return history_stack_push(&history->undo, &history->undo_count,
                              &history->undo_capacity, kind, stroke);
}

const NPHistoryOperation *np_history_peek_undo(const NPHistory *history)
{
    if (history == NULL || history->undo_count == 0) return NULL;
    return &history->undo[history->undo_count - 1];
}

const NPHistoryOperation *np_history_peek_redo(const NPHistory *history)
{
    if (history == NULL || history->redo_count == 0) return NULL;
    return &history->redo[history->redo_count - 1];
}

int np_history_undo(NPHistory *history, NPCanvas *canvas)
{
    NPHistoryOperation operation;
    if (history == NULL || canvas == NULL) return -1;
    if (history->undo_count == 0) return 0;
    history_stack_pop(history->undo, &history->undo_count, &operation);
    if (operation.kind == NP_HISTORY_STROKE) {
        if (np_canvas_erase_stroke(canvas, operation.stroke.id) != 0) {
            history_stack_push(&history->undo, &history->undo_count,
                               &history->undo_capacity, operation.kind,
                               &operation.stroke);
            history_operation_free(&operation);
            return -1;
        }
    } else if (np_canvas_add_stroke(canvas, &operation.stroke) != 0) {
        history_stack_push(&history->undo, &history->undo_count,
                           &history->undo_capacity, operation.kind,
                           &operation.stroke);
        history_operation_free(&operation);
        return -1;
    }
    if (history_stack_push(&history->redo, &history->redo_count,
                           &history->redo_capacity, operation.kind,
                           &operation.stroke) != 0) {
        history_operation_free(&operation);
        return -1;
    }
    history_operation_free(&operation);
    return 0;
}

int np_history_redo(NPHistory *history, NPCanvas *canvas)
{
    NPHistoryOperation operation;
    if (history == NULL || canvas == NULL) return -1;
    if (history->redo_count == 0) return 0;
    history_stack_pop(history->redo, &history->redo_count, &operation);
    if (operation.kind == NP_HISTORY_STROKE) {
        if (np_canvas_add_stroke(canvas, &operation.stroke) != 0) {
            history_stack_push(&history->redo, &history->redo_count,
                               &history->redo_capacity, operation.kind,
                               &operation.stroke);
            history_operation_free(&operation);
            return -1;
        }
    } else if (np_canvas_erase_stroke(canvas, operation.stroke.id) != 0) {
        history_stack_push(&history->redo, &history->redo_count,
                           &history->redo_capacity, operation.kind,
                           &operation.stroke);
        history_operation_free(&operation);
        return -1;
    }
    if (history_stack_push(&history->undo, &history->undo_count,
                           &history->undo_capacity, operation.kind,
                           &operation.stroke) != 0) {
        history_operation_free(&operation);
        return -1;
    }
    history_operation_free(&operation);
    return 0;
}
