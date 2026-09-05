# Webhook Signature Example

This example runs a tiny Drogon server that verifies Stripe webhook signatures
with StripeKit and logs the verified event id and type.

## Build

```bash
cmake -S . -B build -DSTRIPEKIT_BUILD_EXAMPLES=ON
cmake --build build --target stripekit_webhook_signature_example
```

## Run

```bash
export STRIPE_WEBHOOK_SECRET=whsec_your_cli_secret
./build/bin/stripekit_webhook_signature_example
```

In another terminal:

```bash
stripe listen --latest --forward-to http://127.0.0.1:4242/webhook
stripe trigger payment_intent.succeeded
```

Use the exact `whsec_...` value printed by `stripe listen`.