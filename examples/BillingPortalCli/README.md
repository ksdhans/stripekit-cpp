# Billing Portal CLI Example

This example creates one Billing Portal session and prints the hosted portal
URL.

This is a CLI example only. It does not bind an HTTP port or serve the return
page. `STRIPE_RETURN_URL` is the URL that Stripe redirects to after the hosted
Billing Portal flow; use an app you already have running if you want that page
to resolve locally.

## Build

```bash
cmake -S . -B build -DSTRIPEKIT_BUILD_EXAMPLES=ON
cmake --build build --target stripekit_billing_portal_cli
```

## Prepare Stripe Objects

This example needs an existing test-mode Customer because Billing Portal
sessions are created for a Customer.

Create one with the Stripe CLI:

```bash
stripe customers create \
	--email dev@example.com \
	--name "StripeKit Portal Example Customer"
```

Copy the returned `id` value beginning with `cus_...`.

You can also use an existing Customer from the Dashboard. In test mode or in
your sandbox, open **Customers**, choose a customer, and copy the `cus_...` ID.

The hosted Billing Portal is most useful when the Customer has a subscription or
saved payment method, and your Stripe account's Customer Portal settings are
configured. A new empty Customer is enough to test that StripeKit creates the
portal session, but the hosted portal page may not show much.

## Run

Put the required values in `.env.live`:

```bash
# .env.live
STRIPE_SECRET_KEY=sk_test_your_secret_key
STRIPE_CUSTOMER_ID=cus_your_customer_id
STRIPE_RETURN_URL=http://127.0.0.1:4242/account
```

Restrict the file so only your user can read it:

```bash
umask 077
chmod 600 .env.live
chown "$(id -un):$(id -gn)" .env.live
stat -c '%A %U:%G %n' .env.live
```

The expected permissions are `-rw-------`.

Run the example in a subshell so those values are exported only to this process:

```bash
(
	set -a
	. ./.env.live
	set +a
	./build/bin/stripekit_billing_portal_cli
)
```

The example rejects live keys and pins `Stripe-Version: 2024-06-20`.