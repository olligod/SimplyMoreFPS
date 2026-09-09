#include "../geometry_diagnostics.h"
#include "../display_geometry.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <iostream>

int main() {
    using namespace mac;
    auto close = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    image_viewport viewport;

    assert(fit_image(0, 0, 1920, 1080, 1280, 720, viewport));
    assert(close(viewport.x, 0) && close(viewport.y, 0) && close(viewport.width, 1920) && close(viewport.height, 1080));
    assert(close(960 * viewport.scale_x, 640) && close(540 * viewport.scale_y, 360));
    assert(fit_image(0, 0, 1920, 1080, 1024, 768, viewport));
    assert(close(viewport.x, 240) && close(viewport.y, 0) && close(viewport.width, 1440) && close(viewport.height, 1080));
    assert(close((960 - viewport.x) * viewport.scale_x, 512) && close(540 * viewport.scale_y, 384));

    // Retina source pixels and view points are separate units. Both the native
    // resolution and a lower selected resolution must map the image corners.
    for (uint32_t factor : {1u, 2u}) {
        assert(fit_image(0, 0, 1512, 949, 1512 * factor, 949 * factor, viewport));
        assert(close(viewport.scale_x, factor) && close(viewport.scale_y, factor));
        assert(close(viewport.x, 0) && close(viewport.y, 0));
    }

    assert(fit_image(20, 30, 1920, 1080, 1280, 800, viewport));
    assert(close(viewport.x, 116) && close(viewport.y, 30) && close(viewport.width, 1728));
    assert(!fit_image(0, 0, 1920, 1080, 0, 720, viewport));
    assert(!fit_image(0, 0, -1, 1080, 1280, 720, viewport));
    assert(!fit_image(0, 0, INFINITY, 1080, 1280, 720, viewport));

    geometry_diagnostic first;
    geometry_diagnostic healthy;
    assert(!retain_geometry_failure(first, healthy) && first.result == 0);

    geometry_diagnostic original;
    original.result = -11;
    original.reason = 7;
    original.requested_width = 1512;
    original.requested_height = 949;
    original.refreshed_width = 3024;
    original.refreshed_height = 1898;
    original.bounds_width = 1512;
    original.bounds_height = 949;
    original.backing_scale = 2;
    original.drawable_width = 3024;
    original.drawable_height = 1898;
    original.source_device = 0x123456789abcdef0ull;
    original.original_device = 0x123456789abcdef0ull;
    original.flags = 29;

    assert(retain_geometry_failure(first, original));
    auto saved = first;

    // Cleanup with disappeared geometry and later faults must not erase the
    // original requested/display mismatch, full pointer identity, or scale.
    for (int result : {0, 1, -10, -11, -12, -201}) {
        geometry_diagnostic later;
        later.result = result;
        later.reason = 1;
        assert(!retain_geometry_failure(first, later));
        assert(!std::memcmp(&saved, &first, sizeof(first)));
    }

    first = {};
    geometry_diagnostic next;
    next.result = -12;
    next.reason = 4;
    assert(retain_geometry_failure(first, next) && first.reason == 4 && first.result == -12);

    std::cout << "PASS: first geometry failure survives restoration; new session resets receipt\n";
}
