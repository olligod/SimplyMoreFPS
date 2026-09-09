#include "x_error_ledger.h"
#include <X11/Xlibint.h>

namespace linux_session {
    namespace {

        struct error_scope {
            _XAsyncHandler handler{};
            _XAsyncErrorState state{};
        };

    }

    x_error_ledger::x_error_ledger(Display* d) : display(d) {
        auto* e = new error_scope;
        scope = e;

        XLockDisplay(d);
        e->state.min_sequence_number = NextRequest(d);
        e->handler.handler = _XAsyncErrorHandler;
        e->handler.data = reinterpret_cast<XPointer>(&e->state);
        e->handler.next = d->async_handlers;
        d->async_handlers = &e->handler;
        XUnlockDisplay(d);
    }

    std::shared_ptr<x_error_ledger> x_error_ledger::create(Display* d) {
        if (!d || !d->lock || !d->lock_fns || !d->lock_fns->lock_display || !d->lock_fns->unlock_display) return {};
        return std::shared_ptr<x_error_ledger>(new x_error_ledger(d));
    }

    x_error_snapshot x_error_ledger::read() const {
        XLockDisplay(display);
        const auto& e = static_cast<error_scope*>(scope)->state;
        x_error_snapshot out{uint64_t(e.error_count), uint32_t(e.last_error_received)};
        XUnlockDisplay(display);
        return out;
    }

    x_error_ledger::~x_error_ledger() {
        // The last owner unlinks while the borrowed connection is still alive; owners
        // that would outlive it are kept for the life of the process instead.
        auto* e = static_cast<error_scope*>(scope);

        XLockDisplay(display);
        _XDeqAsyncHandler(display, &e->handler);
        XUnlockDisplay(display);
        delete e;
    }

}
