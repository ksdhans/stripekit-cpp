@page writing_an_adapter Writing an Adapter

@tableofcontents

An adapter connects StripeKit to a web framework. StripeKit builds the
requests, verifies the signatures, and decides what should happen; the adapter
supplies the two things that inevitably depend on your framework: sending
HTTPS requests, and owning the route Stripe posts webhooks to.

StripeKit currently includes one adapter: [Drogon](https://drogon.org/). 

@section adapter_what What an adapter provides

| Piece | Required | Purpose |
| --- | --- | --- |
| stripekit::HTTPClient | Yes | Sends requests to Stripe's REST API. |
| A webhook route | Yes | Receives deliveries and passes the raw body to StripeKit. |
| A factory function | Recommended | One call that hands back a ready stripekit::StripeClient. |
| stripekit::Logger | Optional | Bridges StripeKit's diagnostics into the framework's log. |

Nothing else is needed. Request building, authentication headers, signature
verification, and de-duplication are all core behaviour and are not the
adapter's business.

The Drogon adapter is a useful reference throughout: see
`src/adapters/drogon/` for a complete implementation of everything below.

@section adapter_http Implementing the HTTP client

Derive from stripekit::HTTPClient and implement both methods. They receive a
stripekit::HttpRequest and produce a stripekit::HttpResponse.

@code
class MyHTTPClient final : public stripekit::HTTPClient {
public:
    explicit MyHTTPClient(std::string api_key);

    stripekit::Task<stripekit::HttpResponse> send_async(
        const stripekit::HttpRequest& request) override;

    void send(const stripekit::HttpRequest& request, Completion completion) override;
};
@endcode

@subsection adapter_http_contract What your implementation must do

**Resolve the path.** stripekit::HttpRequest::path is relative, like
`/v1/customers`. Join it to Stripe's base URL, `https://api.stripe.com`.

**Send the body as-is.** stripekit::HttpRequest::body is already encoded as
`application/x-www-form-urlencoded`, which is what Stripe's API expects. Do not
re-encode it or convert it to JSON.

**Authenticate.** Add `Authorization: Bearer <secret key>`. The key is
configuration your adapter holds, not part of the request. Never log it.

**Apply the options.** stripekit::HttpRequest::options carries the
per-call settings, and honouring them is what makes safe retries work:

- `idempotency_key` becomes the `Idempotency-Key` header when set. Skipping
  this means a retried call can charge a customer twice.
- `timeout` overrides your default timeout for this call.
- `headers` are merged over your defaults, so an application can set things
  like `Stripe-Version` per call.

**Report the response, not an error.** A 4xx or 5xx from Stripe is a normal
outcome: fill in stripekit::HttpResponse::status_code and
stripekit::HttpResponse::body and return it. StripeKit parses Stripe's error
document and throws stripekit::StripeApiException itself. Reserve exceptions
for problems that stopped you reaching Stripe at all — DNS failures, TLS
errors, timeouts.

**Lower-case the response header keys.** StripeKit looks them up in lower case.

@subsection adapter_http_completion Completion rules

The callback form must invoke `completion` exactly once, on every path
including the error paths. Calling it twice corrupts the coroutine that may be
waiting on it; never calling it hangs the request forever.

stripekit::OnceCallback exists for this. Wrap the callback and let it enforce
the rule:

@code
void MyHTTPClient::send(const stripekit::HttpRequest& request, Completion completion) {
    auto once = std::make_shared<stripekit::OnceCallback<
        std::variant<stripekit::HttpResponse, std::exception_ptr>>>(std::move(completion));

    transport.send(build(request), [once](auto result) {
        once->invoke(std::move(result));
    });
}
@endcode

Both methods may be called from any thread and several calls may be in flight
at once, so the implementation must be thread-safe.

@section adapter_client Providing a factory

Applications should not have to assemble the pieces by hand. Offer one function
that takes your options and returns a ready client:

@code
std::unique_ptr<stripekit::StripeClient> make_stripe_client(ClientOptions options);
@endcode

Internally it constructs your stripekit::HTTPClient and wraps it in a
stripekit::StripeApiClient, which implements stripekit::StripeClient on top of
whatever transport you provide. The returned object owns the HTTP client, so
the caller has a single thing to keep alive.

Keep the options struct small: the API key, the base URL (overridable, for
testing against a mock), and whatever your framework needs to schedule work.

@section adapter_webhook Wiring up webhooks

The webhook side is a route handler. It hands StripeKit the raw request body
and the `Stripe-Signature` header, and turns the outcome into an HTTP status.

@code
auto result = co_await stripekit::webhooks::process_event_once(
    raw_body, signature_header, endpoint_secret, dedup_store, handler);
@endcode

@subsection adapter_webhook_raw Use the raw body

The signature is computed over the exact bytes Stripe sent. Any framework
convenience that parses, re-serialises, or normalises the body will change
those bytes and every signature check will fail. Read the unmodified body.

For the same reason, do not apply request-body transformations to this route:
no JSON parsing middleware, no character-set conversion, no whitespace
trimming.

@subsection adapter_webhook_status Mapping outcomes to status codes

stripekit::process_event_once returns a
stripekit::webhooks::ProcessEventOnceResult whose
stripekit::WebhookResponseAction says what to send:

| Condition | Status | Why |
| --- | --- | --- |
| stripekit::WebhookResponseAction::AcknowledgeSuccess | 200 | Done, or a duplicate that was already done. Stripe stops retrying. |
| stripekit::WebhookResponseAction::RetryLater | 500 | Temporary failure. Stripe will deliver again. |
| stripekit::SignatureVerificationException | 400 | Not from Stripe, or the timestamp is outside tolerance. Never retried. |
| stripekit::ConfigurationException | 400 | The payload was not a usable Stripe event. |

Getting the 400 case right matters: a rejected signature must not be answered
with a 5xx, or Stripe will keep resending a request you will keep rejecting.

@subsection adapter_webhook_ack Acknowledge promptly

Stripe expects a response within seconds and treats a slow endpoint as a
failed delivery. Return the status as soon as StripeKit gives you the outcome,
and do not hold the response open for the application's own slow work.

@section adapter_logging Bridging the log

If your framework has a logging system, implement stripekit::Logger so
StripeKit's diagnostics end up in the same place as everything else:

@code
class MyLogger final : public stripekit::Logger {
public:
    bool should_log(stripekit::LogLevel level) const override;
    void log(stripekit::LogLevel level,
             std::string_view message,
             const stripekit::LogContext& context) override;
};
@endcode

Map stripekit::LogLevel onto your framework's levels, and let
`should_log()` answer honestly so StripeKit can skip building messages that
would be discarded.

stripekit::LogContext carries structured fields — request id, event id,
webhook type — that are worth emitting as separate fields if your logger
supports them, since they are what you search on when tracing a payment.

Do not log secrets. stripekit::is_sensitive_field_name() and
stripekit::redact_sensitive_text() are there to help.

@section adapter_dedup A note on de-duplication stores

stripekit::WebhookDedupStore is not an adapter interface, and an adapter
should not supply one. Which store to use is the application's decision,
because it depends on the application's database and deployment, not on its
web framework. Accept a `WebhookDedupStore&` in your webhook options and pass
it through.

If you do implement one — for a database StripeKit does not support — the
important property is that stripekit::WebhookDedupStore::claim() is atomic.
Two instances racing on the same event id must produce exactly one
stripekit::ClaimOutcome::Claimed. A read-then-write is not good enough; use
your database's conditional insert.

@section adapter_testing Testing an adapter

Point the base URL at a local server and check the requests you produce:

- the `Authorization` header carries the key,
- `Idempotency-Key` appears when stripekit::RequestOptions::idempotency_key is
  set, and not otherwise,
- per-call headers override your defaults,
- a 402 from the server surfaces as stripekit::StripeApiException, not as a
  transport error,
- a dropped connection surfaces as an exception,
- `send()` invokes its completion exactly once on both the success and failure
  paths.

For webhooks, sign a payload with a known secret and confirm a valid delivery
returns 200, a tampered body returns 400, and delivering the same event twice
runs the handler once. StripeKit's own tests under `tests/integration/` cover
the Drogon adapter this way and are a reasonable template.

@section adapter_migration Moving an application from Drogon to another adapter

Your application can keep using stripekit::StripeClient and the same webhook
handler. What changes, is how you create the client and how your server receives webhooks:

1. Use an existing StripeKit adapter for your framework, or write one as
   described in @ref adapter_what and the sections that follow.
2. Build StripeKit with `STRIPEKIT_BUILD_DROGON_ADAPTER=OFF`,
   `STRIPEKIT_BUILD_TESTS=OFF`, and `STRIPEKIT_BUILD_EXAMPLES=OFF`.
   The bundled tests require the Drogon adapter; run the new adapter's tests in
   its own project. Link the application to `StripeKit::stripekit` and the new
   adapter target, not `StripeKit::drogon`.
3. Replace `stripekit::drogon::make_stripe_client()` with your adapter's client
   creation function. Keep the same API key and request settings, including the
   keys that prevent repeated requests from creating duplicate payments.
   Keep the client alive while requests are running. Remove the Drogon
   event-loop settings.
4. Replace `stripekit::drogon::register_webhook_handler()` with the new route.
   Keep the signing secret and pass the request body to StripeKit without
   changing it. Reuse your event handler and database of processed events.
   Keep the same limits for signature age and for retrying an event left
   unfinished after a crash. This prevents the switch from processing completed
   events again.
5. If the public webhook URL changes, update the endpoint in Stripe and use
   that endpoint's signing secret. If the URL stays the same, keep its secret.
   Do not clear the database of processed events when deploying the new route.

Before using the adapter with real payments, run the tests described above and
check these cases:

- An event that was already handled returns a success response without running
  the handler again.
- An event that is still being handled returns a response asking Stripe to
  retry later.
- A failure to read or write the event database also asks Stripe to retry.
- After a crash and restart, an unfinished event can be handled again once the
  configured waiting period has passed.
- Callbacks run on the documented thread or event loop, and requests respect
  their timeouts.
- Retried POST requests keep the same idempotency key.
- Log levels work as expected, and logs do not expose secrets or sensitive data.

Then test against a real Stripe sandbox. Check successful and failed invoice
payments, subscription changes, disputes, refunds, and repeated webhook
deliveries. The existing sandbox tests use Drogon, so add tests for these same
cases with your adapter. You can use your own adapter without waiting for
StripeKit to ship one for your framework.

@section adapter_next Where to go next

- @ref adapter_interfaces — reference for the interfaces above.
- @ref drogon_adapter — the bundled Drogon implementation.
- @ref application_api — what the adapter is ultimately serving.
