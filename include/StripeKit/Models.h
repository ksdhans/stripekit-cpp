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
 * Represents a Stripe customer object used by StripeKit's public API.
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
 * Represents a Stripe Checkout Session object for subscription-mode flows.
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
 * Represents a Stripe subscription object.
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
 * Represents a Stripe invoice object used for entitlement decisions in host applications.
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
 * Represents a Stripe Billing Portal session object.
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
 * Represents a generic Stripe event envelope as emitted by webhook delivery.
 */
struct EventEnvelope {
    /** Stripe event identifier. */
    std::string id;

    /** Stripe event type, for example invoice.paid. */
    std::string type;

    /** Stripe API version associated with the event payload. */
    std::string api_version;

    /** Unix timestamp when the event was created in Stripe. */
    std::int64_t created_unix = 0;

    /** True when the event belongs to a live Stripe account. */
    bool livemode = false;

    /** Stripe request identifier when available. */
    std::string request_id;

    /** Stripe connected account identifier when available. */
    std::string account;

    /** Raw JSON payload for downstream parsing or inspection. */
    std::string payload_json;
};

} // namespace stripekit
