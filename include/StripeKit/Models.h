/** Models.h
 * 
 * Core Stripe object models exposed by StripeKit.
 * 
 * @author Hans de Ruiter
 * 
 * @license See LICENSE.md for details.
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace stripekit {

/**
 * @addtogroup models
 * @{
 */

/**
 * A Stripe customer: the account everything else is billed against.
 *
 * @sa https://docs.stripe.com/api/customers/object
 */
struct Customer {
    /** Stripe-assigned customer identifier. */
    std::string id;

    /** Customer email address. */
    std::string email;

    /** Customer display name. */
    std::string name;

    /** True when the object belongs to a live Stripe account. */
    bool livemode = false;

    /** Free-form metadata key/value pairs attached to this customer. */
    std::unordered_map<std::string, std::string> metadata;
};

/**
 * A Stripe Checkout session: one attempt by a customer to subscribe.
 *
 * Send the customer to `url` to start payment. `subscription_id` is only
 * filled in once checkout completes.
 *
 * @sa https://docs.stripe.com/api/checkout/sessions/object
 */
struct CheckoutSession {
    /** Stripe-assigned checkout session identifier. */
    std::string id;

    /** Stripe customer identifier associated with the session. */
    std::string customer_id;

    /** Stripe subscription identifier created by checkout, when present. */
    std::string subscription_id;

    /** Session status, for example open, complete, or expired. */
    std::string status;

    /** Checkout mode, for example subscription or payment. */
    std::string mode;

    /** Redirect URL for hosted checkout. */
    std::string url;

    /** ISO currency code, for example usd. */
    std::string currency;

    /** Total amount in the smallest currency unit. */
    std::int64_t amount_total = 0;

    /** True when the object belongs to a live Stripe account. */
    bool livemode = false;

    /** Free-form metadata key/value pairs attached to this session. */
    std::unordered_map<std::string, std::string> metadata;
};

/**
 * A Stripe subscription: a customer's recurring agreement to pay.
 *
 * `status` is what decides whether the customer should have access. "active"
 * and "trialing" mean yes; "past_due" means Stripe is retrying a failed
 * payment; "canceled" and "unpaid" mean no.
 *
 * @sa https://docs.stripe.com/api/subscriptions/object
 */
struct Subscription {
    /** Stripe-assigned subscription identifier. */
    std::string id;

    /** Stripe customer identifier owning this subscription. */
    std::string customer_id;

    /** Lifecycle status, for example active, trialing, or past_due. */
    std::string status;

    /** Unix timestamp for the current period start. */
    std::int64_t current_period_start_unix = 0;

    /** Unix timestamp for the current period end. */
    std::int64_t current_period_end_unix = 0;

    /** True when cancellation will occur at period end. */
    bool cancel_at_period_end = false;

    /** True when the object belongs to a live Stripe account. */
    bool livemode = false;

    /** Free-form metadata key/value pairs attached to this subscription. */
    std::unordered_map<std::string, std::string> metadata;
};

/**
 * A Stripe invoice: the bill for one subscription period.
 *
 * A paid invoice is the signal that a period has been paid for, and is
 * normally what access should be granted from.
 *
 * @sa https://docs.stripe.com/api/invoices/object
 */
struct Invoice {
    /** Stripe-assigned invoice identifier. */
    std::string id;

    /** Stripe customer identifier associated with the invoice. */
    std::string customer_id;

    /** Subscription identifier associated with the invoice, when present. */
    std::string subscription_id;

    /** Invoice status, for example paid, open, or uncollectible. */
    std::string status;

    /** ISO currency code, for example usd. */
    std::string currency;

    /** Amount due in the smallest currency unit. */
    std::int64_t amount_due = 0;

    /** Amount paid in the smallest currency unit. */
    std::int64_t amount_paid = 0;

    /** True when Stripe marks the invoice as paid. */
    bool paid = false;

    /** True when the object belongs to a live Stripe account. */
    bool livemode = false;
};

/**
 * A Stripe billing portal session: a short-lived link to the self-serve
 * billing page.
 *
 * @sa https://docs.stripe.com/api/customer_portal/sessions/object
 */
struct BillingPortalSession {
    /** Stripe-assigned billing portal session identifier. */
    std::string id;

    /** Stripe customer identifier linked to this portal session. */
    std::string customer_id;

    /** Hosted billing portal URL for end-user redirection. */
    std::string url;

    /** Return URL used by Stripe after portal navigation. */
    std::string return_url;

    /** Unix timestamp when Stripe created the object. */
    std::int64_t created_unix = 0;

    /** True when the object belongs to a live Stripe account. */
    bool livemode = false;
};

/**
 * A Stripe event, as delivered by a webhook.
 *
 * `type` says what happened; `payload_json` holds Stripe's raw JSON for the
 * event, which is where to look for any detail StripeKit does not model.
 *
 * @sa https://docs.stripe.com/api/events/object
 * @sa @ref webhooks
 */
struct EventEnvelope {
    /** Stripe event identifier, of the form "evt_...". */
    std::string id;

    /** What happened, for example "invoice.paid". */
    std::string type;

    /** Stripe API version the payload is formatted for. */
    std::string api_version;

    /** Unix timestamp of when the event occurred in Stripe. */
    std::int64_t created_unix = 0;

    /** True when the event belongs to a live Stripe account. */
    bool livemode = false;

    /** Id of the API request that caused the event, when there was one. */
    std::string request_id;

    /** Connected account the event belongs to, when using Stripe Connect. */
    std::string account;

    /**
     * Stripe's raw JSON for the whole event.
     *
     * Parse this when the handler needs a field the models above do not carry.
     */
    std::string payload_json;
};

/** @} */

} // namespace stripekit
