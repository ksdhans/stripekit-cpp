# Webhook Signature Example

This example runs a tiny Drogon server that verifies Stripe webhook signatures
with StripeKit and logs the verified event id and type.

## Build

```bash
cmake -S . -B build -DSTRIPEKIT_BUILD_EXAMPLES=ON
cmake --build build --target stripekit_webhook_signature_example
```

## Run

Put the webhook signing secret in `.env.live`:

```bash
# .env.live
STRIPE_WEBHOOK_SECRET=whsec_your_cli_secret
```

Restrict the file so only your user can read it:

```bash
umask 077
chmod 600 .env.live
chown "$(id -un):$(id -gn)" .env.live
stat -c '%A %U:%G %n' .env.live
```

The expected permissions are `-rw-------`.

Run the example in a subshell so the secret is exported only to this process:

```bash
(
	set -a
	. ./.env.live
	set +a
	./build/bin/stripekit_webhook_signature_example
)
```

In another terminal:

```bash
stripe listen --latest --forward-to http://127.0.0.1:4242/webhook
stripe trigger payment_intent.succeeded
```

Use the exact `whsec_...` value printed by `stripe listen`.