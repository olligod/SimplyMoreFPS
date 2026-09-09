#pragma once
#include <cstdint>

namespace linux_session {

    inline bool separate_input_connection(uintptr_t borrowed, uintptr_t owned) {
        return borrowed && owned && borrowed != owned;
    }

    inline bool may_close_input_connection(uintptr_t borrowed, uintptr_t owned) {
        return separate_input_connection(borrowed, owned);
    }

}
