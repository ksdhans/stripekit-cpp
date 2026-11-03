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

## Running With Local Secrets

Keep local example values in `.env.live` at the repository root and load them in
a subshell. The variables are exported only to the example process and disappear
when the subshell exits:

```bash
(
	set -a
	. ./.env.live
	set +a
	./build/bin/stripekit_drogon_checkout_demo
)
```

The `.env.live` file is git-ignored. Despite the file name, use test-mode
Stripe values with these examples; `sk_live_...` secret keys are rejected.
Protect the file at the filesystem level too:

```bash
umask 077
chmod 600 .env.live
chown "$(id -un):$(id -gn)" .env.live
stat -c '%A %U:%G %n' .env.live
```

The expected permissions are `-rw-------`. `chmod 600` is the important step;
`umask 077` also keeps later files created in the same shell user-only by
default.

The Drogon server examples choose return and webhook URLs from the port they bind
to. The CLI examples do not bind ports; their return URL environment variables
are just the URLs Stripe redirects to after the hosted flow.

When an example asks for `cus_...`, `price_...`, or `prod_...` IDs, create those
objects with the Stripe CLI or copy existing IDs from the Dashboard while test
mode or your sandbox is selected. The individual example READMEs include the
exact commands they need.

## Available Examples

- [ProductCatalogCli](ProductCatalogCli/README.md) creates a product and monthly
	price, updates display details, selects a replacement default price, finds it
	by lookup key, and archives the old price. Its Price ID can be used with Checkout.
- [DrogonCheckoutDemo](DrogonCheckoutDemo/README.md) is the main end-to-end
	hosted Checkout example. It serves a small HTML/CSS UI, creates a customer,
	creates a subscription-mode Checkout Session, redirects to Stripe-hosted
	Checkout, optionally starts a card-required 14-day trial, receives webhooks,
	and shows recent verified webhook deliveries.
- [WebhookSignature](WebhookSignature/README.md) is a tiny Drogon webhook
	endpoint that verifies `Stripe-Signature` and logs the event id and type.
- [CheckoutSessionCli](CheckoutSessionCli/README.md) creates one hosted
	subscription-mode Checkout Session and prints the Stripe-hosted URL.
- [BillingPortalCli](BillingPortalCli/README.md) creates one Billing Portal
	session and prints the hosted portal URL, optionally opening it at a specific
	task such as updating the payment method or confirming a plan change.
- [InvoiceStatusCli](InvoiceStatusCli/README.md) reads an invoice and prints
	what is owed, when Stripe will retry, and the link where the customer can pay.

The examples pin `Stripe-Version: 2024-06-20` on outbound calls. When testing
webhooks locally, use `stripe listen --all-snapshot --latest` to forward all
events with full object payloads using the latest API version rather than an
older account default. Current Stripe CLI versions require an explicit event
selection; use `--events` instead of `--all-snapshot` to forward selected event
types.
