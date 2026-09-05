# Checkout Session CLI Example

This example creates one subscription-mode Checkout Session and prints the
Stripe-hosted Checkout URL.

## Build

```bash
cmake -S . -B build -DSTRIPEKIT_BUILD_EXAMPLES=ON
cmake --build build --target stripekit_checkout_session_cli
```

## Run

```bash
export STRIPE_SECRET_KEY=sk_test_your_secret_key
export STRIPE_CUSTOMER_ID=cus_your_customer_id
export STRIPE_PRICE_ID=price_your_recurring_price_id
export STRIPE_RETURN_DOMAIN=http://127.0.0.1:4242
./build/bin/stripekit_checkout_session_cli
```

The example rejects live keys and pins `Stripe-Version: 2024-06-20`.