#include "graph/parallel.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace oc::graph {

namespace {

thread_local bool t_inJob = false;

class Pool {
public:
    static Pool& instance()
    {
        static Pool pool;
        return pool;
    }

    int workers() const { return int(m_threads.size()) + 1; }

    void run(int count, const std::function<void(int)>& fn)
    {
        if (count <= 0)
            return;
        if (count == 1 || m_threads.empty() || t_inJob) {
            for (int i = 0; i < count; ++i)
                fn(i);
            return;
        }
        std::lock_guard job(m_jobMutex); // one job at a time
        {
            std::lock_guard lk(m_mutex);
            m_fn = &fn;
            m_count = count;
            m_next.store(0, std::memory_order_relaxed);
            m_active = int(m_threads.size());
            ++m_generation;
        }
        m_wake.notify_all();
        work(fn, count);
        std::unique_lock lk(m_mutex);
        m_done.wait(lk, [this] { return m_active == 0; });
        m_fn = nullptr;
    }

private:
    Pool()
    {
        const int hw = int(std::thread::hardware_concurrency());
        const int threads = std::clamp(hw - 1, 0, 31);
        m_threads.reserve(size_t(threads));
        for (int i = 0; i < threads; ++i)
            m_threads.emplace_back([this] { loop(); });
    }

    ~Pool()
    {
        {
            std::lock_guard lk(m_mutex);
            m_stop = true;
        }
        m_wake.notify_all();
        for (std::thread& t : m_threads)
            t.join();
    }

    void work(const std::function<void(int)>& fn, int count)
    {
        t_inJob = true;
        for (;;) {
            const int i = m_next.fetch_add(1, std::memory_order_relaxed);
            if (i >= count)
                break;
            fn(i);
        }
        t_inJob = false;
    }

    void loop()
    {
        unsigned long long seen = 0;
        for (;;) {
            const std::function<void(int)>* fn;
            int count;
            {
                std::unique_lock lk(m_mutex);
                m_wake.wait(lk, [&] { return m_stop || m_generation != seen; });
                if (m_stop)
                    return;
                seen = m_generation;
                fn = m_fn;
                count = m_count;
            }
            work(*fn, count);
            std::lock_guard lk(m_mutex);
            if (--m_active == 0)
                m_done.notify_all();
        }
    }

    std::vector<std::thread> m_threads;
    std::mutex m_jobMutex;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_done;
    const std::function<void(int)>* m_fn = nullptr;
    int m_count = 0;
    std::atomic<int> m_next{0};
    int m_active = 0;
    unsigned long long m_generation = 0;
    bool m_stop = false;
};

} // namespace

void parallelFor(int count, const std::function<void(int)>& fn) { Pool::instance().run(count, fn); }

int parallelWorkers() { return Pool::instance().workers(); }

} // namespace oc::graph
