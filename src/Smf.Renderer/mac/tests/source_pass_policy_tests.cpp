#include "../source_pass_policy.h"
#include <cassert>
#include <cstring>
#include <iostream>

using namespace mac;

int main() {
    const source_pass_facts absent{};
    const source_pass_facts matching{true, 50, 0, 0, 0};

    // Unity may have no encoder before and/or after resolving its explicit
    // renderbuffer. A present descriptor must always name that exact texture.
    for (bool before_present : {false, true}) {
        for (bool after_present : {false, true}) {
            auto before = before_present ? matching : absent;
            auto after = after_present ? matching : absent;
            assert(!reject_source_pass(before, 50) && !reject_source_pass(after, 50));
        }
    }

    for (int field = 0; field < 5; ++field) {
        auto bad = matching;
        if (field == 0) bad.texture = 0;
        if (field == 1) bad.texture = 51;
        if (field == 2) bad.level = 1;
        if (field == 3) bad.slice = 1;
        if (field == 4) bad.depth = 1;

        assert(reject_source_pass(bad, 50));
        // An absent descriptor on the other side cannot excuse this mismatch.
        assert(!reject_source_pass(absent, 50));
        assert(reject_source_pass(bad, 50));
    }

    assert(!std::strcmp(reject_source_pass({true, 0, 0, 0, 0}, 50), "no-attachment"));
    assert(!std::strcmp(reject_source_pass(matching, 0), "attachment-mismatch"));

    std::cout << "Source pass policy: nil encoder combinations accepted; actual attachment/subresource mismatches rejected\n";
}
