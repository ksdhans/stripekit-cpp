# Stripe Sandbox End-To-End (E2E) Tests

These tests run against a real Stripe sandbox. They create real (test-mode)
customers, subscriptions, payments, and disputes, then assert that StripeKit
verifies, de-duplicates, and dispatches the webhook events Stripe sends back.

No money moves, but the tests do talk to Stripe over the network, so they are
opt-in and are not part of the default `ctest` run.

## How Inbound Webhooks Reach Your Machine

The suite uses the **Stripe CLI** (`stripe listen`) rather than a public URL.

The CLI opens an *outbound* connection to Stripe and forwards each event to a
local HTTP address. Nothing needs to accept inbound connections, so the same
setup works behind CGNAT, on a laptop, and on an ephemeral CI runner. You do not
need a domain name, a TLS certificate, a tunnel, or a reverse proxy.

Events forwarded this way are signed by Stripe, so signature verification is
genuinely exercised end to end.

## Prerequisites

1. **A Stripe sandbox.** In the Stripe Dashboard, use the account picker >
   **Switch to sandbox** > **Create sandbox**. An account can have up to five.
   Choose *Create an account from scratch* so the environment is reproducible.

   Use a **separate sandbox for CI** from the one you use locally. A sandbox has
   one account-wide event stream, so sharing one means local runs and CI runs see
   each other's events.

2. **The Stripe CLI**, on `PATH` as `stripe`. See
   <https://docs.stripe.com/stripe-cli>. Pin the version in CI so a CLI release
   cannot change the suite's behaviour without a commit.

3. **A build with the suite enabled:**

   ```bash
   cmake -S . -B build -DSTRIPEKIT_BUILD_SANDBOX_TESTS=ON
   cmake --build build
   ```

## Configuration

Every setting is an environment variable, so local shells and CI runners share
one contract.

| Variable | Required | Description |
| --- | --- | --- |
| `STRIPEKIT_SANDBOX_ENABLED` | Yes | Set to `1` to opt in. Without it the suite skips, so a key left in your shell cannot silently start calling Stripe. |
| `STRIPE_API_KEY` | Yes | The sandbox secret key (`sk_test_...`). The Stripe CLI reads this name automatically, so the suite never passes the key on a command line where it would appear in the process list. |
| `STRIPEKIT_SANDBOX_REQUIRED` | No | Set to `1` to fail instead of skip when configuration is missing. Use this in the CI job so a broken secret cannot pass silently. |
| `STRIPEKIT_SANDBOX_STRIPE_CLI` | No | Path to the Stripe CLI binary. Defaults to `stripe`. |
| `STRIPEKIT_SANDBOX_EVENT_TIMEOUT_SECONDS` | No | How long to wait for an expected event. Defaults to `90`. |
| `STRIPEKIT_SANDBOX_MAX_API_CALLS` | No | Stripe API call budget per run, guarding against runaway loops. Defaults to `400`. |

The suite refuses to start when `STRIPE_API_KEY` is a live-mode key, and that
refusal always fails the run.

## Running Locally

Put your sandbox key in a `.env.local` file at the repository root. That path is
git-ignored, so the key cannot be committed:

```bash
# .env.local
STRIPEKIT_SANDBOX_ENABLED=1
STRIPE_API_KEY=sk_test_your_local_sandbox_key
```

Then run the suite:

```bash
set -a; . ./.env.local; set +a
ctest --test-dir build -R stripekit.sandbox --output-on-failure
```

Or run the binary directly, optionally filtering by name:

```bash
./build/bin/stripekit_sandbox_tests --list
./build/bin/stripekit_sandbox_tests SandboxWebhookDeliveryTest
```

Without `STRIPEKIT_SANDBOX_ENABLED`, the suite prints a skip message and exits
successfully, so contributors without Stripe access are unaffected.

## Running in GitLab CI

Store the CI sandbox key as a GitLab CI/CD variable named `STRIPE_API_KEY`. It
is injected into the job as an ordinary environment variable, which is why the
local and CI setups are identical.

Configure the variable as:

- **Protected**, so it is only exposed to protected branches and tags. This is
  what keeps the key away from fork and untrusted merge request pipelines.
- **Masked**, so it cannot be printed in job logs. GitLab restricts masked
  values to the Base64 alphabet plus `@ : . ~`. If GitLab refuses to mask the
  key, store it Base64-encoded and decode it in `before_script` rather than
  leaving it unmasked.

Keep `CI_DEBUG_TRACE` disabled for the job; it defeats masking.

The `test:sandbox` job in `.gitlab-ci.yml` runs only on the protected default
branch, plus as a manual job a maintainer can start on a merge request. It also
sets `resource_group` so two pipelines cannot drive the same sandbox at once.

## What The Tests Cover

| Test file | Coverage |
| --- | --- |
| `TestSubscriptionLifecycle.cpp` | Checkout Session creation, an active subscription emitting `invoice.paid`, a declined card emitting `invoice.payment_failed`, and subscription cancellation. |
| `TestDisputeAndRefund.cpp` | `charge.refunded`, and the dispute lifecycle through `charge.dispute.created` and `charge.dispute.closed`. |
| `TestWebhookDelivery.cpp` | Live signed delivery, duplicate delivery handling, retryable-failure recovery, and rejection of a tampered payload. |

### How Replay Tests Work

The local endpoint records each delivery exactly as Stripe sent it, payload and
`Stripe-Signature` header together. Replay tests re-post that captured pair, so
duplicate handling and signature verification are checked against real Stripe
signatures instead of locally forged ones.

Recovery is tested through a second route whose handler reports a retryable
failure on its first attempt for each event. The first delivery returns 500, and
replaying it returns 200. This checks StripeKit's retry contract without waiting
on Stripe's own redelivery schedule.

## Concurrency And Isolation

Every run generates an id that is stamped onto the objects it creates, and each
test waits only for events that mention an id it created. Concurrent runs
sharing a sandbox therefore cannot consume each other's events. Using separate
sandboxes for local and CI work is still recommended, because it keeps the
event stream quiet and lets you clear test data independently.

## Known Limitations

- **Hosted Checkout cannot be completed headlessly.** Stripe's hosted checkout
  page needs a browser, so the suite asserts that a session is created correctly
  and drives the subscription itself through the API to produce the invoice
  events.
- **Stripe's server-side retry schedule is not exercised.** The Stripe CLI
  forwards events but does not reproduce Stripe's backoff behaviour for a failing
  endpoint. Testing that would require a publicly reachable endpoint registered
  in the Dashboard.
- **Sandbox settings are not seeded.** A from-scratch sandbox may have payment
  methods or capabilities disabled that your main test-mode environment has
  enabled. Check the sandbox's settings first if a flow fails only in CI.
