// Forced include (the Makefile's -include): what picolibc's single-threaded
// libstdc++ lacks. One thread, so the locks do nothing.
#pragma once
#include <sys/types.h>
#ifdef __cplusplus
#include <chrono>
#include <mutex>
#ifndef _GLIBCXX_HAS_GTHREADS
namespace std {
struct recursive_mutex {
    void lock() {}
    void unlock() {}
    bool try_lock() { return true; }
};
struct recursive_timed_mutex : recursive_mutex {
    template <class R, class P> bool try_lock_for(const chrono::duration<R, P> &) { return true; }
    template <class C, class D> bool try_lock_until(const chrono::time_point<C, D> &) { return true; }
};
struct condition_variable_any {
    void notify_one() {}
    void notify_all() {}
    template <class L> void wait(L &) {}
    template <class L, class R, class P> bool wait_for(L &, const chrono::duration<R, P> &) { return true; }
};
}
#endif
#endif
