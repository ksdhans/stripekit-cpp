/** Async.h
 * 
 * Coroutine task primitives used by StripeKit async methods.
 * 
 * @author Hans de Ruiter
 * 
 * @license See LICENSE.md for details.
 */

#pragma once

#include <coroutine>
#include <exception>
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
         * Ends suspended to allow result retrieval after completion.
         */
        std::suspend_always final_suspend() noexcept {
            return {};
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
        return !handle || handle.done();
    }

    /**
     * Runs the coroutine to completion and returns the result.
     */
    TResult result() {
        if (handle && !handle.done()) {
            handle.resume();
        }

        if (!handle) {
            throw std::runtime_error("Task has no coroutine state");
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
                return !task_handle || task_handle.done();
            }

            void await_suspend(std::coroutine_handle<> awaiting) const {
                if (task_handle && !task_handle.done()) {
                    task_handle.resume();
                }
                awaiting.resume();
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
    std::coroutine_handle<promise_type> handle;
};

} // namespace stripekit
