#pragma once

// Hand-offs between the GUI thread and worker threads that ThreadSanitizer can follow. The system Qt isn't
// instrumented, so the synchronisation inside QThreadPool::start() and queued QMetaObject::invokeMethod() is
// invisible to it and every hand-off through them reads as a data race. startInPool() annotates the hand-off to the
// pool thread; postToMainThread() hands functions to the GUI thread under a std::mutex. Both are plain Qt calls in
// builds without TSan (plus one mutex for postToMainThread()).

#include <QRunnable>
#include <QThreadPool>

#include <functional>
#include <utility>

namespace oc {

namespace tsan {
void release(); // everything this thread did so far happens-before the next acquire()
void acquire();
} // namespace tsan

// QThreadPool::start(fn): all the caller did before (building `fn` included) happens-before `fn` runs.
template <typename F>
void startInPool(QThreadPool* pool, F&& fn)
{
    QRunnable* r = QRunnable::create([fn = std::forward<F>(fn)]() mutable {
        tsan::acquire();
        fn();
    });
    tsan::release();
    pool->start(r);
}

// Runs `fn` on the GUI thread, in post order — like QMetaObject::invokeMethod(qApp, fn, Qt::QueuedConnection).
// Callable from any thread; `fn` must check that the objects it touches still exist (QPointer made on the GUI thread).
void postToMainThread(std::function<void()> fn);

} // namespace oc
