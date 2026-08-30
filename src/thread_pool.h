#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>

namespace custom_port {

class WorkerThreadPool {
public:
    explicit WorkerThreadPool(size_t threads = std::thread::hardware_concurrency()) {
        for (size_t i = 0; i < threads; ++i) {
            m_workers.emplace_back([this](std::stop_token stop_token) {
                WorkerLoop(stop_token);
            });
        }
    }

    ~WorkerThreadPool() {
        m_stop = true;
        m_cv.notify_all();
    }

    template<class F, class... Args>
    auto Enqueue(F&& f, Args&&... args) 
        -> std::future<typename std::invoke_result<F, Args...>::type> {
        using return_type = typename std::invoke_result<F, Args...>::type;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        
        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            if (m_stop) {
                throw std::runtime_error("Enqueue on stopped WorkerThreadPool");
            }
            m_tasks.emplace([task]() { (*task)(); });
        }
        m_cv.notify_one();
        return res;
    }

private:
    void WorkerLoop(std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(m_queue_mutex);
                m_cv.wait(lock, [this, &stop_token] {
                    return m_stop || !m_tasks.empty() || stop_token.stop_requested();
                });
                if ((m_stop || stop_token.stop_requested()) && m_tasks.empty()) {
                    return;
                }
                task = std::move(m_tasks.front());
                m_tasks.pop();
            }
            task();
        }
    }

    std::vector<std::jthread> m_workers;
    std::queue<std::function<void()>> m_tasks;
    std::mutex m_queue_mutex;
    std::condition_variable m_cv;
    bool m_stop{false};
};

} // namespace custom_port
