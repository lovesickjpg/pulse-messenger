#include <pulse/database_worker.hpp>

#include <future>

namespace pulse::storage {

DatabaseWorker::DatabaseWorker(boost::asio::any_io_executor results,
                               const std::filesystem::path &file, std::size_t max_queue)
    : results_(std::move(results)), max_queue_(max_queue) {
    // shared so the worker thread can't outlive it, whatever order the two finish in
    auto opened = std::make_shared<std::promise<void>>();
    auto result = opened->get_future();
    thread_ = std::thread([this, file, opened] {
        run(file, [opened](std::exception_ptr error) {
            if (error) {
                opened->set_exception(error);
            } else {
                opened->set_value();
            }
        });
    });
    try {
        result.get();
    } catch (...) {
        thread_.join();
        throw;
    }
}

DatabaseWorker::~DatabaseWorker() { stop(); }

void DatabaseWorker::run(const std::filesystem::path &file,
                         std::function<void(std::exception_ptr)> ready) {
    std::optional<Database> database;
    try {
        database.emplace(file);
    } catch (...) {
        ready(std::current_exception());
        return;
    }
    ready(nullptr);

    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                return;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        job(&*database, nullptr);
    }
}

void DatabaseWorker::enqueue(Job job) {
    std::exception_ptr rejected;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            rejected = std::make_exception_ptr(
                StorageError(ErrorKind::Shutdown, "Database worker is stopping"));
        } else if (queue_.size() >= max_queue_) {
            rejected = std::make_exception_ptr(
                StorageError(ErrorKind::Overloaded, "Database queue is full"));
        } else {
            queue_.push_back(std::move(job));
            wake_.notify_one();
            return;
        }
    }
    job(nullptr, rejected);
}

void DatabaseWorker::stop() {
    std::deque<Job> abandoned;
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        abandoned.swap(queue_);
        wake_.notify_all();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    const auto reason = std::make_exception_ptr(
        StorageError(ErrorKind::Shutdown, "Database worker stopped before running this job"));
    for (auto &job : abandoned) {
        job(nullptr, reason);
    }
}

} // namespace pulse::storage
