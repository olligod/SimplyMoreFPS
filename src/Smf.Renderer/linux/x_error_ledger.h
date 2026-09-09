#pragma once
#include <X11/Xlib.h>
#include <cstdint>
#include <memory>

namespace linux_session {

    struct x_error_snapshot {
        uint64_t count = 0;
        uint32_t last = 0;
    };

    // One error counter shared by source, router and worker on Unity's connection.
    // Every read and handler-list change holds only the Xlib display lock, and no
    // caller keeps that lock across GL drawing, swapping or GPU waits.
    class x_error_ledger {
        Display* display = nullptr;
        void* scope = nullptr;

        explicit x_error_ledger(Display*);

    public:
        static std::shared_ptr<x_error_ledger> create(Display*);
        ~x_error_ledger();
        x_error_ledger(const x_error_ledger&) = delete;
        x_error_ledger& operator=(const x_error_ledger&) = delete;

        x_error_snapshot read() const;

        Display* connection() const {
            return display;
        }
    };

}
