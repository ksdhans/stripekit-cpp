# Billing Portal CLI Example

This example creates one Billing Portal session and prints the hosted portal
URL. It can also open the portal directly at one task, such as updating the
payment method or confirming a plan change, which is what "Fix payment" and
"Change plan" buttons in an app usually do.

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

The CLI creates a new Billing Portal session for `STRIPE_CUSTOMER_ID` and
prints two values:

- `Billing Portal Session` is the Stripe session id for this one portal visit.
	It is useful for logs, debugging, and matching the CLI output to the object in
	Stripe. Your app normally does not show this id to the customer.
- `Hosted URL` is the Stripe-hosted billing portal link for that session. Open
	this URL in a browser to view the customer's self-serve portal, where Stripe
	can show subscription, invoice, payment method, and cancellation options based
	on your Customer Portal settings and the customer's current data.

The hosted URL is temporary and intended for the customer represented by
`STRIPE_CUSTOMER_ID`. Create a new Billing Portal session when you need a fresh
portal link.

## Open the Portal at a Specific Task

Command-line options choose which portal page the customer sees. This makes it
easy to try each task without editing `.env.live`:

| Option | Purpose |
| --- | --- |
| `--flow <task>` | The task to open: `payment_method_update`, `subscription_cancel`, `subscription_update`, or `subscription_update_confirm`. Leave it out for the portal's home page. |
| `--subscription <id>` | The `sub_...` subscription to cancel or change. Required for every task except `payment_method_update`. |
| `--price <id>` | The `price_...` to switch to. Required for `subscription_update_confirm`. |
| `--configuration <id>` | A `bpc_...` portal configuration. Leave it out to use your account's default portal settings. |
| `--locale <locale>` | The portal's language, such as `de` or `pt-BR`. Leave it out to let Stripe choose. |

Run `./build/bin/stripekit_billing_portal_cli --help` for a summary.

For example, to send a customer straight to the "update payment method" page:

```bash
(
	set -a
	. ./.env.live
	set +a
	./build/bin/stripekit_billing_portal_cli --flow payment_method_update
)
```

To let a customer confirm a switch to another price:

```bash
(
	set -a
	. ./.env.live
	set +a
	./build/bin/stripekit_billing_portal_cli \
		--flow subscription_update_confirm \
		--subscription sub_your_subscription_id \
		--price price_your_new_price_id
)
```

The confirm task reads the subscription to find the item being changed, so this
example only supports subscriptions with a single item.

The plan-change tasks need plan switching turned on in your portal settings,
and the new price must be one of the prices listed there. Turn it on in the
Dashboard's Customer Portal settings, or create a configuration with
`billing_portal().configurations()` as described in the
[Billing Portal and Invoices](../../docs/BillingPortalAndInvoices.md) guide.
Otherwise Stripe rejects the request with an error message.

Opening the portal does not change anything by itself. The customer still has
to confirm in the portal, and Stripe then reports the change through webhooks
such as `customer.subscription.updated`.

The example rejects live keys and pins `Stripe-Version: 2024-06-20`.
