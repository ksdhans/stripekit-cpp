# Billing Portal and Invoices {#billing_portal_and_invoices}

[TOC]

The **billing portal** is a Stripe-hosted page where customers manage their own
billing: they can update their card, download past invoices, switch plans, or
cancel. Your application never handles card details. It asks Stripe for a
short-lived portal link and sends the customer there.

An **invoice** is Stripe's bill for one billing period or plan change. Reading
an invoice tells your application how much is still owed, whether Stripe will
retry the payment, and where the customer can pay it.

A zero-value trial invoice can be `paid` without a card charge. Check the
subscription's status and trial end when deciding trial access; a paid
invoice alone is not proof of money collected. At trial end, a saved card can
still need authentication. For an open invoice with money remaining, use its
`hosted_invoice_url` to let the customer pay or authenticate, then wait for
verified billing updates before restoring paid access.

Use these features when you want customers to fix their own payment problems
and change their own plans, instead of building those pages yourself.

@section portal_setup Before you start

Create a Stripe client as shown in @ref quickstart_client. The examples below
use that client as `stripe` and run inside a coroutine. Every operation also
has a callback form, such as `billing_portal().sessions().create()`, for
applications that do not use coroutines. See @ref client for the complete API.

The customer must already exist in Stripe, usually because they went through
Checkout. Store their customer ID (`cus_...`) when you receive it.

@section portal_session Send a customer to the portal

Create a session and redirect the customer's browser to its `url`. The link
expires shortly, so create a new one each time the customer clicks your
"Manage billing" button.

@code
stripekit::CreateBillingPortalSessionRequest request;
request.customer_id = customer_id;
request.return_url = "https://example.com/billing";
request.locale = "de"; // optional; empty lets Stripe choose

const auto session = co_await stripe->billing_portal().sessions().create_async(request);
// Redirect the customer to session.url
@endcode

Without a `configuration_id`, Stripe uses the account's default portal
configuration, which you can edit in the Stripe Dashboard or replace with one
created as described in @ref portal_configuration.

@section portal_flows Open the portal at a specific task

A **flow** opens the portal directly at one task instead of its home page. This
is useful for "Fix payment" or "Change plan" buttons.

| Flow type | What the customer sees | Needs a subscription ID |
|-----------|------------------------|-------------------------|
| `PaymentMethodUpdate` | Add or replace a payment method. | No |
| `SubscriptionCancel` | Confirm cancelling a subscription. | Yes |
| `SubscriptionUpdate` | Pick a new plan from the configured list. | Yes |
| `SubscriptionUpdateConfirm` | Confirm a plan change your application already chose. | Yes |

For example, to let a customer confirm a switch to a yearly price:

@code
stripekit::BillingPortalFlow flow;
flow.type = stripekit::BillingPortalFlowType::SubscriptionUpdateConfirm;
flow.subscription_id = subscription.id;
flow.items.push_back({ .item_id = subscription.items.front().id,
                       .price_id = "price_yearly" });
flow.after_completion = stripekit::BillingPortalAfterCompletion::Redirect;
flow.redirect_url = "https://example.com/billing/processing";

request.flow = flow;
const auto session = co_await stripe->billing_portal().sessions().create_async(request);
@endcode

After the customer finishes, the portal can return to its home page (the
default), show a confirmation message on Stripe's page (`HostedConfirmation`,
with an optional `confirmation_message` of up to 500 characters), or send the
customer to `redirect_url` (`Redirect`).

StripeKit checks the flow before contacting Stripe and throws
stripekit::ConfigurationException if, for example, a subscription ID is
missing, or a confirm flow does not list exactly one item. Stripe also rejects
flows the portal configuration does not allow: a confirm flow can only switch
to a price listed in the configuration's plan products, and each flow needs its
feature turned on.

Remember that the portal only collects the customer's choice. Stripe reports
the result through webhooks such as `customer.subscription.updated` and
`invoice.paid`; update your application's records from those events.

@section portal_configuration Choose which portal features are shown

A **portal configuration** decides what customers can do in the portal. You
can create one per application, and pass its ID as `configuration_id` when
creating sessions.

@code
stripekit::CreateBillingPortalConfigurationRequest config;
config.name = "My app plans";
config.headline = "Manage your My App subscription";
config.default_return_url = "https://example.com/billing";

config.features.customer_update.enabled = true;
config.features.customer_update.allowed_updates = { "email", "address" };
config.features.subscription_cancel.enabled = true;
config.features.subscription_cancel.mode = "at_period_end";
config.features.subscription_update.enabled = true;
config.features.subscription_update.default_allowed_updates = { "price" };
config.features.subscription_update.products = {
    { .product_id = "basic", .price_ids = { "price_basic_monthly", "price_basic_yearly" } },
    { .product_id = "pro", .price_ids = { "price_pro_monthly", "price_pro_yearly" } },
};
config.features.subscription_update.proration_behavior = "create_prorations";

const auto created =
    co_await stripe->billing_portal().configurations().create_async(config);
// Store created.id and use it as the sessions' configuration_id
@endcode

Things to know:

- **Invoice history and payment method updates are on by default** in
  StripeKit, because they are how customers fix most payment problems. Turn
  them off explicitly if you do not want them. (Stripe's own API treats an
  unset feature as off.)
- Plan switching needs allowed changes, at least one product, and payment
  method updates turned on. Stripe allows at most **10 products**, so very
  large catalogs need another way to change plans.
- Use `configurations().update_async()` to change an existing configuration.
  Only the fields you set are sent. Setting `features` replaces all feature
  settings, and an empty list clears that list in Stripe.
- Configurations cannot be deleted. Set `active = false` to retire one.
- StripeKit throws stripekit::ConfigurationException for values Stripe would
  reject, such as unknown cancellation modes or names longer than 256
  characters.

@section invoices_retrieve Read an invoice

Retrieve an invoice by ID, usually the `latest_invoice_id` of a subscription or
the invoice ID from an `invoice.*` webhook event:

@code
const auto invoice = co_await stripe->invoices().retrieve_async(invoice_id);
if (!invoice.paid && invoice.status == "open") {
    // Show the amount owed and a "Pay now" link to invoice.hosted_invoice_url
}
@endcode

Useful fields:

| Field | Meaning |
|-------|---------|
| `status` | `draft`, `open`, `paid`, `uncollectible`, or `void`. |
| `amount_due`, `amount_paid`, `amount_remaining` | Amounts in the currency's smallest unit, such as cents. |
| `hosted_invoice_url` | A Stripe page where the customer can pay the invoice, including completing a 3D Secure check. |
| `invoice_pdf` | A link to download the invoice as a PDF. |
| `billing_reason` | Why it was created, such as `subscription_cycle` or `subscription_update`. |
| `attempt_count` | How many payment attempts were made. A manual payment after a failure does not always increase it. |
| `next_payment_attempt_unix` | When Stripe will retry automatically; empty when no retry is planned. |
| `due_date_unix` | When payment is due, for invoices sent to the customer rather than charged automatically. |

`hosted_invoice_url` is the simplest way to recover a payment that needs the
customer's action, such as an extra bank authentication. Send the customer
there, then wait for the `invoice.paid` webhook.

@section collection_controls Schedule cancellation or pause collection

Use period-end cancellation to let an existing subscription finish its current
billing period. Clear it before that period ends if the customer changes their mind:

@code{.cpp}
const auto scheduled = co_await stripe->subscriptions().set_cancel_at_period_end_async(
    subscription_id, true, { .idempotency_key = "saved-cancellation-key" });
const auto continued = co_await stripe->subscriptions().set_cancel_at_period_end_async(
    subscription_id, false);
@endcode

A collection pause stops collecting new invoices without changing the
subscription's status to `paused`. Choose what happens to invoices created
during the pause:

| `CollectionPauseBehavior` | New invoices |
|---------------------------|--------------|
| `Void` | Voided, so they cannot be collected. |
| `KeepAsDraft` | Kept as drafts, without automatic collection. |
| `MarkUncollectible` | Marked as uncollectible. |

@code{.cpp}
stripekit::PauseSubscriptionCollectionRequest pause;
pause.behavior = stripekit::CollectionPauseBehavior::Void;
// Optional positive Unix timestamp; otherwise pause until explicitly resumed.
pause.resumes_at = resume_timestamp;
const auto paused = co_await stripe->subscriptions().pause_collection_async(subscription_id, pause);
const auto resumed = co_await stripe->subscriptions().resume_collection_async(subscription_id);
@endcode

Read `subscription.pause_collection` to inspect the saved behavior and optional
`resumes_at_unix`. It is absent when collection is not paused. Resuming affects
future billing; it does not automatically collect invoices from the pause.
Existing invoices, including their retries, are not changed by setting a pause.
Handle them separately when stopping all collection. Application access rules
also remain your responsibility.

@section invoices_manage List and clear unpaid invoices

Invoice listing returns one page at a time. Set `subscription_id` to limit the
results to one subscription; omit it to list all invoices:

@code{.cpp}
stripekit::ListInvoicesRequest filter;
filter.subscription_id = subscription_id;
filter.limit = 100;
auto page = co_await stripe->invoices().list_async(filter);
if (page.has_more) {
    filter.starting_after = page.data.back().id;
    page = co_await stripe->invoices().list_async(filter);
}
@endcode

To discard an unpaid invoice, inspect its status first:

- Void an `open` invoice with `invoices().void_async(invoice_id)`.
- Delete a standalone `draft` with `invoices().delete_async(invoice_id)`.
- Stripe cannot delete subscription-linked drafts. Use
  `invoices().finalize_async(invoice_id)` to finalize with `auto_advance=false`,
  then void the resulting `open` invoice. Finalization alone does not discard it.
- Do not delete or void paid invoices. Refund a collected payment instead.

Check returned IDs, ownership, and status before proceeding. Finalization returns
the optional `invoice.auto_advance` setting so callers can confirm automatic
collection is disabled; deletion returns a `DeletedInvoice` confirmation.
Use a saved idempotency key for each distinct POST operation, and reuse it only
when retrying that same operation. Stripe failures throw `StripeApiException`.
Callbacks receive either the typed result or a failure: `list()`, `finalize()`,
`void_invoice()`, and `delete_invoice()`.

Applications implementing custom `Subscriptions` or `Invoices` interfaces must
implement these new methods as well as their coroutine forms.

@section invoice_payments Find the payment behind an invoice

A **charge** is an attempt to collect money. Retrieve one with
`stripe->charges().retrieve_async(charge_id)` to check its customer, amount,
payment status, and whether it is disputed. To find a customer's charges:

@code{.cpp}
stripekit::ListChargesRequest request;
request.customer_id = customer_id;
request.limit = 100;
const auto page = co_await stripe->charges().list_async(request);
@endcode

Only one page is returned. If `page.has_more` is true, set
`request.starting_after` to `page.data.back().id` and request the next page.
Page sizes must be from 1 to 100. An empty customer filter lists all customers.
Verify that returned charges belong to the customer your application expects.
The charge's amounts, including `amount_refunded`, use the currency's smallest unit.

On older Stripe API versions a charge can contain `invoice_id`. From
`2025-03-31.basil`, use its `payment_intent_id` to find the invoice allocations:

@code{.cpp}
stripekit::ListInvoicePaymentsRequest request;
request.payment_intent_id = charge.payment_intent_id;
request.limit = 100;
stripekit::RequestOptions options;
options.headers["Stripe-Version"] = "2025-03-31.basil";
const auto page = co_await stripe->invoice_payments().list_async(request, options);
@endcode

Use a nonempty PaymentIntent ID when looking up a particular payment. Omitting
the filter lists all invoice payments. Pagination works like charge listing,
using the last allocation ID as the next cursor. This endpoint requires a Stripe
API version that supports invoice payments, such as `2025-03-31.basil` or later.

Each allocation identifies its invoice, payment type and matching payment
reference, status, currency, and requested and paid amounts. `amount_paid` is
absent until paid; it is not assumed to be zero. A payment can pay more than
one invoice, so inspect all pages rather than assuming a single invoice owns it.
Unknown payment types remain available in `payment_type`.

All three read operations also have callback forms: `charges().retrieve()`,
`charges().list()`, and `invoice_payments().list()`. They report either the
result or the failure exactly once. Invalid IDs and page sizes fail before
sending a request; Stripe rejections and transport failures are reported too.

@section refunds Return money and recover refund requests

Use `stripe->refunds()` to return money for a charge and read its refund history:

@code{.cpp}
stripekit::CreateRefundRequest request;
request.charge_id = charge_id;
request.amount = 500; // Smallest currency unit; omit to refund the remaining balance.
request.metadata["operation"] = saved_operation_id;
stripekit::RequestOptions options;
options.idempotency_key = saved_operation_id;
const auto refund = co_await stripe->refunds().create_async(request, options);
@endcode

Verify that the charge belongs to the expected customer before requesting a
refund. Stripe rejects amounts above the remaining refundable balance. Amounts
must be positive when supplied. Save the operation ID and retry with the same
idempotency key and request values, rather than issuing another refund.

The result includes the refund ID, charge and payment-intent references, amount,
currency, status, metadata, and a failure reason when Stripe supplies one.
Only `succeeded` confirms completion. `pending` and `requires_action` are not
complete, but still reserve the requested amount; `failed` and `canceled` do
not. Unknown status strings are preserved for the application to handle explicitly.

@code{.cpp}
stripekit::ListRefundsRequest filter;
filter.charge_id = charge_id;
filter.limit = 100;
const auto page = co_await stripe->refunds().list_async(filter);
@endcode

Listing returns one page, including failed and pending refunds. When `has_more`
is true, use the last refund ID as `starting_after` for the next page. Page sizes
must be from 1 to 100. An empty charge filter lists all refunds.

Keep a durable operation ID in metadata to find a prior refund after Stripe's
idempotency retention period. Read all pages and verify the saved charge and
amount before accepting a match. Do not retry creation merely because a saved
refund has not succeeded yet.

The callback methods `refunds().create()` and `refunds().list()` report the same
results and failures exactly once. Custom `StripeClient` implementations must
provide `refunds()` as well as `charges()` and `invoice_payments()`. Custom
response parsers inherit JSON decoding for these objects and can override it.

@section portal_next Where to go next

- @ref products_and_prices — create the products and prices offered for plan switching.
- @ref quickstart — receiving the webhooks that report portal and invoice changes.
- @ref client — the full reference for the types above.
