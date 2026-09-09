// No X connection is opened. These Xlib stubs stand in for the display mutex
// and handler list so the real ledger can be exercised on the CPU.
#include "../x_error_ledger.h"
#include <X11/Xlibint.h>
#undef min
#undef max
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>
#include <type_traits>

static std::recursive_mutex fake_lock;
static thread_local unsigned depth = 0;
static unsigned removed = 0;

extern "C" void XLockDisplay(Display*) {
    fake_lock.lock();
    ++depth;
}

extern "C" void XUnlockDisplay(Display*) {
    assert(depth);
    --depth;
    fake_lock.unlock();
}

extern "C" void _XDeqAsyncHandler(Display* d, _XAsyncHandler* h) {
    assert(depth);

    auto** p = &d->async_handlers;
    while (*p && *p != h) p = &(*p)->next;
    assert(*p == h);
    *p = h->next;
    ++removed;
}

extern "C" Bool _XAsyncErrorHandler(Display*, xReply*, char*, int, XPointer pointer) {
    assert(depth);

    auto& e = *reinterpret_cast<_XAsyncErrorState*>(pointer);
    ++e.error_count;
    e.last_error_received = e.error_count % 251;
    return True;
}

static void marker() {}

int main() {
    Display display{};
    std::remove_reference<decltype(*display.lock_fns)>::type locks{};
    display.lock = reinterpret_cast<decltype(display.lock)>(uintptr_t(1));
    locks.lock_display = reinterpret_cast<decltype(locks.lock_display)>(&marker);
    locks.unlock_display = reinterpret_cast<decltype(locks.unlock_display)>(&marker);
    display.lock_fns = &locks;

    auto source = linux_session::x_error_ledger::create(&display);
    assert(source);
    assert(display.async_handlers);

    auto router = source;
    auto worker = source;
    auto source_copy = source;

    router.reset();
    source.reset();
    assert(!removed);
    assert(display.async_handlers);

    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (unsigned i = 0; i < 20000; i++) {
            XLockDisplay(&display);
            auto* h = display.async_handlers;
            h->handler(&display, nullptr, nullptr, 0, h->data);
            XUnlockDisplay(&display);
        }
        done = true;
    });

    do {
        auto snap = worker->read();
        assert(snap.last == snap.count % 251);
    } while (!done.load());

    writer.join();
    assert(worker->read().count == 20000);

    worker.reset();
    assert(!removed);
    assert(display.async_handlers);
    source_copy.reset();
    assert(removed == 1);
    assert(!display.async_handlers);

    // The same numeric connection reused after full retirement gets a fresh ledger.
    auto next = linux_session::x_error_ledger::create(&display);
    assert(next->read().count == 0);
    next.reset();
    assert(removed == 2);
    assert(!display.async_handlers);
    std::cout << "PASS shared error ledger lifetime, synchronized coherent snapshots, final-owner unlink and reuse\n";
}
