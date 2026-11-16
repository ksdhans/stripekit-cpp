# Checkout Session CLI Example

This example creates one subscription-mode Checkout Session and prints the
Stripe-hosted Checkout URL.

This is a CLI example only. It does not open a browser, bind an HTTP port, or
serve the return pages. After the program prints `Hosted URL`, copy that URL
into a browser and complete the Stripe-hosted Checkout flow there.

`STRIPE_RETURN_BASE_URL` is the base URL that Stripe redirects to after hosted
Checkout. The CLI appends `/success?session_id={CHECKOUT_SESSION_ID}` and
`/cancel` when it creates the session. Use an app you already have running at
that base URL, or use the DrogonCheckoutDemo when you want the example itself to
serve the return page.

## Build

```bash
cmake -S . -B build -DSTRIPEKIT_BUILD_EXAMPLES=ON
cmake --build build --target stripekit_checkout_session_cli
```

## Prepare Stripe Objects

This example needs an existing test-mode Customer and recurring Price because it
only demonstrates Checkout Session creation.

Alternatively, use [ProductCatalogCli](../ProductCatalogCli/README.md) to create
the product and recurring price with StripeKit, then copy its replacement Price
ID and skip the product/price commands below. You still need a Customer.

Create the needed objects with the Stripe CLI:

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

The output includes a Checkout Session ID and a hosted Stripe Checkout URL:

```text
Checkout Session: cs_test_...
Hosted URL: https://checkout.stripe.com/c/pay/cs_test_...
```

Open the hosted URL in a browser. When checkout succeeds, Stripe redirects the
browser to `STRIPE_RETURN_BASE_URL/success?session_id=cs_test_...`; when it is
canceled, Stripe redirects to `STRIPE_RETURN_BASE_URL/cancel`. Those pages must
be served by your own app if you want the browser to show a real confirmation
page. If nothing is listening at `STRIPE_RETURN_BASE_URL`, session creation still
works, but the final redirect will fail in the browser after leaving Stripe.

The example rejects live keys and pins `Stripe-Version: 2026-10-28.endive`.