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
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

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

/** When Stripe should validate a customer's tax location. */
enum class TaxLocationValidation { Automatic, Deferred, Immediately };

/** Customer fields that can be changed when correcting billing location details. */
struct UpdateCustomerRequest {
    /** Replacement email address; an empty value clears it. */
    std::optional<std::string> email;
    /** Replacement display name; an empty value clears it. */
    std::optional<std::string> name;
    /** Supplied fields replace matching billing-address fields; empty values clear them. */
    std::optional<BillingAddress> address;
    /** Optional Stripe tax-location validation policy. */
    std::optional<TaxLocationValidation> validate_tax_location;
};

/**
 * Describes the Checkout session to create.
 *
 * StripeKit creates Checkout sessions for subscriptions (`mode = "subscription"`)
 * or one-time payments (`mode = "payment"`). Price the item one of two ways:
 * set `price_id` to reuse a Price already configured in the Stripe dashboard,
 * or leave it empty and fill in `product_name`, `currency`, `unit_amount`, and
 * (for subscriptions) `recurring_interval` to define the price inline.
 *
 * @sa https://docs.stripe.com/api/checkout/sessions/create
 */
struct CreateCheckoutSessionRequest {
    /** Id of an existing Stripe customer to bill. Optional for one-time payments. */
    std::string customer_id;

    /**
     * Mode of the Checkout session: "subscription" or "payment".
     *
     * Defaults to "subscription". Use "payment" for one-time purchases.
     */
    std::string mode = "subscription";

    /** Id of an existing Stripe Price. Leave empty to price the line item inline. */
    std::string price_id;

    /** Product name, for inline pricing. Ignored when `price_id` is set. */
    std::string product_name;

    /** ISO currency code for inline pricing, for example "usd". */
    std::string currency = "usd";

    /**
     * Amount per unit for inline pricing, in the currency's smallest unit.
     *
     * That means cents for USD, so 1499 is $14.99.
     */
    std::int64_t unit_amount = 0;

    /**
     * Billing interval for inline pricing in subscription mode: "day", "week", "month", or "year".
     *
     * Ignored when `mode` is "payment".
     */
    std::string recurring_interval = "month";

    /**
     * Where Stripe sends the customer's browser after a successful checkout.
     *
     * Reaching this URL is not proof of payment; the customer may close the tab
     * first, and anyone can visit the URL directly. Use verified webhooks and
     * current subscription state to decide access, including trial eligibility.
     */
    std::string success_url;

    /** Where Stripe sends the customer's browser if they abandon checkout. */
    std::string cancel_url;

    /** Number of units of the item to bill for. */
    std::int64_t quantity = 1;

    /** Free-form key/value pairs to store on the session. */
    std::unordered_map<std::string, std::string> metadata;

    /** Email for a new customer; do not combine with customer_id. */
    std::string customer_email;

    /**
     * Subscription trial duration, from 1 to 730 days; absent means no trial.
     *
     * Only valid in subscription mode. For a card-required trial, also set
     * payment_method_collection to "always" and payment_method_types to {"card"}.
     */
    std::optional<std::int64_t> trial_period_days;

    /** Metadata on the resulting subscription, included in subscription webhooks. */
    std::unordered_map<std::string, std::string> subscription_metadata;

    /**
     * "always" or "if_required"; empty uses Stripe's default.
     *
     * The documented trial flow requires a card. Cardless trials also need a
     * missing-payment-method policy at trial end; StripeKit does not expose
     * trial_settings.end_behavior for configuring that policy.
     */
    std::string payment_method_collection;

    /**
     * Allowed payment methods; empty uses the Stripe account's defaults.
     *
     * Stripe filters out methods incompatible with the payment.
     */
    std::vector<std::string> payment_method_types;

    /** Session expiry as Unix seconds; absent uses Stripe's default. */
    std::optional<std::int64_t> expires_at;

    /** Enable or disable Stripe's automatic tax calculation; unset preserves Stripe defaults. */
    std::optional<bool> automatic_tax_enabled;

    /** "auto" or "required"; empty preserves Stripe's default collection behavior. */
    std::string billing_address_collection;

    /** Request Checkout to save a collected address or name for an existing Customer. */
    std::optional<bool> customer_update_address;
    std::optional<bool> customer_update_name;

    /** Collect an optional business tax ID in hosted Checkout when set. */
    std::optional<bool> tax_id_collection_enabled;
    /** Billing mode for subscriptions created by Checkout; flexible allows true pauses. */
    std::optional<SubscriptionBillingMode> subscription_billing_mode;
};

/** Which task a billing portal session should take the customer straight to. */
enum class BillingPortalFlowType {
    PaymentMethodUpdate,      /**< Add or replace the customer's payment method. */
    SubscriptionCancel,       /**< Confirm cancelling one subscription. */
    SubscriptionUpdate,       /**< Pick a new plan for one subscription. */
    SubscriptionUpdateConfirm /**< Review and confirm a plan change chosen by your app. */
};

/** Where the customer goes after finishing a billing portal flow. */
enum class BillingPortalAfterCompletion {
    PortalHomepage,     /**< Stay in the portal and show its home page. */
    HostedConfirmation, /**< Show a Stripe-hosted confirmation, optionally with a message. */
    Redirect            /**< Send the customer back to your app at `redirect_url`. */
};

/** The new price or quantity for the subscription item in a confirm flow. */
struct BillingPortalFlowItem {
    /** Existing subscription item ID, of the form "si_...". */
    std::string item_id;

    /**
     * Price to switch the item to.
     *
     * The price must be listed in the portal configuration's plan-switching
     * products. Leave empty to change only the quantity.
     */
    std::string price_id;

    /** New quantity, or empty to keep the current one. */
    std::optional<std::int64_t> quantity;
};

/**
 * Sends the customer straight to one task in the billing portal, instead of
 * the portal's home page.
 *
 * Use SubscriptionUpdateConfirm when your own app lets the customer pick a
 * plan: the portal then only shows the price change and asks them to confirm
 * and pay. Stripe can only confirm a change for a subscription with a single
 * item.
 *
 * @sa https://docs.stripe.com/customer-management/portal-deep-links
 */
struct BillingPortalFlow {
    /** The task to show. */
    BillingPortalFlowType type = BillingPortalFlowType::PaymentMethodUpdate;

    /** Subscription to cancel or change. Required for every type except PaymentMethodUpdate. */
    std::string subscription_id;

    /** The item change to confirm. Required, with exactly one entry, for SubscriptionUpdateConfirm.
     */
    std::vector<BillingPortalFlowItem> items;

    /** What happens after the customer finishes, or empty for Stripe's default. */
    std::optional<BillingPortalAfterCompletion> after_completion;

    /** Where to send the customer afterwards. Required when after_completion is Redirect. */
    std::string redirect_url;

    /** Optional message of up to 500 characters for a HostedConfirmation. */
    std::string confirmation_message;
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

    /**
     * Portal configuration to use, of the form "bpc_...".
     *
     * Leave empty to use the account's default configuration from the Stripe
     * dashboard.
     */
    std::string configuration_id;

    /**
     * Language to show the portal in, as a Stripe locale such as "en", "nl",
     * or "pt-BR".
     *
     * Leave empty to let Stripe choose from the customer's preferences or
     * browser.
     */
    std::string locale;

    /** Task to send the customer straight to, or empty to open the portal's home page. */
    std::optional<BillingPortalFlow> flow;
};

/**
 * Describes a new billing portal configuration.
 *
 * Every feature is sent to Stripe explicitly. By default, invoice history and
 * payment method updates are on, while cancelling, plan changes, and editing
 * customer details are off until you turn them on.
 *
 * @sa https://docs.stripe.com/api/customer_portal/configurations/create
 */
struct CreateBillingPortalConfigurationRequest {
    /** Features the portal offers. */
    BillingPortalFeatures features;

    /** Internal name of up to 256 characters, shown only in the Stripe dashboard. */
    std::string name;

    /** Optional heading shown at the top of the portal. */
    std::string headline;

    /** Where to send customers when a session does not give its own return URL. */
    std::string default_return_url;

    /** Free-form key/value pairs to store on the configuration. */
    std::unordered_map<std::string, std::string> metadata;
};

/**
 * Describes changes to a billing portal configuration.
 *
 * Only fields that are set are changed. Setting `features` replaces every
 * feature setting, as with creation.
 *
 * @sa https://docs.stripe.com/api/customer_portal/configurations/update
 */
struct UpdateBillingPortalConfigurationRequest {
    /** Set to false to stop new sessions from using the configuration. */
    std::optional<bool> active;

    /** New feature settings. */
    std::optional<BillingPortalFeatures> features;

    /** New internal name. */
    std::optional<std::string> name;

    /** New heading. An empty string removes it. */
    std::optional<std::string> headline;

    /** New default return URL. An empty string removes it. */
    std::optional<std::string> default_return_url;

    /** Metadata keys to add or replace. Existing keys not listed are kept. */
    std::unordered_map<std::string, std::string> metadata;
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

/** Inputs for creating a reusable Stripe product. */
struct CreateProductRequest {
    /** Optional caller-assigned ID, useful for recovering interrupted creation. */
    std::string id;
    /** Required customer-facing name. */
    std::string name;
    /** Optional customer-facing description. */
    std::string description;
    /** Whether new purchases can use the product. */
    bool active = true;
    /** Application-defined attributes. */
    std::unordered_map<std::string, std::string> metadata;
};

/** Product changes. Unset fields are left unchanged. */
struct UpdateProductRequest {
    /** Replacement name; must not be empty if supplied. */
    std::optional<std::string> name;
    /** Replacement description; an empty string clears it. */
    std::optional<std::string> description;
    /** Whether new purchases can use the product. */
    std::optional<bool> active;
    /** Metadata changes; empty values remove individual keys. */
    std::unordered_map<std::string, std::string> metadata;
    /** Active Price ID belonging to this product. Does not reprice existing subscriptions. */
    std::optional<std::string> default_price_id;
};

/** Mutable price details. Amount, currency, and recurrence cannot be changed. */
struct UpdatePriceRequest {
    /** Set false to archive; existing subscriptions are unaffected. */
    std::optional<bool> active;
    /** Internal description; an empty string clears it. */
    std::optional<std::string> nickname;
    /** Replacement lookup key; an empty string clears it. */
    std::optional<std::string> lookup_key;
    /** Move the supplied nonempty lookup key from another price atomically. */
    std::optional<bool> transfer_lookup_key;
    /** Metadata changes; empty values remove individual keys. */
    std::unordered_map<std::string, std::string> metadata;
};

/** How a subscription change accounts for the unused portion of a billing period. */
enum class ProrationBehavior {
    CreateProrations, /**< Create adjustments, without necessarily invoicing immediately. */
    AlwaysInvoice,    /**< Create and immediately invoice adjustments. */
    None              /**< Do not create adjustments; a change can still trigger billing. */
};

/** What happens when an automatically charged subscription update requires payment. */
enum class PaymentBehavior {
    AllowIncomplete,     /**< Apply changes even if payment fails; follow past_due recovery. */
    PendingIfIncomplete, /**< Apply changes only after payment; inspect pending_update. */
    ErrorIfIncomplete,   /**< Reject changes if payment cannot complete. */
    DefaultIncomplete    /**< Require explicit confirmation of the invoice's payment intent. */
};

/** Replace the price of an existing licensed subscription item, not add another item. */
struct UpdateSubscriptionItemRequest {
    /** Existing subscription item ID (si_...), not the subscription or price ID. */
    std::string item_id;
    /** Replacement recurring Price ID. */
    std::string price_id;
    /** Required positive quantity. Copy the existing quantity to preserve it. */
    std::int64_t quantity = 0;
};

/** Named values accepted by Stripe's subscription cancel_at parameter. */
enum class SubscriptionCancelAt {
    Clear,          ///< Remove an explicitly scheduled cancellation date.
    MinPeriodEnd,   ///< Earliest current period end across the subscription's items.
    MaxPeriodEnd,   ///< Latest current period end across the subscription's items.
    MaxBilledUntil, ///< Latest date through which the subscription's items have been billed.
};

/** Fields to update on an existing subscription; omitted fields are not sent to Stripe. */
struct UpdateSubscriptionRequest {
    /** Existing items to reprice, at most twenty; other items remain unchanged. */
    std::vector<UpdateSubscriptionItemRequest> items;
    /** Optional adjustment policy; omission uses Stripe's default. */
    std::optional<ProrationBehavior> proration_behavior;
    /** Optional payment-failure policy; omission uses Stripe's default. */
    std::optional<PaymentBehavior> payment_behavior;
    /** Enable or disable automatic tax. */
    std::optional<bool> automatic_tax_enabled;
    /** Positive Unix timestamp or a named cancellation boundary; Clear sends an empty value. */
    std::optional<std::variant<std::int64_t, SubscriptionCancelAt>> cancel_at;
    /** Set or clear cancellation at period end; omission leaves the setting unchanged. */
    std::optional<bool> cancel_at_period_end;
    /** Metadata entries to update; empty values remove keys, and omitted keys are unchanged. */
    std::unordered_map<std::string, std::string> metadata;
    /** charge_automatically or send_invoice. Pausing and resuming require charge_automatically. */
    std::optional<std::string> collection_method;
};

/** Create a fixed per-unit price. Does not change existing subscriptions. */
struct CreatePriceRequest {
    /** Existing Stripe product ID. */
    std::string product_id;
    /** Lower-case three-letter currency code, such as usd. */
    std::string currency;
    /** Non-negative amount in the currency's smallest unit. */
    std::int64_t unit_amount = 0;
    /** Empty for one-time pricing; otherwise day, week, month, or year. */
    std::string recurring_interval;
    /** Number of intervals between bills; positive and 1 for one-time prices. */
    std::int64_t recurring_interval_count = 1;
    /** Optional unique lookup key for finding a price after an interrupted request. */
    std::string lookup_key;
    /** inclusive, exclusive, or unspecified; empty uses Stripe's default. */
    std::string tax_behavior;
    /** Application-defined attributes. */
    std::unordered_map<std::string, std::string> metadata;
};

/** Price filters and pagination. No automatic pagination or hidden retries. */
struct ListPricesRequest {
    /** Optional product ID filter. */
    std::string product_id;
    /** Optional availability filter; unset includes active and inactive prices. */
    std::optional<bool> active;
    /** Optional exact lookup keys; Stripe accepts at most ten. */
    std::vector<std::string> lookup_keys;
    /** Page size from 1 to 100. */
    std::int64_t limit = 10;
    /** Return the page after this price ID. Empty starts at the first page. */
    std::string starting_after;
};

/**
 * Create, retrieve, and update catalog products through StripeClient::products().
 * Failures propagate as StripeApiException, transport errors, or ConfigurationException.
 * Callbacks run exactly once and may run on another thread.
 */
class Products {
public:
    /** Callback receiving a product or an exception. */
    using Completion = std::function<void(CallbackResult<Product>)>;
    /** Destroy the operation group. */
    virtual ~Products() = default;
    /**
     * Create a product. Use an idempotency key for safe retries.
     *
     * @param request Product details.
     * @param options Request settings.
     *
     * @return Created product.
     */
    virtual Task<Product> create_async(const CreateProductRequest& request,
                                       const RequestOptions& options = {}) = 0;
    /**
     * Create a product, reporting the result through a callback.
     *
     * @param request Product details.
     * @param options Request settings.
     * @param completion Receives the created product or the failure exactly once.
     */
    virtual void create(const CreateProductRequest& request,
                        const RequestOptions& options,
                        Completion completion) = 0;
    /**
     * Retrieve a product by ID; deleted products have deleted=true.
     *
     * @param product_id Existing Stripe product ID.
     * @param options Request settings.
     *
     * @return The product.
     */
    virtual Task<Product> retrieve_async(std::string product_id,
                                         const RequestOptions& options = {}) = 0;
    /**
     * Retrieve a product, reporting the result through a callback.
     *
     * @param product_id Existing Stripe product ID.
     * @param options Request settings.
     * @param completion Receives the product or the failure exactly once.
     */
    virtual void
    retrieve(std::string product_id, const RequestOptions& options, Completion completion) = 0;
    /**
     * Update display details, availability, or default price, not existing subscriptions.
     *
     * @param product_id Existing Stripe product ID.
     * @param request Supplied fields to change.
     * @param options Request settings.
     *
     * @return The updated product.
     */
    virtual Task<Product> update_async(std::string product_id,
                                       const UpdateProductRequest& request,
                                       const RequestOptions& options = {}) = 0;
    /**
     * Update a product, reporting the result through a callback.
     *
     * @param product_id Existing Stripe product ID.
     * @param request Supplied fields to change.
     * @param options Request settings.
     * @param completion Receives the updated product or the failure exactly once.
     */
    virtual void update(std::string product_id,
                        const UpdateProductRequest& request,
                        const RequestOptions& options,
                        Completion completion) = 0;
};

/**
 * Create, inspect, and update mutable price details through StripeClient::prices().
 * Creation supports fixed amounts and licensed recurrence, not tiered, metered,
 * or custom amounts. Retrieval exposes those distinctions for safe validation.
 * Failures propagate as StripeApiException, transport errors, or ConfigurationException.
 * Callbacks run exactly once and may run on another thread.
 */
class Prices {
public:
    /** Callback receiving a price or an exception. */
    using Completion = std::function<void(CallbackResult<Price>)>;
    /** Callback receiving one page or an exception. */
    using ListCompletion = std::function<void(CallbackResult<PriceList>)>;
    /** Destroy the operation group. */
    virtual ~Prices() = default;
    /**
     * Create a new price; does not replace prices on existing subscriptions.
     *
     * @param request Price details.
     * @param options Request settings.
     *
     * @return The created price.
     */
    virtual Task<Price> create_async(const CreatePriceRequest& request,
                                     const RequestOptions& options = {}) = 0;
    /**
     * Create a price, reporting the result through a callback.
     *
     * @param request Price details.
     * @param options Request settings.
     * @param completion Receives the created price or the failure exactly once.
     */
    virtual void create(const CreatePriceRequest& request,
                        const RequestOptions& options,
                        Completion completion) = 0;
    /**
     * Update mutable details or archive a price; core pricing terms remain unchanged.
     * @param price_id Existing Price ID.
     * @param request Supplied fields to change.
     * @param options Per-request settings.
     * @return Updated price.
     */
    virtual Task<Price> update_async(std::string price_id,
                                     const UpdatePriceRequest& request,
                                     const RequestOptions& options = {}) = 0;
    /**
     * Update a price, reporting validation, transport, and API failures through completion.
     * @param price_id Existing Price ID.
     * @param request Supplied fields to change.
     * @param options Per-request settings.
     * @param completion Receives the updated price or failure exactly once.
     */
    virtual void update(std::string price_id,
                        const UpdatePriceRequest& request,
                        const RequestOptions& options,
                        Completion completion) = 0;
    /**
     * Retrieve an active or archived price by its ID.
     *
     * @param price_id Existing Price ID.
     * @param options Request settings.
     *
     * @return The price.
     */
    virtual Task<Price> retrieve_async(std::string price_id,
                                       const RequestOptions& options = {}) = 0;
    /**
     * Retrieve a price, reporting the result through a callback.
     *
     * @param price_id Existing Price ID.
     * @param options Request settings.
     * @param completion Receives the price or the failure exactly once.
     */
    virtual void
    retrieve(std::string price_id, const RequestOptions& options, Completion completion) = 0;
    /**
     * Retrieve one filtered page, including lookup-key results.
     *
     * @param request Filters and pagination settings.
     * @param options Request settings.
     *
     * @return One page of matching prices.
     */
    virtual Task<PriceList> list_async(const ListPricesRequest& request = {},
                                       const RequestOptions& options = {}) = 0;
    /**
     * List one page, reporting the result through a callback.
     *
     * @param request Filters and pagination settings.
     * @param options Request settings.
     * @param completion Receives the page or the failure exactly once.
     */
    virtual void list(const ListPricesRequest& request,
                      const RequestOptions& options,
                      ListCompletion completion) = 0;
};

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

    /**
     * Update customer contact or billing-address details.
     *
     * @param customer_id Existing Stripe customer ID.
     * @param request Fields to change; omitted fields remain unchanged.
     * @param options Per-request settings.
     *
     * @return Updated customer, including Stripe's tax-location status when available.
     *
     * @throws ConfigurationException If the ID is empty or no fields are supplied.
     * @throws StripeApiException If Stripe rejects the change.
     */
    virtual Task<Customer> update_async(std::string customer_id,
                                        const UpdateCustomerRequest& request,
                                        const RequestOptions& options = {}) = 0;

    /**
     * Update customer fields and report the result or failure exactly once.
     *
     * @param customer_id Existing Stripe customer ID.
     * @param request Fields to change.
     * @param options Per-request settings.
     * @param completion Receives the updated customer or failure.
     */
    virtual void update(std::string customer_id,
                        const UpdateCustomerRequest& request,
                        const RequestOptions& options,
                        RetrieveCompletion completion) = 0;
};

/** Filters for one page of Checkout sessions. No automatic pagination. */
struct ListCheckoutSessionsRequest {
    std::string payment_intent_id; ///< Optional payment intent to find its Checkout session.
    std::int64_t limit = 10;       ///< Page size, from 1 to 100.
    std::string starting_after;    ///< Last session ID from the previous page; empty for the first.
};

/** Page size and cursor for purchased line items. */
struct ListPurchasedLineItemsRequest {
    std::int64_t limit = 10;    ///< Page size, from 1 to 100.
    std::string starting_after; ///< Last line-item ID from the previous page.
};

/**
 * Create, retrieve, expire, and list Stripe-hosted Checkout sessions.
 *
 * Reach an instance through StripeClient::checkout(). Checkout is a payment
 * page Stripe hosts: create a session, redirect the customer to
 * CheckoutSession::url, and Stripe collects the card details.
 *
 * @sa https://docs.stripe.com/api/checkout/sessions
 */
class CheckoutSessions {
public:
    /** Receives a created or expired session, or the request failure. */
    using CreateCompletion = std::function<void(CallbackResult<CheckoutSession>)>;
    /** Receives one page of sessions, or the request failure. */
    using ListCompletion = std::function<void(CallbackResult<CheckoutSessionList>)>;
    using LineItemsCompletion = std::function<void(CallbackResult<PurchasedLineItemList>)>;

    /**
     * Expire an open session so it can no longer be completed.
     * Stripe rejects sessions that cannot be expired, including completed sessions.
     *
     * @param session_id Existing Checkout session ID.
     * @param options Request timeout and optional idempotency key.
     *
     * @return The expired session; verify its status before treating expiry as confirmed.
     *
     * @throws ConfigurationException If the session ID is empty.
     * @throws StripeApiException If Stripe rejects expiry.
     */
    virtual Task<CheckoutSession> expire_async(std::string session_id,
                                               const RequestOptions& options = {}) = 0;
    /**
     * Expire a session and report its result or failure exactly once.
     *
     * @param session_id Existing Checkout session ID.
     * @param options Per-request settings.
     * @param completion Receives the expired session or failure.
     */
    virtual void
    expire(std::string session_id, const RequestOptions& options, CreateCompletion completion) = 0;
    /**
     * Retrieve one page of sessions, optionally filtered by payment intent.
     *
     * @param request Filter, page size, and optional cursor.
     * @param options Per-request settings.
     *
     * @return Sessions and whether another page exists.
     *
     * @throws ConfigurationException If the page size is outside 1 through 100.
     * @throws StripeApiException If Stripe rejects the request.
     */
    virtual Task<CheckoutSessionList> list_async(const ListCheckoutSessionsRequest& request = {},
                                                 const RequestOptions& options = {}) = 0;
    /**
     * Retrieve one page and report its result or failure exactly once.
     *
     * @param request Filter and pagination settings.
     * @param options Per-request settings.
     * @param completion Receives the page or failure.
     */
    virtual void list(const ListCheckoutSessionsRequest& request,
                      const RequestOptions& options,
                      ListCompletion completion) = 0;

    /**
     * Read one page of prices and quantities purchased in a Checkout session.
     *
     * @param session_id Existing Checkout session ID.
     * @param request Page size and cursor.
     * @param options Per-request settings.
     *
     * @return One page of line items; further pages are not fetched automatically.
     */
    virtual Task<PurchasedLineItemList>
    list_line_items_async(std::string session_id,
                          const ListPurchasedLineItemsRequest& request = {},
                          const RequestOptions& options = {}) = 0;

    /**
     * Read Checkout line items and report the page or failure exactly once.
     *
     * @param session_id Existing Checkout session ID.
     * @param request Page size and cursor.
     * @param options Per-request settings.
     * @param completion Receives the page or failure.
     */
    virtual void list_line_items(std::string session_id,
                                 const ListPurchasedLineItemsRequest& request,
                                 const RequestOptions& options,
                                 LineItemsCompletion completion) = 0;

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

    /**
     * Retrieves a Checkout Session so callers can check whether it is open,
     * complete, or expired.
     *
     * @param session_id Stripe session identifier.
     * @param options Per-request settings.
     *
     * @return Current session state. Completion alone is not proof of payment.
     *
     * @throws StripeApiException If Stripe rejects the request.
     */
    virtual Task<CheckoutSession> retrieve_async(std::string session_id,
                                                 const RequestOptions& options = {}) = 0;
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

/** What Stripe should do when a subscription schedule reaches its final phase. */
enum class SubscriptionScheduleEndBehavior { Release, Cancel };

/** Calendar duration from a scheduled pause to its resume attempt. */
struct SubscriptionPauseDuration {
    /** day, week, month, or year. */
    std::string interval;
    /** Positive number of calendar intervals. */
    std::int64_t interval_count = 1;
};

/** When a scheduled action runs. A pause accepts now or timestamp; a resume also accepts duration.
 */
struct SubscriptionPauseTime {
    /** now, timestamp, or (for resumes only) duration. */
    std::string type;
    /** Positive Unix seconds; required only for type timestamp. */
    std::optional<std::int64_t> timestamp;
    /** Required only for type duration. */
    std::optional<SubscriptionPauseDuration> duration;
};

/** Scheduled pause changes. Supply the existing key to keep settings you leave unset. */
struct SubscriptionSchedulePauseRequest {
    /** Pause time; leave unset to keep the existing date when using the same key. */
    std::optional<SubscriptionPauseTime> pause_at;
    /** Optional credits, usage, and invoicing choices. */
    std::optional<SubscriptionPauseSettings> settings;
};

/** Scheduled resume changes. Supply the existing key to keep settings you leave unset. */
struct SubscriptionScheduleResumeRequest {
    /** Resume time; leave unset to keep the existing date when using the same key. */
    std::optional<SubscriptionPauseTime> resume_at;
    /** Optional billing and payment choices. */
    std::optional<SubscriptionScheduleResumeSettings> settings;
};

/** Add or edit a pause schedule. Include key to preserve existing settings and status. */
struct SubscriptionSchedulePauseScheduleRequest {
    /** Existing key to edit, or empty to replace the pause schedule with a new one. */
    std::string key;
    /** Omit when scheduling a resume for an already-paused subscription. */
    std::optional<SubscriptionSchedulePauseRequest> pause;
    /** Leave unset for a new pause without a resume, or to keep a resume with the same key. */
    std::optional<SubscriptionScheduleResumeRequest> resume;
    /** Remove the planned resume. Supply the existing key and leave resume unset. */
    bool clear_resume = false;
};

/** Create a schedule for an existing subscription, copying its current billing settings. */
struct CreateSubscriptionScheduleRequest {
    /** Existing subscription ID. Update an attached schedule instead of creating another. */
    std::string from_subscription;
    /** At most one pause schedule, applied as part of creation. */
    std::vector<SubscriptionSchedulePauseScheduleRequest> pause_schedules;
};

/** How long a schedule phase lasts, independently of its prices' billing intervals. */
struct SubscriptionSchedulePhaseDuration {
    /** "day", "week", "month", or "year". */
    std::string interval;
    /** Positive number of intervals. */
    std::int64_t interval_count = 1;
};

/** A phase to save; specify either an end date or a duration. */
struct SubscriptionSchedulePhaseRequest {
    std::int64_t start_date = 0;
    std::optional<std::int64_t> end_date;
    /** Phase length when no explicit end date is supplied. */
    std::optional<SubscriptionSchedulePhaseDuration> duration;
    std::vector<SubscriptionScheduleItem> items;
    ProrationBehavior proration_behavior = ProrationBehavior::None;
    std::unordered_map<std::string, std::string> metadata;
    /** Optional automatic-tax override for this phase. */
    std::optional<bool> automatic_tax_enabled;
    /** Discounts to retain or apply for this phase. */
    std::vector<SubscriptionScheduleDiscount> discounts;
    /** Optional override; scheduled pauses require charge_automatically in every phase. */
    std::optional<std::string> collection_method;
};

/** Update phases or pause schedules. Leave phases empty for pause-only changes. */
struct UpdateSubscriptionScheduleRequest {
    std::vector<SubscriptionSchedulePhaseRequest> phases;
    /** Defaults to Release for phase replacements; otherwise omitted. */
    std::optional<SubscriptionScheduleEndBehavior> end_behavior;
    /** Defaults to None for phase replacements; otherwise omitted. */
    std::optional<ProrationBehavior> proration_behavior;
    std::unordered_map<std::string, std::string> metadata;
    /** Default automatic-tax setting for the schedule. */
    std::optional<bool> default_automatic_tax_enabled;
    /** Leave unset to keep pause settings, or supply an empty vector to remove them. */
    std::optional<std::vector<SubscriptionSchedulePauseScheduleRequest>> pause_schedules;
    /** charge_automatically or send_invoice; unset leaves the default unchanged. */
    std::optional<std::string> default_collection_method;
};

/** Create, read, and change future billing settings through StripeClient::subscription_schedules().
 */
class SubscriptionSchedules {
public:
    using Completion = std::function<void(CallbackResult<SubscriptionSchedule>)>;
    /** Destroy this operations instance. */
    virtual ~SubscriptionSchedules() = default;
    /**
     * Read a schedule and its current, future, or released state.
     *
     * @param schedule_id Existing schedule ID.
     * @param options Per-request headers and timeout.
     *
     * @return Schedule with phase dates, prices, quantities, discounts, and tax settings.
     *
     * @throws ConfigurationException If the ID is empty or the response is malformed.
     * @throws StripeApiException If Stripe rejects the request.
     */
    virtual Task<SubscriptionSchedule> retrieve_async(std::string schedule_id,
                                                      const RequestOptions& options = {}) = 0;
    /**
     * Read a schedule and report its result or failure exactly once.
     *
     * @param schedule_id Existing schedule ID.
     * @param options Per-request settings.
     * @param completion Receives the schedule or failure.
     */
    virtual void
    retrieve(std::string schedule_id, const RequestOptions& options, Completion completion) = 0;
    /**
     * Create a schedule from an existing subscription, preserving its current settings.
     *
     * @param request Existing subscription ID and optional pause and resume settings.
     * @param options Per-request settings; use a saved idempotency key for retries.
     *
     * @return Schedule linked to the subscription.
     *
     * @throws ConfigurationException If the subscription ID is empty.
     * @throws StripeApiException If Stripe rejects the request, including an already scheduled
     * subscription.
     */
    virtual Task<SubscriptionSchedule>
    create_async(const CreateSubscriptionScheduleRequest& request,
                 const RequestOptions& options = {}) = 0;
    /**
     * Create a schedule and report its result or failure exactly once.
     *
     * @param request Existing subscription ID and optional pause and resume settings.
     * @param options Per-request settings.
     * @param completion Receives the schedule or failure.
     */
    virtual void create(const CreateSubscriptionScheduleRequest& request,
                        const RequestOptions& options,
                        Completion completion) = 0;
    /**
     * Update pause schedules or replace current and future phases.
     *
     * Supplied phases replace current and future phases: include every setting you intend to
     * retain. Include the existing automatic-tax and discount settings when replacing phases.
     * Release keeps the subscription running after the final phase; cancel ends it.
     * For pause-only changes, leave phases and billing policies unset to retain them.
     *
     * @param schedule_id Existing schedule ID.
     * @param request Pause changes or one through ten replacement phases and billing policies.
     * @param options Per-request settings; use a saved idempotency key for retries.
     *
     * @return Updated schedule; inspect its phases to confirm the intended change.
     *
     * @throws ConfigurationException If dates, items, policies, or quantities are invalid.
     * @throws StripeApiException If Stripe rejects the change.
     */
    virtual Task<SubscriptionSchedule>
    update_async(std::string schedule_id,
                 const UpdateSubscriptionScheduleRequest& request,
                 const RequestOptions& options = {}) = 0;
    /**
     * Update a schedule and report its result or failure exactly once.
     *
     * @param schedule_id Existing schedule ID.
     * @param request Pause changes or replacement phases and billing settings.
     * @param options Per-request settings.
     * @param completion Receives the schedule or failure.
     */
    virtual void update(std::string schedule_id,
                        const UpdateSubscriptionScheduleRequest& request,
                        const RequestOptions& options,
                        Completion completion) = 0;
};

/** Settings for invoices created during a collection pause; existing invoices are unchanged. */
struct PauseSubscriptionCollectionRequest {
    CollectionPauseBehavior behavior = CollectionPauseBehavior::Void;
    std::optional<std::int64_t>
        resumes_at; ///< Positive Unix timestamp; absent pauses until explicitly resumed.
};

/** Pause service and invoice generation, not merely payment collection. */
using PauseSubscriptionRequest = SubscriptionPauseSettings;

/** Billing cycle anchor for a direct resume. */
struct ResumeSubscriptionBillingCycleAnchor {
    /** now or unchanged. */
    std::string type;
};

/** Resume a paused subscription. Leave fields unset to use Stripe's defaults. */
struct ResumeSubscriptionRequest {
    /** now (Stripe default) or unchanged. */
    std::optional<ResumeSubscriptionBillingCycleAnchor> billing_cycle_anchor;
    /** resume_on_payment_attempt (Stripe default) or resume_on_payment_success. */
    std::string payment_behavior;
    /** Adjustment policy; omission uses create_prorations. */
    std::optional<ProrationBehavior> proration_behavior;
    /** Positive Unix seconds at which to calculate adjustments. */
    std::optional<std::int64_t> proration_date;
};

/** Upgrade a classic-billing subscription to flexible billing mode. */
struct MigrateSubscriptionRequest {
    /** Must be flexible; switching back to classic is not supported. */
    SubscriptionBillingMode billing_mode{ "flexible" };
};

/** Extra fields to request when retrieving a subscription. */
struct RetrieveSubscriptionRequest {
    /** Field paths, such as items.data.billed_until. Leave empty for a normal retrieval. */
    std::vector<std::string> expand;
};

/** Filters and pagination for invoice items, including charges and credits. */
struct ListInvoiceItemsRequest {
    /** Customer to list items for; empty lists all customers. */
    std::string customer_id;
    /** Invoice to list items for; empty includes items not yet on an invoice. */
    std::string invoice_id;
    /** True for pending items, false for items on invoices, or unset for both. */
    std::optional<bool> pending;
    /** Page size, from 1 to 100. */
    std::int64_t limit = 10;
    /** Last item ID from the previous page; empty starts at the newest item. */
    std::string starting_after;
};

/** Filters for one invoice page. Empty subscription_id lists all invoices. */
struct ListInvoicesRequest {
    std::string subscription_id;
    std::int64_t limit = 10;    ///< Page size, from 1 to 100.
    std::string starting_after; ///< Last invoice ID from the previous page.
};

/** Draft invoice fields that can be corrected before finalization. */
struct UpdateInvoiceRequest {
    /** Enable or disable automatic tax before finalization. */
    std::optional<bool> automatic_tax_enabled;
    /** Whether Stripe should continue collection after finalization. */
    std::optional<bool> auto_advance;
};

/**
 * Read subscriptions and manage billing, pauses, and cancellations.
 *
 * Reach an instance through StripeClient::subscriptions(). Subscriptions are
 * created by Checkout rather than here. Application access decisions remain
 * separate from Stripe's billing and collection state.
 *
 * @sa https://docs.stripe.com/api/subscriptions
 */
class Subscriptions {
public:
    /**
     * Retrieve a subscription with optional expanded fields.
     * Use items.data.billed_until to request each item's billed-through date.
     * A missing date is std::nullopt. Pass {} for options to use default request settings.
     *
     * @param subscription_id Existing subscription ID.
     * @param request Fields to expand, such as items.data.billed_until.
     * @param options Per-request headers and timeout.
     * @return Current subscription state.
     * @throws ConfigurationException If the ID or an expansion is empty, or the response is
     * malformed.
     * @throws StripeApiException If Stripe rejects the request.
     */
    virtual Task<Subscription> retrieve_async(std::string subscription_id,
                                              const RetrieveSubscriptionRequest& request,
                                              const RequestOptions& options) = 0;
    /**
     * Retrieve expanded fields and deliver the subscription or failure exactly once.
     *
     * @param subscription_id Existing subscription ID.
     * @param request Extra fields to request, such as items.data.billed_until.
     * @param options Per-request headers and timeout.
     * @param completion Receives the subscription or failure; may run on another thread.
     */
    virtual void retrieve(std::string subscription_id,
                          const RetrieveSubscriptionRequest& request,
                          const RequestOptions& options,
                          std::function<void(CallbackResult<Subscription>)> completion) = 0;
    /**
     * Pause service and invoice generation immediately, unlike pause_collection().
     * Requires flexible billing and charge_automatically. For an attached schedule,
     * update its pause_schedules instead.
     *
     * @param subscription_id Existing subscription ID.
     * @param request Pause credits, metered usage, and invoicing choices.
     * @param options Per-request headers, timeout, and retry key.
     * @return Subscription state returned by Stripe.
     * @throws ConfigurationException If the ID or pause settings are invalid.
     * @throws StripeApiException If Stripe rejects the pause.
     */
    virtual Task<Subscription> pause_async(std::string subscription_id,
                                           const PauseSubscriptionRequest& request = {},
                                           const RequestOptions& options = {}) = 0;
    /**
     * Pause and deliver the returned subscription or failure exactly once.
     * @param subscription_id Existing subscription ID.
     * @param request Pause billing choices.
     * @param options Per-request settings.
     * @param completion Receives the subscription or failure.
     */
    virtual void pause(std::string subscription_id,
                       const PauseSubscriptionRequest& request,
                       const RequestOptions& options,
                       std::function<void(CallbackResult<Subscription>)> completion) = 0;
    /**
     * Start a resume attempt. Requires charge_automatically; update an attached schedule instead.
     * A successful request can remain paused awaiting payment: inspect status,
     * pending_update, and latest_invoice_id before restoring access.
     *
     * @param subscription_id Existing subscription ID.
     * @param request Billing anchor, payment behavior, and proration choices.
     * @param options Per-request headers, timeout, and retry key.
     * @return Subscription state, possibly still paused awaiting payment.
     */
    virtual Task<Subscription> resume_async(std::string subscription_id,
                                            const ResumeSubscriptionRequest& request = {},
                                            const RequestOptions& options = {}) = 0;
    /**
     * Resume and deliver the returned subscription or failure exactly once.
     * @param subscription_id Existing subscription ID.
     * @param request Resume billing and payment choices.
     * @param options Per-request settings.
     * @param completion Receives the subscription or failure.
     */
    virtual void resume(std::string subscription_id,
                        const ResumeSubscriptionRequest& request,
                        const RequestOptions& options,
                        std::function<void(CallbackResult<Subscription>)> completion) = 0;
    /**
     * Upgrade to flexible billing mode. Stripe checks whether the upgrade is allowed.
     * @param subscription_id Existing subscription ID.
     * @param request Target flexible billing mode.
     * @param options Per-request headers, timeout, and retry key.
     * @return Subscription state after migration.
     */
    virtual Task<Subscription> migrate_async(std::string subscription_id,
                                             const MigrateSubscriptionRequest& request = {},
                                             const RequestOptions& options = {}) = 0;
    /**
     * Migrate and deliver the returned subscription or failure exactly once.
     * @param subscription_id Existing subscription ID.
     * @param request Target flexible billing mode.
     * @param options Per-request settings.
     * @param completion Receives the subscription or failure.
     */
    virtual void migrate(std::string subscription_id,
                         const MigrateSubscriptionRequest& request,
                         const RequestOptions& options,
                         std::function<void(CallbackResult<Subscription>)> completion) = 0;
    /**
     * Set or clear cancellation at the end of the current billing period.
     * This does not cancel immediately or decide application access.
     *
     * @param subscription_id Existing subscription ID.
     * @param cancel_at_period_end True to schedule cancellation; false to clear it.
     * @param options Per-request settings, including a retry idempotency key.
     *
     * @return Updated subscription; inspect cancel_at_period_end for confirmation.
     *
     * @throws ConfigurationException If the ID is empty.
     * @throws StripeApiException If Stripe rejects the change.
     */
    virtual Task<Subscription>
    set_cancel_at_period_end_async(std::string subscription_id,
                                   bool cancel_at_period_end,
                                   const RequestOptions& options = {}) = 0;
    /**
     * Schedule or clear cancellation and report its result or failure exactly once.
     *
     * @param subscription_id Existing subscription ID.
     * @param cancel_at_period_end Whether cancellation should occur at period end.
     * @param options Per-request settings.
     * @param completion Receives the updated subscription or failure.
     */
    virtual void
    set_cancel_at_period_end(std::string subscription_id,
                             bool cancel_at_period_end,
                             const RequestOptions& options,
                             std::function<void(CallbackResult<Subscription>)> completion) = 0;
    /**
     * Pause collection without changing subscription status or existing invoices.
     *
     * @param subscription_id Existing subscription ID.
     * @param request How to handle new invoices and when to resume, if specified.
     * @param options Per-request settings, including a retry idempotency key.
     *
     * @return Updated subscription with pause_collection populated.
     *
     * @throws ConfigurationException If the ID, behavior, or resume timestamp is invalid.
     * @throws StripeApiException If Stripe rejects the change.
     */
    virtual Task<Subscription>
    pause_collection_async(std::string subscription_id,
                           const PauseSubscriptionCollectionRequest& request,
                           const RequestOptions& options = {}) = 0;
    /**
     * Pause collection and report its result or failure exactly once.
     *
     * @param subscription_id Existing subscription ID.
     * @param request Collection behavior and optional resume date.
     * @param options Per-request settings.
     * @param completion Receives the updated subscription or failure.
     */
    virtual void pause_collection(std::string subscription_id,
                                  const PauseSubscriptionCollectionRequest& request,
                                  const RequestOptions& options,
                                  std::function<void(CallbackResult<Subscription>)> completion) = 0;
    /**
     * Clear a collection pause. Previously paused invoices are not collected automatically.
     *
     * @param subscription_id Existing subscription ID.
     * @param options Per-request settings, including a retry idempotency key.
     *
     * @return Updated subscription; pause_collection is absent once resumed.
     *
     * @throws ConfigurationException If the ID is empty.
     * @throws StripeApiException If Stripe rejects the change.
     */
    virtual Task<Subscription> resume_collection_async(std::string subscription_id,
                                                       const RequestOptions& options = {}) = 0;
    /**
     * Resume collection and report its result or failure exactly once.
     *
     * @param subscription_id Existing subscription ID.
     * @param options Per-request settings.
     * @param completion Receives the updated subscription or failure.
     */
    virtual void
    resume_collection(std::string subscription_id,
                      const RequestOptions& options,
                      std::function<void(CallbackResult<Subscription>)> completion) = 0;
    /** Completion callback for the callback form of retrieve(). */
    using RetrieveCompletion = std::function<void(CallbackResult<Subscription>)>;

    /** Completion callback for the callback form of cancel(). */
    using CancelCompletion = std::function<void(CallbackResult<Subscription>)>;
    /** Completion callback receiving an updated subscription or a failure. */
    using UpdateCompletion = std::function<void(CallbackResult<Subscription>)>;

    /**
     * Update subscription items, cancellation timing, metadata, or automatic tax.
     *
     * Omitted billing behaviors use Stripe's defaults. A successful request
     * does not prove payment or that a pending change was applied: inspect
     * status, pending_update, and latest_invoice_id and follow billing webhooks.
     * Use an idempotency key when retrying. cancel_at schedules cancellation;
     * item changes otherwise apply immediately, subject to the payment behavior.
     *
     * @param subscription_id Existing subscription ID.
     * @param request Fields to change and optional billing policies.
     * @param options Per-request settings.
     *
     * @return Subscription state returned by Stripe.
     *
     * @throws ConfigurationException If the ID, cancellation date, or item fields are invalid.
     * @throws StripeApiException If Stripe rejects the update.
     */
    virtual Task<Subscription> update_async(std::string subscription_id,
                                            const UpdateSubscriptionRequest& request,
                                            const RequestOptions& options = {}) = 0;
    /**
     * Update a subscription, reporting failures through completion.
     *
     * @param subscription_id Existing subscription ID.
     * @param request Fields to change and optional billing policies.
     * @param options Per-request settings.
     * @param completion Receives the returned subscription or failure exactly once.
     */
    virtual void update(std::string subscription_id,
                        const UpdateSubscriptionRequest& request,
                        const RequestOptions& options,
                        UpdateCompletion completion) = 0;

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
 * List, retrieve, and delete invoice items.
 *
 * Reach an instance through StripeClient::invoice_items(). Check ownership,
 * service dates, and references to the original charge before deleting an adjustment.
 *
 * @sa https://docs.stripe.com/api/invoiceitems
 */
class InvoiceItems {
public:
    /** Receives one invoice item or a failure. */
    using Completion = std::function<void(CallbackResult<InvoiceItem>)>;
    /** Receives one invoice-item page or a failure. */
    using ListCompletion = std::function<void(CallbackResult<InvoiceItemList>)>;
    /** Receives the deleted ID and confirmation, or a failure. */
    using DeleteCompletion = std::function<void(CallbackResult<DeletedInvoiceItem>)>;
    virtual ~InvoiceItems() = default;

    /**
     * Read one page, newest first. If has_more is true, use the last item's ID as starting_after.
     * To find a subscription's items, filter by customer and check each item's optional
     * parent->subscription_details; Stripe does not offer a subscription filter.
     *
     * @param request Customer, invoice, pending filter, and pagination.
     * @param options Per-request headers and timeout.
     * @return One page with IDs suitable for starting_after.
     * @throws ConfigurationException If limit is outside 1 through 100 or the response is
     * malformed.
     * @throws StripeApiException If Stripe rejects the request.
     */
    virtual Task<InvoiceItemList> list_async(const ListInvoiceItemsRequest& request = {},
                                             const RequestOptions& options = {}) = 0;
    /**
     * Read one page and deliver it or its failure exactly once.
     *
     * @param request Customer, invoice, pending filter, and pagination.
     * @param options Per-request headers and timeout.
     * @param completion Receives the page or failure; may run on another thread.
     */
    virtual void list(const ListInvoiceItemsRequest& request,
                      const RequestOptions& options,
                      ListCompletion completion) = 0;
    /**
     * Read one invoice item, including ownership and proration references when available.
     *
     * @param id Invoice-item ID (ii_...), not an invoice line ID (il_...).
     * @param options Per-request headers and timeout.
     * @return Item with its amount, service dates, and available original-charge references.
     * @throws ConfigurationException If the ID is empty or the response is malformed.
     * @throws StripeApiException If Stripe rejects the request, including not found.
     */
    virtual Task<InvoiceItem> retrieve_async(std::string id,
                                             const RequestOptions& options = {}) = 0;
    /**
     * Read one item and deliver it or its failure exactly once.
     *
     * @param id Invoice-item ID (ii_...), not an invoice line ID (il_...).
     * @param options Per-request headers and timeout.
     * @param completion Receives the item or failure; may run on another thread.
     */
    virtual void retrieve(std::string id, const RequestOptions& options, Completion completion) = 0;
    /**
     * Delete a pending item or an item on a draft invoice.
     *
     * @param id Invoice-item ID (ii_...) that your application has identified for removal.
     * @param options Per-request headers and timeout.
     * @return Deleted ID and Stripe's explicit true confirmation.
     * @throws ConfigurationException If the ID is empty or deletion is not confirmed.
     * @throws StripeApiException If Stripe rejects deletion, including not found.
     */
    virtual Task<DeletedInvoiceItem> delete_async(std::string id,
                                                  const RequestOptions& options = {}) = 0;
    /**
     * Delete one item and deliver confirmation or failure exactly once.
     *
     * @param id Invoice-item ID (ii_...) that your application has identified for removal.
     * @param options Per-request headers and timeout.
     * @param completion Receives deletion confirmation or failure; may run on another thread.
     */
    virtual void delete_invoice_item(std::string id,
                                     const RequestOptions& options,
                                     DeleteCompletion completion) = 0;
};

/**
 * Read, finalize, void, and delete invoices.
 *
 * Reach an instance through StripeClient::invoices(). Use it to show what a
 * customer owes, or to fetch the link where they can pay an invoice that
 * needs their attention, such as one awaiting 3D Secure authentication.
 *
 * @sa https://docs.stripe.com/api/invoices
 */
class Invoices {
public:
    /** Receives one invoice page or a failure. */
    using ListCompletion = std::function<void(CallbackResult<InvoiceList>)>;
    /** Receives draft deletion confirmation or a failure. */
    using DeleteCompletion = std::function<void(CallbackResult<DeletedInvoice>)>;
    /** Receives an invoice or the failure. */
    using Completion = std::function<void(CallbackResult<Invoice>)>;
    /** Receives one page of purchased invoice lines or a failure. */
    using LineItemsCompletion = std::function<void(CallbackResult<PurchasedLineItemList>)>;
    /**
     * Read one page, optionally for a specific subscription.
     *
     * @param request Subscription filter, page size and cursor.
     * @param options Per-request settings.
     *
     * @return One page; further pages are not fetched automatically.
     *
     * @throws ConfigurationException If limit is outside 1 through 100.
     * @throws StripeApiException If Stripe rejects the request.
     */
    virtual Task<InvoiceList> list_async(const ListInvoicesRequest& request = {},
                                         const RequestOptions& options = {}) = 0;
    /**
     * Read one page and report its result or failure exactly once.
     *
     * @param request Subscription filter and pagination settings.
     * @param options Per-request settings.
     * @param completion Receives the page or failure.
     */
    virtual void list(const ListInvoicesRequest& request,
                      const RequestOptions& options,
                      ListCompletion completion) = 0;

    /**
     * Read one page of purchased prices and quantities on an invoice.
     *
     * @param invoice_id Existing Stripe invoice ID.
     * @param request Page size and cursor.
     * @param options Per-request settings.
     *
     * @return One page of line items; further pages are not fetched automatically.
     *
     * @throws ConfigurationException If the ID is empty or the page size is invalid.
     * @throws StripeApiException If Stripe rejects the request.
     */
    virtual Task<PurchasedLineItemList>
    list_line_items_async(std::string invoice_id,
                          const ListPurchasedLineItemsRequest& request = {},
                          const RequestOptions& options = {}) = 0;

    /**
     * Read invoice line items and report the page or failure exactly once.
     *
     * @param invoice_id Existing Stripe invoice ID.
     * @param request Page size and cursor.
     * @param options Per-request settings.
     * @param completion Receives the page or failure.
     */
    virtual void list_line_items(std::string invoice_id,
                                 const ListPurchasedLineItemsRequest& request,
                                 const RequestOptions& options,
                                 LineItemsCompletion completion) = 0;
    /**
     * Update automatic-tax or collection settings on a draft invoice.
     *
     * @param invoice_id Existing draft invoice ID.
     * @param request Fields to change.
     * @param options Per-request settings.
     *
     * @return Updated invoice with the resulting draft settings.
     *
     * @throws ConfigurationException If the ID is empty or no fields are supplied.
     * @throws StripeApiException If Stripe rejects the change.
     */
    virtual Task<Invoice> update_async(std::string invoice_id,
                                       const UpdateInvoiceRequest& request,
                                       const RequestOptions& options = {}) = 0;

    /**
     * Update a draft invoice and report its result or failure exactly once.
     *
     * @param invoice_id Existing draft invoice ID.
     * @param request Fields to change.
     * @param options Per-request settings.
     * @param completion Receives the updated invoice or failure.
     */
    virtual void update(std::string invoice_id,
                        const UpdateInvoiceRequest& request,
                        const RequestOptions& options,
                        Completion completion) = 0;
    /**
     * Finalize a draft with automatic collection disabled.
     *
     * Subscription-linked drafts cannot be deleted. Finalize them before voiding.
     *
     * @param invoice_id Existing draft invoice ID.
     * @param options Per-request settings, including a retry idempotency key.
     *
     * @return Finalized invoice; inspect its status before taking further action.
     *
     * @throws ConfigurationException If the ID is empty.
     * @throws StripeApiException If Stripe rejects finalization.
     */
    virtual Task<Invoice> finalize_async(std::string invoice_id,
                                         const RequestOptions& options = {}) = 0;
    /**
     * Finalize without automatic collection and report its result or failure exactly once.
     *
     * @param invoice_id Existing draft invoice ID.
     * @param options Per-request settings.
     * @param completion Receives the finalized invoice or failure.
     */
    virtual void finalize(std::string invoice_id,
                          const RequestOptions& options,
                          std::function<void(CallbackResult<Invoice>)> completion) = 0;
    /**
     * Void a finalized invoice so Stripe cannot collect it. Paid invoices cannot be voided.
     *
     * @param invoice_id Existing invoice ID.
     * @param options Per-request settings, including a retry idempotency key.
     *
     * @return Updated invoice; inspect its status for confirmation.
     *
     * @throws ConfigurationException If the ID is empty.
     * @throws StripeApiException If Stripe rejects voiding.
     */
    virtual Task<Invoice> void_async(std::string invoice_id,
                                     const RequestOptions& options = {}) = 0;
    /**
     * Void an invoice and report its result or failure exactly once.
     *
     * @param invoice_id Existing invoice ID.
     * @param options Per-request settings.
     * @param completion Receives the updated invoice or failure.
     */
    virtual void void_invoice(std::string invoice_id,
                              const RequestOptions& options,
                              std::function<void(CallbackResult<Invoice>)> completion) = 0;
    /**
     * Delete a manually created draft invoice.
     *
     * Subscription-linked drafts cannot be deleted; finalize and void them instead.
     *
     * @param invoice_id Existing draft invoice ID.
     * @param options Per-request settings.
     *
     * @return Stripe's deletion confirmation.
     *
     * @throws ConfigurationException If the ID is empty or confirmation is malformed.
     * @throws StripeApiException If Stripe rejects deletion.
     */
    virtual Task<DeletedInvoice> delete_async(std::string invoice_id,
                                              const RequestOptions& options = {}) = 0;
    /**
     * Delete a draft and report its result or failure exactly once.
     *
     * @param invoice_id Existing draft invoice ID.
     * @param options Per-request settings.
     * @param completion Receives deletion confirmation or failure.
     */
    virtual void delete_invoice(std::string invoice_id,
                                const RequestOptions& options,
                                DeleteCompletion completion) = 0;
    /** Destroys the API instance. */
    virtual ~Invoices() = default;

    /**
     * Fetches an invoice.
     *
     * @param invoice_id Stripe invoice ID, of the form "in_...".
     * @param options Per-request settings such as a timeout.
     *
     * @return A task producing the invoice.
     *
     * @throws ConfigurationException The invoice ID was empty.
     *
     * @throws StripeApiException No such invoice, or Stripe rejected the request.
     */
    virtual Task<Invoice> retrieve_async(std::string invoice_id,
                                         const RequestOptions& options = {}) = 0;

    /**
     * Fetches an invoice, reporting the result through a callback.
     *
     * The callback is invoked exactly once, and may run on a different thread
     * than the caller.
     *
     * @param invoice_id Stripe invoice ID, of the form "in_...".
     * @param options Per-request settings such as a timeout.
     * @param completion Receives the invoice or the failure.
     */
    virtual void
    retrieve(std::string invoice_id, const RequestOptions& options, Completion completion) = 0;
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
     * @param request The customer to create the session for, where to return
     *        them afterwards, and optionally a configuration, language, and
     *        task to open.
     * @param options Per-request settings such as a timeout.
     *
     * @return A task producing the session, whose `url` is where the customer
     *         should be sent.
     *
     * @throws ConfigurationException The customer ID was empty, or the flow
     *         was missing details its type needs.
     *
     * @throws StripeApiException Stripe rejected the request. A common cause is
     *         the billing portal not yet being configured in the dashboard.
     */
    virtual Task<BillingPortalSession>
    create_async(const CreateBillingPortalSessionRequest& request,
                 const RequestOptions& options = {}) = 0;

    /**
     * Creates a billing portal session, reporting the result through a callback.
     *
     * The callback is invoked exactly once, and may run on a different thread
     * than the caller. Invalid requests are reported through the callback too.
     *
     * @param request The customer to create the session for, where to return
     *        them afterwards, and optionally a configuration, language, and
     *        task to open.
     * @param options Per-request settings such as a timeout.
     * @param completion Receives the created session or the failure.
     */
    virtual void create(const CreateBillingPortalSessionRequest& request,
                        const RequestOptions& options,
                        CreateCompletion completion) = 0;
};

/**
 * Creates and manages billing portal configurations.
 *
 * Reach an instance through BillingPortal::configurations(). A configuration
 * decides which features the portal offers, such as which plans customers can
 * switch to. Creating one from code means the app does not depend on settings
 * someone made by hand in the Stripe dashboard.
 *
 * @sa https://docs.stripe.com/api/customer_portal/configurations
 */
class BillingPortalConfigurations {
public:
    /** Callback receiving a configuration or the failure. */
    using Completion = std::function<void(CallbackResult<BillingPortalConfiguration>)>;

    /** Destroys the API instance. */
    virtual ~BillingPortalConfigurations() = default;

    /**
     * Creates a portal configuration. Use an idempotency key for safe retries.
     *
     * @param request Features and wording for the portal.
     * @param options Per-request settings.
     *
     * @return A task producing the created configuration.
     *
     * @throws ConfigurationException The request had invalid feature settings.
     *
     * @throws StripeApiException Stripe rejected the request.
     */
    virtual Task<BillingPortalConfiguration>
    create_async(const CreateBillingPortalConfigurationRequest& request,
                 const RequestOptions& options = {}) = 0;

    /**
     * Creates a billing portal configuration, reporting the result through a callback.
     *
     * The callback is invoked exactly once. Invalid requests are reported
     * through the callback too.
     *
     * @param request Features and wording for the portal.
     * @param options Per-request settings.
     * @param completion Receives the created configuration or the failure.
     */
    virtual void create(const CreateBillingPortalConfigurationRequest& request,
                        const RequestOptions& options,
                        Completion completion) = 0;

    /**
     * Fetches a portal configuration.
     *
     * @param configuration_id Configuration ID, of the form "bpc_...".
     * @param options Per-request settings.
     *
     * @return A task producing the configuration.
     *
     * @throws ConfigurationException The configuration ID was empty.
     *
     * @throws StripeApiException No such configuration, or Stripe rejected the request.
     */
    virtual Task<BillingPortalConfiguration> retrieve_async(std::string configuration_id,
                                                            const RequestOptions& options = {}) = 0;

    /**
     * Fetches a billing portal configuration, reporting the result through a callback.
     *
     * The callback is invoked exactly once.
     *
     * @param configuration_id Configuration ID, of the form "bpc_...".
     * @param options Per-request settings.
     * @param completion Receives the configuration or the failure.
     */
    virtual void retrieve(std::string configuration_id,
                          const RequestOptions& options,
                          Completion completion) = 0;

    /**
     * Changes a portal configuration, or deactivates it.
     *
     * Changes apply to portal sessions created afterwards.
     *
     * @param configuration_id Configuration ID, of the form "bpc_...".
     * @param request The fields to change.
     * @param options Per-request settings.
     *
     * @return A task producing the updated configuration.
     *
     * @throws ConfigurationException The ID was empty or the request had
     *         invalid feature settings.
     *
     * @throws StripeApiException Stripe rejected the request.
     */
    virtual Task<BillingPortalConfiguration>
    update_async(std::string configuration_id,
                 const UpdateBillingPortalConfigurationRequest& request,
                 const RequestOptions& options = {}) = 0;

    /**
     * Changes a billing portal configuration, reporting the result through a callback.
     *
     * The callback is invoked exactly once. Invalid requests are reported
     * through the callback too.
     *
     * @param configuration_id Configuration ID, of the form "bpc_...".
     * @param request The fields to change.
     * @param options Per-request settings.
     * @param completion Receives the updated configuration or the failure.
     */
    virtual void update(std::string configuration_id,
                        const UpdateBillingPortalConfigurationRequest& request,
                        const RequestOptions& options,
                        Completion completion) = 0;
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

    /**
     * Returns the billing portal configuration operations.
     *
     * @return A reference owned by the client; it stays valid as long as the
     *         client does.
     */
    virtual BillingPortalConfigurations& configurations() = 0;
};

/** Create a refund for a charge, optionally limiting it to part of the remaining balance. */
struct CreateRefundRequest {
    std::string charge_id;
    std::optional<std::int64_t> amount; ///< Positive amount in the smallest currency unit; absent
                                        ///< refunds the remaining balance.
    std::unordered_map<std::string, std::string>
        metadata; ///< Save a durable application operation ID for recovery.
};

/** Filters for one page of refunds. Empty charge_id lists refunds for all charges. */
struct ListRefundsRequest {
    std::string charge_id;
    std::int64_t limit = 10;    ///< Page size, from 1 to 100.
    std::string starting_after; ///< Last refund ID from the previous page.
};

/** Create refunds and read refund history through StripeClient::refunds(). */
class Refunds {
public:
    /** Receives the created refund or the failure. */
    using Completion = std::function<void(CallbackResult<Refund>)>;
    /** Receives one page of refunds or the failure. */
    using ListCompletion = std::function<void(CallbackResult<RefundList>)>;
    /** Destroy the operations instance. */
    virtual ~Refunds() = default;
    /**
     * Request money back for a charge. Save and reuse an idempotency key for retries.
     * Stripe checks the remaining refundable balance. Inspect status before
     * treating the refund as complete.
     *
     * @param request Charge, optional amount, and metadata.
     * @param options Per-request timeout, headers, and idempotency key.
     *
     * @return The refund, which may still be pending or require customer action.
     *
     * @throws ConfigurationException If the charge is empty or the supplied amount is not positive.
     * @throws StripeApiException If Stripe rejects the request.
     */
    virtual Task<Refund> create_async(const CreateRefundRequest& request,
                                      const RequestOptions& options = {}) = 0;
    /**
     * Create a refund and report its result or failure exactly once.
     *
     * @param request Charge, optional amount, and metadata.
     * @param options Per-request settings; reuse an idempotency key for retries.
     * @param completion Receives the refund or failure.
     */
    virtual void create(const CreateRefundRequest& request,
                        const RequestOptions& options,
                        Completion completion) = 0;
    /**
     * Read one page of refunds, optionally for a specific charge.
     * Metadata can recover a saved operation after its idempotency key expires.
     *
     * @param request Charge filter, page size, and cursor.
     * @param options Per-request settings.
     *
     * @return One page; further pages are not fetched automatically.
     *
     * @throws ConfigurationException If limit is outside 1 through 100.
     * @throws StripeApiException If Stripe rejects the request.
     */
    virtual Task<RefundList> list_async(const ListRefundsRequest& request = {},
                                        const RequestOptions& options = {}) = 0;
    /**
     * Read one page and report its result or failure exactly once.
     *
     * @param request Charge filter and pagination settings.
     * @param options Per-request settings.
     * @param completion Receives the page or failure.
     */
    virtual void list(const ListRefundsRequest& request,
                      const RequestOptions& options,
                      ListCompletion completion) = 0;
};

/** Filters for one page of charges. Empty customer_id lists all customers. */
struct ListChargesRequest {
    std::string customer_id;
    std::int64_t limit = 10;    ///< Page size, from 1 to 100.
    std::string starting_after; ///< Last charge ID from the previous page.
};

/** Filters for one page of invoice payments. Empty payment_intent_id lists all payments. */
struct ListInvoicePaymentsRequest {
    std::string payment_intent_id;
    std::int64_t limit = 10;    ///< Page size, from 1 to 100.
    std::string starting_after; ///< Last invoice payment ID from the previous page.
};

/** Read payment attempts and discover a customer's charges through StripeClient::charges(). */
class Charges {
public:
    /** Receives a charge or the failure. */
    using Completion = std::function<void(CallbackResult<Charge>)>;
    /** Receives one page or the failure. */
    using ListCompletion = std::function<void(CallbackResult<ChargeList>)>;
    /** Destroy the operations instance. */
    virtual ~Charges() = default;

    /**
     * Retrieve a charge, including customer and payment references.
     *
     * @param charge_id Existing Stripe charge ID.
     * @param options Per-request timeout and headers.
     *
     * @return The charge, including payment and dispute status.
     *
     * @throws ConfigurationException If the ID is empty.
     * @throws StripeApiException If Stripe rejects the request.
     */
    virtual Task<Charge> retrieve_async(std::string charge_id,
                                        const RequestOptions& options = {}) = 0;
    /**
     * Retrieve a charge and report its result or failure exactly once.
     *
     * @param charge_id Existing Stripe charge ID.
     * @param options Per-request settings.
     * @param completion Receives the charge or failure.
     */
    virtual void
    retrieve(std::string charge_id, const RequestOptions& options, Completion completion) = 0;
    /**
     * List one page of charges, optionally limited to a customer.
     *
     * @param request Customer filter, page size and cursor.
     * @param options Per-request settings.
     *
     * @return One page; no further pages are fetched automatically.
     *
     * @throws ConfigurationException If limit is outside 1 through 100.
     * @throws StripeApiException If Stripe rejects the request.
     */
    virtual Task<ChargeList> list_async(const ListChargesRequest& request = {},
                                        const RequestOptions& options = {}) = 0;
    /**
     * List one page and report its result or failure exactly once.
     *
     * @param request Customer filter and pagination settings.
     * @param options Per-request settings.
     * @param completion Receives the page or failure.
     */
    virtual void list(const ListChargesRequest& request,
                      const RequestOptions& options,
                      ListCompletion completion) = 0;
};

/**
 * Read invoice payment allocations through StripeClient::invoice_payments().
 * Use a charge's payment_intent_id to find its invoice payment allocations.
 */
class InvoicePayments {
public:
    /** Receives one page or the failure. */
    using ListCompletion = std::function<void(CallbackResult<InvoicePaymentList>)>;
    /** Destroy the operations instance. */
    virtual ~InvoicePayments() = default;
    /**
     * List invoice allocations, optionally for a specific PaymentIntent.
     * One payment may be allocated to more than one invoice.
     *
     * @param request Payment intent filter, page size and cursor.
     * @param options Per-request settings, including a supported Stripe-Version.
     *
     * @return One page; no further pages are fetched automatically.
     *
     * @throws ConfigurationException If limit is outside 1 through 100.
     * @throws StripeApiException If Stripe rejects the request or API version.
     */
    virtual Task<InvoicePaymentList> list_async(const ListInvoicePaymentsRequest& request = {},
                                                const RequestOptions& options = {}) = 0;
    /**
     * List one page and report its result or failure exactly once.
     *
     * @param request Payment intent filter and pagination settings.
     * @param options Per-request settings.
     * @param completion Receives the page or failure.
     */
    virtual void list(const ListInvoicePaymentsRequest& request,
                      const RequestOptions& options,
                      ListCompletion completion) = 0;
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
     * Return subscription schedule operations.
     *
     * @return Operations owned by this client and valid for its lifetime.
     */
    virtual SubscriptionSchedules& subscription_schedules() = 0;
    /**
     * Return refund creation and listing operations.
     *
     * @return Operations owned by this client and valid for its lifetime.
     */
    virtual Refunds& refunds() = 0;

    /**
     * Return charge retrieval and listing operations.
     *
     * @return Operations owned by this client and valid for its lifetime.
     */
    virtual Charges& charges() = 0;

    /**
     * Return invoice payment allocation operations.
     *
     * @return Operations owned by this client and valid for its lifetime.
     */
    virtual InvoicePayments& invoice_payments() = 0;

    /**
     * Destroys the client instance.
     */
    virtual ~StripeClient() = default;

    /**
     * Returns the catalog product operations.
     *
     * @return A reference owned by the client; it stays valid as long as the
     *         client does.
     */
    virtual Products& products() = 0;

    /**
     * Returns the catalog price operations.
     *
     * @return A reference owned by the client; it stays valid as long as the
     *         client does.
     */
    virtual Prices& prices() = 0;

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
     * Returns the invoice operations.
     *
     * @return A reference owned by the client; it stays valid as long as the
     *         client does.
     */
    virtual Invoices& invoices() = 0;

    /**
     * Return invoice-item operations.
     *
     * @return A reference owned by this client and valid for its lifetime.
     */
    virtual InvoiceItems& invoice_items() = 0;

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
