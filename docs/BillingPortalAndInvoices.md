@page billing_portal_and_invoices Billing Portal and Invoices

@tableofcontents

The **billing portal** is a Stripe-hosted page where customers manage their own
billing: they can update their card, download past invoices, switch plans, or
cancel. Your application never handles card details. It asks Stripe for a
short-lived portal link and sends the customer there.

An **invoice** is Stripe's bill for one billing period or plan change. Reading
an invoice tells your application how much is still owed, whether Stripe will
retry the payment, and where the customer can pay it.

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

@section portal_next Where to go next

- @ref products_and_prices — create the products and prices offered for plan switching.
- @ref quickstart — receiving the webhooks that report portal and invoice changes.
- @ref client — the full reference for the types above.
