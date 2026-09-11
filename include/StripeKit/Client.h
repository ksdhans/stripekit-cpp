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
 * @addtogroup client
 * @{
 */

/**
 * Describes the customer to create.
 *
 * @sa https://docs.stripe.com/api/customers/create
 */
struct CreateCustomerRequest {
    /** Customer email address. Stripe uses it for receipts and invoices. */
    std::string email;

    /** Customer display name, as it should appear in the Stripe dashboard. */
    std::string name;

    /**
     * Free-form key/value pairs to store on the customer.
     *
     * Stripe echoes metadata back on related webhook events, so this is a good
     * place to record the application's own user id.
     */
    std::unordered_map<std::string, std::string> metadata;
};

/**
 * Describes the Checkout session to create.
 *
 * Checkout sessions are created in subscription mode. Price the subscription
 * one of two ways: set `price_id` to reuse a Price already configured in the
 * Stripe dashboard, or leave it empty and fill in `product_name`, `currency`,
 * `unit_amount`, and `recurring_interval` to define the price inline.
 *
 * @sa https://docs.stripe.com/api/checkout/sessions/create
 */
struct CreateCheckoutSessionRequest {
    /** Id of an existing Stripe customer to bill. */
    std::string customer_id;

    /** Id of an existing Stripe Price. Leave empty to price the line item inline. */
    std::string price_id;

    /** Product name, for inline pricing. Ignored when `price_id` is set. */
    std::string product_name;

    /** ISO currency code for inline pricing, for example "usd". */
    std::string currency = "usd";

    /**
     * Amount per interval for inline pricing, in the currency's smallest unit.
     *
     * That means cents for USD, so 1499 is $14.99.
     */
    std::int64_t unit_amount = 0;

    /** Billing interval for inline pricing: "day", "week", "month", or "year". */
    std::string recurring_interval = "month";

    /**
     * Where Stripe sends the customer's browser after a successful checkout.
     *
     * Reaching this URL is not proof of payment; the customer may close the tab
     * first, and anyone can visit the URL directly. Grant access from the
     * `invoice.paid` webhook instead.
     */
    std::string success_url;

    /** Where Stripe sends the customer's browser if they abandon checkout. */
    std::string cancel_url;

    /** Number of units of the subscription to bill for. */
    std::int64_t quantity = 1;

    /** Free-form key/value pairs to store on the session. */
    std::unordered_map<std::string, std::string> metadata;
};

/**
 * Describes the billing portal session to create.
 *
 * @sa https://docs.stripe.com/api/customer_portal/sessions/create
 */
struct CreateBillingPortalSessionRequest {
    /** Id of the Stripe customer who should manage their billing. */
    std::string customer_id;

    /** Where Stripe sends the customer when they leave the portal. */
    std::string return_url;
};

/**
 * Result passed to a completion callback: either the value, or the failure.
 *
 * Use `std::holds_alternative<std::exception_ptr>()` to check which arrived,
 * and `std::rethrow_exception()` to inspect a failure.
 *
 * @tparam TResult Type produced when the operation succeeds.
 */
template <typename TResult>
using CallbackResult = std::variant<TResult, std::exception_ptr>;

/**
 * Creates and retrieves Stripe customers.
 *
 * Reach an instance through StripeClient::customers(). A Stripe customer is
 * the object everything else hangs off, so create one per application user and
 * store the resulting id; do not create a fresh customer per checkout.
 *
 * @sa https://docs.stripe.com/api/customers
 */
class Customers {
public:
    /** Completion callback for the callback form of create(). */
    using CreateCompletion = std::function<void(CallbackResult<Customer>)>;

    /** Completion callback for the callback form of retrieve(). */
    using RetrieveCompletion = std::function<void(CallbackResult<Customer>)>;

    /**
     * Destroys the API instance.
     */
    virtual ~Customers() = default;

    /**
     * Creates a customer in Stripe.
     *
     * @param request Details of the customer to create.
     * @param options Per-request settings such as an idempotency key.
     * @return A task producing the created customer, including its Stripe id.
     * @throws StripeApiException Stripe rejected the request.
     */
    virtual Task<Customer> create_async(const CreateCustomerRequest& request,
                                        const RequestOptions& options = {}) = 0;

    /**
     * Creates a customer, reporting the result through a callback.
     *
     * Use this in code that is not using coroutines; create_async() is
     * otherwise preferable. The callback is invoked exactly once, and may run
     * on a different thread than the caller.
     *
     * @param request Details of the customer to create.
     * @param options Per-request settings such as an idempotency key.
     * @param completion Receives the created customer or the failure.
     */
    virtual void create(const CreateCustomerRequest& request,
                        const RequestOptions& options,
                        CreateCompletion completion) = 0;

    /**
     * Fetches an existing customer.
     *
     * @param customer_id Stripe customer id, of the form "cus_...".
     * @param options Per-request settings such as a timeout.
     * @return A task producing the customer.
     * @throws StripeApiException No such customer, or Stripe rejected the request.
     */
    virtual Task<Customer> retrieve_async(std::string customer_id,
                                          const RequestOptions& options = {}) = 0;

    /**
     * Fetches an existing customer, reporting the result through a callback.
     *
     * The callback is invoked exactly once, and may run on a different thread
     * than the caller.
     *
     * @param customer_id Stripe customer id, of the form "cus_...".
     * @param options Per-request settings such as a timeout.
     * @param completion Receives the customer or the failure.
     */
    virtual void retrieve(std::string customer_id,
                          const RequestOptions& options,
                          RetrieveCompletion completion) = 0;
};

/**
 * Creates Stripe-hosted Checkout sessions.
 *
 * Reach an instance through StripeClient::checkout(). Checkout is a payment
 * page Stripe hosts: create a session, redirect the customer to
 * CheckoutSession::url, and Stripe collects the card details.
 *
 * @sa https://docs.stripe.com/api/checkout/sessions
 */
class CheckoutSessions {
public:
    /** Completion callback for the callback form of create(). */
    using CreateCompletion = std::function<void(CallbackResult<CheckoutSession>)>;

    /**
     * Destroys the API instance.
     */
    virtual ~CheckoutSessions() = default;

    /**
     * Creates a Checkout session.
     *
     * @param request Details of the session to create.
     * @param options Per-request settings. Set an idempotency key so a retry
     *        cannot start a second subscription.
     * @return A task producing the session, whose `url` is where the customer
     *         should be sent.
     * @throws StripeApiException Stripe rejected the request.
     */
    virtual Task<CheckoutSession> create_async(const CreateCheckoutSessionRequest& request,
                                               const RequestOptions& options = {}) = 0;

    /**
     * Creates a Checkout session, reporting the result through a callback.
     *
     * The callback is invoked exactly once, and may run on a different thread
     * than the caller.
     *
     * @param request Details of the session to create.
     * @param options Per-request settings such as an idempotency key.
     * @param completion Receives the created session or the failure.
     */
    virtual void create(const CreateCheckoutSessionRequest& request,
                        const RequestOptions& options,
                        CreateCompletion completion) = 0;
};

/**
 * Groups the Checkout operations.
 *
 * Mirrors Stripe's own grouping, so `checkout().sessions()` matches the
 * Checkout Sessions section of Stripe's API reference.
 */
class Checkout {
public:
    /**
     * Destroys the API instance.
     */
    virtual ~Checkout() = default;

    /**
     * Returns the Checkout session operations.
     *
     * @return A reference owned by the client; it stays valid as long as the
     *         client does.
     */
    virtual CheckoutSessions& sessions() = 0;
};

/**
 * Retrieves and cancels Stripe subscriptions.
 *
 * Reach an instance through StripeClient::subscriptions(). Subscriptions are
 * created by Checkout rather than here; this interface covers inspecting one
 * and ending it.
 *
 * @sa https://docs.stripe.com/api/subscriptions
 */
class Subscriptions {
public:
    /** Completion callback for the callback form of retrieve(). */
    using RetrieveCompletion = std::function<void(CallbackResult<Subscription>)>;

    /** Completion callback for the callback form of cancel(). */
    using CancelCompletion = std::function<void(CallbackResult<Subscription>)>;

    /**
     * Destroys the API instance.
     */
    virtual ~Subscriptions() = default;

    /**
     * Fetches a subscription and its current status.
     *
     * This reports what Stripe believes right now, which is useful for
     * reconciliation. For day-to-day access decisions, follow the webhooks
     * instead of polling.
     *
     * @param subscription_id Stripe subscription id, of the form "sub_...".
     * @param options Per-request settings such as a timeout.
     * @return A task producing the subscription.
     * @throws StripeApiException No such subscription, or Stripe rejected the request.
     */
    virtual Task<Subscription> retrieve_async(std::string subscription_id,
                                              const RequestOptions& options = {}) = 0;

    /**
     * Fetches a subscription, reporting the result through a callback.
     *
     * The callback is invoked exactly once, and may run on a different thread
     * than the caller.
     *
     * @param subscription_id Stripe subscription id, of the form "sub_...".
     * @param options Per-request settings such as a timeout.
     * @param completion Receives the subscription or the failure.
     */
    virtual void retrieve(std::string subscription_id,
                          const RequestOptions& options,
                          RetrieveCompletion completion) = 0;

    /**
     * Cancels a subscription immediately.
     *
     * Billing stops at once rather than at the end of the paid period. Stripe
     * sends a `customer.subscription.deleted` webhook, which is where access
     * should actually be revoked.
     *
     * @param subscription_id Stripe subscription id, of the form "sub_...".
     * @param options Per-request settings such as an idempotency key.
     * @return A task producing the subscription in its cancelled state.
     * @throws StripeApiException Stripe rejected the request.
     */
    virtual Task<Subscription> cancel_async(std::string subscription_id,
                                            const RequestOptions& options = {}) = 0;

    /**
     * Cancels a subscription, reporting the result through a callback.
     *
     * The callback is invoked exactly once, and may run on a different thread
     * than the caller.
     *
     * @param subscription_id Stripe subscription id, of the form "sub_...".
     * @param options Per-request settings such as an idempotency key.
     * @param completion Receives the cancelled subscription or the failure.
     */
    virtual void cancel(std::string subscription_id,
                        const RequestOptions& options,
                        CancelCompletion completion) = 0;
};

/**
 * Creates billing portal sessions.
 *
 * Reach an instance through StripeClient::billing_portal(). The billing portal
 * is a Stripe-hosted page where customers update their card, download
 * invoices, and cancel — which saves building any of that.
 *
 * @sa https://docs.stripe.com/api/customer_portal/sessions
 */
class BillingPortalSessions {
public:
    /** Completion callback for the callback form of create(). */
    using CreateCompletion = std::function<void(CallbackResult<BillingPortalSession>)>;

    /**
     * Destroys the API instance.
     */
    virtual ~BillingPortalSessions() = default;

    /**
     * Creates a billing portal session.
     *
     * Sessions are short-lived, so create one per visit rather than storing
     * the URL.
     *
     * @param request The customer to create the session for, and where to
     *        return them afterwards.
     * @param options Per-request settings such as a timeout.
     * @return A task producing the session, whose `url` is where the customer
     *         should be sent.
     * @throws StripeApiException Stripe rejected the request. A common cause is
     *         the billing portal not yet being configured in the dashboard.
     */
    virtual Task<BillingPortalSession> create_async(
        const CreateBillingPortalSessionRequest& request,
        const RequestOptions& options = {}) = 0;

    /**
     * Creates a billing portal session, reporting the result through a callback.
     *
     * The callback is invoked exactly once, and may run on a different thread
     * than the caller.
     *
     * @param request The customer to create the session for, and where to
     *        return them afterwards.
     * @param options Per-request settings such as a timeout.
     * @param completion Receives the created session or the failure.
     */
    virtual void create(const CreateBillingPortalSessionRequest& request,
                        const RequestOptions& options,
                        CreateCompletion completion) = 0;
};

/**
 * Groups the billing portal operations.
 */
class BillingPortal {
public:
    /**
     * Destroys the API instance.
     */
    virtual ~BillingPortal() = default;

    /**
     * Returns the billing portal session operations.
     *
     * @return A reference owned by the client; it stays valid as long as the
     *         client does.
     */
    virtual BillingPortalSessions& sessions() = 0;
};

/**
 * The entry point for talking to Stripe.
 *
 * Operations are grouped the way Stripe's own API reference groups them, so
 * `checkout().sessions().create_async()` corresponds to Stripe's "Create a
 * Checkout Session" endpoint.
 *
 * Obtain one from an adapter's factory function, such as
 * stripekit::drogon::make_stripe_client(). One client is enough for the whole
 * application: it is safe to share across threads and reuses connections to
 * Stripe, so creating one per request only adds latency.
 *
 * @code
 * auto stripe = stripekit::drogon::make_stripe_client({.api_key = "sk_test_..."});
 * const auto customer = co_await stripe->customers().create_async({.email = "a@b.com"});
 * @endcode
 *
 * @sa @ref quickstart
 */
class StripeClient {
public:
    /**
     * Destroys the client instance.
     */
    virtual ~StripeClient() = default;

    /**
     * Returns the customer operations.
     *
     * @return A reference owned by the client; it stays valid as long as the
     *         client does.
     */
    virtual Customers& customers() = 0;

    /**
     * Returns the Checkout operations.
     *
     * @return A reference owned by the client; it stays valid as long as the
     *         client does.
     */
    virtual Checkout& checkout() = 0;

    /**
     * Returns the subscription operations.
     *
     * @return A reference owned by the client; it stays valid as long as the
     *         client does.
     */
    virtual Subscriptions& subscriptions() = 0;

    /**
     * Returns the billing portal operations.
     *
     * @return A reference owned by the client; it stays valid as long as the
     *         client does.
     */
    virtual BillingPortal& billing_portal() = 0;
};

/** @} */

} // namespace stripekit
