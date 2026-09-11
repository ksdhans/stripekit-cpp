@page quickstart Quickstart

@tableofcontents

This page builds a working subscription integration: create a Checkout session
so a customer can subscribe, then handle the webhook that tells you they did.
It uses the Drogon adapter throughout.

Everything here uses Stripe test mode. Test keys start with `sk_test_`, and
nothing you do with them touches real money.

@section quickstart_prerequisites Before you start

You need a Stripe account in test mode, and from its dashboard:

- a **secret API key** (`sk_test_...`),
- a **price id** (`price_...`) for a recurring product,
- the [Stripe CLI](https://docs.stripe.com/stripe-cli), for delivering test
  webhooks to your machine.

Keep the key in an environment variable rather than in source. The examples
read `STRIPE_SECRET_KEY`.

@section quickstart_build Adding StripeKit to your build

StripeKit installs a CMake package, so consuming it is two lines:

@code{.cmake}
find_package(StripeKit CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE StripeKit::stripekit StripeKit::drogon)
@endcode

Link `StripeKit::stripekit` for the application API and `StripeKit::drogon` for
the Drogon adapter. An application using a different framework links only the
first, and provides its own adapter — see @ref writing_an_adapter.

@section quickstart_client Creating a client

stripekit::drogon::make_stripe_client() returns a
stripekit::StripeClient wired to Drogon's HTTP client:

@code
#include <StripeKit/Adapters/Drogon.h>
#include <StripeKit/StripeKit.h>

auto stripe = stripekit::drogon::make_stripe_client({
    .api_key = std::getenv("STRIPE_SECRET_KEY")
});
@endcode

One client is enough for the whole application. It is safe to share across
threads, and it keeps connections to Stripe alive between calls, so creating a
fresh one per request only wastes time.

By default the client runs its requests on an event loop it owns. If your
application is already a Drogon server, hand it Drogon's own loop instead with
stripekit::drogon::EventLoopOptions::drogon_framework() so the two are not
competing for threads.

@section quickstart_customer Creating a customer

Stripe needs a customer object before it can bill anyone:

@code
stripekit::CreateCustomerRequest request;
request.email = "buyer@example.com";
request.name  = "Example Buyer";
request.metadata["app_user_id"] = "42";

const stripekit::Customer customer =
    co_await stripe->customers().create_async(request);
@endcode

That `metadata` entry is worth setting. Stripe echoes metadata back on the
webhooks it sends later, so it is how you work out which of your users an
event belongs to without keeping a separate mapping table.

Store `customer.id` against your user record. Create the Stripe customer once,
not per checkout.

@section quickstart_checkout Starting a subscription

Checkout is a Stripe-hosted payment page. You create a session, redirect the
customer to it, and Stripe handles the card details:

@code
stripekit::CreateCheckoutSessionRequest request;
request.customer_id = customer.id;
request.price_id    = "price_...";
request.success_url = "https://example.com/thanks";
request.cancel_url  = "https://example.com/pricing";

const stripekit::CheckoutSession session =
    co_await stripe->checkout().sessions().create_async(request);

// Send the customer to session.url.
@endcode

The full program is in the CheckoutSessionCli example, and
DrogonCheckoutDemo shows the same flow inside a real Drogon server with
rendered pages.

A warning about `success_url`: it is where the customer's browser lands, not
proof that you were paid. A customer can close the tab before the redirect, and
the URL can be visited by anyone who knows it. Treat it as a thank-you page and
nothing more. The actual "they paid" signal is the webhook.

@section quickstart_options Idempotency and timeouts

stripekit::RequestOptions carries the per-call settings:

@code
stripekit::RequestOptions options;
options.idempotency_key = "checkout-for-order-1234";
options.timeout = std::chrono::seconds(30);

const auto session =
    co_await stripe->checkout().sessions().create_async(request, options);
@endcode

Set an idempotency key on anything that creates or charges. If the response is
lost to a timeout and you retry, the key tells Stripe it is the same request,
and you get the original result back instead of a second subscription. Derive
the key from something stable in your own system, like an order id.

@section quickstart_errors Handling failures

Failed calls throw. stripekit::StripeApiException carries Stripe's error
details:

@code
try {
    const auto session = co_await stripe->checkout().sessions().create_async(request);
} catch (const stripekit::StripeApiException& error) {
    const auto& details = error.payload();
    if (details.retryable) {
        // Transient: a later attempt may succeed.
    }
    log_error(details.message, details.request_id);
}
@endcode

Log `request_id`. It identifies the exact call in Stripe's own logs, which
turns most "why did this fail" questions into a two-minute lookup.

@section quickstart_webhook Handling the webhook

This is the part that actually grants access. Register a route, give it a
durable store, and write a handler:

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
            // Extend the customer's access here.
            co_return stripekit::webhooks::ProcessResult::Handled;
        }
        co_return stripekit::webhooks::ProcessResult::Ignored;
    }
});
@endcode

The registration verifies Stripe's signature, reserves the event id in the
store, and only then calls your handler — so it runs once per event no matter
how many times Stripe delivers it. Returning
stripekit::webhooks::ProcessResult::Ignored for event types you do not care
about is normal and costs nothing.

The store must outlive the handler. A `static` or application-scoped object is
the usual choice; a local that goes out of scope is a dangling reference.

If your handler needs a field StripeKit does not model, parse it out of
stripekit::EventEnvelope::payload_json, which holds Stripe's raw JSON.

@section quickstart_webhook_test Testing the webhook

The Stripe CLI forwards real test-mode events to your local server:

@code{.sh}
stripe listen --forward-to localhost:8080/stripe/webhook
@endcode

It prints a signing secret starting with `whsec_`. That is the value for
`endpoint_secret`, and it differs from the one your deployed endpoint uses. In
another terminal, fire an event:

@code{.sh}
stripe trigger invoice.paid
@endcode

Run it twice. Your handler should still only act once — that is the
de-duplication doing its job.

The WebhookSignature example is the smallest possible version of this: one
route, signature verification, nothing else.

@section quickstart_events Which events to handle

For subscription billing, the ones that change what a customer may do:

| Event | Meaning |
| --- | --- |
| `checkout.session.completed` | Checkout finished; a subscription now exists. |
| `invoice.paid` | A period was paid for. Grant or extend access. |
| `invoice.payment_failed` | Payment failed. Stripe will retry; warn the customer. |
| `customer.subscription.updated` | Plan, quantity, or status changed. |
| `customer.subscription.deleted` | The subscription ended. Revoke access. |

Drive access from `invoice.paid` and `customer.subscription.deleted` rather
than from the checkout redirect. Renewals happen months later with no browser
involved, and only the webhooks tell you about them.

Stripe's [event reference](https://docs.stripe.com/api/events/types) lists the
full set.

@section quickstart_production Going to production

Before switching to live keys:

- Point the store at durable storage. SQLite is fine on one host; use your
  shared database when running several instances.
- Register the production endpoint in the Stripe dashboard and use the signing
  secret it gives you.
- Return quickly from handlers. Stripe times out slow endpoints and retries
  them; queue long work rather than doing it inline.
- Give StripeKit a logger — stripekit::drogon::LogBridge sends its diagnostics
  through Drogon's log.
- Never log the API key or the endpoint secret. stripekit::redact_sensitive_text()
  strips secret-looking tokens from text you are about to log.

@section quickstart_next Where to go next

- @ref application_api — the full reference for these types.
- @ref overview — how the pieces fit together.
- @ref writing_an_adapter — using a framework other than Drogon.
