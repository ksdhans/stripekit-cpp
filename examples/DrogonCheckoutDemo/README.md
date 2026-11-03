# Drogon Checkout Demo

A small web app demonstrating Stripe-hosted subscription Checkout, with or
without a card-required 14-day trial. It shows verified webhook deliveries
and redirects back to a simple return page after Checkout.

Use Stripe test-mode keys and test cards only. The demo rejects live keys.

## Build And Run

```bash
cmake -S . -B build -DSTRIPEKIT_BUILD_EXAMPLES=ON
cmake --build build --target stripekit_drogon_checkout_demo
./build/bin/stripekit_drogon_checkout_demo
```

Open `http://127.0.0.1:4242` and enter your `sk_test_...` secret key.

The demo provides a monthly product and price by default, so you do not need to
create them in Stripe first. To use your own pricing, enter an existing recurring
`price_...` ID in the setup form.

### Optional Environment Settings

You can preconfigure the demo with a git-ignored `.env.live` file at the
repository root. Despite the filename, use test-mode values:

```bash
STRIPE_SECRET_KEY=sk_test_your_secret_key
# Optional: an existing recurring Price.
STRIPE_PRICE_ID=price_your_recurring_price_id
# Optional: the signing secret from your local Stripe CLI listener.
STRIPE_WEBHOOK_SECRET=whsec_your_cli_secret
```

Restrict the file so only your user can read it:

```bash
umask 077
chmod 600 .env.live
chown "$(id -un):$(id -gn)" .env.live
stat -c '%A %U:%G %n' .env.live
```

The expected permissions are `-rw-------`. Load the settings in a subshell:

```bash
(
	set -a
	. ./.env.live
	set +a
	./build/bin/stripekit_drogon_checkout_demo
)
```

## Receive Webhooks

Install and log in to the Stripe CLI, then click **Start Stripe CLI listener**.
The demo starts this listener and fills in its signing secret:

```bash
stripe listen --all-snapshot --latest --forward-to http://127.0.0.1:4242/webhook
```

You can also run the command yourself and paste its `whsec_...` value into the
setup form. If automatic setup fails, the page shows the listener log path.
Open that file for the error message or signing secret.

## Try Checkout

Enter an email address and click **Continue to Checkout** to start a subscription
without a trial. Stripe collects payment on its hosted page.

Use a test card with any future expiry and a valid-format CVC:

| Scenario | Test card number |
| --- | --- |
| Successful payment | `4242 4242 4242 4242` |
| Declined payment | `4000 0000 0000 0002` |
| Always authenticate (3D Secure) | `4000 0027 6000 3184` |

See [Stripe's test cards](https://docs.stripe.com/testing) for other options.
Never use real card details.

After Checkout, Stripe returns you to the demo. Go back to the home page and
refresh it to see the verified webhook deliveries. Use the Stripe sandbox
Dashboard to view the resulting subscription and invoices.

## Try A Card-Required Trial

1. Configure the key, pricing, and webhook forwarding as described above.
   Enter an email address and click **Start a 14-day free trial**.
2. Complete Checkout with `4242 4242 4242 4242`. Stripe requires a card and shows
   no charge today, the recurring price, and the first billing date.
3. Return to the demo's home page and refresh it to see the verified webhooks.
   In the Stripe sandbox Dashboard, open the new subscription to see its
   `trialing` status and trial end. A zero-value paid invoice is normal at the
   start of a trial; it does not mean a card was charged.
4. To end the trial before billing starts, cancel the subscription in the
   Stripe sandbox Dashboard.

Unless canceled, monthly billing starts after the 14-day trial. A saved card
can still be declined or require authentication when billing begins.

The trial request uses `trial_period_days = 14`,
`payment_method_collection = "always"`, and `payment_method_types = {"card"}`.
Both buttons create a test customer from the email entered, then create a
subscription-mode Checkout Session.

This example demonstrates card-required trials, not cardless trials.

The demo runs locally and keeps its settings and webhook history only until
it exits. Each submission starts a new Checkout flow. Tenant access and trial
eligibility remain the application's responsibility.
