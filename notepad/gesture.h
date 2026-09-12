#ifndef KNO_GESTURE_H
#define KNO_GESTURE_H

/* Screen coordinates are pixels after the caller's touch-axis conversion. */
typedef struct {
    int active;
    int id;
    double x;
    double y;
} GestureContact;

typedef enum {
    GESTURE_NONE = 0,
    GESTURE_PAN,
    GESTURE_ZOOM
} GestureKind;

typedef struct {
    GestureKind kind;
    double dx;
    double dy;
    double scale;
    double anchor_x;
    double anchor_y;
    int ended;
} GestureUpdate;

/* A finger must move farther than this before a pan starts. */
#define GESTURE_PAN_THRESHOLD_PX 2.0

typedef struct {
    /* These fields are implementation state; callers should use gesture_init. */
    int previous_count;
    GestureContact one;
    GestureContact pan_origin;
    GestureContact zoom_a;
    GestureContact zoom_b;
    int pan_started;
} GestureState;

/*
 * Contacts must already be filtered by the caller: inactive entries are
 * ignored, and the caller is responsible for excluding pen and palm contacts.
 * Active contact ids are expected to be unique tracking ids.
 *
 * Each call compares against the previous call, not against a hidden clock.
 * Therefore a caller may call this for every decoded report and accumulate
 * updates while repainting less often, or call it only on sampled reports;
 * the pan deltas telescope exactly in either case.
 */
void gesture_init(GestureState *state);
void gesture_report(GestureState *state, const GestureContact *contacts,
                    int count, GestureUpdate *out);

#endif
