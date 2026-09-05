# Billing Portal CLI Example

This example creates one Billing Portal session and prints the hosted portal
URL.

## Build

```bash
cmake -S . -B build -DSTRIPEKIT_BUILD_EXAMPLES=ON
cmake --build build --target stripekit_billing_portal_cli
```

## Run

```bash
export STRIPE_SECRET_KEY=sk_test_your_secret_key
export STRIPE_CUSTOMER_ID=cus_your_customer_id
export STRIPE_RETURN_URL=http://127.0.0.1:4242/account
./build/bin/stripekit_billing_portal_cli
```

The example rejects live keys and pins `Stripe-Version: 2024-06-20`.