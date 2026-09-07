/** Async.h
 * 
 * Coroutine task primitives used by StripeKit async methods.
 * 
 * @author Hans de Ruiter
 * 
 * @license See LICENSE.md for details.
 */

#pragma once

#include <condition_variable>
#include <coroutine>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace stripekit {

/**
 * Move-only coroutine task that yields a single result value.
 */
template <typename TResult>
class Task {
public:
    /**
     * Promise type used by the C++ coroutine machinery.
     */
    struct promise_type {
        /**
         * Creates the task object associated with this promise.
         */
        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        /**
         * Starts suspended so callers control scheduling.
         */
        std::suspend_always initial_suspend() noexcept {
            return {};
        }

        /**
         * Notifies waiters and resumes any awaiting coroutine.
         */
        auto final_suspend() noexcept {
            struct FinalAwaiter {
                bool await_ready() const noexcept {
                    return false;
                }

                void await_suspend(std::coroutine_handle<promise_type> handle) const noexcept {
                    std::coroutine_handle<> continuation;
                    {
                        std::lock_guard<std::mutex> lock(handle.promise().mutex);
                        handle.promise().completed = true;
                        continuation = handle.promise().continuation;
                    }
                    handle.promise().ready.notify_all();
                    if (continuation) {
                        continuation.resume();
                    }
                }

                void await_resume() const noexcept {}
            };

            return FinalAwaiter{};
        }

        /**
         * Stores the returned result value.
         */
        void return_value(TResult value) {
            result = std::move(value);
        }

        /**
         * Stores unhandled exceptions for later rethrow.
         */
        void unhandled_exception() {
            error = std::current_exception();
        }

        std::optional<TResult> result;
        std::exception_ptr error;
        std::mutex mutex;
        std::condition_variable ready;
        std::coroutine_handle<> continuation;
        bool started = false;
        bool completed = false;
    };

    /**
     * Creates an empty task.
     */
    Task() = default;

    /**
     * Releases coroutine resources.
     */
    ~Task() {
        if (handle) {
            handle.destroy();
        }
    }

    /**
     * Creates a task from an existing coroutine handle.
     */
    explicit Task(std::coroutine_handle<promise_type> coroutine)
        : handle(coroutine) {
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    /**
     * Moves a task.
     */
    Task(Task&& other) noexcept
        : handle(std::exchange(other.handle, {})) {
    }

    /**
     * Move-assigns a task.
     */
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle) {
                handle.destroy();
            }
            handle = std::exchange(other.handle, {});
        }
        return *this;
    }

    /**
     * Returns true when the task has already completed.
     */
    bool is_ready() const {
        return !handle || handle.promise().completed;
    }

    /**
     * Starts the coroutine if needed, waits for completion, and returns the result.
     */
    TResult result() {
        if (!handle) {
            throw std::runtime_error("Task has no coroutine state");
        }

        start();

        {
            std::unique_lock<std::mutex> lock(handle.promise().mutex);
            handle.promise().ready.wait(lock, [this]() {
                return handle.promise().completed;
            });
        }

        if (handle.promise().error) {
            std::rethrow_exception(handle.promise().error);
        }

        return std::move(*handle.promise().result);
    }

    /**
     * Enables co_await support for this task type.
     */
    auto operator co_await() {
        struct Awaiter {
            std::coroutine_handle<promise_type> task_handle;

            bool await_ready() const noexcept {
                return !task_handle || task_handle.promise().completed;
            }

            void await_suspend(std::coroutine_handle<> awaiting) const {
                if (!task_handle) {
                    awaiting.resume();
                    return;
                }

                bool resume_task = false;
                {
                    std::lock_guard<std::mutex> lock(task_handle.promise().mutex);
                    if (task_handle.promise().completed) {
                        awaiting.resume();
                        return;
                    } else {
                        task_handle.promise().continuation = awaiting;
                        resume_task = !task_handle.promise().started;
                        task_handle.promise().started = true;
                    }
                }

                if (resume_task) {
                    task_handle.resume();
                }
            }

            TResult await_resume() {
                if (!task_handle) {
                    throw std::runtime_error("Task has no coroutine state");
                }

                if (task_handle.promise().error) {
                    std::rethrow_exception(task_handle.promise().error);
                }

                return std::move(*task_handle.promise().result);
            }
        };

        return Awaiter{handle};
    }

private:
    void start() {
        bool should_resume = false;
        {
            std::lock_guard<std::mutex> lock(handle.promise().mutex);
            should_resume = !handle.promise().started && !handle.promise().completed;
            handle.promise().started = true;
        }
        if (should_resume) {
            handle.resume();
        }
    }

    std::coroutine_handle<promise_type> handle;
};

} // namespace stripekit
