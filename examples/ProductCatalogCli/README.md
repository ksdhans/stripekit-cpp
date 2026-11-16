# Product Catalog CLI Example

This example creates a reusable product and monthly price, changes the product's
display details, then introduces a replacement price. It finds that price by
lookup key, selects it as the product's default, and archives the old price.

The original price is **25 USD/month** and the replacement is **30 USD/month**.
Amounts are in cents. Changing the amount requires creating another Price;
neither selecting a default nor archiving the old price changes existing
subscriptions. This program does not create or migrate subscriptions.

## Build

```bash
cmake -S . -B build -DSTRIPEKIT_BUILD_EXAMPLES=ON
cmake --build build --target stripekit_product_catalog_cli
```

## Run

No existing Stripe objects or webhook listener are needed. Use a test-mode
secret key and choose a unique catalog label for this walkthrough:

```bash
# .env.live (git-ignored; test-mode values only)
STRIPE_SECRET_KEY=sk_test_your_secret_key
STRIPEKIT_CATALOG_ID=my_catalog_demo_01
```

The label accepts 1-64 ASCII letters, digits, underscores, or hyphens.
It determines the product ID, versioned price lookup keys, and idempotency keys.
Use a label reserved for this example, not an existing application catalog.
The program rejects live and publishable keys before contacting Stripe.

Protect the secrets file and load it only in a subshell:

```bash
chmod 600 .env.live
(
    set -a
    . ./.env.live
    set +a
    ./build/bin/stripekit_product_catalog_cli
)
```

Use `--help` to display the required settings without contacting Stripe.
Requests pin `Stripe-Version: 2026-10-28.endive`.

Output includes:

```text
Product: stripekit_catalog_my_catalog_demo_01
Original price (USD 25.00/month): price_...
Replacement price (USD 30.00/month): price_...
Recovered by lookup key: price_...
Updated product: StripeKit Standard Plus
Default price: price_...
Archived original price: price_...
Use with CheckoutSessionCli: STRIPE_PRICE_ID=price_...
Existing subscriptions have not been changed.
Test product and replacement price remain active for use with Checkout.
```

Copy the replacement Price ID into `STRIPE_PRICE_ID` for
[CheckoutSessionCli](../CheckoutSessionCli/README.md), along with that example's
customer and return URL settings. Selecting a product default does not make
Checkout select it automatically; supply the Price ID explicitly.

## Reruns And Interrupted Requests

Each mutation has a separate, stable idempotency key. If a request fails, rerun
with the same label and unchanged program to reuse Stripe's retained responses.
An earlier step may have succeeded even if the program exits with an error.
The program does not automatically retry or roll back partial changes.

Stripe's idempotency records are not permanent; keys can be removed after at
least 24 hours. This walkthrough is not a long-running catalog synchronizer.
For a new walkthrough or changed pricing inputs, choose a new label. Do not
reuse the label after manually modifying or cleaning up its objects.

A real application should save product and price IDs. When recovering a price
by lookup key, validate its product, currency, amount, recurrence, and availability,
as this example does; matching a key alone is not enough.

## Objects Left In Stripe

The program intentionally leaves the product and replacement price active so
you can use them with Checkout. The old price is archived. No money is charged.

When finished, use the same test account or sandbox in the Dashboard to archive
the product. To archive its default price too, first change or clear the
product's default through Stripe's API, then archive the price. StripeKit
supports selecting a replacement default but does not expose clearing it.

For existing subscription migrations, explicit payment/proration choices, and
payment recovery, see [Products and Prices](../../docs/ProductsAndPrices.md).
