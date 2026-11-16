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

/** Customer billing-address fields supported by Stripe. */
struct BillingAddress {
    /** First street-address line. */
    std::optional<std::string> line1;
    /** Second street-address line. */
    std::optional<std::string> line2;
    /** City or locality. */
    std::optional<std::string> city;
    /** State, province, or region. */
    std::optional<std::string> state;
    /** Postal or ZIP code. */
    std::optional<std::string> postal_code;
    /** Two-letter country code. */
    std::optional<std::string> country;
};

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

    /** Billing address, absent when Stripe has no address on file. */
    std::optional<BillingAddress> address;

    /** Stripe's tax-location validation status, when returned. */
    std::string tax_location_status;

    /** Source Stripe used to determine the customer's tax location, when returned. */
    std::string tax_location_source;

    /** True when the object belongs to a live Stripe account. */
    bool livemode = false;

    /** Free-form metadata key/value pairs attached to this customer. */
    std::unordered_map<std::string, std::string> metadata;
};

/** Stripe's automatic-tax state, including location-related status when available. */
struct AutomaticTax {
    /** Whether automatic tax is enabled. */
    bool enabled = false;
    /** Stripe's calculation status, when supplied. */
    std::string status;
    /** Why Stripe disabled automatic tax, when supplied. */
    std::string disabled_reason;
};

/** A price and quantity returned for a Checkout or invoice line. */
struct PurchasedLineItem {
    /** Stripe line-item identifier, used as the next-page cursor. */
    std::string id;
    /** Stripe Price ID. */
    std::string price_id;
    /** Purchased quantity, absent when Stripe omits it. */
    std::optional<std::int64_t> quantity;
    /** Line amount in the currency's smallest unit, when supplied. */
    std::optional<std::int64_t> amount;
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

    /** Pre-tax amount in the smallest currency unit, when supplied. */
    std::optional<std::int64_t> amount_subtotal;

    /** Discount amount in the smallest currency unit, when supplied. */
    std::optional<std::int64_t> amount_discount;

    /** Tax amount in the smallest currency unit, when supplied. */
    std::optional<std::int64_t> amount_tax;

    /** Payment state such as unpaid, paid, or no_payment_required. */
    std::string payment_status;

    /** Automatic-tax state, absent when not included in the response. */
    std::optional<AutomaticTax> automatic_tax;

    /** Purchased prices and quantities when Stripe returns expanded line items. */
    std::vector<PurchasedLineItem> line_items;

    /** True when Stripe returned only the first page of line items. */
    bool line_items_has_more = false;

    /** True when the object belongs to a live Stripe account. */
    bool livemode = false;

    /** Free-form metadata key/value pairs attached to this session. */
    std::unordered_map<std::string, std::string> metadata;
    /** Session expiry as Unix seconds; zero when absent. */
    std::int64_t expires_at_unix = 0;
    /** Billing address collection mode, such as auto or required; empty when unavailable. */
    std::string billing_address_collection;
    /** Whether hosted Checkout offers business tax-ID collection; absent when unavailable. */
    std::optional<bool> tax_id_collection_enabled;
};

/**
 * One attempt to collect a payment. Amounts are in the currency's smallest unit.
 * Find invoice allocations through InvoicePayments using payment_intent_id.
 */
struct Charge {
    std::string id;
    std::string customer_id;       ///< Empty for a payment without a saved customer.
    std::string payment_intent_id; ///< Empty for charges not created by a PaymentIntent.
    std::string status;            ///< Stripe status, such as succeeded, pending, or failed.
    std::string currency;
    std::int64_t amount = 0;
    std::int64_t amount_refunded = 0;
    bool paid = false;
    bool disputed = false;
    bool livemode = false;
};

/**
 * Money returned, or requested to be returned, for a payment.
 * Amounts use the currency's smallest unit. A refund is not complete until
 * status is succeeded; pending and requires_action still reserve its amount.
 */
struct Refund {
    std::string id;
    std::string charge_id;
    std::string payment_intent_id; ///< Empty when Stripe does not supply a PaymentIntent.
    std::int64_t amount = 0;
    std::string currency;
    std::string status;         ///< succeeded, pending, requires_action, failed, or canceled.
    std::string failure_reason; ///< Empty unless Stripe explains a failure.
    std::unordered_map<std::string, std::string> metadata;
};

/** One page of refunds; use the last refund ID as the next cursor when has_more is true. */
struct RefundList {
    std::vector<Refund> data;
    bool has_more = false;
};

/** One page of charges; use the last ID as starting_after when has_more is true. */
struct ChargeList {
    std::vector<Charge> data;
    bool has_more = false;
};

/**
 * A payment allocated to an invoice, not necessarily its full balance.
 * Keep payment_type when handling future Stripe payment types; only the
 * reference matching that type is populated.
 */
struct InvoicePayment {
    std::string id;
    std::string invoice_id;
    std::string payment_type; ///< For example payment_intent, charge, or payment_record.
    std::string payment_intent_id;
    std::string charge_id;
    std::string payment_record_id;
    std::string status; ///< For example open, paid, or canceled.
    std::string currency;
    std::int64_t amount_requested = 0;
    std::optional<std::int64_t> amount_paid; ///< Absent until the allocation is paid.
    bool is_default = false;
    bool livemode = false;
};

/** One page of invoice allocations; use the last ID as the next cursor. */
struct InvoicePaymentList {
    std::vector<InvoicePayment> data;
    bool has_more = false;
};

/** One page of Checkout sessions; request the next page only when has_more is true. */
struct CheckoutSessionList {
    std::vector<CheckoutSession> data;
    bool has_more = false;
};

/** One page of purchased prices and quantities. */
struct PurchasedLineItemList {
    std::vector<PurchasedLineItem> data;
    bool has_more = false;
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
    /** Discounts applied to this item, as Stripe discount IDs. */
    std::vector<std::string> discount_ids;
    /** Tax rates applied to this item, as Stripe tax rate IDs. */
    std::vector<std::string> tax_rate_ids;
};

/** Changes awaiting payment, not yet applied to the subscription. */
struct PendingSubscriptionUpdate {
    /** Unix timestamp after which Stripe discards this pending update. */
    std::int64_t expires_at_unix = 0;
    /** Proposed item changes. */
    std::vector<SubscriptionItem> items;
};

/** How Stripe handles invoices created while subscription collection is paused. */
enum class CollectionPauseBehavior { KeepAsDraft, MarkUncollectible, Void };

/** Collection pause reported by Stripe; an absent resume date means paused until cleared. */
struct SubscriptionCollectionPause {
    std::string
        behavior; ///< keep_as_draft, mark_uncollectible, or void; unknown values are preserved.
    std::optional<std::int64_t> resumes_at_unix;
};

/** Billing mode used by a subscription or subscription schedule. */
struct SubscriptionBillingMode {
    /** classic or flexible; true subscription pauses require flexible. */
    std::string type;
};

/** Point in the billing period from which to credit time or collect usage. */
struct SubscriptionPauseBillingPoint {
    /** now for direct pauses, pause_at for scheduled pauses, none, or item_current_period_start. */
    std::string type;
};

/** Credits and usage charges to create when pausing a subscription. */
struct SubscriptionPauseBillFor {
    /** Credit unused licensed time from this point. */
    SubscriptionPauseBillingPoint unused_time_from;
    /** Charge metered usage through this point; only now, pause_at, or none is allowed. */
    SubscriptionPauseBillingPoint outstanding_usage_through;
};

/** Pause billing choices. Leave fields empty to use defaults or keep settings with the same key. */
struct SubscriptionPauseSettings {
    /** Credits and metered charges to create when pausing. */
    SubscriptionPauseBillFor bill_for;
    /** invoice or pending_invoice_item. */
    std::string invoicing_behavior;
    /** subscription, or empty to use Stripe's default. */
    std::string type;
};

/** Billing and payment choices for a scheduled resume. */
struct SubscriptionScheduleResumeSettings {
    /** resume_at or unchanged; empty preserves an existing choice or uses Stripe's default. */
    std::string billing_cycle_anchor;
    /** resume_on_payment_attempt or resume_on_payment_success. */
    std::string payment_behavior;
    /** create_prorations, always_invoice, or none. */
    std::string proration_behavior;
};

/** Why a subscription was paused. */
struct SubscriptionPauseReason {
    /** For example pause_requested, trial_end_without_payment_method, or system. */
    std::string type;
};

/** When and why the subscription entered its paused status. */
struct SubscriptionPausedStatusDetails {
    /** Reason from Stripe's status_details.paused.subscription object. */
    SubscriptionPauseReason subscription;
    /** Unix seconds when the subscription became paused. */
    std::int64_t transitioned_at_unix = 0;
    /** Source of the pause details, normally subscription. */
    std::string type;
};

/** Details of the current status. Use Subscription::status to decide whether it is paused. */
struct SubscriptionStatusDetails {
    /** Present when Stripe reports details for a paused subscription. */
    std::optional<SubscriptionPausedStatusDetails> paused;
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
    /** Explicit cancellation timestamp, absent when no date is scheduled. */
    std::optional<std::int64_t> cancel_at_unix;

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

    /** Present when collection is paused; the subscription can still be active. */
    std::optional<SubscriptionCollectionPause> pause_collection;

    /** Schedule controlling future changes, empty when absent. */
    std::string schedule_id;
    /** Discounts applied to the subscription. */
    std::vector<std::string> discount_ids;
    /** Default tax rates used by this subscription. */
    std::vector<std::string> default_tax_rate_ids;
    /** Whether Stripe calculates tax automatically. */
    bool automatic_tax_enabled = false;
    /** Why Stripe disabled automatic tax, when present. */
    std::string automatic_tax_disabled_reason;
    /** Absent when Stripe does not report the billing mode. */
    std::optional<SubscriptionBillingMode> billing_mode;
    /** charge_automatically is required for pause and resume. */
    std::string collection_method;
    /** Pause reason and transition time, when provided by Stripe. */
    std::optional<SubscriptionStatusDetails> status_details;
};

/** Stripe's explanation of a failed scheduled pause or resume. */
struct SubscriptionScheduleActionError {
    /** Machine-readable code, empty when absent. */
    std::string code;
    /** Description of the failure. */
    std::string message;
};

/** Status of one scheduled action, separate from the subscription's status. */
struct SubscriptionScheduleActionStatus {
    /** scheduled, pending, requires_action, succeeded, or error. Stripe may add other values. */
    std::string type;
    /** Details of a failed action, when supplied. */
    std::optional<SubscriptionScheduleActionError> error;
};

/** A scheduled pause and its current result. */
struct SubscriptionSchedulePause {
    /** Pause date in Unix seconds. */
    std::int64_t pause_at_unix = 0;
    /** Billing settings, absent when Stripe omits them. */
    std::optional<SubscriptionPauseSettings> settings;
    /** Result of the pause, not the subscription's current status. */
    SubscriptionScheduleActionStatus status;
};

/** A scheduled resume attempt; check subscription status before restoring service. */
struct SubscriptionScheduleResume {
    /** Resume-attempt date in Unix seconds. */
    std::int64_t resume_at_unix = 0;
    /** Billing and payment choices for the attempt. */
    std::optional<SubscriptionScheduleResumeSettings> settings;
    /** Attempt status; pending or requires_action does not restore access. */
    SubscriptionScheduleActionStatus status;
};

/** One pause schedule returned by Stripe. Keep its key when editing it. */
struct SubscriptionSchedulePauseSchedule {
    /** Pass this key when editing to retain omitted settings. */
    std::string key;
    /** Pause date, choices, and result, when reported. */
    std::optional<SubscriptionSchedulePause> pause;
    /** Planned resume attempt, absent when no resume is scheduled. */
    std::optional<SubscriptionScheduleResume> resume;
};

/** A discount reference that can be sent back when replacing schedule phases. */
struct SubscriptionScheduleDiscount {
    /** Existing Stripe discount ID, when the response provides it. */
    std::string discount_id;
    /** Coupon ID; reusing it retains the coupon's configured duration. */
    std::string coupon_id;
    /** Promotion-code ID; reusing it retains its associated coupon duration. */
    std::string promotion_code_id;
};

/** A price, quantity, and optional item-level discounts in one schedule phase. */
struct SubscriptionScheduleItem {
    std::string price_id;
    /** Absent when Stripe does not supply a quantity, for example metered usage. */
    std::optional<std::int64_t> quantity;
    /** Discounts applied only to this item. */
    std::vector<SubscriptionScheduleDiscount> discounts;
};

/** One period of scheduled subscription settings, with dates in Unix seconds. */
struct SubscriptionSchedulePhase {
    std::int64_t start_date_unix = 0;
    std::int64_t end_date_unix = 0;
    std::vector<SubscriptionScheduleItem> items;
    /** Adjustment policy reported by Stripe; unknown values are preserved. */
    std::string proration_behavior;
    std::unordered_map<std::string, std::string> metadata;
    std::optional<AutomaticTax> automatic_tax;
    std::vector<SubscriptionScheduleDiscount> discounts;
    /** Empty when inherited from schedule defaults. */
    std::string collection_method;
};

/** Start and end of the currently running schedule phase, absent after release or cancellation. */
struct SubscriptionScheduleCurrentPhase {
    std::int64_t start_date_unix = 0;
    std::int64_t end_date_unix = 0;
};

/** Future subscription settings and their lifecycle, including the subscription after release. */
struct SubscriptionSchedule {
    std::string id;
    std::string customer_id;
    /** Empty after release; use released_subscription_id to find the continuing subscription. */
    std::string subscription_id;
    std::string released_subscription_id;
    /** For example active, released, completed, canceled, or not_started. */
    std::string status;
    /** What happens after the final phase, normally release or cancel. */
    std::string end_behavior;
    std::optional<SubscriptionScheduleCurrentPhase> current_phase;
    /** Default tax behavior inherited by phases that do not override it. */
    std::optional<AutomaticTax> default_automatic_tax;
    std::vector<SubscriptionSchedulePhase> phases;
    std::unordered_map<std::string, std::string> metadata;
    /** Billing mode inherited by the subscription. */
    std::optional<SubscriptionBillingMode> billing_mode;
    /** Collection default inherited by phases without an override. */
    std::string default_collection_method;
    /** The pause schedule and each action's current result. */
    std::vector<SubscriptionSchedulePauseSchedule> pause_schedules;
};

/**
 * A Stripe invoice: the bill for one subscription period.
 *
 * A paid invoice has no outstanding balance; it does not prove that a card was
 * charged. A trial can start with a zero-value paid invoice. Decide access
 * using current subscription state, trial dates, and your application's policy.
 * An open invoice that still has
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

    /** Pre-tax amount in the smallest currency unit, when supplied. */
    std::optional<std::int64_t> subtotal;

    /** Total discount amount in the smallest currency unit, when supplied. */
    std::optional<std::int64_t> amount_discount;

    /** Total tax in the smallest currency unit, when supplied. */
    std::optional<std::int64_t> amount_tax;

    /** Final invoice total in the smallest currency unit, when supplied. */
    std::optional<std::int64_t> total;

    /** Automatic-tax state, absent when not included in the response. */
    std::optional<AutomaticTax> automatic_tax;

    /** Code explaining why finalization failed, when Stripe supplies one. */
    std::string last_finalization_error_code;

    /** Purchased prices and quantities when Stripe returns invoice lines. */
    std::vector<PurchasedLineItem> line_items;

    /** True when Stripe returned only the first page of invoice lines. */
    bool line_items_has_more = false;

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

    /** Whether automatic collection is enabled; absent when Stripe omits the setting. */
    std::optional<bool> auto_advance;
};

/** One page of invoices; use the last ID as the next cursor when has_more is true. */
struct InvoiceList {
    std::vector<Invoice> data;
    bool has_more = false;
};

/** Confirmation that Stripe deleted a draft invoice. */
struct DeletedInvoice {
    std::string id;
    bool deleted = false;
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
