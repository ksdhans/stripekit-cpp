# Drogon Checkout Demo

This example is a minimal hosted Checkout app built with Drogon and StripeKit.
It serves plain HTML/CSS through Drogon CSP templates, creates a test-mode
customer, creates a subscription-mode Checkout Session with inline demo product
data, redirects to Stripe-hosted Checkout, and shows recent verified webhook
deliveries.

## Build

```bash
cmake -S . -B build -DSTRIPEKIT_BUILD_EXAMPLES=ON
cmake --build build --target stripekit_drogon_checkout_demo
```

Standalone example builds enable Drogon's `drogon_ctl` target so CI can compile
the CSP templates from the fetched Drogon build.

Run the demo:

```bash
./build/bin/stripekit_drogon_checkout_demo
```

Or load local values from `.env.live` without leaving them in your shell:

```bash
# .env.live
STRIPE_SECRET_KEY=sk_test_your_secret_key
STRIPE_WEBHOOK_SECRET=whsec_your_cli_secret
# Optional: use a dashboard-managed recurring Price instead of inline demo data.
# STRIPE_PRICE_ID=price_your_recurring_price_id
```

```bash
(
	set -a
	. ./.env.live
	set +a
	./build/bin/stripekit_drogon_checkout_demo
)
```

Restrict the file so only your user can read it:

```bash
umask 077
chmod 600 .env.live
chown "$(id -un):$(id -gn)" .env.live
stat -c '%A %U:%G %n' .env.live
```

The expected permissions are `-rw-------`.

Open `http://127.0.0.1:4242`, enter your `sk_test_...` secret key, then set up
webhook forwarding. The app rejects `sk_live_...` keys. By default, Checkout
uses inline demo product data, so you do not need to create a Product or Price
in Stripe first. You can still enter an existing recurring `price_...` id if you
want to test a dashboard-managed Price.

## Webhooks

The setup page checks whether the Stripe CLI is installed and can start this
fixed local listener for convenience:

```bash
stripe listen --latest --forward-to http://127.0.0.1:4242/webhook
```

Copy the `whsec_...` signing secret printed by the CLI into the setup form. The
app keeps it in memory only.

## Try Checkout

Use the mock sales page to enter an email address and start Checkout. Pay with a
Stripe test card such as `4242 4242 4242 4242`, any future expiry, and any CVC.
Stripe redirects back to `/return?session_id=...` after the hosted checkout
page completes.

The return page is only user feedback. Treat verified webhook events, especially
`checkout.session.completed`, `invoice.paid`, and `invoice.payment_failed`, as
the source of truth for fulfillment and entitlement changes.