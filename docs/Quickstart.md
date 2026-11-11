@page quickstart Quickstart

@tableofcontents

This page explains the handful of ideas you need, then walks through building 
a small subscription feature: a page where a customer signs up and pays, and 
code in your server that finds out once they have.

By the end you will have used the four pieces every StripeKit integration
needs: a client to talk to Stripe, a Checkout session to take payment, a
webhook handler to hear back from Stripe, and a store that stops that handler
running twice for the same event.

This page uses the [Drogon](https://drogon.org/) adapter, since that is the
one StripeKit ships with. The `examples/` directory in the source tree has
runnable versions of everything shown here: `CheckoutSessionCli` and
`WebhookSignature` are the same two things this page builds, `DrogonCheckoutDemo`
puts them together into one small server, and `BillingPortalCli` shows the
billing portal mentioned at the end.

Everything here uses Stripe test mode, which behaves exactly like the real
thing but never touches real money. Test API keys start with `sk_test_`, and
you should not have to think about "test vs. live" again until you are ready
to go into production.

@section quickstart_concepts A few things worth knowing first

If you already know what Checkout, webhooks, and subscriptions are, skip ahead
to @ref quickstart_prerequisites.

**Stripe hosts the payment page.** Your application never touches a card
number. Instead, you ask Stripe to create a "Checkout session", send the
customer to the web page Stripe gives you, and Stripe takes care of collecting
payment details safely.

**Stripe tells you what happened by calling your server.** When a customer
pays, or a subscription renews, or a card is declined, Stripe sends an HTTP
request to a URL you register — this is called a "webhook". Your server reads
that request and reacts to it, for example by giving the customer access.

**A subscription is an ongoing agreement to pay.** A customer subscribes once,
through Checkout, and Stripe then bills them automatically every month (or
week, or year) until they cancel. StripeKit's Checkout support handles both recurring
subscriptions (`mode = "subscription"`) and one-time purchases (`mode = "payment"`).

@section quickstart_prerequisites Before you start

You need:

- a free [Stripe account](https://dashboard.stripe.com/register), used in test
  mode,
- a **secret API key** from that account, which looks like `sk_test_...` and
  is found under Developers > API keys in the Stripe dashboard,
- a **price**, which is Stripe's record of what you charge and how often —
  create one under Product catalog in the dashboard, or through the API as shown
  in @ref products_and_prices, and keep its id, which looks like `price_...`,
- the [Stripe CLI](https://docs.stripe.com/stripe-cli), a small command-line
  program used later in this guide to send test webhooks to your machine.

Keep the API key out of your source code. Put it in an environment variable
instead, and read it from there:

@code{.sh}
export STRIPE_SECRET_KEY=sk_test_...
@endcode

The examples in the source tree read this same variable.

@section quickstart_build Adding StripeKit to your project

If you have not built StripeKit yet, see the "Build" section of the project
`README.md` first.

Once it is built and installed, add it to your project's CMake file:

@code{.cmake}
find_package(StripeKit CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE StripeKit::stripekit StripeKit::drogon)
@endcode

`StripeKit::stripekit` is the part described on this page: customers,
Checkout, subscriptions, and webhooks. `StripeKit::drogon` is the adapter that
lets those pieces talk to Stripe using the Drogon framework. If your
application uses a different framework, link only `StripeKit::stripekit` and
see @ref writing_an_adapter for how to supply your own adapter.

@section quickstart_client Step 1: Create a client

Everything StripeKit does starts from a "client" object. Create one when your
application starts, using the API key from earlier:

@code
#include <StripeKit/Adapters/Drogon.h>
#include <StripeKit/StripeKit.h>

auto stripe = stripekit::drogon::make_stripe_client({
    .api_key = std::getenv("STRIPE_SECRET_KEY")
});
@endcode

Create this once and reuse it for the whole lifetime of your application,
rather than creating a new one for each request — it keeps its connection to
Stripe open, so reusing it is both simpler and faster.

If your application is itself built on Drogon, add
`.event_loop = stripekit::drogon::EventLoopOptions::drogon_framework()` to the
options above, so StripeKit shares Drogon's own networking loop rather than
starting a second one. This does not matter for a simple command-line tool
like the one this page builds.

@section quickstart_customer Step 2: Create a customer

Before Stripe can bill someone, it needs a customer record for them. Create
one the first time a person signs up in your application, and keep its id:

@code
stripekit::CreateCustomerRequest request;
request.email = "buyer@example.com";
request.name  = "Example Buyer";
request.metadata["app_user_id"] = "42";

const stripekit::Customer customer =
    co_await stripe->customers().create_async(request);
@endcode

Store `customer.id` (something like `cus_...`) alongside that person's record
in your own database. You only need to create the Stripe customer once per
person, not every time they check out.

The `metadata["app_user_id"]` line is worth keeping. It is a free-form note
you attach to the customer, and Stripe copies it onto related events later.
That is how, when a webhook arrives, you can tell which of your users it is
about.

@section quickstart_checkout Step 3: Send the customer to Checkout

Now create a Checkout session and send the customer to the web page it gives
you:

@code
stripekit::CreateCheckoutSessionRequest request;
request.customer_id = customer.id;
request.price_id    = "price_...";
request.success_url = "https://example.com/thanks";
request.cancel_url  = "https://example.com/pricing";

const stripekit::CheckoutSession session =
    co_await stripe->checkout().sessions().create_async(request);

// Send the customer's browser to session.url.
@endcode

`session.url` is a Stripe-hosted page where the customer enters their card
details. Once they finish, Stripe sends them to `success_url`; if they give
up, it sends them to `cancel_url` instead.

Do not treat reaching `success_url` as proof that the customer has paid — a
customer can close their browser before the redirect happens, and the URL
itself is not secret. Use it only to show a "thank you" message. The real
confirmation that payment succeeded is the webhook covered next.

The `CheckoutSessionCli` example in the source tree is this same code as a
complete, runnable program; `DrogonCheckoutDemo` shows it wired into an
actual web page.

### Starting a subscription with a trial

For a recurring price, add a trial duration to the Checkout request. This
example starts a 14-day trial, collects a card before the trial begins, and
adds your user ID to the resulting subscription:

@code
request.trial_period_days = 14;
request.payment_method_collection = "always";
request.payment_method_types = { "card" };
request.subscription_metadata["app_user_id"] = "42";

const auto session = co_await stripe->checkout().sessions().create_async(
    request, stripekit::RequestOptions{ .idempotency_key = "saved_signup_attempt_id" });
@endcode

**Choose the customer.** If you already created a Stripe customer in Step 2,
set `request.customer_id` as shown earlier. Otherwise, leave both customer
fields empty: for a subscription, Checkout creates a Stripe customer using the
details the customer enters. Checkout asks for an email address if you do not
set `request.customer_email`; set it only to prefill an email you already know.
Do not set both customer fields.

**Retry safely.** Save the idempotency key and reuse it with the same request
values if you retry session creation. `metadata` belongs to the Checkout
session; use `subscription_metadata` for information you need on the
subscription, such as in subscription webhooks.

**Check the session and confirm the subscription.** Retrieve a saved session
with `stripe->checkout().sessions().retrieve_async(session.id)`. Its `status`
can be `open`, `complete`, or `expired`; `expires_at_unix` gives its expiry
time in Unix seconds. You can set `request.expires_at` when creating a session,
but Stripe requires it to be 30 minutes to 24 hours in the future.

**Close an abandoned session.** Call
`stripe->checkout().sessions().expire_async(session.id, options)` to prevent an
open session from being completed. Reuse an idempotency key in `options` for
retries. The result is the updated session; Stripe rejects expiry if the
session has already completed. Check ownership before expiring a saved session.

**Find sessions for a payment.** If you have a payment's `pi_...` ID and need
to find the Checkout session that created it, use the payment intent filter.
A PaymentIntent is Stripe's record of a payment and any steps needed to complete it:

@code{.cpp}
stripekit::ListCheckoutSessionsRequest filter;
filter.payment_intent_id = "pi_...";
filter.limit = 100;
auto page = co_await stripe->checkout().sessions().list_async(filter);
while (page.has_more) {
    // Process page.data before moving to the next page.
    filter.starting_after = page.data.back().id;
    page = co_await stripe->checkout().sessions().list_async(filter);
}
// Process the final page.data too.
@endcode

Each call returns only one page. Page sizes must be from 1 to 100. Omit the
payment intent filter to list all sessions. The callback methods `expire`
and `list` report the same results and failures as their coroutine counterparts.

A `complete` session does not prove that payment succeeded or confirm trial
access. Verify the subscription using Stripe webhooks before granting access.
Stripe bills the subscription after the trial unless it is canceled.

**Supported trial flow.** This guide covers card-required Checkout trials,
lasting 1 to 730 days. Trials are not valid in one-time payment mode.
Although `payment_method_collection` also accepts `if_required`, a cardless
trial needs a policy for a missing payment method when the trial ends.
StripeKit does not yet expose `trial_settings.end_behavior`; cardless trial
setup is outside this guide's supported flow.

**Decide trial access separately from payment.** After a verified
`checkout.session.completed` or subscription webhook, retrieve the subscription
and check that it belongs to your user, is `trialing`, and has a future
`trial_end_unix`. Use that date to limit trial access. Do not grant access
for an open, abandoned, or expired Checkout session. A trial can create a
zero-value invoice and emit `invoice.paid` without charging the card; that
event alone does not establish a paid subscription.

Billing-period dates may be on `Subscription` with older Stripe API versions,
or on each `SubscriptionItem` with newer versions. Prefer the item's optional
dates when present; use the subscription dates only when nonzero. Neither
the trial end nor an item's period end proves a payment succeeded.

At trial end, check the current subscription and the specific new invoice.
Handle `invoice.payment_failed` and `invoice.payment_action_required`:
collecting a card during signup does not guarantee that the first charge
succeeds without extra authentication. Send the customer to the invoice's
`hosted_invoice_url` to pay or authenticate, or to the Billing Portal to
update their payment method. Wait for verified payment/subscription updates
before restoring paid access. Your application chooses any grace period.

Offer cancellation before billing starts. The Billing Portal's
`SubscriptionCancel` flow can do this; ensure cancellation is enabled in the
portal configuration. Check `cancel_at_period_end` as well as `status`:
a subscription can remain `trialing` while cancellation is scheduled.
See @ref billing_portal_and_invoices.

**Retry and expire safely.** Persist the request inputs and signup-attempt
idempotency key before contacting Stripe. A retry must reuse both, even if your
price configuration has changed. Stripe may return the original cached
creation response, so retrieve the saved session to check its current state.
After expiry, start a new attempt with a new key; never interpret an expired
session as completed signup. Stripe can prune idempotency keys after 24 hours,
so do not use an old key as a permanent duplicate-subscription guard.

@section quickstart_webhook Step 4: Find out when the customer has paid

This is the step that actually matters: your server needs to hear from
Stripe. Register a URL Stripe can call, and write the code that runs when it
does:

@code
#include <StripeKit/Adapters/Drogon.h>
#include <StripeKit/SqliteWebhookDedupStore.h>

stripekit::SqliteWebhookDedupStore dedup_store("stripe-webhooks.sqlite3");

stripekit::drogon::register_webhook_handler({
    .path            = "/stripe/webhook",
    .endpoint_secret = std::getenv("STRIPE_WEBHOOK_SECRET"),
    .dedup_store     = dedup_store,
    .handler = [](const stripekit::webhooks::Event& event)
        -> stripekit::Task<stripekit::webhooks::ProcessResult> {
        if (event.type == "invoice.paid") {
            // Check the current subscription, invoice, and access policy here.
            co_return stripekit::webhooks::ProcessResult::Handled;
        }
        co_return stripekit::webhooks::ProcessResult::Ignored;
    }
});
@endcode

A few things worth explaining here:

- **`endpoint_secret`** proves the request really came from Stripe, and not
  from anyone who happened to find your URL. You get one when you register the
  webhook endpoint; see the testing section below for where to find it while
  developing locally.
- **`dedup_store`** keeps a record of which events your handler has already
  seen. Stripe sometimes sends the same event more than once, so without this,
  a customer could accidentally be billed or credited twice.
  `SqliteWebhookDedupStore` saves that record to a file on disk, and must stay
  alive for as long as your application runs — create it once near the top of
  your program, as above, rather than as a local variable inside a function.
- **`event.type`** tells you what happened. There is a full list in
  @ref quickstart_events below. Returning `ProcessResult::Ignored` for types
  you do not care about is completely normal.

Behind the scenes, this checks the request really came from Stripe, checks
whether it has been seen before, and only then runs your code above — so your
handler is called exactly once per real event, no matter how many times
Stripe delivers it.

If you ever need a piece of information this page's types do not include,
`event.payload_json` holds the complete original data Stripe sent, as text.

The `WebhookSignature` example in the source tree is the smallest possible
version of this: one webhook, nothing else.

@section quickstart_webhook_test Trying the webhook on your own machine

Stripe cannot send requests to your laptop directly, so the Stripe CLI stands
in for it during development. With your application running, in a separate
terminal run:

@code{.sh}
stripe listen --all-snapshot --forward-to localhost:8080/stripe/webhook
@endcode

This prints a secret starting with `whsec_`. Set that as `STRIPE_WEBHOOK_SECRET`
before starting your application — it is the `endpoint_secret` from the code
above. It is different from the secret your real, deployed endpoint will use
later.

Leave that command running, and in another terminal, send a fake event:

@code{.sh}
stripe trigger invoice.paid
@endcode

Your handler should run once. Try the same command again: it should still only
run once per event, which is the de-duplication working as intended.

@section quickstart_options Extra: making requests safe to retry

Every call above accepts an optional second argument for settings that apply
to just that one request:

@code
stripekit::RequestOptions options;
options.idempotency_key = "checkout-for-order-1234";
options.timeout = std::chrono::seconds(30);

const auto session =
    co_await stripe->checkout().sessions().create_async(request, options);
@endcode

Set `idempotency_key` on anything that creates something or charges money.
If your network connection drops after Stripe has processed the request but
before you see the reply, retrying with the same key tells Stripe "this is the
same request, not a new one", and you get back the original result instead of
creating a second subscription by accident. Build the key from something
already unique in your own system, such as an order id.

@section quickstart_errors Extra: handling things going wrong

If a call to Stripe fails, StripeKit throws an exception rather than
returning an error code:

@code
try {
    const auto session = co_await stripe->checkout().sessions().create_async(request);
} catch (const stripekit::StripeApiException& error) {
    const auto& details = error.payload();
    if (details.retryable) {
        // This was probably a temporary problem; trying again later may work.
    }
    log_error(details.message, details.request_id);
}
@endcode

It is worth logging `details.request_id`. If you ever need Stripe's support,
or want to look up exactly what happened in the Stripe dashboard, that id is
how you find the specific request.

@section quickstart_events Which events are worth handling

For the kind of subscription flow this page builds, these are the ones that
matter:

| Event | What it means |
| --- | --- |
| `checkout.session.completed` | The customer finished Checkout and now has a subscription. |
| `invoice.paid` | An invoice has no outstanding balance. Trial invoices can be zero; check the subscription and invoice before granting or extending access. |
| `invoice.payment_failed` | A payment failed. Stripe will automatically retry it a few times. |
| `customer.subscription.updated` | Something about the subscription changed, for example its plan or status. |
| `customer.subscription.deleted` | The subscription has ended. This is usually where you take access away. |
| `customer.subscription.trial_will_end` | A trial ends in about three days; remind the customer and check billing readiness. |
| `invoice.payment_action_required` | The customer must complete authentication before payment can succeed. |
| `checkout.session.expired` | Checkout expired without completion; no trial or paid access should be granted. |

Base access on verified billing webhooks and current subscription state,
rather than on the Checkout redirect from step 3. For trial access, check
`trialing` and the trial end; for paid access, follow your invoice and subscription
policy. Events can be duplicated or arrive out of order, so fetch current state
rather than replacing it with an older event snapshot. Renewals happen
automatically, weeks or months later, with no browser involved.

Stripe's own [list of event types](https://docs.stripe.com/api/events/types)
covers everything Stripe can send, including things outside subscription
billing.

@section quickstart_production Extra: moving to production

A few things to check before switching from a test key to a live one:

- Use durable storage for the de-duplication store. `SqliteWebhookDedupStore`
  is fine for a single server; if you run more than one instance of your
  application, use a store backed by a database they all share instead (see
  @ref writing_an_adapter for what that involves).
- Register your real, public webhook URL in the Stripe dashboard, and use the
  signing secret it gives you there — it is different from the one the Stripe
  CLI gave you during development.
- Keep webhook handlers fast. Stripe expects a reply within a few seconds and
  treats a slow one as a failure, so hand off any slow work instead of doing it
  inside the handler.
- Connect StripeKit's logging to your own, for example with
  `stripekit::drogon::LogBridge`, so problems show up where you already look
  for them.
- Never write the API key or the webhook secret to a log.
  `stripekit::redact_sensitive_text()` can help strip secret-looking values out
  of text before it is logged.

@section quickstart_next Where to go next

- @ref products_and_prices — reusable pricing, lookup keys, and keeping old prices.
- @ref billing_portal_and_invoices — letting customers fix payments and change plans.
- @ref application_api — full reference for everything used on this page.
- @ref overview — a step back, to see how the pieces fit together.
- @ref writing_an_adapter — using a web framework other than Drogon.
