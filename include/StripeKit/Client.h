/** Client.h
 * 
 * The Stripe client for creating customers, checkout sessions, subscriptions,
 * and billing portal sessions.
 * 
 * @author Hans de Ruiter
 * 
 * @license See LICENSE.md for details.
 */

#pragma once

#include "StripeKit/Async.h"
#include "StripeKit/Models.h"
#include "StripeKit/RequestOptions.h"

#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <unordered_map>
#include <variant>

namespace stripekit {

/**
 * Request payload for creating a Stripe customer.
 */
struct CreateCustomerRequest {
    /** Customer email address. */
    std::string email;

    /** Customer display name. */
    std::string name;

    /** Free-form metadata attached to the customer. */
    std::unordered_map<std::string, std::string> metadata;
};

/**
 * Request payload for creating a hosted Checkout session in subscription mode.
 */
struct CreateCheckoutSessionRequest {
    /** Existing Stripe customer id to bill. */
    std::string customer_id;

    /** Existing Stripe Price id used for the subscription line item. */
    std::string price_id;

    /** Product name used when creating inline subscription price data. */
    std::string product_name;

    /** ISO currency code used for inline price data, for example usd. */
    std::string currency = "usd";

    /** Inline subscription price amount in the smallest currency unit. */
    std::int64_t unit_amount = 0;

    /** Inline recurring interval, for example month or year. */
    std::string recurring_interval = "month";

    /** Redirect URL used after successful checkout completion. */
    std::string success_url;

    /** Redirect URL used when checkout is canceled. */
    std::string cancel_url;

    /** Quantity for the subscription line item. */
    std::int64_t quantity = 1;

    /** Free-form metadata attached to the checkout session. */
    std::unordered_map<std::string, std::string> metadata;
};

/**
 * Request payload for creating a customer billing portal session.
 */
struct CreateBillingPortalSessionRequest {
    /** Existing Stripe customer id for portal access. */
    std::string customer_id;

    /** Return URL used when leaving the billing portal. */
    std::string return_url;
};

/**
 * Callback result wrapper used by compatibility overloads.
 */
template <typename TResult>
using CallbackResult = std::variant<TResult, std::exception_ptr>;

/**
 * Abstract base class for creating and retrieving Stripe customers.
 */
class Customers {
public:
    /** Callback type for create operation compatibility overloads. */
    using CreateCompletion = std::function<void(CallbackResult<Customer>)>;

    /** Callback type for retrieve operation compatibility overloads. */
    using RetrieveCompletion = std::function<void(CallbackResult<Customer>)>;

    /**
     * Destroys the API instance.
     */
    virtual ~Customers() = default;

    /**
     * Starts a coroutine that creates a customer in Stripe.
     */
    virtual Task<Customer> create_async(const CreateCustomerRequest& request,
                                        const RequestOptions& options = {}) = 0;

    /**
     * Creates a customer using callback compatibility semantics.
     *
     * Implementations must complete exactly once.
     */
    virtual void create(const CreateCustomerRequest& request,
                        const RequestOptions& options,
                        CreateCompletion completion) = 0;

    /**
     * Starts a coroutine that fetches a customer by id.
     */
    virtual Task<Customer> retrieve_async(std::string customer_id,
                                          const RequestOptions& options = {}) = 0;

    /**
     * Retrieves a customer using callback compatibility semantics.
     *
     * Implementations must complete exactly once.
     */
    virtual void retrieve(std::string customer_id,
                          const RequestOptions& options,
                          RetrieveCompletion completion) = 0;
};

/**
 * Abstract base class for creating Stripe-hosted Checkout sessions.
 *
 * This interface is used to create Stripe-hosted checkout pages.
 */
class CheckoutSessions {
public:
    /** Callback type for create operation compatibility overloads. */
    using CreateCompletion = std::function<void(CallbackResult<CheckoutSession>)>;

    /**
     * Destroys the API instance.
     */
    virtual ~CheckoutSessions() = default;

    /**
     * Starts a coroutine that creates a Stripe-hosted checkout session.
     */
    virtual Task<CheckoutSession> create_async(const CreateCheckoutSessionRequest& request,
                                               const RequestOptions& options = {}) = 0;

    /**
     * Creates a checkout session using callback compatibility semantics.
     *
     * Implementations must complete exactly once.
     */
    virtual void create(const CreateCheckoutSessionRequest& request,
                        const RequestOptions& options,
                        CreateCompletion completion) = 0;
};

/**
 * Abstract base class that groups Checkout session operations.
 */
class Checkout {
public:
    /**
     * Destroys the API instance.
     */
    virtual ~Checkout() = default;

    /**
     * Returns checkout session operations.
     */
    virtual CheckoutSessions& sessions() = 0;
};

/**
 * Abstract base class for retrieving and canceling Stripe subscriptions.
 */
class Subscriptions {
public:
    /** Callback type for retrieve operation compatibility overloads. */
    using RetrieveCompletion = std::function<void(CallbackResult<Subscription>)>;

    /** Callback type for cancel operation compatibility overloads. */
    using CancelCompletion = std::function<void(CallbackResult<Subscription>)>;

    /**
     * Destroys the API instance.
     */
    virtual ~Subscriptions() = default;

    /**
     * Starts a coroutine that fetches a subscription by id.
     */
    virtual Task<Subscription> retrieve_async(std::string subscription_id,
                                              const RequestOptions& options = {}) = 0;

    /**
     * Retrieves a subscription using callback compatibility semantics.
     *
     * Implementations must complete exactly once.
     */
    virtual void retrieve(std::string subscription_id,
                          const RequestOptions& options,
                          RetrieveCompletion completion) = 0;

    /**
     * Starts a coroutine that cancels a subscription.
     */
    virtual Task<Subscription> cancel_async(std::string subscription_id,
                                            const RequestOptions& options = {}) = 0;

    /**
     * Cancels a subscription using callback compatibility semantics.
     *
     * Implementations must complete exactly once.
     */
    virtual void cancel(std::string subscription_id,
                        const RequestOptions& options,
                        CancelCompletion completion) = 0;
};

/**
 * Abstract base class for creating customer billing portal sessions.
 *
 * This interface is used to create customer self-serve billing links.
 */
class BillingPortalSessions {
public:
    /** Callback type for create operation compatibility overloads. */
    using CreateCompletion = std::function<void(CallbackResult<BillingPortalSession>)>;

    /**
     * Destroys the API instance.
     */
    virtual ~BillingPortalSessions() = default;

    /**
     * Starts a coroutine that creates a billing portal session.
     */
    virtual Task<BillingPortalSession> create_async(
        const CreateBillingPortalSessionRequest& request,
        const RequestOptions& options = {}) = 0;

    /**
     * Creates a billing portal session using callback compatibility semantics.
     *
     * Implementations must complete exactly once.
     */
    virtual void create(const CreateBillingPortalSessionRequest& request,
                        const RequestOptions& options,
                        CreateCompletion completion) = 0;
};

/**
 * Abstract base class that groups billing portal session operations.
 */
class BillingPortal {
public:
    /**
     * Destroys the API instance.
     */
    virtual ~BillingPortal() = default;

    /**
     * Returns billing portal session operations.
     */
    virtual BillingPortalSessions& sessions() = 0;
};

/**
 * Abstract base class for a Stripe client object.
 *
 * The interface mirrors Stripe's resource grouping style: for example,
 * `customers()`, `checkout().sessions()`, and `billing_portal().sessions()`.
 */
class StripeClient {
public:
    /**
     * Destroys the client instance.
     */
    virtual ~StripeClient() = default;

    /**
     * Returns customer operations.
     */
    virtual Customers& customers() = 0;

    /**
     * Returns checkout operations.
     */
    virtual Checkout& checkout() = 0;

    /**
     * Returns subscription operations.
     */
    virtual Subscriptions& subscriptions() = 0;

    /**
     * Returns billing portal operations.
     */
    virtual BillingPortal& billing_portal() = 0;
};

} // namespace stripekit
