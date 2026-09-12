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
 * @ingroup async
 *
 * The result of an asynchronous StripeKit operation.
 *
 * Every `*_async` method returns one of these. `co_await` it to get the value:
 *
 * @code
 * const stripekit::Customer customer = co_await stripe->customers().create_async(request);
 * @endcode
 *
 * Outside a coroutine, result() blocks until the value is ready. That is
 * convenient in command-line tools and tests, but must never be done on a
 * thread that is running an event loop, since the work being waited on may
 * need that loop to make progress.
 *
 * The task starts on the first `co_await` or result(), not on construction, so
 * a task that is created and dropped never runs. Tasks are move-only, and each
 * one produces its value once.
 *
 * @tparam TResult Type of value the operation produces.
 */
template <typename TResult>
class Task {
public:
    /**
     * Coroutine promise type. Used by the compiler; not called directly.
     */
    struct promise_type {
        /**
         * Creates the task object handed back to the caller.
         *
         * @return The task wrapping this coroutine.
         */
        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        /**
         * Suspends the coroutine at its start so the caller controls when it runs.
         *
         * @return An awaitable that always suspends.
         */
        std::suspend_always initial_suspend() noexcept {
            return {};
        }

        /**
         * Wakes anything waiting on this task once it finishes.
         *
         * @return An awaitable that notifies waiters and resumes the awaiting
         *         coroutine.
         */
        auto final_suspend() noexcept {
            struct FinalAwaiter {
                bool await_ready() const noexcept {
                    return false;
                }

                void await_suspend(std::coroutine_handle<promise_type> handle) const noexcept {
                    std::coroutine_handle<> continuation;
                    {
                        // Notify while still holding the lock: a waiter woken by
                        // notify_all() cannot reacquire the mutex, and therefore
                        // cannot destroy this coroutine's promise (mutex and
                        // condition_variable included), until this scope releases
                        // it below. Notifying after unlocking would let the
                        // waiter finish and destroy the promise concurrently with
                        // this notify_all() call, corrupting the heap.
                        std::lock_guard<std::mutex> lock(handle.promise().mutex);
                        handle.promise().completed = true;
                        continuation = handle.promise().continuation;
                        handle.promise().ready.notify_all();
                    }
                    if (continuation) {
                        continuation.resume();
                    }
                }

                void await_resume() const noexcept {}
            };

            return FinalAwaiter{};
        }

        /**
         * Stores the value the coroutine returned.
         *
         * @param value The produced value.
         */
        void return_value(TResult value) {
            result = std::move(value);
        }

        /**
         * Captures an escaping exception so it can be rethrown to the waiter.
         */
        void unhandled_exception() {
            error = std::current_exception();
        }

        std::optional<TResult> result; /**< The produced value, once available. */
        std::exception_ptr error; /**< The exception that escaped, if one did. */
        std::mutex mutex; /**< Guards the fields below. */
        std::condition_variable ready; /**< Signalled when the coroutine finishes. */
        std::coroutine_handle<> continuation; /**< Coroutine waiting on this task. */
        bool started = false; /**< True once the coroutine has been resumed. */
        bool completed = false; /**< True once the coroutine has finished. */
    };

    /**
     * Creates a task with no coroutine attached.
     *
     * Awaiting or querying an empty task throws.
     */
    Task() = default;

    /**
     * Destroys the task and its coroutine state.
     */
    ~Task() {
        if (handle) {
            handle.destroy();
        }
    }

    /**
     * Adopts an existing coroutine handle.
     *
     * @param coroutine The coroutine this task takes ownership of.
     */
    explicit Task(std::coroutine_handle<promise_type> coroutine)
        : handle(coroutine) {
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    /**
     * Takes over another task's coroutine, leaving it empty.
     *
     * @param other The task to move from.
     */
    Task(Task&& other) noexcept
        : handle(std::exchange(other.handle, {})) {
    }

    /**
     * Takes over another task's coroutine, leaving it empty.
     *
     * @param other The task to move from.
     * @return This task.
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
     * Reports whether the value is already available.
     *
     * @return True when the operation has finished, or the task is empty.
     */
    bool is_ready() const {
        if (!handle) {
            return true;
        }
        std::lock_guard<std::mutex> lock(handle.promise().mutex);
        return handle.promise().completed;
    }

    /**
     * Waits for the operation to finish and returns its value.
     *
     * Starts the operation if it has not started yet, then blocks the calling
     * thread. Do not call this from an event loop thread: the work being waited
     * on may need that loop to run, which would deadlock. Prefer `co_await`.
     *
     * @return The produced value, moved out of the task.
     * @throws std::runtime_error The task is empty.
     * @throws StripeApiException Or any other exception the operation threw.
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
     * Makes the task awaitable, so `co_await` yields its value.
     *
     * Starts the operation if it has not started yet, suspends the awaiting
     * coroutine, and resumes it with the value. An exception thrown by the
     * operation is rethrown at the `co_await`.
     *
     * @return The awaiter used by the compiler.
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
