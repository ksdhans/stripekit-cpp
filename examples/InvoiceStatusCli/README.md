# Invoice Status CLI Example

This example reads one invoice and prints what a "payment problem" notice in
your app would need: how much is still owed, when Stripe will retry the
payment, and the Stripe-hosted page where the customer can pay.

This is a CLI example only. It does not bind an HTTP port.

## Build

```bash
cmake -S . -B build -DSTRIPEKIT_BUILD_EXAMPLES=ON
cmake --build build --target stripekit_invoice_status_cli
```

## Prepare Stripe Objects

You need either an invoice ID (`in_...`) or a subscription ID (`sub_...`).

To see an unpaid invoice, create a subscription for a customer whose card is
declined. This uses the Stripe CLI and Stripe's `pm_card_chargeCustomerFail`
test card, which can be saved but fails when charged:

```bash
stripe customers create \
	--email dev@example.com \
	--payment-method pm_card_chargeCustomerFail \
	-d "invoice_settings[default_payment_method]=pm_card_chargeCustomerFail"

stripe subscriptions create \
	--customer cus_your_customer_id \
	-d "items[0][price]=price_your_monthly_price_id" \
	--payment-behavior allow_incomplete
```

Copy the subscription's `id` value beginning with `sub_...`. If you need a
price, the [ProductCatalogCli](../ProductCatalogCli/README.md) example creates
one.

## Run

Put your secret key in `.env.live`:

```bash
# .env.live
STRIPE_SECRET_KEY=sk_test_your_secret_key
```

Restrict the file so only your user can read it, as described in the
[examples README](../README.md#running-with-local-secrets), then run the
example in a subshell. Choose the invoice on the command line:

| Option | Purpose |
| --- | --- |
| `--invoice <id>` | The `in_...` invoice to show. |
| `--subscription <id>` | Show the latest invoice of this `sub_...` subscription. |

Give exactly one of them. For example:

```bash
(
	set -a
	. ./.env.live
	set +a
	./build/bin/stripekit_invoice_status_cli --subscription sub_your_subscription_id
)
```

For the declined subscription above, the output looks like this:

```text
Invoice in_1Abc... (ABCD1234-0001): open
Owed: 15.00 USD of 15.00 USD
Pay now: https://invoice.stripe.com/i/...
PDF: https://pay.stripe.com/invoice/...
```

- **Owed** is the amount still unpaid, out of the invoice total.
- **Next automatic retry** appears when Stripe plans to try the payment again.
  Stripe does not retry the first invoice of a subscription that never became
  active, so it is missing in the example above.
- **Due** appears for invoices sent to the customer to pay, rather than charged
  automatically.
- **Pay now** appears while the invoice is open. Opening the link lets the
  customer pay with another card, or complete an extra bank check such as
  3D Secure. Stripe then sends the `invoice.paid` webhook.
- **PDF** downloads a copy of the invoice.

The example rejects live keys and pins `Stripe-Version: 2024-06-20`.
