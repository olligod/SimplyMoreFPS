#pragma once
#include <mutex>
#include <thread>

namespace linux_session {

    // Call with the session gate held after setting the process-exit flag; a source
    // callback that already entered the gate may still be creating the worker.
    inline bool join_quit_worker(std::thread& worker, std::unique_lock<std::mutex>& lock) {
        if (!lock.owns_lock()) return false;
        if (!worker.joinable()) return true;
        if (worker.get_id() == std::this_thread::get_id()) return false;

        lock.unlock();
        worker.join();
        lock.lock();
        return true;
    }

}
