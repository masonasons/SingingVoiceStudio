#include "app/worker.h"

namespace svs {

Worker::Worker(wxEvtHandler *home) : home_(home), thread_([this] { run(); }) {}

Worker::~Worker() { close(); }

void Worker::post(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (closing_) return;
        queue_.push_back(std::move(fn));
    }
    cv_.notify_one();
}

void Worker::run() {
    while (true) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return closing_ || !queue_.empty(); });
            if (closing_) return;
            fn = std::move(queue_.front());
            queue_.pop_front();
        }
        fn();
    }
}

void Worker::close() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (closing_ && !thread_.joinable()) return;
        closing_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

}  // namespace svs
