#pragma once

#include <pulse/outcome.hpp>
#include <pulse/storage.hpp>

#include <boost/asio.hpp>

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace pulse::storage {

using pulse::Outcome;
using pulse::Unit;

// Owns a Database on its own thread and runs jobs one at a time, in order.
//
//   worker.submit([](Database &db) { return db.find_user("alice"); },
//                 [](Outcome<std::optional<User>> result) { ... });
//
// The completion handler always runs on the `results` executor, never on the
// worker thread. The executor must outlive the worker, so destroy the worker
// before the io_context. While a job is pending, the io_context keeps running.
//
// Work and handler objects must be copy-constructible (std::function rules).
class DatabaseWorker {
  public:
    // Opens (and migrates) the database on the worker thread and waits for that,
    // so a bad path or schema fails right here.
    DatabaseWorker(boost::asio::any_io_executor results, const std::filesystem::path &file,
                   std::size_t max_queue = 256);
    ~DatabaseWorker();
    DatabaseWorker(const DatabaseWorker &) = delete;
    DatabaseWorker &operator=(const DatabaseWorker &) = delete;

    // If the queue is full, or after stop(), `done` still fires (on the results
    // executor) with Overloaded or Shutdown.
    template <class Work, class Done> void submit(Work work, Done done) {
        using Raw = std::invoke_result_t<Work &, Database &>;
        using Value = std::conditional_t<std::is_void_v<Raw>, Unit, Raw>;
        using Guard = boost::asio::executor_work_guard<boost::asio::any_io_executor>;

        auto keep_alive = std::make_shared<Guard>(boost::asio::make_work_guard(results_));
        auto handler = std::make_shared<Done>(std::move(done));
        enqueue([executor = results_, keep_alive, handler,
                 work = std::move(work)](Database *database, std::exception_ptr rejected) mutable {
            Outcome<Value> outcome(rejected);
            if (database != nullptr) {
                try {
                    if constexpr (std::is_void_v<Raw>) {
                        work(*database);
                        outcome = Outcome<Value>(Unit{});
                    } else {
                        outcome = Outcome<Value>(work(*database));
                    }
                } catch (...) {
                    outcome = Outcome<Value>(std::current_exception());
                }
            }
            boost::asio::post(executor, [outcome = std::move(outcome), handler,
                                         keep_alive]() mutable { (*handler)(std::move(outcome)); });
        });
    }

    // Lets the running job finish, fails everything still queued with Shutdown,
    // then joins the thread and closes the database. Safe to call twice.
    void stop();

  private:
    using Job = std::function<void(Database *, std::exception_ptr)>;

    void enqueue(Job job);
    void run(const std::filesystem::path &file, std::function<void(std::exception_ptr)> ready);

    boost::asio::any_io_executor results_;
    std::size_t max_queue_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> queue_;
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace pulse::storage
