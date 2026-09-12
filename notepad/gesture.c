#include "gesture.h"

#include <math.h>

static void update_clear(GestureUpdate *out)
{
    out->kind = GESTURE_NONE;
    out->dx = 0.0;
    out->dy = 0.0;
    out->scale = 1.0;
    out->anchor_x = 0.0;
    out->anchor_y = 0.0;
    out->ended = 0;
}

void gesture_init(GestureState *state)
{
    if (state == 0) {
        return;
    }

    state->previous_count = 0;
    state->one.active = 0;
    state->one.id = 0;
    state->one.x = 0.0;
    state->one.y = 0.0;
    state->pan_origin.active = 0;
    state->pan_origin.id = 0;
    state->pan_origin.x = 0.0;
    state->pan_origin.y = 0.0;
    state->zoom_a.active = 0;
    state->zoom_a.id = 0;
    state->zoom_a.x = 0.0;
    state->zoom_a.y = 0.0;
    state->zoom_b.active = 0;
    state->zoom_b.id = 0;
    state->zoom_b.x = 0.0;
    state->zoom_b.y = 0.0;
    state->pan_started = 0;
}

static int same_zoom_pair(const GestureState *state,
                          const GestureContact *a,
                          const GestureContact *b)
{
    return state->zoom_a.id == a->id && state->zoom_b.id == b->id;
}

static double distance_between(const GestureContact *a,
                               const GestureContact *b)
{
    const double dx = a->x - b->x;
    const double dy = a->y - b->y;

    return sqrt(dx * dx + dy * dy);
}

static void choose_two(const GestureContact *contacts, int count,
                       GestureContact *a, GestureContact *b, int *active_count)
{
    int i;
    int found = 0;
    /* Fully zeroed, not just `.active`: with no active contacts the loop never
       runs and these are still copied to *a and *b. The caller returns before
       reading them today, but copying indeterminate values is undefined
       behaviour and a trap for the next reader of this function. */
    GestureContact first = {0, 0, 0.0, 0.0};
    GestureContact second = {0, 0, 0.0, 0.0};
    for (i = 0; i < count; ++i) {
        if (!contacts[i].active) {
            continue;
        }
        ++(*active_count);

        if (found == 0) {
            first = contacts[i];
            second = contacts[i];
            found = 1;
        } else if (contacts[i].id < first.id) {
            second = first;
            first = contacts[i];
            if (found < 2) {
                ++found;
            }
        } else if (found < 2 || contacts[i].id < second.id) {
            second = contacts[i];
            if (found < 2) {
                ++found;
            }
        }
    }

    *a = first;
    *b = second;
}

void gesture_report(GestureState *state, const GestureContact *contacts,
                    int count, GestureUpdate *out)
{
    GestureContact a;
    GestureContact b;
    int active_count = 0;

    if (state == 0 || out == 0) {
        return;
    }
    update_clear(out);
    if (count < 0) {
        count = 0;
    }
    if (contacts == 0) {
        count = 0;
    }

    choose_two(contacts, count, &a, &b, &active_count);
    if (active_count == 0) {
        if (state->previous_count > 0) {
            out->ended = 1;
        }
        state->previous_count = 0;
        state->pan_started = 0;
        return;
    }

    if (active_count >= 2) {
        /* Lowest ids keep a stable pair when a third contact appears. */
        if (state->previous_count >= 2 && same_zoom_pair(state, &a, &b)) {
            const double previous_distance =
                distance_between(&state->zoom_a, &state->zoom_b);
            const double current_distance = distance_between(&a, &b);

            out->kind = GESTURE_ZOOM;
            if (previous_distance > 0.0) {
                out->scale = current_distance / previous_distance;
            }
            out->anchor_x = (a.x + b.x) / 2.0;
            out->anchor_y = (a.y + b.y) / 2.0;
        }

        state->zoom_a = a;
        state->zoom_b = b;
        state->previous_count = active_count;
        state->pan_started = 0;
        return;
    }

    /* One contact after a pinch is a new baseline, never a pan jump. */
    if (state->previous_count != 1 || state->one.id != a.id) {
        state->one = a;
        state->pan_origin = a;
        state->pan_started = 0;
        state->previous_count = 1;
        return;
    }

    if (!state->pan_started) {
        const double dx_from_start = a.x - state->pan_origin.x;
        const double dy_from_start = a.y - state->pan_origin.y;
        const double distance_from_start =
            sqrt(dx_from_start * dx_from_start + dy_from_start * dy_from_start);

        if (distance_from_start > GESTURE_PAN_THRESHOLD_PX) {
            state->pan_started = 1;
            out->kind = GESTURE_PAN;
            /* Include all motion since the threshold baseline. */
            out->dx = a.x - state->pan_origin.x;
            out->dy = a.y - state->pan_origin.y;
        }
    } else {
        out->kind = GESTURE_PAN;
        out->dx = a.x - state->one.x;
        out->dy = a.y - state->one.y;
    }

    state->one = a;
    state->previous_count = 1;
}
