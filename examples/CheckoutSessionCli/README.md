# Checkout Session CLI Example

This example creates one subscription-mode Checkout Session and prints the
Stripe-hosted Checkout URL.

This is a CLI example only. It does not bind an HTTP port or serve the return
pages. `STRIPE_RETURN_BASE_URL` is the base URL that Stripe redirects to after
hosted Checkout; use an app you already have running, or use the
DrogonCheckoutDemo when you want the example itself to serve the return page.

## Build

```bash
cmake -S . -B build -DSTRIPEKIT_BUILD_EXAMPLES=ON
cmake --build build --target stripekit_checkout_session_cli
```

## Prepare Stripe Objects

This example needs an existing test-mode Customer and recurring Price because it
only demonstrates Checkout Session creation.

Create them with the Stripe CLI:

```bash
stripe customers create \
	--email dev@example.com \
	--name "StripeKit Example Customer"
```

Copy the returned `id` value beginning with `cus_...`.

```bash
stripe products create --name="StripeKit CLI demo plan"
```

Copy the returned `id` value beginning with `prod_...`, then create a recurring
Price for that Product:

```bash
stripe prices create \
	-d product=prod_your_product_id \
	-d unit_amount=1200 \
	-d currency=usd \
	-d recurring[interval]=month
```

Copy the returned `id` value beginning with `price_...`.

You can also use existing objects from the Dashboard. In test mode or in your
sandbox, open **Customers** to copy a `cus_...` ID, and open **Product catalog**
to copy a recurring `price_...` ID from a Product's pricing section.

## Run

Put the required values in `.env.live`:

```bash
# .env.live
STRIPE_SECRET_KEY=sk_test_your_secret_key
STRIPE_CUSTOMER_ID=cus_your_customer_id
STRIPE_PRICE_ID=price_your_recurring_price_id
STRIPE_RETURN_BASE_URL=http://127.0.0.1:4242
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
	./build/bin/stripekit_checkout_session_cli
)
```

The example rejects live keys and pins `Stripe-Version: 2024-06-20`.