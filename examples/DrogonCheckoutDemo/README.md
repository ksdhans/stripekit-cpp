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
# Optional: preconfigure if you do not want the app to start Stripe CLI.
# STRIPE_WEBHOOK_SECRET=whsec_your_cli_secret
# Optional: use a dashboard-managed recurring Price instead of inline demo data.
# STRIPE_PRICE_ID=price_your_recurring_price_id
```

Restrict the file so only your user can read it:

```bash
umask 077
chmod 600 .env.live
chown "$(id -un):$(id -gn)" .env.live
stat -c '%A %U:%G %n' .env.live
```

The expected permissions are `-rw-------`.

```bash
(
	set -a
	. ./.env.live
	set +a
	./build/bin/stripekit_drogon_checkout_demo
)
```

Open `http://127.0.0.1:4242`, enter your `sk_test_...` secret key, then set up
webhook forwarding. The app rejects `sk_live_...` keys. By default, Checkout
uses inline demo product data, so you do not need to create a Product or Price
in Stripe first. You can still enter an existing recurring `price_...` id if you
want to test a dashboard-managed Price.

## Event Loop Use

This example runs StripeKit requests on Drogon's framework event loop by using
`EventLoopOptions::drogon_framework()` when creating the Stripe client. The
checkout handler is a Drogon coroutine, so it can `co_await` `create_async()`
without blocking the server thread. Standalone CLI examples use StripeKit's
default own event loop so they can call `create_async().result()` without
starting a Drogon app.

## Webhooks

The setup page checks whether the Stripe CLI is installed. Click **Start Stripe
CLI listener** to run this local listener:

```bash
stripe listen --all-snapshot --latest --forward-to http://127.0.0.1:4242/webhook
```

The app reads the `whsec_...` signing secret from `stripe-listener.log` and keeps
it in memory. If Stripe CLI is not logged in, or if the listener does not print a
secret quickly enough, the setup page shows the full listener log path. Open that
file, copy the `whsec_...` value, and paste it into the setup form.

## Try Checkout

Use the mock sales page to enter an email address and start Checkout. Pay with a
Stripe test card with any future expiry and a valid-format CVC:

| Scenario | Test card number |
| --- | --- |
| Successful payment | `4242 4242 4242 4242` |
| Declined payment | `4000 0000 0000 0002` |
| Always authenticate (3D Secure) | `4000 0027 6000 3184` |

See [Stripe's testing documentation](https://docs.stripe.com/testing) for the
complete list of test cards and their expected behavior. Never use real card
details in this demo.

Stripe redirects back to `/return?session_id=...` after the hosted checkout
page completes.

The return page is only user feedback. Treat verified webhook events, especially
`checkout.session.completed`, `invoice.paid`, and `invoice.payment_failed`, as
the source of truth for fulfillment and entitlement changes.

## Manually Check 3D Secure

1. Configure a test-mode key and webhook forwarding as described above.
2. Start a new Checkout session and enter the **always authenticate** card,
   `4000 0027 6000 3184`, with any future expiry and a valid-format CVC.
3. Submit payment and choose the successful authentication option on Stripe's
   test challenge screen.
4. Confirm that Checkout completes, a verified `invoice.paid` event appears,
   and the resulting subscription is `active` in the Stripe sandbox Dashboard.
5. Start a fresh Checkout session and choose the failed authentication option.
   Repeat with another fresh session and close or cancel the challenge.
   Confirm that these unpaid attempts do not grant access in your application.

Closing a challenge does not necessarily cancel the subscription immediately
or produce a particular failure event. Check the invoice and subscription in
the Dashboard; do not treat reaching a return page as proof of payment.
The demo displays events but does not grant application access.

This checks authentication during initial Checkout, not recovery after an
existing subscription's price changes. For that separate procedure, see
[manual subscription payment recovery](../../tests/sandbox/README.md#manually-check-3d-secure-payment-recovery).