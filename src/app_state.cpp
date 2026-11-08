#include "app_state.h"

#include <deque>
#include <mutex>

namespace {
std::mutex g_queue_mutex;
std::deque<std::function<void()>> g_queue;

std::mutex g_status_mutex;
app_state::SendspinStatus g_status;
}  // namespace

namespace app_state {

void post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_queue.push_back(std::move(fn));
}

void run_pending() {
    std::deque<std::function<void()>> work;
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        work.swap(g_queue);
    }
    for (auto& fn : work) {
        fn();
    }
}

void set_sendspin(const SendspinStatus& status) {
    std::lock_guard<std::mutex> lock(g_status_mutex);
    g_status = status;
}

SendspinStatus get_sendspin() {
    std::lock_guard<std::mutex> lock(g_status_mutex);
    return g_status;
}

}  // namespace app_state
