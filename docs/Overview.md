@page overview Overview

@tableofcontents

StripeKit is a C++ toolkit for the parts of Stripe most applications actually
need: charging customers for subscriptions, letting them manage their own
billing, and reacting to what Stripe tells you afterwards.

It supports hosted Checkout, customer billing portal sessions and portal
configurations, reusable products and fixed prices, subscription retrieval,
price changes and cancellation, invoice retrieval, and verified webhooks.
It does not cover every Stripe operation. Webhook events include the original
JSON for reading fields not exposed by the supplied models; this does not add
outbound API operations that StripeKit does not yet support.

@section overview_shape The shape of the library

StripeKit is split into two halves.

The **application API** is what you write your billing logic against. It is
plain C++20 with no web framework in sight: request structs, model structs,
exceptions, and two entry points — stripekit::StripeClient for outbound calls
and stripekit::webhooks for inbound events.

The **adapter API** is the plumbing underneath. Somebody has to open an HTTPS
connection to Stripe and somebody has to own the route that Stripe posts
webhooks to, and both of those depend on which web framework your application
already uses. An adapter provides them.

@dot
digraph stripekit {
    rankdir=TB;
    node [shape=box, fontname="Helvetica", fontsize=10];

    app  [label="Your application"];
    api  [label="Application API\nStripeClient, webhooks::*"];
    core [label="StripeKit core\nrequest building, signature\nverification, de-duplication"];
    adpt [label="Adapter\nHTTPClient, webhook route"];
    fw   [label="Web framework\n(Drogon, ...)"];

    app -> api -> core -> adpt -> fw;
}
@enddot

The split means your billing code does not change if you switch web
frameworks, and that StripeKit can be dropped into an application that already
has its own HTTP stack.

@section overview_outbound Calling Stripe

stripekit::StripeClient groups operations the same way Stripe's API reference
does, so `checkout().sessions().create_async()` corresponds to Stripe's
"Create a Checkout Session" endpoint. An adapter's factory function builds one
for you:

@code
auto stripe = stripekit::drogon::make_stripe_client({.api_key = "sk_test_..."});

stripekit::CreateCheckoutSessionRequest request;
request.customer_id = "cus_...";
request.price_id    = "price_...";
request.success_url = "https://example.com/done";
request.cancel_url  = "https://example.com/cancel";

const auto session = co_await stripe->checkout().sessions().create_async(request);
// Redirect the customer to session.url.
@endcode

Every operation comes in two forms. The `*_async` form returns a
stripekit::Task you can `co_await`, and is what you want in new code. The
plain form takes a completion callback, for codebases that are not using
coroutines. Both do the same work.

Failures arrive as exceptions. stripekit::StripeApiException carries Stripe's
own error details — the category, message, HTTP status, request id, and
whether retrying is worthwhile.

@section overview_inbound Receiving webhooks

Stripe reports the results of billing asynchronously. A customer completing
Checkout, a renewal succeeding, a card being declined weeks later: all of it
arrives as a webhook, and all of it matters for deciding what a customer is
entitled to.

Two things make webhooks harder than they look, and StripeKit handles both.

The first is trust. Anyone can post JSON to a public URL, so Stripe signs each
delivery and you must check that signature before believing a word of it.
stripekit::webhooks::construct_event does the check and throws
stripekit::SignatureVerificationException if it fails.

The second is duplicates. Stripe retries a delivery whenever it cannot confirm
you received it, so the same event will arrive more than once. If your handler
grants a month of service each time it runs, that is a real problem.
stripekit::webhooks::process_event_once reserves the event id in a
stripekit::WebhookDedupStore before calling your handler. Completed events are
not handled again while their records are retained. Failed attempts are retried,
and unfinished work can be retried after a crash. Make your handler safe to run
again, since its database changes and the event record are not one transaction.

@code
auto result = co_await stripekit::webhooks::process_event_once(
    raw_body, signature_header, "whsec_...", dedup_store,
    [&](const stripekit::webhooks::Event& event)
        -> stripekit::Task<stripekit::webhooks::ProcessResult> {
        if (event.type == "invoice.paid") {
            // Check subscription state and your access policy, including trials.
            co_return stripekit::webhooks::ProcessResult::Handled;
        }
        co_return stripekit::webhooks::ProcessResult::Ignored;
    });
@endcode

Your handler's return value decides what Stripe is told. Returning
stripekit::webhooks::ProcessResult::RetryableFailure asks Stripe to deliver
again later, which is what you want when your database was briefly
unavailable.

@section overview_storage Choosing a de-duplication store

De-duplication is only as durable as the store behind it.
stripekit::InMemoryWebhookDedupStore forgets everything when the process
restarts, so it suits tests and local development.
stripekit::SqliteWebhookDedupStore persists to disk and is the reasonable
default for a single-host deployment. Applications running several instances
should implement stripekit::WebhookDedupStore against their shared database so
all instances agree on which events are done.

@section overview_next Where to go next

- @ref quickstart — a working integration, step by step.
- @ref products_and_prices — create reusable prices and preserve existing pricing.
- @ref billing_portal_and_invoices — self-service billing, plan changes, and invoices.
- @ref application_api — the full reference for the types above.
- @ref writing_an_adapter — supporting a framework other than Drogon.
