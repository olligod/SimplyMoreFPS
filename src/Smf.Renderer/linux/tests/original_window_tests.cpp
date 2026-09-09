#include "../original_window.h"
#include <cassert>
#include <iostream>

using namespace linux_session;

int main() {
    window_candidate valid{42, 1, 2, 123, 1280, 720, 24, true, true, false, false};
    assert(eligible_window(valid, 123));

    window_selection s;
    assert(s.result(true, true) == 1);
    s.observe(valid, 123);
    s.observe(valid, 123);
    assert(s.count == 1);
    assert(s.result(true, true) == 0);

    auto other = valid;
    other.window = 43;
    s.observe(other, 123);
    assert(s.result(true, true) == 2);
    assert(s.result(false, true) == -5);
    assert(s.result(true, false) == -4);

    for (unsigned test = 0; test < 7; test++) {
        auto bad = valid;
        switch (test) {
        case 0:
            bad.pid = 999;
            break;
        case 1:
            bad.input_output = false;
            break;
        case 2:
            bad.viewable = false;
            break;
        case 3:
            bad.owned_ancestor = true;
            break;
        case 4:
            bad.width = 0;
            break;
        case 5:
            bad.height = 20000;
            break;
        default:
            bad.window = 0;
            break;
        }

        assert(!eligible_window(bad, 123));
    }

    // EWMH and WM_CLASS are recorded but never required for eligibility.
    valid.ewmh_listed = true;
    assert(eligible_window(valid, 123));

    original_window packet;
    assert(packet.size == 256);
    assert(packet.version == 1);
    assert(packet.window == 0);
    std::cout << "PASS unique own visible top-level selection, ambiguity, bounds, foreign/child/unmapped rejection, ABI\n";
}
