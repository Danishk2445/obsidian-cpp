#include "core/util/threads.h"

#include <QCoreApplication>
#include <QObject>
#include <QPointer>
#include <QThread>

#include <mutex>
#include <vector>

#if defined(__SANITIZE_THREAD__)
#define OC_TSAN_ENABLED 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define OC_TSAN_ENABLED 1
#endif
#endif
#ifdef OC_TSAN_ENABLED
extern "C" void __tsan_acquire(void* addr);
extern "C" void __tsan_release(void* addr);
#endif

namespace oc {

namespace tsan {
#ifdef OC_TSAN_ENABLED
namespace {
char g_handoff; // one annotation address for every pool hand-off
}
void release() { __tsan_release(&g_handoff); }
void acquire() { __tsan_acquire(&g_handoff); }
#else
void release() {}
void acquire() {}
#endif
} // namespace tsan

namespace {

// Lives in the GUI thread; the queue is handed over under m_mutex and drained by a string-based queued call, which
// Qt builds entirely inside itself (a functor call would be allocated by the posting thread).
class MainThreadMailbox : public QObject {
    Q_OBJECT
public:
    void post(std::function<void()> fn)
    {
        bool schedule = false;
        {
            std::lock_guard lock(m_mutex);
            m_queue.push_back(std::move(fn));
            schedule = !m_scheduled;
            m_scheduled = true;
        }
        if (schedule)
            QMetaObject::invokeMethod(this, "drain", Qt::QueuedConnection);
    }

    Q_INVOKABLE void drain()
    {
        std::vector<std::function<void()>> tasks;
        {
            std::lock_guard lock(m_mutex);
            tasks.swap(m_queue);
            m_scheduled = false;
        }
        for (auto& t : tasks)
            t();
    }

private:
    std::mutex m_mutex;
    std::vector<std::function<void()>> m_queue;
    bool m_scheduled = false;
};

MainThreadMailbox* mailbox()
{
    static MainThreadMailbox* box = [] {
        auto* b = new MainThreadMailbox; // never deleted: posts may come until the process ends
        if (QCoreApplication* app = QCoreApplication::instance(); app && b->thread() != app->thread())
            b->moveToThread(app->thread());
        return b;
    }();
    return box;
}

// Made on the GUI thread when the application starts: workers then only see it through the static's guard.
void createMailbox() { (void)mailbox(); }
Q_COREAPP_STARTUP_FUNCTION(createMailbox)

} // namespace

void postToMainThread(std::function<void()> fn) { mailbox()->post(std::move(fn)); }

} // namespace oc

#include "threads.moc"
