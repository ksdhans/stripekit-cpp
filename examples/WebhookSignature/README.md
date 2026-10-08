# Webhook Signature Example

This example runs a tiny Drogon server that verifies Stripe webhook signatures
with StripeKit and logs the verified event id and type.

## Build

```bash
cmake -S . -B build -DSTRIPEKIT_BUILD_EXAMPLES=ON
cmake --build build --target stripekit_webhook_signature_example
```

## Run

The webhook signing secret comes from `stripe listen`. Start the listener first;
it can run before the example server is up.

Terminal 1:

```bash
stripe listen --all-snapshot --latest --forward-to http://127.0.0.1:4242/webhook
```

Copy the `whsec_...` value printed by the listener:

```text
Ready! Your webhook signing secret is whsec_...
```

Put that value in `.env.live`:

```bash
# .env.live
STRIPE_WEBHOOK_SECRET=whsec_your_listener_secret
```

Restrict the file so only your user can read it:

```bash
umask 077
chmod 600 .env.live
chown "$(id -un):$(id -gn)" .env.live
stat -c '%A %U:%G %n' .env.live
```

The expected permissions are `-rw-------`.

Terminal 2: run the example in a subshell so the secret is exported only to this
process:

```bash
(
	set -a
	. ./.env.live
	set +a
	./build/bin/stripekit_webhook_signature_example
)
```

Terminal 3: trigger a test event:

```bash
stripe trigger payment_intent.succeeded
```

The example server should print the verified event id and type. Use the exact
`whsec_...` value printed by the `stripe listen` process that is forwarding to
this server. If you stop and restart `stripe listen`, copy the new secret into
`.env.live` and restart the example.