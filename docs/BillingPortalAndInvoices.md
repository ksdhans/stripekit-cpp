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
is useful for "Fix payment" or "Change plan" buttons. Stripe currently offers
these six deep-link flow types:

| Flow type | What the customer sees | Needs a subscription ID |
|-----------|------------------------|-------------------------|
| `PaymentMethodUpdate` | Add or replace a payment method. | No |
| `CustomerUpdate` | Update billing information, such as a billing address or tax ID. | No |
| `SubscriptionCancel` | Confirm cancelling a subscription. | Yes |
| `SubscriptionPause` | Pause a subscription. The portal configuration must enable pausing. | Yes |
| `SubscriptionUpdate` | Pick a new plan from the configured list. | Yes |
| `SubscriptionUpdateConfirm` | Confirm a plan change your application already chose. | Yes |

To send a customer straight to billing details:

@code
stripekit::CreateBillingPortalSessionRequest request;
request.customer_id = customer_id;
request.return_url = "https://example.com/billing";
request.flow = stripekit::BillingPortalFlow{
    .type = stripekit::BillingPortalFlowType::CustomerUpdate
};
const auto session = co_await stripe->billing_portal().sessions().create_async(request);
// Redirect the customer to session.url
@endcode

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
| `subtotal`, `amount_discount`, `amount_tax`, `total` | Pre-tax amount, discounts, tax, and final total when the API version returns them. |
| `automatic_tax` | Whether Stripe calculated tax and any status or disabled reason it returned. |
| `last_finalization_error_code` | Stripe's code when draft finalization failed. |
| `line_items` | Price IDs, quantities, and amounts returned with the invoice. |
| `hosted_invoice_url` | A Stripe page where the customer can pay the invoice, including completing a 3D Secure check. |
| `invoice_pdf` | A link to download the invoice as a PDF. |
| `billing_reason` | Why it was created, such as `subscription_cycle` or `subscription_update`. |
| `attempt_count` | How many payment attempts were made. A manual payment after a failure does not always increase it. |
| `next_payment_attempt_unix` | When Stripe will retry automatically; empty when no retry is planned. |
| `due_date_unix` | When payment is due, for invoices sent to the customer rather than charged automatically. |

`hosted_invoice_url` is the simplest way to recover a payment that needs the
customer's action, such as an extra bank authentication. Send the customer
there, then wait for the `invoice.paid` webhook.

@section invoice_tax_recovery Correct a draft invoice before finalization

StripeKit reports automatic-tax status and invoice-finalization error codes so
an application can distinguish an address problem from an ordinary payment
failure. Stripe may return tax fields only for API versions that support them;
an absent status is not a successful tax calculation.

After correcting the customer's address, retrieve the customer or update its
`BillingAddress` with `validate_tax_location = true`. This asks Stripe to
validate the new location and returns its tax-location status and source when
available. For a draft invoice, restore automatic tax and then retry
finalization:

@code{.cpp}
stripekit::UpdateInvoiceRequest correction;
correction.automatic_tax_enabled = true;
co_await stripe->invoices().update_async(invoice_id, correction);
const auto finalized = co_await stripe->invoices().finalize_async(invoice_id);
@endcode

Verify the returned invoice's automatic-tax state and amounts. Stripe may
reject an invalid location during finalization; do not treat a zero-tax total
or a paid invoice as proof that required tax was calculated.

Invoice responses may include only the first page of line items. To read all
prices and quantities, call `invoices().list_line_items_async(invoice_id)` and
continue with the last item's `id` while `has_more` is true.

@section invoice_items Read and delete invoice items

Use `stripe->invoice_items()` to find individual charges or credits and remove
ones that should no longer be billed. A **pending invoice item** has not yet been
added to an invoice. Invoice-item IDs start with `ii_`; invoice line IDs start
with `il_` and cannot be passed to these operations.

To read a customer's pending items, request each page until `has_more` is false:

@code{.cpp}
stripekit::ListInvoiceItemsRequest request;
request.customer_id = customer_id;
request.pending = true;
request.limit = 100;
for (;;) {
    const auto page = co_await stripe->invoice_items().list_async(request);
    for (const auto& item : page.data) {
        // Check each item's customer, subscription, and service period here.
    }
    if (!page.has_more) break;
    request.starting_after = page.data.back().id;
}
@endcode

Page sizes are from 1 to 100. Leave `pending` unset to include both pending items
and items already on an invoice; set it to `false` for only the latter. Set
`request.invoice_id` to list items on a particular invoice, leaving `customer_id`
empty: Stripe rejects requests that specify both. With neither filter set,
listing includes items for all customers.

Stripe does not offer a subscription filter for this list endpoint. To find a
subscription's items, filter by customer, read all pages, and check each item's
subscription details:

@code{.cpp}
const auto item = co_await stripe->invoice_items().retrieve_async(invoice_item_id);
if (item.parent && item.parent->subscription_details) {
    const auto& owner = *item.parent->subscription_details;
    // Compare owner.subscription_id and owner.subscription_item_id with your saved IDs.
}
@endcode

Use these fields to decide whether an item is the adjustment you are looking for:

| Field | What to check |
|-------|---------------|
| `customer_id` | The customer the item belongs to. |
| `invoice_id` | The assigned invoice, or an empty string for a pending item. |
| `parent->subscription_details` | The subscription and subscription-item IDs, when supplied. Check both optional objects before using them. |
| `amount`, `currency` | The amount in the currency's smallest unit. A negative amount alone does not identify a particular credit. |
| `period.start_unix`, `period.end_unix` | The service dates, in seconds since January 1, 1970 (UTC). |
| `proration` | Whether Stripe created the item as a billing adjustment, such as a credit for unused service. |
| `metadata` | Any application-defined key/value pairs attached to the item. |

For a credit, `proration_details->credited_items` can link back to the original
charge. Check both optional objects before reading the reference:

- If `type` is `"invoice_item"`, `invoice_item_id` identifies the original pending
  charge.
- If `type` is `"invoice_line_items"`, check `invoice_line_item_details`, then
  read its `invoice_id` and `invoice_line_item_ids` for the original billed lines.

These references and subscription details are not always supplied. Do not guess
a missing reference or delete an item whose purpose is unclear. See Stripe's
[invoice-item reference](https://docs.stripe.com/api/invoiceitems/object) for the
full object.

After identifying the item to remove:

@code{.cpp}
const auto deleted =
    co_await stripe->invoice_items().delete_async(confirmed_invoice_item_id);
// deleted.id is the removed item's ID; deleted.deleted confirms deletion.
@endcode

Only pending items and items on draft invoices can be deleted. Stripe rejections,
including not found, throw `StripeApiException`; invalid IDs or page sizes throw
`ConfigurationException`. Network failures are also reported.

The callback forms are `invoice_items().list(request, options, completion)`,
`retrieve(id, options, completion)`, and
`delete_invoice_item(id, options, completion)`. Each callback receives either the
result or a failure exactly once.

@section subscription_billed_until Read a subscription item's billed-through date

`SubscriptionItem::billed_until_unix` is the date through which Stripe reports an
item has been billed. Billed does not necessarily mean paid, and this date can
differ from the item's current period end.

Stripe does not return this field by default. Request it using `expand`:

@code{.cpp}
stripekit::RetrieveSubscriptionRequest request;
request.expand = { "items.data.billed_until" };
const auto subscription =
    co_await stripe->subscriptions().retrieve_async(subscription_id, request, {});
@endcode

Read `billed_until_unix` on each entry in `subscription.items`. It contains Unix
seconds when supplied, or `std::nullopt` when Stripe omits it or returns null.
Check the optional value before using it; do not substitute the current period
end for an absent billed-through date. Crediting an item's entire first period
can leave no billed-through date. If `items_has_more` is true, the returned items
are not the complete subscription.

Pass `{}` for request options when you do not need custom headers or a timeout.
For a normal retrieval without extra fields, use
`subscriptions().retrieve_async(subscription_id)`. The callback form with
expansions is `subscriptions().retrieve(subscription_id, request, options, completion)`.

@subsection held_invoice_recovery Recover a held renewal invoice

A **held invoice** here is a draft with `auto_advance = false`, so Stripe will not
automatically finalize or collect it. If you pause service while correcting its
tax settings, the pause can create a pending credit for the same service period.
Use the invoice-item operations to remove that specific credit before resuming.
This requires flexible billing and `charge_automatically`; see
@ref subscription_pause_requirements for pause requirements.

Before pausing, save the subscription and item IDs, the held invoice's service
dates and line IDs, and the customer's existing pending invoice-item IDs. Pause
with these settings to credit the whole current period without billing usage:

@code{.cpp}
stripekit::PauseSubscriptionRequest pause;
pause.bill_for.unused_time_from.type = "item_current_period_start";
pause.bill_for.outstanding_usage_through.type = "none";
pause.invoicing_behavior = "pending_invoice_item";
co_await stripe->subscriptions().pause_async(subscription_id, pause);
@endcode

Retrieve the subscription with `items.data.billed_until` expanded to check that
the billed-through date has moved back. Read the pending invoice-item pages
again and find the new credit using its subscription and item IDs, service
dates, proration flag, and original-charge references when supplied. Do not
select it by negative amount alone. Leave unrelated items untouched, and stop
if more than one item could match.

Delete the identified credit, then retrieve the expanded subscription again to
confirm the billed-through date remains rolled back. After correcting the
customer's tax location, enable automatic tax on the held draft and the
subscription. See @ref invoice_tax_recovery for correcting a draft.

Choose how to resume based on whether the held service period has ended:

| Recovery time | Held invoice | `ResumeSubscriptionRequest` settings |
|---------------|--------------|--------------------------------------|
| Before that period ends | Collect the corrected invoice and confirm payment first. | `billing_cycle_anchor = ResumeSubscriptionBillingCycleAnchor{ "unchanged" }`, `proration_behavior = ProrationBehavior::None`. |
| After that period ends | Finalize without collection, then void the corrected invoice. | `billing_cycle_anchor = ResumeSubscriptionBillingCycleAnchor{ "now" }`, `proration_behavior = ProrationBehavior::None`. |

Check the resulting invoices, tax, service dates, and subsequent renewal before
restoring paid access. Resumption alone is not proof of payment. To use automatic
collection for the corrected draft, set its `auto_advance` to `true` and wait for
payment confirmation before resuming. No invoice needs to be created manually.

@section subscription_schedules Change a price at a future renewal

Use a **subscription schedule** when a price should change at renewal rather
than immediately. A schedule is a sequence of **phases**: each phase has start
and end dates, prices, and quantities. Stripe applies those settings when the
phase starts.

Create a schedule from an existing subscription to copy its current billing
settings. This does not create another subscription or immediately change its
price. You can also include `pause_schedules` when creating from a subscription;
set metadata and future phases in a separate update.

@code{.cpp}
stripekit::RequestOptions options;
options.idempotency_key = saved_operation_id + ":create";
const auto schedule = co_await stripe->subscription_schedules().create_async(
    { .from_subscription = subscription_id }, options);
@endcode

If a subscription already has a schedule, read it with
`subscription_schedules().retrieve_async(subscription.schedule_id)` rather than
creating another. Do not overwrite changes scheduled by another workflow.

To keep the current price until renewal, then use a new price for one month,
save two phases. This example assumes an active subscription with one
fixed-price item, a positive quantity, and no discounts, tax settings, active trial, or
pending changes. Check those conditions before creating the schedule.
Use the returned `current_phase` dates after checking that it is present:

@code{.cpp}
const auto& current = schedule.current_phase.value();
stripekit::UpdateSubscriptionScheduleRequest update;
update.phases = {
    { .start_date = current.start_date_unix,
      .end_date = current.end_date_unix,
      .items = { { current_price_id, quantity } } },
    { .start_date = current.end_date_unix,
      .duration = stripekit::SubscriptionSchedulePhaseDuration{ "month", 1 },
      .items = { { replacement_price_id, quantity } } }
};
update.metadata["operation"] = saved_operation_id;
options.idempotency_key = saved_operation_id + ":update";
const auto changed = co_await stripe->subscription_schedules().update_async(
    schedule.id, update, options);
@endcode

Things to know:

- Supplying phases replaces the current and future phases, rather than changing
  just one field. Include every price, quantity, automatic-tax setting, and
  discount you want to keep. Reuse the returned coupon or promotion-code IDs so
  Stripe retains the discount's configured duration.
- Specify either `end_date` or `duration` for each phase, not both.
  A duration of `{ "month", 1 }` means one calendar month, independently of
  the prices' billing intervals.
  Dates are Unix seconds, and adjacent phases must meet at the same timestamp.
- When replacing phases, `proration_behavior` defaults to
  `ProrationBehavior::None`: no partial-period charges or credits. Set it on
  the request to control adjustments to the current phase, and on each phase
  to control adjustments when that phase starts.
- When replacing phases, `end_behavior` defaults to
  `SubscriptionScheduleEndBehavior::Release`. After the final phase, the
  subscription keeps running at its last settings; it does not revert to the
  old price. `Cancel` ends the subscription instead.
- Schedule metadata belongs to the schedule. Phase metadata is applied to the
  subscription when that phase starts.
- Read `schedule_id`, `discount_ids`, `default_tax_rate_ids`, and
  `automatic_tax_enabled` on the subscription before changing its billing.
  Items also report their discount and tax rate IDs.

Save separate retry keys for creation and update. If creation succeeds but the
response is lost, retry it with the same key before attempting to create another
schedule. Stripe can stop remembering keys after 24 hours; if a schedule is
already attached and you cannot establish that it belongs to your operation,
leave it unchanged and investigate.

Check the returned schedule's customer, subscription, phases, quantities, and
end behavior before confirming the change in your application. A scheduled
change is not proof of a later payment; follow subscription and invoice webhooks.
After release, `subscription_id` is empty and `released_subscription_id`
identifies the continuing subscription.

The callback methods are `create()`, `retrieve()`, and `update()`. They report a
schedule or a failure exactly once. Invalid dates, empty IDs, or invalid
quantities fail with `ConfigurationException`; Stripe rejections throw
`StripeApiException`.

@section subscription_pause Pause and resume subscriptions

Use `pause_async()` when a customer temporarily stops using the service.
It changes the subscription's `status` to `paused` and stops new invoices
without canceling the subscription. Your application must stop service access.
Use `pause_collection_async()` instead if the customer should keep access;
it leaves the subscription's status and invoice generation unchanged.

@subsection subscription_pause_requirements Before you pause

Pausing requires **flexible billing mode** and `charge_automatically`
collection. Scheduled pauses also require these settings on the schedule and
every phase. Check `subscription.billing_mode` and `collection_method`.
Stripe rejects subscriptions that cannot pause, such as those with an active
subscription-level trial. See Stripe's
[pause requirements](https://docs.stripe.com/billing/subscriptions/pause#limitations)
for the full list.

For new Checkout subscriptions, set
`CreateCheckoutSessionRequest::subscription_billing_mode` to
`SubscriptionBillingMode{ "flexible" }`. To upgrade an existing subscription:

@code{.cpp}
const auto migrated = co_await stripe->subscriptions().migrate_async(
    subscription_id, {}, { .idempotency_key = "saved-billing-mode-migration" });
@endcode

For classic billing, call `migrate_async()` before pausing; Stripe checks
whether the upgrade is allowed. To change collection, set
`UpdateSubscriptionRequest::collection_method`. Schedules report collection
settings in `default_collection_method` and each phase's `collection_method`.

@subsection subscription_pause_now Pause immediately

For a subscription **without an attached schedule**:

@code{.cpp}
stripekit::PauseSubscriptionRequest pause;
pause.bill_for.unused_time_from.type = "now";
pause.bill_for.outstanding_usage_through.type = "now";
pause.invoicing_behavior = "pending_invoice_item";
const auto paused = co_await stripe->subscriptions().pause_async(
    subscription_id, pause, { .idempotency_key = "saved-pause" });
@endcode

Choose how to handle the current billing period:

| Setting | Choices |
|---------|---------|
| `bill_for.unused_time_from.type` | Credit unused time from `now` (default) or `item_current_period_start`; `none` gives no credit. |
| `bill_for.outstanding_usage_through.type` | Bill metered usage through `now` (default); `none` skips it. |
| `invoicing_behavior` | `invoice` bills immediately; `pending_invoice_item` (default) saves credits and charges for a later invoice. |

Existing invoices and payment retries continue. Coupon validity periods also
continue during a pause.

@subsection subscription_resume_now Resume immediately

For a paused subscription **without an attached schedule**:

@code{.cpp}
stripekit::ResumeSubscriptionRequest resume;
resume.billing_cycle_anchor = stripekit::ResumeSubscriptionBillingCycleAnchor{ "now" };
resume.payment_behavior = "resume_on_payment_success";
resume.proration_behavior = stripekit::ProrationBehavior::CreateProrations;
const auto resumed = co_await stripe->subscriptions().resume_async(
    subscription_id, resume, { .idempotency_key = "saved-resume" });
@endcode

The billing anchor accepts `now` (default) to start a new billing period, or
`unchanged` to keep the existing billing dates. Use `proration_date` to choose
the Unix timestamp for calculating partial-period adjustments.

With `resume_on_payment_success`, Stripe attempts payment if it creates a
resume invoice. The subscription stays paused while payment is outstanding.
Without an explicit payment behavior, `resume_async()` uses
`resume_on_payment_attempt`: Stripe finalizes any resume invoice without
attempting payment. If no invoice is needed, the subscription can become
active immediately.

A successful request **does not mean the subscription is active**. Check
`status`, `pending_update`, and `latest_invoice_id` before restoring access.

@subsection subscription_pause_schedules Schedule pauses and resume attempts

If `subscription.schedule_id` is present, read and update that schedule instead
of using the subscription's `pause_async()` or `resume_async()` methods.
Do not create another schedule or release the existing one just to pause.

This example adds a new pause and resume date to an existing schedule.
If it already has a pause schedule, edit that entry as shown in
@ref subscription_pause_edit instead.
Leave `phases`, `end_behavior`, and `proration_behavior` unset to keep its
current prices, future phases, and billing settings:

@code{.cpp}
stripekit::SubscriptionSchedulePauseScheduleRequest entry;
entry.pause = stripekit::SubscriptionSchedulePauseRequest{
    .pause_at = stripekit::SubscriptionPauseTime{ .type = "now" } };
entry.resume = stripekit::SubscriptionScheduleResumeRequest{
    .resume_at = stripekit::SubscriptionPauseTime{
        .type = "timestamp", .timestamp = future_resume_unix_seconds } };

stripekit::UpdateSubscriptionScheduleRequest changes;
changes.pause_schedules =
    std::vector<stripekit::SubscriptionSchedulePauseScheduleRequest>{ entry };
const auto changed = co_await stripe->subscription_schedules().update_async(
    subscription.schedule_id, changes, { .idempotency_key = "saved-scheduled-pause" });
@endcode

When there is **no attached schedule**, create one with the same pause entry:

@code{.cpp}
const auto created = co_await stripe->subscription_schedules().create_async(
    { .from_subscription = subscription_id, .pause_schedules = { entry } });
@endcode

- Use `pause_at.type = "timestamp"` with `timestamp` for a future pause.
  When creating a pause, omit `resume` to leave it paused until you resume it.
- To schedule a resume for an already-paused subscription, omit `pause` and
  supply only `resume`.
- A resume time accepts `now`, `timestamp`, or `duration`. For duration, set
  `duration = SubscriptionPauseDuration{ "month", 1 }` to start a resume
  attempt one month after pausing.
- Pause `settings` use the same billing choices as `pause_async()`, but use
  `pause_at` instead of `now` for billing points. Resume `settings` accept
  `billing_cycle_anchor` (`resume_at` or `unchanged`), `payment_behavior`, and
  `proration_behavior`.
- Scheduled resumes default to `resume_at`, `resume_on_payment_success`, and
  `create_prorations`. Unlike `resume_async()`, they attempt payment by default.
- Resume must be after pause. Dates must fit the schedule's start and end dates,
  avoid trials, and be no more than 20 years in the future. Stripe checks these
  dates when you save the schedule.

@subsection subscription_pause_edit Change or remove planned actions

Each schedule supports one pause schedule. **Include its existing `key` when
editing** to keep settings you do not supply. Without that key, your entry
replaces the pause schedule. For example, to resume now without changing the
saved billing settings:

@code{.cpp}
stripekit::SubscriptionSchedulePauseScheduleRequest edit;
edit.key = changed.pause_schedules.at(0).key;
edit.resume = stripekit::SubscriptionScheduleResumeRequest{
    .resume_at = stripekit::SubscriptionPauseTime{ .type = "now" } };
changes.pause_schedules =
    std::vector<stripekit::SubscriptionSchedulePauseScheduleRequest>{ edit };
const auto result = co_await stripe->subscription_schedules().update_async(
    changed.id, changes);
@endcode

To remove only a planned resume, send the entry's key and `clear_resume = true`,
with `resume` unset. To clear the whole pause schedule, set `pause_schedules`
to an empty vector. Leaving `pause_schedules` unset makes no pause changes.
Removing a planned action does not undo a pause or resume a subscription.

The returned `pause` and `resume` report their dates and `status.type`.
Read `status.error` if an action fails. If a resume is `pending` or
`requires_action`, handle its unpaid invoice before editing or removing the
attempt. Even a `succeeded` resume can leave the subscription `past_due`;
check subscription and invoice status before restoring access.
Schedule phases can still change while the subscription is paused.

Follow verified `customer.subscription.paused`, `customer.subscription.resumed`,
and `customer.subscription.updated` webhooks for access changes. Merely
scheduling an action does not emit paused or resumed events. A paused
subscription's `status_details.paused` reports its transition time and reason.
The subscription methods also have callback forms: `pause()`, `resume()`, and
`migrate()`. They report a result or failure exactly once.

See Stripe's [scheduled pause guide](https://docs.stripe.com/billing/subscriptions/pause/schedules)
for date restrictions, payment outcomes, and rescheduling failed actions.

@section collection_controls Schedule cancellation or pause collection

Use period-end cancellation to let an existing subscription finish its current
billing period without renewing. Clear it before that period ends if the
customer changes their mind:

@code{.cpp}
const auto scheduled = co_await stripe->subscriptions().set_cancel_at_period_end_async(
    subscription_id, true, { .idempotency_key = "saved-cancellation-key" });
const auto continued = co_await stripe->subscriptions().set_cancel_at_period_end_async(
    subscription_id, false);
@endcode

Check `cancel_at_period_end` on the returned subscription to confirm the setting.
Scheduling cancellation does not end the subscription immediately.

For a specific cancellation date, use the general subscription update API:

@code{.cpp}
stripekit::UpdateSubscriptionRequest changes;
changes.cancel_at = cancellation_unix_seconds;
changes.proration_behavior = stripekit::ProrationBehavior::CreateProrations;
const auto dated = co_await stripe->subscriptions().update_async(
    subscription_id, changes, { .idempotency_key = "dated-cancellation" });
@endcode

`cancel_at` accepts a positive Unix timestamp or a `SubscriptionCancelAt` value:

| Value | Meaning |
|-------|---------|
| `MinPeriodEnd` | Earliest current period end among the subscription's items. |
| `MaxPeriodEnd` | Latest current period end among its items. |
| `MaxBilledUntil` | Latest date through which an item has been billed, including advance billing. Billed does not necessarily mean paid. |
| `Clear` | Remove the scheduled cancellation date by sending an empty `cancel_at`. |

Stripe resolves named billing boundaries to a timestamp when it processes the
request. Mixed billing intervals and advance billing require flexible billing mode. Inspect
`Subscription::cancel_at_unix` for the resulting date; it is absent when Stripe
returns no scheduled date.

Choose any supported `ProrationBehavior`, or omit it to use Stripe's default.
Moving cancellation into the current period can produce adjustments. Stripe
also documents that cancellation during a future billing period produces
prorations for that period even if you request `None`.

To clear an explicit date:

@code{.cpp}
stripekit::UpdateSubscriptionRequest continue_billing;
continue_billing.cancel_at = stripekit::SubscriptionCancelAt::Clear;
const auto continued = co_await stripe->subscriptions().update_async(
    subscription_id, continue_billing);
@endcode

The general update request also accepts `cancel_at_period_end = true` or `false`,
without requiring item changes. Leave cancellation fields unset for unrelated
metadata, automatic-tax, or item updates. Metadata entries with empty values
remove those keys; unrelated metadata remains unchanged.

The callback equivalent is `subscriptions().update(id, request, options, completion)`.
Both forms preserve request options and report validation, transport, and Stripe
errors. Scheduling cancellation does not prove that invoices have been paid;
follow invoice and subscription webhooks. Once a subscription has actually
canceled, clearing the date does not reactivate it.

Use a collection pause to temporarily stop charging for future billing periods
without ending the subscription. This does not change its status to `paused`.
Choose what happens to invoices created during the pause:

| `CollectionPauseBehavior` | New invoices |
|---------------------------|--------------|
| `Void` | Voided, so they cannot be collected. |
| `KeepAsDraft` | Kept as drafts, without automatic collection. |
| `MarkUncollectible` | Recorded as unpaid, with automatic payment attempts stopped. |

@code{.cpp}
stripekit::PauseSubscriptionCollectionRequest pause;
pause.behavior = stripekit::CollectionPauseBehavior::Void;
// Optional future date, in seconds since January 1, 1970 (UTC).
// Omit it to pause until your application explicitly resumes collection.
pause.resumes_at = resume_timestamp;
const auto paused = co_await stripe->subscriptions().pause_collection_async(subscription_id, pause);
@endcode

To resume before that date, or end a pause with no resume date:

@code{.cpp}
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
results to one subscription; omit it to list all invoices. Page sizes must be
from 1 to 100. To read further pages, pass the last invoice ID from the previous
page as `starting_after`:

@code{.cpp}
stripekit::ListInvoicesRequest filter;
filter.subscription_id = subscription_id;
filter.limit = 100;
auto page = co_await stripe->invoices().list_async(filter);
while (page.has_more) {
    // Process page.data before moving to the next page.
    filter.starting_after = page.data.back().id;
    page = co_await stripe->invoices().list_async(filter);
}
// Process the final page.data too.
@endcode

To discard an unpaid invoice, inspect its status first:

- Void an `open` invoice with `invoices().void_async(invoice_id)`.
- Delete a standalone `draft` with `invoices().delete_async(invoice_id)`.
- Stripe cannot delete subscription-linked drafts. Use
  `invoices().finalize_async(invoice_id)` to finalize with `auto_advance=false`,
  then void the resulting `open` invoice. Finalization alone does not discard it.
- Do not delete or void paid invoices. Refund a collected payment instead.

Verify that each invoice belongs to the expected customer and subscription
before changing it. Check the returned invoice's ID and status to confirm the
change. After finalization, also check that `auto_advance` is present and `false`
before voiding. A successful deletion returns the invoice ID and `deleted=true`.

An **idempotency key** is a unique retry key that lets Stripe recognise a repeated
request instead of performing the same change again. Save a separate key for
each cancellation, pause, resume, finalization, or voiding operation. Reuse it
with the same request values when retrying that operation.

Stripe rejections throw `StripeApiException`; invalid IDs or page sizes throw
`ConfigurationException`. Network failures are also reported to the caller.
Callbacks receive either the typed result or a failure: `list()`, `finalize()`,
`void_invoice()`, and `delete_invoice()`.

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

Use the charge's `payment_intent_id` to find its invoice payment records.
A **PaymentIntent** tracks a payment through any required customer
authentication and payment attempts. An **invoice payment record** links that
payment to an invoice and shows how much of the payment was applied to it:

@code{.cpp}
stripekit::ListInvoicePaymentsRequest request;
request.payment_intent_id = charge.payment_intent_id;
request.limit = 100;
const auto page = co_await stripe->invoice_payments().list_async(request);
@endcode

Use a nonempty PaymentIntent ID when looking up a particular payment. Omitting
the filter lists all invoice payments. Pagination works like charge listing,
using the last record's ID as `starting_after`. StripeKit's default API version
supports invoice payments.

Each record identifies its invoice, payment type and matching payment
reference, status, currency, and requested and paid amounts. `amount_paid` is
absent until paid; it is not assumed to be zero. A payment can pay more than
one invoice, so inspect all pages rather than assuming a single invoice owns it.
Unknown payment types remain available in `payment_type`.

All three read operations also have callback forms: `charges().retrieve()`,
`charges().list()`, and `invoice_payments().list()`. They report either the
result or the failure exactly once. Invalid IDs and page sizes fail before
sending a request; Stripe rejections and transport failures are reported too.

@section refunds Return money and check earlier refund requests

Use `stripe->refunds()` to return money for a charge and read its refund history.
Generate a unique operation ID and save it with the requested charge, amount,
and currency in your database before contacting Stripe:

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

Include the saved operation ID in the refund's metadata, as shown above, so you
can find that refund again if the response is lost. Stripe can stop remembering
an idempotency key after 24 hours; reusing an old key can then create another
refund. Before retrying an old request, read all refund pages and look for the
saved operation ID. Verify its charge, amount, and currency before accepting a
match. Do not create another refund merely because an existing one has not
succeeded yet.

The callback methods `refunds().create()` and `refunds().list()` report the same
results and failures exactly once.

@section portal_next Where to go next

- @ref products_and_prices — create the products and prices offered for plan switching.
- @ref quickstart — receiving the webhooks that report portal and invoice changes.
- @ref client — the full reference for the types above.
