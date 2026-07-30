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
 * Wraps a callback and guarantees at-most-once invocation semantics.
 *
 * This utility is intended for adapter callback compatibility overloads.
 */
template <typename... Args>
class OnceCallback {
public:
    /** Callback signature managed by this wrapper. */
    using Callback = std::function<void(Args...)>;

    /**
     * Creates a wrapper for the provided callback.
     */
    explicit OnceCallback(Callback callback)
        : callback_fn(std::move(callback)) {
    }

    /**
     * Invokes the callback if it has not been invoked before.
     *
     * Returns true when this call performed the callback invocation.
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
     * Returns true when the callback has already been invoked.
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
