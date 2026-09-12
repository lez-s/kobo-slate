#include "gesture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int tests_run;

static void check(int condition, const char *message)
{
    ++tests_run;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static int close_enough(double actual, double expected)
{
    return fabs(actual - expected) < 1e-9;
}

static GestureContact contact(int id, double x, double y)
{
    GestureContact result;

    result.active = 1;
    result.id = id;
    result.x = x;
    result.y = y;
    return result;
}

static GestureContact inactive(void)
{
    GestureContact result;

    result.active = 0;
    result.id = -1;
    result.x = 0.0;
    result.y = 0.0;
    return result;
}

static void test_pan_and_end(void)
{
    GestureState state;
    GestureUpdate update;
    GestureContact finger = contact(7, 100.0, 100.0);
    GestureContact none = inactive();

    gesture_init(&state);
    gesture_report(&state, &finger, 1, &update);
    check(update.kind == GESTURE_NONE, "finger down is only a baseline");

    finger.x = 101.0;
    gesture_report(&state, &finger, 1, &update);
    check(update.kind == GESTURE_NONE, "sub-threshold motion does not pan");

    finger.x = 103.0;
    gesture_report(&state, &finger, 1, &update);
    check(update.kind == GESTURE_PAN && close_enough(update.dx, 3.0),
          "pan starts at the threshold with accumulated delta");

    finger.x = 105.0;
    finger.y = 102.0;
    gesture_report(&state, &finger, 1, &update);
    check(update.kind == GESTURE_PAN && close_enough(update.dx, 2.0) &&
              close_enough(update.dy, 2.0),
          "pan reports the delta from the previous report");

    gesture_report(&state, &none, 1, &update);
    check(update.ended == 1 && update.kind == GESTURE_NONE,
          "last lift ends the gesture once");
    gesture_report(&state, &none, 1, &update);
    check(update.ended == 0, "empty reports after a lift do not re-end");
}

static void test_zoom_and_lowest_ids(void)
{
    GestureState state;
    GestureUpdate update;
    GestureContact pair[3];

    gesture_init(&state);
    pair[0] = contact(1, 0.0, 0.0);
    pair[1] = contact(2, 10.0, 0.0);
    gesture_report(&state, pair, 2, &update);
    check(update.kind == GESTURE_NONE, "second finger establishes zoom baseline");

    pair[0].x = 0.0;
    pair[1].x = 20.0;
    gesture_report(&state, pair, 2, &update);
    check(update.kind == GESTURE_ZOOM && close_enough(update.scale, 2.0) &&
              close_enough(update.anchor_x, 10.0) &&
              close_enough(update.anchor_y, 0.0) && close_enough(update.dx, 0.0),
          "two fingers zoom around their midpoint without panning");

    pair[2] = contact(20, 20.0, 0.0);
    pair[0].id = 30;
    pair[1].id = 10;
    pair[0].x = 30.0;
    pair[1].x = 10.0;
    /* Pair changed, so this report must re-baseline rather than jump. */
    gesture_report(&state, pair, 3, &update);
    check(update.kind == GESTURE_NONE, "pair changes re-baseline zoom");

    pair[1].x = 40.0;
    gesture_report(&state, pair, 3, &update);
    check(update.kind == GESTURE_ZOOM && close_enough(update.scale, 2.0) &&
              close_enough(update.anchor_x, 30.0),
          "three contacts zoom using the two lowest ids");
}

static void test_transitions_rebaseline(void)
{
    GestureState state;
    GestureUpdate update;
    GestureContact fingers[2];

    gesture_init(&state);
    fingers[0] = contact(1, 0.0, 0.0);
    gesture_report(&state, fingers, 1, &update);
    fingers[0].x = 4.0;
    gesture_report(&state, fingers, 1, &update);
    check(update.kind == GESTURE_PAN && close_enough(update.dx, 4.0),
          "one-finger pan starts normally");

    fingers[1] = contact(2, 104.0, 0.0);
    gesture_report(&state, fingers, 2, &update);
    check(update.kind == GESTURE_NONE, "adding a finger does not make a pan jump");
    fingers[0].x = 5.0;
    fingers[1].x = 105.0;
    gesture_report(&state, fingers, 2, &update);
    check(update.kind == GESTURE_ZOOM && close_enough(update.scale, 1.0),
          "pinch starts from the transition baseline");

    fingers[1].active = 0;
    fingers[0].x = 50.0;
    gesture_report(&state, fingers, 2, &update);
    check(update.kind == GESTURE_NONE, "lifting from pinch establishes a pan baseline");
    fingers[0].x = 53.0;
    gesture_report(&state, fingers, 2, &update);
    check(update.kind == GESTURE_PAN && close_enough(update.dx, 3.0),
          "pan after pinch has no stale pinch jump");
}

static void test_inactive_and_sampled_deltas(void)
{
    GestureState every_state;
    GestureState sampled_state;
    GestureUpdate every_update;
    GestureUpdate sampled_update;
    GestureContact reports[11];
    GestureContact entries[2];
    double every_total = 0.0;
    double sampled_total = 0.0;
    int i;

    gesture_init(&every_state);
    gesture_init(&sampled_state);
    entries[0] = inactive();
    entries[1] = contact(9, 0.0, 0.0);
    gesture_report(&every_state, entries, 2, &every_update);
    check(every_update.kind == GESTURE_NONE,
          "inactive contacts are ignored entirely");

    for (i = 0; i < 11; ++i) {
        reports[i] = contact(9, (double)(i * 3), 0.0);
        gesture_report(&every_state, &reports[i], 1, &every_update);
        if (every_update.kind == GESTURE_PAN) {
            every_total += every_update.dx;
        }
    }

    gesture_report(&sampled_state, &reports[0], 1, &sampled_update);
    for (i = 5; i < 11; i += 5) {
        gesture_report(&sampled_state, &reports[i], 1, &sampled_update);
        if (sampled_update.kind == GESTURE_PAN) {
            sampled_total += sampled_update.dx;
        }
    }
    gesture_report(&sampled_state, &reports[10], 1, &sampled_update);
    if (sampled_update.kind == GESTURE_PAN) {
        sampled_total += sampled_update.dx;
    }
    check(close_enough(every_total, 30.0) &&
              close_enough(sampled_total, every_total),
          "pan deltas telescope when intermediate reports are skipped");
}

int main(void)
{
    test_pan_and_end();
    test_zoom_and_lowest_ids();
    test_transitions_rebaseline();
    test_inactive_and_sampled_deltas();
    printf("gesture tests: %d passed\n", tests_run);
    return 0;
}
