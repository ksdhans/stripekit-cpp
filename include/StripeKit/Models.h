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
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace stripekit {

/** A catalog product. Deleted products only guarantee their ID and deleted flag. */
struct Product {
    /** Stripe product identifier, including a caller-supplied ID when used. */
    std::string id;
    /** Customer-facing display name. */
    std::string name;
    /** Customer-facing description; empty when absent. */
    std::string description;
    /** Whether the product can be used for new purchases. */
    bool active = false;
    /** True for a deleted product returned by retrieval. */
    bool deleted = false;
    /** True for a live-mode object. */
    bool livemode = false;
    /** Application-defined attributes. */
    std::unordered_map<std::string, std::string> metadata;
    /** Default Price ID, empty when unset; expanded prices are also returned as IDs. */
    std::string default_price_id;
};

/** Recurrence attached to a Stripe price. */
struct PriceRecurrence {
    /** day, week, month, or year. */
    std::string interval;
    /** Number of intervals per billing period. */
    std::int64_t interval_count = 1;
    /** licensed or metered; callers must not treat them interchangeably. */
    std::string usage_type;
};

/** Quantity adjustment applied before calculating a price. */
struct PriceQuantityTransform {
    /** Divisor applied to the purchased quantity. */
    std::int64_t divide_by = 1;
    /** up or down. */
    std::string round;
};

/** A Stripe price. Changing an amount requires creating a new price. */
struct Price {
    /** Stripe price identifier. */
    std::string id;
    /** Associated product ID, whether the response contains an ID or expanded product. */
    std::string product_id;
    /** Lower-case ISO currency code. */
    std::string currency;
    /** Integer amount in the smallest currency unit; absent for non-integer or custom pricing. */
    std::optional<std::int64_t> unit_amount;
    /** Exact decimal amount when provided by Stripe. */
    std::optional<std::string> unit_amount_decimal;
    /** one_time or recurring. */
    std::string type;
    /** per_unit or tiered. */
    std::string billing_scheme;
    /** inclusive, exclusive, or unspecified. */
    std::string tax_behavior;
    /** Whether the price is available for new purchases. */
    bool active = false;
    /** True for a live-mode object. */
    bool livemode = false;
    /** Stable application lookup key; empty when absent. */
    std::string lookup_key;
    /** Recurrence, absent for one-time prices. */
    std::optional<PriceRecurrence> recurring;
    /** Quantity adjustment, absent when quantities are used unchanged. */
    std::optional<PriceQuantityTransform> transform_quantity;
    /** True when the customer chooses the amount rather than paying a fixed amount. */
    bool custom_unit_amount = false;
    /** Application-defined attributes. */
    std::unordered_map<std::string, std::string> metadata;
    /** Internal description, not shown to customers; empty when unset. */
    std::string nickname;
};

/** One page of prices. Use the last ID as starting_after when has_more is true. */
struct PriceList {
    /** Prices in Stripe's response order. */
    std::vector<Price> data;
    /** Whether further pages are available. */
    bool has_more = false;
};

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
 * A Stripe Checkout session: one attempt by a customer to pay or subscribe.
 *
 * Send the customer to `url` to start payment. `subscription_id` or
 * `payment_intent_id` is filled in depending on whether `mode` is subscription
 * or payment.
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

    /** Stripe payment intent identifier created by checkout, when present. */
    std::string payment_intent_id;

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
    /** Session expiry as Unix seconds; zero when absent. */
    std::int64_t expires_at_unix = 0;
};

/** A current subscription item, or a proposed item in a pending update. */
struct SubscriptionItem {
    /** Subscription item ID (si_...). */
    std::string id;
    /** Current or proposed Price ID. */
    std::string price_id;
    /** Quantity, absent for metered pricing or when not supplied by Stripe. */
    std::optional<std::int64_t> quantity;
    /** Item billing-period start in Unix seconds, absent when Stripe omits it. */
    std::optional<std::int64_t> current_period_start_unix;
    /** Item billing-period end in Unix seconds, absent when Stripe omits it. */
    std::optional<std::int64_t> current_period_end_unix;
};

/** Changes awaiting payment, not yet applied to the subscription. */
struct PendingSubscriptionUpdate {
    /** Unix timestamp after which Stripe discards this pending update. */
    std::int64_t expires_at_unix = 0;
    /** Proposed item changes. */
    std::vector<SubscriptionItem> items;
};

/**
 * A Stripe subscription: a customer's recurring agreement to pay.
 *
 * Use `status` with payment webhooks and your access policy. "past_due" means
 * payment is outstanding; recovery may need retries, a new payment method, or
 * customer authentication. Your application decides any grace period.
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

    /** Trial end in Unix seconds, absent when the subscription has no trial. */
    std::optional<std::int64_t> trial_end_unix;

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

    /** Current items returned by Stripe; do not assume a single item. */
    std::vector<SubscriptionItem> items;

    /** True if items contains only the first page. No automatic pagination. */
    bool items_has_more = false;

    /** Latest invoice ID, empty when absent; not proof of payment. */
    std::string latest_invoice_id;

    /** Present when a payment-dependent change has not yet been applied. */
    std::optional<PendingSubscriptionUpdate> pending_update;
};

/**
 * A Stripe invoice: the bill for one subscription period.
 *
 * A paid invoice is the signal that a period has been paid for, and is
 * normally what access should be granted from. An open invoice that still has
 * an amount remaining needs the customer's attention; send them to
 * `hosted_invoice_url` to pay it or to complete extra authentication such as
 * 3D Secure.
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

    /** Invoice status: draft, open, paid, uncollectible, or void. */
    std::string status;

    /** ISO currency code, for example usd. */
    std::string currency;

    /** Amount due in the smallest currency unit. */
    std::int64_t amount_due = 0;

    /** Amount paid in the smallest currency unit. */
    std::int64_t amount_paid = 0;

    /** Amount still owed in the smallest currency unit. */
    std::int64_t amount_remaining = 0;

    /** True when the invoice has been paid in full. */
    bool paid = false;

    /** True when the object belongs to a live Stripe account. */
    bool livemode = false;

    /** Customer-facing invoice number, empty until the invoice is finalized. */
    std::string number;

    /**
     * Stripe-hosted page where the customer can view and pay the invoice.
     *
     * Empty until the invoice is finalized. The link stays valid, so it can be
     * included in reminder messages.
     */
    std::string hosted_invoice_url;

    /** Link to download the invoice as a PDF, empty until the invoice is finalized. */
    std::string invoice_pdf;

    /**
     * Why the invoice was created, for example subscription_create,
     * subscription_cycle, subscription_update, or manual.
     */
    std::string billing_reason;

    /** Number of payment attempts made so far. */
    std::int64_t attempt_count = 0;

    /** Unix timestamp when Stripe created the invoice. */
    std::int64_t created_unix = 0;

    /** Unix timestamp when payment is due, for invoices that are sent rather than charged. */
    std::optional<std::int64_t> due_date_unix;

    /** Unix timestamp of the next automatic payment attempt, when one is scheduled. */
    std::optional<std::int64_t> next_payment_attempt_unix;
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

    /** ID of the portal configuration the session uses, of the form "bpc_...". */
    std::string configuration_id;

    /** Language the portal is shown in, or empty when Stripe picks it automatically. */
    std::string locale;
};

/** A product, and the prices of it, that customers can switch to in the billing portal. */
struct BillingPortalProduct {
    /** Stripe product ID, of the form "prod_...". */
    std::string product_id;

    /** Prices of the product that customers can choose, of the form "price_...". */
    std::vector<std::string> price_ids;
};

/** Whether customers can edit their own billing details in the portal. */
struct BillingPortalCustomerUpdate {
    /** True when the feature is shown. */
    bool enabled = false;

    /** Details customers can edit: address, email, name, phone, shipping, or tax_id. */
    std::vector<std::string> allowed_updates;
};

/** Whether customers can cancel subscriptions in the portal, and how. */
struct BillingPortalSubscriptionCancel {
    /** True when the feature is shown. */
    bool enabled = false;

    /**
     * When cancellation takes effect: at_period_end or immediately.
     *
     * Empty means Stripe's default, which is at_period_end.
     */
    std::string mode;

    /**
     * How an immediate cancellation credits unused time: none or create_prorations.
     *
     * Empty means Stripe's default.
     */
    std::string proration_behavior;
};

/** Whether customers can switch plans in the portal, and which plans they can pick. */
struct BillingPortalSubscriptionUpdate {
    /**
     * True when the feature is shown.
     *
     * Turning it on requires allowed changes, products, and payment method
     * updates.
     */
    bool enabled = false;

    /** What customers can change: price, quantity, or promotion_code. */
    std::vector<std::string> default_allowed_updates;

    /**
     * Products and prices customers can switch between.
     *
     * Stripe allows at most 10 products. A plan change in the portal can only
     * pick a price listed here.
     */
    std::vector<BillingPortalProduct> products;

    /**
     * How the switch bills for the unused part of the period: none,
     * create_prorations, or always_invoice.
     *
     * Empty means Stripe's default.
     */
    std::string proration_behavior;
};

/** The features shown in the billing portal. */
struct BillingPortalFeatures {
    /** Editing of the customer's billing details. */
    BillingPortalCustomerUpdate customer_update;

    /** True when customers can see and download past invoices. On unless turned off. */
    bool invoice_history_enabled = true;

    /**
     * True when customers can add, remove, and replace payment methods. On
     * unless turned off.
     *
     * Payment method deep links only work when this is on, and plan changes
     * cannot be enabled without it.
     */
    bool payment_method_update_enabled = true;

    /** Cancelling subscriptions. */
    BillingPortalSubscriptionCancel subscription_cancel;

    /** Switching plans. */
    BillingPortalSubscriptionUpdate subscription_update;
};

/**
 * A billing portal configuration: the features and wording a portal session
 * offers.
 *
 * Pass its ID when creating a portal session to use it instead of the
 * account's default configuration from the Stripe dashboard. Configurations
 * cannot be deleted; deactivate them instead.
 *
 * @sa https://docs.stripe.com/api/customer_portal/configurations/object
 */
struct BillingPortalConfiguration {
    /** Stripe-assigned configuration ID, of the form "bpc_...". */
    std::string id;

    /** True when new portal sessions can use the configuration. */
    bool active = false;

    /** True when this is the account's default configuration. */
    bool is_default = false;

    /** Internal name, shown only in the Stripe dashboard. */
    std::string name;

    /** Heading shown at the top of the portal, or empty when not set. */
    std::string headline;

    /** Where to send customers when a session does not give its own return URL. */
    std::string default_return_url;

    /** Features the portal offers. */
    BillingPortalFeatures features;

    /** Unix timestamp when Stripe created the configuration. */
    std::int64_t created_unix = 0;

    /** True when the object belongs to a live Stripe account. */
    bool livemode = false;

    /** Free-form key/value pairs stored on the configuration. */
    std::unordered_map<std::string, std::string> metadata;
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
