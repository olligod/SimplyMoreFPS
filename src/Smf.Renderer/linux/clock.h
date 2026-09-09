#pragma once
#include <cstdint>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

inline uint32_t native_thread() {
    return uint32_t(syscall(SYS_gettid));
}

inline int64_t native_now() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
