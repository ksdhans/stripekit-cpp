/** Callback.h
 * 
 * Callback compatibility helpers for exactly-once completion semantics.
 * 
 * @author Hans de Ruiter
 * 
 * @license See LICENSE.md for details.
 */

#pragma once

#include <functional>
#include <mutex>
#include <utility>

namespace stripekit {

/**
 * @ingroup async
 *
 * Makes a callback safe to hand to code that might fire it more than once.
 *
 * StripeKit's callback-form methods must invoke their completion exactly once:
 * calling it twice corrupts whatever is waiting on it, and never calling it
 * hangs the request. Adapters wrap the completion in one of these so the rule
 * holds even on unusual error paths.
 *
 * @code
 * auto once = std::make_shared<stripekit::OnceCallback<Result>>(std::move(completion));
 * transport.send(request, [once](Result result) { once->invoke(std::move(result)); });
 * @endcode
 *
 * Safe to call from several threads at once.
 *
 * @tparam Args Argument types the wrapped callback takes.
 *
 * @sa @ref writing_an_adapter
 */
template <typename... Args>
class OnceCallback {
public:
    /** Signature of the wrapped callback. */
    using Callback = std::function<void(Args...)>;

    /**
     * Wraps a callback.
     *
     * @param callback The callback to guard. May be empty, in which case
     *        invoke() still reports the first call but does nothing.
     */
    explicit OnceCallback(Callback callback)
        : callback_fn(std::move(callback)) {
    }

    /**
     * Calls the wrapped callback, unless it has already been called.
     *
     * @param args Arguments forwarded to the callback.
     * @return True if this call ran the callback; false if it had already run,
     *         in which case the arguments are discarded.
     */
    bool invoke(Args... args) {
        Callback callback_copy;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (invoked) {
                return false;
            }
            invoked = true;
            callback_copy = callback_fn;
        }

        if (callback_copy) {
            callback_copy(std::forward<Args>(args)...);
        }

        return true;
    }

    /**
     * Reports whether the callback has run.
     *
     * @return True once invoke() has been called.
     */
    bool was_invoked() const {
        std::lock_guard<std::mutex> lock(mutex);
        return invoked;
    }

private:
    mutable std::mutex mutex;
    bool invoked = false;
    Callback callback_fn;
};

} // namespace stripekit
