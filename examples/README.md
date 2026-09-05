# Examples

StripeKit examples are intentionally small. They show the library calls and
Drogon adapter wiring without adding a JavaScript framework or a separate
frontend build step.

## Build

When StripeKit is configured as the top-level project, examples are built by
default. When StripeKit is added to a larger CMake project, examples default to
off. You can set the option explicitly either way:

```bash
cmake -S . -B build -DSTRIPEKIT_BUILD_EXAMPLES=ON
cmake --build build --target stripekit_drogon_checkout_demo
```

All examples use Stripe test mode. Do not use live keys with these programs.

## Available Examples

- [DrogonCheckoutDemo](DrogonCheckoutDemo/README.md) is the main end-to-end
	hosted Checkout example. It serves a small HTML/CSS UI, creates a customer,
	creates a subscription-mode Checkout Session, redirects to Stripe-hosted
	Checkout, receives webhooks, and shows recent verified webhook deliveries.
- [WebhookSignature](WebhookSignature/README.md) is a tiny Drogon webhook
	endpoint that verifies `Stripe-Signature` and logs the event id and type.
- [CheckoutSessionCli](CheckoutSessionCli/README.md) creates one hosted
	subscription-mode Checkout Session and prints the Stripe-hosted URL.
- [BillingPortalCli](BillingPortalCli/README.md) creates one Billing Portal
	session and prints the hosted portal URL.

The examples pin `Stripe-Version: 2024-06-20` on outbound calls. When testing
webhooks locally, use `stripe listen --latest` so Stripe CLI event payloads are
not shaped by an older account default API version.
