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
week, or year) until they cancel. StripeKit's Checkout support is for setting
up this kind of subscription; it does not currently handle a single one-time
purchase.

@section quickstart_prerequisites Before you start

You need:

- a free [Stripe account](https://dashboard.stripe.com/register), used in test
  mode,
- a **secret API key** from that account, which looks like `sk_test_...` and
  is found under Developers > API keys in the Stripe dashboard,
- a **price**, which is Stripe's record of what you charge and how often —
  create one under Product catalog in the dashboard, and copy its id, which
  looks like `price_...`,
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
            // A payment succeeded: give the customer access here.
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
stripe listen --forward-to localhost:8080/stripe/webhook
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
| `invoice.paid` | A payment for a billing period succeeded. This is usually where you grant or extend access. |
| `invoice.payment_failed` | A payment failed. Stripe will automatically retry it a few times. |
| `customer.subscription.updated` | Something about the subscription changed, for example its plan or status. |
| `customer.subscription.deleted` | The subscription has ended. This is usually where you take access away. |

It is best to base access on `invoice.paid` and `customer.subscription.deleted`
rather than on the Checkout redirect from step 3. Renewals happen automatically,
weeks or months later, with no browser involved at all — the webhook is the
only way to find out about them.

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

- @ref application_api — full reference for everything used on this page.
- @ref overview — a step back, to see how the pieces fit together.
- @ref writing_an_adapter — using a web framework other than Drogon.
