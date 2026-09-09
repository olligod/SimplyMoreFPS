#include "../display_ownership.h"
#include "../quit_contract.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <mutex>
#include <thread>

using namespace linux_session;

int main() {
    assert(separate_input_connection(100, 200));
    assert(!separate_input_connection(100, 100));
    assert(!separate_input_connection(0, 200));
    assert(may_close_input_connection(100, 200));
    assert(!may_close_input_connection(100, 100));
    assert(!may_close_input_connection(100, 0));

    // A source callback that entered before the quit flag can still create a
    // worker while main waits for the gate; the join must cover that handle.
    std::mutex gate;
    std::thread worker;
    std::atomic<bool> entered{false};
    std::atomic<bool> quit{false};
    std::atomic<bool> ran{false};

    std::thread source([&] {
        std::unique_lock<std::mutex> lock(gate);
        entered = true;
        while (!quit.load()) std::this_thread::yield();
        worker = std::thread([&] {
            std::lock_guard<std::mutex> own(gate);
            ran = true;
        });
    });

    while (!entered.load()) std::this_thread::yield();
    quit = true;

    {
        std::unique_lock<std::mutex> lock(gate);
        assert(join_quit_worker(worker, lock));
        assert(lock.owns_lock());
        assert(ran.load());
        assert(!worker.joinable());
        assert(join_quit_worker(worker, lock));
    }
    source.join();

    // A join asked for from the worker itself is rejected and the gate stays held.
    std::atomic<bool> published{false};
    std::atomic<bool> rejected{false};

    worker = std::thread([&] {
        while (!published.load()) std::this_thread::yield();
        std::unique_lock<std::mutex> lock(gate);
        rejected = !join_quit_worker(worker, lock) && lock.owns_lock();
    });

    published = true;
    worker.join();
    assert(rejected);

    std::unique_lock<std::mutex> unlocked(gate, std::defer_lock);
    assert(!join_quit_worker(worker, unlocked));
    std::cout << "PASS borrowed/input ownership, in-flight creation, real outside-gate join, duplicate quit, self-join rejection\n";
}
