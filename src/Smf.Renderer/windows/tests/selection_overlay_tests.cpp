// Selection outline geometry from the shared overlay header. No window or GPU.
#include "../../common/selection_overlay.h"
#include <cstdio>
#include <cstdlib>

struct affine {
    double a;
    double b;
    double c;
    double d;
    double e;
    double f;
};

static void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
}

int main() {
    smf_selection_state published{88, 1, 1, 1, 1, 1, 7, 1, 10, 20, 1, {1, 1, 1, 1}, 0};
    auto& mailbox = selection::state();
    selection::worker worker;
    check(mailbox.publish(&published, 88) == 0, "first publication stored");

    const affine camera{10, 0, 0, 0, -10, 500};
    auto rect = worker.read(1, 1, 7, camera, true, true, true, 250, 450);
    check(rect.visible && rect.edges[0].left == 100 && rect.edges[0].top == 300 && rect.edges[0].right == 250, "anchor at the drag start");

    // The native pointer moves while no new game state is published; the anchor stays put.
    rect = worker.read(1, 1, 7, camera, true, true, true, 320, 470);
    check(rect.visible && rect.edges[0].left == 100 && rect.edges[0].top == 300 && rect.edges[0].right == 320, "pointer-only movement");
    check(!worker.read(1, 1, 7, camera, true, true, true, 103, 304).visible, "exactly half a cell is not a drag");

    // Camera movement transforms the world anchor, not the current pointer.
    rect = worker.read(1, 1, 7, affine{20, 0, -50, 0, -20, 700}, true, true, true, 320, 470);
    check(rect.visible && rect.edges[0].left == 150 && rect.edges[0].top == 300 && rect.edges[0].right == 320, "camera mapping");
    check(!worker.read(1, 2, 7, camera, true, true, true, 320, 470).visible, "content fence");
    check(!worker.read(1, 1, 8, camera, true, true, true, 320, 470).visible, "map fence");

    // Release hides immediately; a stale active publication cannot bring it back.
    check(!worker.read(1, 1, 7, camera, true, true, false, 320, 470).visible, "release hides");
    published.publication++;
    check(mailbox.publish(&published, 88) == 0, "republish after release");
    check(!worker.read(1, 1, 7, camera, true, true, true, 320, 470).visible, "stale drag stays dismissed");

    published.publication++;
    published.drag++;
    published.ui_scale = 1.5f;
    check(mailbox.publish(&published, 88) == 0, "new drag published");
    rect = worker.read(1, 1, 7, camera, true, true, true, 320, 470);
    check(rect.visible && rect.edges[0].bottom - rect.edges[0].top == 3, "edge thickness follows the UI scale");

    check(!worker.read(1, 1, 7, camera, false, true, true, 320, 470).visible, "focus loss hides");
    check(!worker.read(1, 1, 7, camera, true, true, true, 320, 470).visible, "focus loss dismisses the drag");
    published.publication++;
    published.active = 0;
    check(mailbox.publish(&published, 88) == 0, "inactive state published");
    check(!worker.read(1, 1, 7, camera, true, true, true, 320, 470).visible, "inactive state hides");

    check(mailbox.publish(&published, 88) < 0, "same publication rejected");
    check(mailbox.publish(&published, 87) < 0, "wrong size rejected");

    // Interleaving: sample released input, then main publishes a new press before the
    // read. Only the snapshot taken before the input sample may be used.
    const auto before_press = worker.latch();
    published.publication++;
    published.drag++;
    published.active = 1;
    check(mailbox.publish(&published, 88) == 0, "press published after the sample");
    check(!worker.read(before_press, 1, 1, 7, camera, true, true, false, 320, 470).visible, "old snapshot cannot dismiss the new drag");
    const auto held = worker.latch();
    check(worker.read(held, 1, 1, 7, camera, true, true, true, 320, 470).visible, "current snapshot draws");

    // A real mouse-up still dismisses immediately, including a stale active state.
    check(!worker.read(held, 1, 1, 7, camera, true, true, false, 320, 470).visible, "mouse-up dismisses");
    check(!worker.read(held, 1, 1, 7, camera, true, true, true, 320, 470).visible, "dismissed drag stays hidden");

    // A replaced snapshot can neither draw nor dismiss a later drag.
    published.publication++;
    published.drag++;
    check(mailbox.publish(&published, 88) == 0, "later drag published");
    const auto later = worker.latch();
    check(!worker.read(held, 1, 1, 7, camera, true, true, false, 320, 470).visible, "replaced snapshot cannot dismiss");
    check(worker.read(later, 1, 1, 7, camera, true, true, true, 320, 470).visible, "later snapshot draws");
    check(!worker.read(later, 1, 2, 7, camera, true, true, true, 320, 470).visible, "content fence on the snapshot read");

    published.publication++;
    published.ui_scale = 0;
    check(mailbox.publish(&published, 88) < 0, "zero UI scale rejected");

    std::puts("PASS selection anchor, pointer-only movement, camera mapping, threshold, scale, fences, snapshot interleaving and stale-release rejection");
    return 0;
}
