// worker.h -- the engines, answering on a thread of their own.
//
// A render is fast, not instant -- a long song is still a fraction of a
// second, and a fraction of a second of a frozen window is the difference
// between an interface that answers and one that stutters. Requests go on a
// queue, are answered in the order they were asked, and the answer is handed
// back to the window on its own thread.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include <wx/event.h>

namespace svs {

class Worker {
public:
    //: Answers are delivered through `home`, which must outlive the worker
    //: (the main window closes the worker before it goes).
    explicit Worker(wxEvtHandler *home);
    ~Worker();

    //: Run `job` on the worker thread, then `done` with its result on the
    //: window's thread. A job that throws still answers: `done` is given a
    //: default-constructed result and `on_error` hears what went wrong.
    template <class R>
    void ask(std::function<R()> job, std::function<void(R)> done) {
        post([this, job, done]() {
            R result{};
            std::string failure;
            try {
                result = job();
            } catch (const std::exception &e) {
                failure = e.what();
            } catch (...) {
                failure = "unknown failure";
            }
            home_->CallAfter([this, done, result, failure]() {
                if (!failure.empty() && on_error) on_error(failure);
                if (done) done(result);
            });
        });
    }

    void close();
    std::function<void(const std::string &)> on_error;

private:
    void post(std::function<void()> fn);
    void run();

    wxEvtHandler *home_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    bool closing_ = false;
    std::thread thread_;
};

}  // namespace svs
