# Stripe Sandbox End-To-End (E2E) Tests

These tests run against a real Stripe sandbox. They create real (test-mode)
products, prices, customers, subscriptions, payments, and disputes. They check
catalog operations against Stripe and verify that StripeKit verifies,
de-duplicates, and dispatches the webhook events Stripe sends back.

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

3. **Drogon with TLS support.** Stripe's API only accepts HTTPS. When StripeKit
   fetches Drogon itself, the build requires Trantor's OpenSSL backend.

4. **A build with the suite enabled:**

   ```bash
   cmake -S . -B build -DSTRIPEKIT_BUILD_SANDBOX_TESTS=ON
   cmake --build build --parallel
   ```

## Configuration

Every setting is an environment variable, so local shells and CI runners share
one contract.

| Variable | Required | Description |
| --- | --- | --- |
| `STRIPEKIT_SANDBOX_ENABLED` | Yes | Set to `1` to opt in. Without it the suite skips, so a key left in your shell cannot silently start calling Stripe. |
| `STRIPE_API_KEY` | Yes | The **sandbox secret API key** (`sk_test_...`) from Dashboard > Developers > API keys while the sandbox is selected. Do not use the publishable key (`pk_test_...`). The Stripe CLI reads this name automatically, so the suite never passes the key on a command line where it would appear in the process list. |
| `STRIPEKIT_SANDBOX_REQUIRED` | No | Set to `1` to fail instead of skip when configuration is missing. Use this in the CI job so a broken secret cannot pass silently. |
| `STRIPEKIT_SANDBOX_STRIPE_CLI` | No | Path to the Stripe CLI binary. Defaults to `stripe`. |
| `STRIPEKIT_SANDBOX_EVENT_TIMEOUT_SECONDS` | No | How long to wait for an expected event. Defaults to `90`. |
| `STRIPEKIT_SANDBOX_MAX_API_CALLS` | No | Stripe API call budget per run, guarding against runaway loops. Defaults to `400`. |

The suite refuses to start when `STRIPE_API_KEY` is a publishable key or a
live-mode key, and that refusal always fails the run. The required value is the
secret key beginning with `sk_test_`.

## Running Locally

Put your **sandbox secret API key** in a `.env.local` file at the repository
root. In the Dashboard, select the CI or local sandbox, open **Developers > API
keys**, and copy its **Secret key** beginning with `sk_test_`. Do not copy the
Publishable key beginning with `pk_test_`. The path is git-ignored, so the key
cannot be committed:

```bash
# .env.local
STRIPEKIT_SANDBOX_ENABLED=1
STRIPE_API_KEY=sk_test_your_local_sandbox_secret_key
```

Protect the file at the filesystem level as well. It should be owned by your
user and readable/writable only by that user:

```bash
umask 077
chmod 600 .env.local
chown "$(id -un):$(id -gn)" .env.local
stat -c '%A %U:%G %n' .env.local
```

The expected permissions are `-rw-------`. `chmod 600` is the important step;
`umask 077` also ensures files you create later in the same shell default to
user-only access. The file is listed in `.gitignore`, but verify that Git is
ignoring it before running tests:

```bash
git check-ignore -v .env.local
```

Then run the suite:

```bash
(
   set -a
   . ./.env.local
   set +a
   ctest --test-dir build -R stripekit.sandbox -V
)
```

Loading the file inside the subshell keeps `STRIPE_API_KEY` and the other
sandbox variables available to `ctest` and its child processes only. They are
removed from the shell environment when the subshell exits.

Verbose CTest output is recommended for this suite because live Stripe webhook
delivery can take several seconds. The sandbox helper prints `[SANDBOX]`
progress lines while it makes Stripe API calls and waits for incoming webhooks.

Or run the binary directly, optionally filtering by name:

```bash
(
   set -a
   . ./.env.local
   set +a
   ./build/bin/stripekit_sandbox_tests --list
   ./build/bin/stripekit_sandbox_tests SandboxWebhookDeliveryTest
)
```

Without `STRIPEKIT_SANDBOX_ENABLED`, the suite prints a skip message and exits
successfully, so contributors without Stripe access are unaffected.

## Running in GitLab CI

Store the CI sandbox **secret API key** as a GitLab CI/CD variable named
`STRIPE_API_KEY`. Use the `sk_test_...` value from the CI sandbox's **Secret key**
field, never its `pk_test_...` Publishable key. GitLab injects it into the job
as an ordinary environment variable, which is why the local and CI setups are
identical.

Configure the variable as:

- **Protected**, so it is only exposed to protected branches and tags. This is
  what keeps the key away from fork and untrusted merge request pipelines.
- **Masked and hidden**, so it is both redacted from job logs and not revealable
   later in the GitLab settings UI. Choose this when creating the variable; GitLab
   does not allow an existing variable to be changed to hidden. The value must
   satisfy GitLab's masked-variable rules.

Use **Masked** instead only when you need to view or edit the value later. Both
settings mask the value in job logs; hiding controls whether the value can be
revealed in the GitLab UI. If GitLab refuses to mask the key, check the value
has no whitespace or line breaks and that variable expansion is disabled.

Keep `CI_DEBUG_TRACE` disabled for the job; it defeats masking.

The `test:sandbox` job in `.gitlab-ci.yml` runs only on the protected default
branch, plus as a manual job a maintainer can start on a merge request. It also
sets `resource_group` so two pipelines cannot drive the same sandbox at once.

## What The Tests Cover

| Test file | Coverage |
| --- | --- |
| `TestCatalog.cpp` | Product creation, retrieval, updates, default-price selection and rejection of invalid defaults; price creation, mutable updates, clearing fields, lookup-key transfers, archiving and reactivation; filtering and pagination; Checkout using catalog prices; grandfathered subscriptions; migrations preserving quantities and other items; all four payment behaviors, failed-payment recovery, authentication-required payments, deterministic proration amounts, idempotent retries, and related invoice/subscription webhooks. |
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

- **Catalog tests do not advance subscriptions to renewal.** They check immediate
  changes and preview the next invoice. Future-dated price changes through
  Subscription Schedules are not covered or exposed by StripeKit.
  Catalog tests expire their Checkout sessions, cancel their subscriptions, and
  clear product defaults before archiving their prices and products, including
  after an assertion fails. Test clocks and their customers are deleted.
  Archived objects and test customers remain in the sandbox for inspection.
- **Authentication completion needs a browser.** The automated test verifies the
  `requires_action` payment state and signed action-required webhook. Completing
  the 3D Secure challenge through your payment or recovery page remains a manual
  check.
- **Hosted Checkout cannot be completed headlessly.** Stripe's hosted checkout
  page needs a browser, so the suite asserts that a session is created correctly
  and drives the subscription itself through the API to produce the invoice
  events. Use the Drogon Checkout demo below to cover the part the automated
  suite cannot reach.
- **Stripe's server-side retry schedule is not exercised.** The Stripe CLI
  forwards events but does not reproduce Stripe's backoff behaviour for a failing
  endpoint. Testing that would require a publicly reachable endpoint registered
  in the Dashboard.
- **Sandbox settings are not seeded.** A from-scratch sandbox may have payment
  methods or capabilities disabled that your main test-mode environment has
  enabled. Check the sandbox's settings first if a flow fails only in CI.

## Verifying Hosted Checkout With The Demo

`SandboxSubscriptionTest.CheckoutSessionIsCreatedForSubscriptionMode` only
checks that StripeKit can create a Checkout Session correctly. It cannot drive
the hosted page itself, complete a real card entry, or follow the redirect back
to `success_url`, since that page is rendered and operated by Stripe in a
browser. Cover that part with [examples/DrogonCheckoutDemo](../../examples/DrogonCheckoutDemo/README.md)
before releases that touch checkout, or when changing
`CreateCheckoutSessionRequest` handling:

1. Build the demo:

   ```bash
   cmake -S . -B build -DSTRIPEKIT_BUILD_EXAMPLES=ON
   cmake --build build --target stripekit_drogon_checkout_demo
   ```

2. Run `./build/bin/stripekit_drogon_checkout_demo` and open
   `http://127.0.0.1:4242`.
3. Enter a sandbox **secret API key** (`sk_test_...`) and the `whsec_...` value
   from:

   ```bash
   stripe listen --all-snapshot --latest --forward-to http://127.0.0.1:4242/webhook
   ```

4. Start Checkout from the mock sales page and pay with a
   [Stripe test card](https://docs.stripe.com/testing#cards), for example
   `4242 4242 4242 4242` with any future expiry and any CVC.
5. Confirm the browser redirects to the demo return page.
6. Confirm the terminal running `stripe listen` shows `invoice.paid`, and that
   the demo's webhook table shows the verified event.
7. Repeat with a declining test card (for example `4000 0000 0000 0002`) and
   confirm the checkout page shows a decline and no `invoice.paid` event
   arrives.
