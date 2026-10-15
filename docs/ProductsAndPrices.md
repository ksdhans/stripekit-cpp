@page products_and_prices Products and Prices

@tableofcontents

A **product** describes what you sell, such as a Standard subscription.
A **price** describes how much it costs, in which currency, and how often the
customer pays. One product can have several prices, such as monthly and yearly
prices or prices introduced at different times.

Use reusable prices when customers purchase the same offering, or when existing
subscribers must keep their original pricing after you change prices for new
customers. StripeKit can create, retrieve, and update products and prices, list
prices, and switch existing subscription items to new prices.
Your application decides when to call these operations
and stores the resulting IDs.

@section catalog_setup Before you start

Create a Stripe client as shown in @ref quickstart_client. The examples below
use that client as `stripe` and run inside a coroutine. Use a test-mode secret
key during development; test and live accounts have separate products and prices.
Never put secret keys in source code.

Every operation also has a callback form, such as `products().create()`, for
applications that do not use coroutines. See @ref client for the complete API.

@section catalog_create Create a product and recurring price

Create a product once for an offering, then reuse its ID when adding prices:

@code
stripekit::CreateProductRequest product_request;
product_request.id = "standard";
product_request.name = "Standard";
product_request.description = "Standard subscription";

stripekit::RequestOptions product_options;
product_options.idempotency_key = "create-standard-product";
const auto product =
    co_await stripe->products().create_async(product_request, product_options);

stripekit::CreatePriceRequest price_request;
price_request.product_id = product.id;
price_request.currency = "usd";
price_request.unit_amount = 2500;
price_request.recurring_interval = "month";
price_request.recurring_interval_count = 1;
price_request.lookup_key = "standard-usd-monthly-v1";

stripekit::RequestOptions price_options;
price_options.idempotency_key = "create-standard-monthly-v1";
const auto price =
    co_await stripe->prices().create_async(price_request, price_options);
@endcode

This creates a price of 25 USD per month. Amounts use the currency's smallest
unit: cents for USD. Leave `recurring_interval` empty for a one-time price.
Set `recurring_interval_count` to charge every several intervals, such as
three months; one-time prices require a count of one.

The caller-assigned product ID is optional. If you omit it, Stripe assigns one.
Choose IDs, lookup keys, and idempotency keys from stable identifiers in your
application, not a new random value on each retry. The values above assume one
offering and pricing version; use distinct values for other offerings or versions.

Save `product.id` and `price.id` in your database. For Checkout, set
`CreateCheckoutSessionRequest::price_id` to the saved price ID, as shown in
@ref quickstart_checkout. Use subscription mode for a recurring price and payment
mode for a one-time price.

@section catalog_recovery Find pricing after an interrupted request

If the connection fails, Stripe may have created the object even though your
application did not receive its ID. Retry the same creation request with the
same idempotency key and unchanged parameters. Stripe's idempotency records
are not permanent; do not rely on them as your only stored identity.

A caller-assigned product ID lets you retrieve that product later. A price's
lookup key lets you find it without knowing its price ID:

@code
stripekit::ListPricesRequest query;
query.lookup_keys = { "standard-usd-monthly-v1" };
const auto page = co_await stripe->prices().list_async(query);
@endcode

An empty result means no matching price was returned. Before reusing a result,
check its product, amount, currency, recurrence, and availability against the
offering you intend to sell. A matching key alone is not enough.

List calls return one page and do not automatically fetch the rest. When
`page.has_more` is true, pass the last returned price's ID as `starting_after`
on the next request, keeping the same filters. The default page size is ten;
`limit` accepts 1 through 100. A request accepts at most ten lookup keys.
Set `product_id` to list a product's prices.

Lists include active and archived prices by default. Set `query.active = true`
to request only active prices, or `false` to request only archived prices.

@section catalog_changes Change display details or introduce new pricing

To change a product's display text:

@code
stripekit::UpdateProductRequest changes;
changes.name = "Standard Plus";
changes.description = "";
const auto updated = co_await stripe->products().update_async(product.id, changes);
@endcode

Unset fields are left unchanged. An empty description clears it; an empty name
is rejected. Setting `active = false` archives the product. Display changes
can affect how existing subscriptions are presented, but do not change their
price amounts.

To change an amount or billing interval, create another price on the same
product with a new lookup key and idempotency key. Use its ID for new purchases.
Keep the old price ID associated with existing subscriptions.

**Creating a new price does not change existing subscriptions.** This supports
grandfathered pricing: existing customers keep the old price while new customers
purchase at the new price. Moving existing subscriptions to a different price
requires a separate subscription change, as described below.

@subsection catalog_default Choose the product's default price

After creating a replacement price, you can make it the product's default:

@code
stripekit::UpdateProductRequest defaults;
defaults.default_price_id = price.id;
const auto product_with_default =
    co_await stripe->products().update_async(product.id, defaults);
@endcode

The price must be active and belong to this product. Stripe validates these
requirements. `Product::default_price_id` reports the selected price, or an
empty string when none is set. Changing it does not change existing subscriptions.
Checkout still requires you to supply `CreateCheckoutSessionRequest::price_id`.

@subsection catalog_archive Archive prices and move lookup keys

@code
stripekit::UpdatePriceRequest archive;
archive.active = false;
const auto archived = co_await stripe->prices().update_async(old_price_id, archive);
@endcode

Archiving prevents new purchases without changing existing subscriptions.
You can also update `nickname` (an internal label), metadata, and `lookup_key`.
Unset fields remain unchanged; empty nickname or lookup-key strings clear them.
An empty metadata value removes that key.

To move a stable lookup key to a replacement price atomically, supply the key
and set `transfer_lookup_key = true` in the replacement price's update request.
Only transfer a key when its meaning is "current price"; keep versioned keys
distinct when they identify specific pricing terms.

Amounts, currencies, and billing intervals cannot be changed by a price update.

@section catalog_subscriptions Change an existing subscription's pricing

Retrieve the subscription and choose the item to change from `Subscription::items`.
Each item exposes its ID, price ID, and optional quantity. Do not assume every
subscription contains only one item. `items_has_more` indicates a partial list;
StripeKit does not fetch more pages or expose subscription-item listing yet.

For a licensed item, explicitly copy its quantity when replacing its price.
Stripe otherwise resets the quantity to one. Supply the existing `si_...` item
ID, not just the new price ID: omitting the item ID in Stripe's API adds another
item rather than replacing it. StripeKit requires an existing item ID.

@code
stripekit::UpdateSubscriptionRequest migration;
migration.items.push_back({
    .item_id = saved_item_id,
    .price_id = replacement_price_id,
    .quantity = saved_quantity
});
migration.proration_behavior = stripekit::ProrationBehavior::None;
migration.payment_behavior = stripekit::PaymentBehavior::AllowIncomplete;

stripekit::RequestOptions migration_options;
migration_options.idempotency_key = "subscription-migration-v2";
const auto changed = co_await stripe->subscriptions().update_async(
    subscription_id, migration, migration_options);
@endcode

Use a distinct, stable idempotency key for each subscription change. Retry with
the same key and unchanged inputs after an interrupted request.
Only listed items change. A request accepts one through twenty distinct item
IDs and requires a positive quantity for each. This operation targets licensed,
fixed-quantity subscriptions, not metered subscription changes.

Both behaviors are required; leaving either unset raises `ConfigurationException`
before sending a request.

| ProrationBehavior | Effect |
| --- | --- |
| `CreateProrations` | Create adjustments for unused time; they are not necessarily invoiced immediately. |
| `AlwaysInvoice` | Create and immediately invoice prorated adjustments. |
| `None` | Do not create prorated adjustments. |

No proration does not mean no immediate payment. Switching billing intervals,
for example, can reset the billing date and trigger an immediate invoice.

| PaymentBehavior | Effect when an automatically charged update requires payment |
| --- | --- |
| `AllowIncomplete` | Apply the change even if collection fails or requires authentication; follow payment recovery for `past_due`. |
| `PendingIfIncomplete` | Keep the change pending until its invoice is paid. |
| `ErrorIfIncomplete` | Reject the change if payment cannot complete. |
| `DefaultIncomplete` | Use a flow requiring explicit confirmation of the invoice's PaymentIntent. |

**A successful API call is not proof of payment or of an applied pending change.**
Inspect `status`, `latest_invoice_id`, and `pending_update`, and follow invoice
and subscription webhooks. Current prices are in `items`; proposed prices and
the expiry time of a pending change are in `pending_update`. Stripe can restrict
which updates and payment methods support pending changes.

For optional upgrades, `PendingIfIncomplete` with `AlwaysInvoice` lets payment
gate the upgrade. For a mandatory price migration, `AllowIncomplete` avoids
keeping the old price merely because collection failed. Notify customers,
provide a way to complete authentication or replace a payment method, configure
Stripe's retries, and enforce a finite grace period in your access policy.
Stripe Billing can send hosted payment and authentication links. StripeKit
does not automatically notify customers, collect outstanding invoices, confirm
PaymentIntents, or change application access.

These updates take effect immediately. For a future change at renewal, use
Stripe Subscription Schedules through another integration; StripeKit does not
yet expose schedules. An immediate update with `None` is not a scheduled change.

@section catalog_validation Check returned prices and handle failures

Price creation supports fixed integer amounts and licensed recurrence, meaning
a fixed charge for each purchased unit. Creating tiered, usage-based, decimal,
or customer-chosen amounts is not supported.

Retrieved prices expose their pricing type and billing scheme, optional
recurrence and quantity adjustments, and whether the customer chooses the amount.
`unit_amount` is optional: an absent value is not a zero-priced item. Expanded
product responses are returned as product IDs. Archived prices remain readable.
Deleted product responses provide only the product ID and deleted flag.

Stripe API failures throw `StripeApiException`, which includes the HTTP status,
request ID, and whether the failure may be temporary. Transport failures and
malformed responses also raise exceptions. Callback methods deliver failures
through their result argument. StripeKit does not automatically retry these
operations. See @ref quickstart_errors and @ref quickstart_options for error
handling and request settings.

Your application owns pricing decisions, saved ID mappings, synchronization,
and retries. These operations do not run scheduled checks or keep an application
catalog synchronized automatically.
