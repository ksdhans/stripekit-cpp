# StripeKit C++ Implementation Plan

## Goal
Create StripeKit C++ as a standalone BSD-3-Clause open-source repository for Stripe integration in C++, designed for framework extensibility while shipping a Drogon-first MVP backend.

Repository URL stub:
- https://gitlab.com/<group-or-user>/stripekit-cpp

## Core Decisions

1. License: BSD-3-Clause.
2. MVP backend policy:
   - Drogon is the only first-party inbound and outbound backend in MVP.
   - Keep explicit interfaces so additional framework adapters can be added later.
3. Testing policy:
   - Full Stripe sandbox E2E is mandatory for trusted main-branch pipelines.
   - MR pipelines run secret-free test subsets.
4. Dependency policy:
   - Do not vendor external dependencies into the repo.
   - Support external dependency injection from parent projects.
   - Provide fallback fetching when dependencies are not supplied.
5. Public API style policy:
   - Mirror Stripe official server SDK patterns where practical: client object with resource-grouped methods.
   - Implement only MVP-required resources/methods first.
6. Error model policy:
   - Exceptions-only for public API failures (no status-code return values in primary API).
7. Async/concurrency policy:
   - Coroutine-first outbound API surface with callback-based compatibility overloads.
   - Inbound webhook verification/parsing remains synchronous and framework-agnostic in core.
8. SaaS billing policy for MVP:
   - Paid-plan entitlement is based on invoice payment outcome.
   - `invoice.paid` and `invoice.payment_failed` are required webhook events.
9. Billing portal policy:
   - Include `billing_portal.sessions.create` in MVP for end-user self-serve billing management.
10. PaymentIntent policy:
   - Defer direct PaymentIntent APIs/events unless a concrete MVP flow requires standalone non-Checkout payment orchestration.
11. Idempotency policy:
   - Support idempotency keys on all outbound Stripe `POST` operations.
   - Provide safe retry behavior with idempotency propagation.
12. Business-logic boundary policy:
   - StripeKit provides Stripe API access, verification, and normalized event signals.
   - Entitlement state transitions and billing business policy remain in application or framework code.
13. Card payment-method policy for MVP:
   - MVP supports card-based flows only.
   - Keep webhook/event contracts extensible so delayed and non-card payment methods can be added later without API breakage.
14. Webhook de-duplication storage policy:
   - StripeKit defines a store interface; the host app/framework provides the concrete durable store.
   - Preferred MVP deployment uses the app's primary database through that interface.
   - Provide in-memory store for tests and local development.
   - Provide optional SQLite store adapter as a convenience default for Drogon consumers.
15. Callback delivery policy:
   - Callback overloads must invoke completion exactly once.
   - Drogon adapter callback completions execute on the Drogon event loop bound to the operation context.
   - For operations without request context, callbacks execute on a configured default adapter event loop.
16. Crypto implementation policy:
   - MVP uses libsodium-backed HMAC verification implementation.
   - Keep an internal crypto interface boundary so future backend substitution does not require public API breakage.
17. Logging policy:
   - StripeKit defines a logger interface with app-configurable log level filtering.
   - Adapters should bridge to framework-native logging when available.
   - If no logger is provided and backend logging bridge is unavailable, fallback to console logging.
   - Default Drogon level mapping:
     - `trace` -> `TRACE`
     - `debug` -> `DEBUG`
     - `info` -> `INFO`
     - `warn` -> `WARN`
     - `error` -> `ERROR`
18. Webhook lifecycle policy:
   - Define explicit handling for duplicate-processed, duplicate-inflight, retryable-store-error, and fatal-store-error outcomes.
   - Define stale-inflight reclaim timeout semantics so crash/restart does not block later deliveries indefinitely.
   - Default stale-inflight reclaim timeout: 15 minutes (configurable).
   - Default outcome policy aligned with Stripe webhook retry guidance:
     - `duplicate-processed` -> return success/ack (`2xx`).
     - `duplicate-inflight` -> return retryable response (`5xx`) so Stripe retries later.
     - `retryable-store-error` -> return retryable response (`5xx`).
     - `fatal-store-error` -> return retryable response (`5xx`) and emit high-severity logs/metrics.
19. Logging safety policy:
   - Never log secrets or full sensitive payloads (for example: API keys, webhook secrets, authorization headers, and raw card/bank details).
   - Structured logs must redact sensitive fields by default.

## Scope

In scope:
- Framework-agnostic core models, contracts, and verification logic.
- Drogon adapter for outbound API and inbound webhook handling.
- Stripe-like client/resource MVP APIs for SaaS billing:
   - `customers`
   - `checkout.sessions` (subscription mode MVP)
   - `subscriptions`
   - `billing_portal.sessions.create`
- Required inbound webhook handling for SaaS MVP:
   - `checkout.session.completed`
   - `customer.subscription.created`
   - `customer.subscription.updated`
   - `customer.subscription.deleted`
   - `invoice.paid`
   - `invoice.payment_failed`
- Required asynchronous post-payment card events for SaaS safety:
   - `charge.dispute.created`
   - `charge.dispute.closed`
   - `charge.refunded`
- Test pyramid including sandbox E2E.
- GitLab CI with protected secrets and abuse controls.
- Packaging and integration documentation.

Out of scope:
- App billing policies and UI logic.
- ZitaForge billing business state machine internals.
- Direct, broad PaymentIntent client surface in MVP (unless required by concrete product flow).
- Non-MVP webhook families outside checkout/subscription/invoice/dispute/refund set.
- Non-card payment methods in MVP.

## Suggested Repository Structure

- CMakeLists.txt
- LICENSE (BSD-3-Clause)
- README.md
- include/StripeKit/
- src/core/
- src/adapters/drogon/
- tests/unit/
- tests/integration/
- tests/sandbox/
- examples/
- docs/
- cmake/
- .gitlab-ci.yml
- ci/

## Phase 1: Repository Foundation

1. Initialize repository with BSD-3-Clause license and baseline docs.
2. Create CMake project with install/export targets.
3. Define versioning policy and release tagging format.

Deliverables:
- Buildable skeleton repo with docs and CI placeholders.

## Phase 2: Core API And Extension Contracts

1. Implement framework-agnostic core models:
   - Customer, CheckoutSession, Subscription, Invoice, BillingPortalSession, Event envelope.
2. Implement client API contracts for Stripe operations.
3. Implement webhook signature verification core using libsodium.
4. Define HTTP client interfaces for outbound HTTP and inbound event dispatch so future backends can be added without API breakage.
5. Define request options shared by outbound methods:
   - Idempotency key, timeout, and per-request header overrides.
6. Define exception taxonomy and payload for API failures:
   - Stripe error type/code/message, HTTP status, request id, retryability.
7. Define async interface pattern:
   - Coroutine-first methods plus callback compatibility overloads.
8. Define event-signal boundary:
   - Core exposes a Stripe-style `webhooks::construct_event` helper for verified event construction.
   - Core exposes `webhooks::process_event_once` for durable de-duplicated processing.
   - Core does not enforce app entitlement/business transitions.
9. Define webhook de-duplication store contract:
   - Atomic first-seen claim/mark by Stripe event id.
   - Configurable retention/TTL semantics.
   - Framework/app-owned persistence implementation.
   - Include explicit claim outcomes (for example: claimed, duplicate-processed, duplicate-inflight, retryable-store-error, fatal-store-error).
   - Define stale-inflight reclaim timeout and re-claim behavior after process crashes.
   - Default stale-inflight reclaim timeout is 15 minutes, configurable by application.
   - Define default response mapping for each claim outcome, with `2xx` for already-processed duplicates and `5xx` for retryable conditions.
10. Define persistence adapter contract defaults:
   - In-memory store implementation for tests/local development.
   - Optional SQLite adapter implementation suitable for app-owned SQLite deployments.
   - Document recommendation to use app primary database unless strict isolation is required.
11. Define callback compatibility contract:
   - Exactly-once completion semantics.
   - Adapter-documented callback execution context.
12. Define logging contract:
   - Log levels (trace/debug/info/warn/error) and per-client configurable threshold.
   - Structured context fields for request id, event id, webhook type, retry attempt, and adapter name.
   - Null logger behavior and console fallback behavior.
   - Required redaction behavior for secrets and sensitive payload fields.

Deliverables:
- Stable public headers and core interfaces.

## Phase 3: Drogon MVP Adapter

1. Implement Drogon outbound adapter for Stripe API requests.
2. Implement Drogon webhook endpoint adapter for inbound events.
3. Implement adapter-level error mapping and retries policy hooks.
4. Keep API surface independent of Drogon types where possible in core headers.
5. Implement idempotency header propagation for outbound `POST` requests.
6. Implement webhook dispatch for required MVP event types and deterministic ack/retry outcomes.
7. Implement de-duplication and replay-safe dispatch for repeated webhook deliveries using app-provided store interface.
   - Implement stale-inflight reclaim behavior based on configured timeout.
8. Ensure callback overload completions are delivered exactly once on documented adapter execution context.
9. Provide optional SQLite store adapter wiring path for Drogon users who want a turnkey default.
10. Bridge StripeKit logs into Drogon logger with configurable level mapping.

Deliverables:
- Working Drogon adapter package as the only first-party runtime backend in MVP.
   - Public adapter API is exposed through `StripeKit/Adapters/Drogon.h`.
   - The adapter provides `make_stripe_client` and `register_webhook_handler` convenience entry points.

## Phase 4: Dependency Strategy

1. External dependency injection first:
   - If consumer provides libsodium and Drogon targets, reuse them.
2. Fallback strategy:
   - Fetch dependencies when targets are unavailable.
3. Ensure no duplicate vendored dependency trees are committed.

Deliverables:
- Flexible CMake dependency resolution.

## Phase 5: Testing Pyramid

1. Unit tests:
   - Signature verification.
   - Request/response model behavior.
   - Error mapping and idempotency utilities.
   - Exception taxonomy behavior and retryability hints.
   - Webhook de-duplication store contract behavior.
   - Claim outcome semantics and retryability mapping.
   - Callback exactly-once completion contract behavior.
   - Logger threshold and fallback behavior.
2. Integration tests:
   - Drogon outbound adapter behavior.
   - Drogon webhook endpoint processing and dispatch.
   - Callback compatibility wrappers for coroutine-backed outbound operations.
   - Duplicate webhook delivery handling and idempotent dispatch behavior.
   - Crash/restart stale-inflight reclaim behavior.
   - Drogon callback execution context behavior.
   - Drogon log bridge behavior and level mapping.
   - Redaction behavior for sensitive log fields.
3. Sandbox E2E tests:
   - Real Stripe test-mode lifecycle flows.
   - Webhook delivery, replay, and recovery behavior.
   - Subscription checkout success path with `invoice.paid` signal emission for app-level entitlement decisions.
   - Failed renewal/payment path with `invoice.payment_failed` signal emission for app-level entitlement decisions.
   - Card dispute lifecycle signal path (`charge.dispute.created` and `charge.dispute.closed`).
   - Card refund signal path (`charge.refunded`).

Acceptance criteria:
- Fast unit/integration checks reliable in MR pipelines.
- Full sandbox E2E reliable in trusted pipelines.

## Phase 6: GitLab CI And Sandbox Abuse Controls

1. MR pipeline policy:
   - Run unit and integration only.
   - Do not expose Stripe secrets to untrusted/fork contexts.
2. Trusted pipeline policy:
   - On protected main branch, run full suite including sandbox E2E.
   - Add optional maintainer-approved manual job for pre-merge sandbox runs.
3. Secret handling:
   - Store Stripe test keys as protected and masked GitLab variables.
4. Abuse prevention:
   - Concurrency limits for sandbox jobs.
   - Rate/quota guards in test harness.
   - Environment scoping for destructive test classes.

Deliverables:
- Hardened .gitlab-ci.yml and ci scripts with clear gating rules.

## Phase 7: Packaging And Consumer Integration

1. Add install/export targets for downstream CMake consumers.
2. Document consumer integration patterns for CMake-based projects.
3. Add migration guidance for adding future non-Drogon adapters.

Deliverables:
- Consumer-ready package and integration docs.

## Future Adapter Roadmap (Post-MVP)

1. Keep adapter interface stable.
2. Add optional adapters for other frameworks only when demand is validated.
3. Validate each new adapter against the same sandbox E2E contract suite.

## Implementation Checklist
- [x] Repository initialized with BSD-3-Clause and baseline docs.
- [x] Core public API and models implemented (customers, checkout sessions, subscriptions, billing portal sessions). (The HTTP-client-backed `StripeApiClient` implements the MVP resource operations.)
- [x] Webhook signature verification implemented.
- [x] Drogon outbound and inbound adapters implemented (including idempotency propagation and required webhook dispatch). (The adapter is an optional CMake target and requires Drogon with its system prerequisites.)
- [x] Dependency injection and fallback fetching wired.
- [x] Unit/integration/sandbox E2E suites implemented (including invoice-based entitlement signals, dispute events, and refund signal coverage). (Sandbox coverage is opt-in locally and runs serially against a real Stripe sandbox.)
- [x] GitLab CI gating and protected secret policy implemented. (Normal pipelines run CTest without sandbox secrets; protected default-branch pipelines run sandbox E2E, and same-project merge requests can run it manually with serialized sandbox access.)
- [x] Packaging and consumer integration docs completed. (Installed targets and dependencies are wired; CTest includes an installed-package consumer check. The README and Quickstart cover CMake consumers, and WritingAnAdapter includes migration and validation guidance.)
- [x] README refreshed for MVP release messaging and public-facing scope summary.

## Completion Validation

- Local build and unit, integration, and installed-package consumer tests pass.
- The installed package exposes `StripeKit::stripekit` and `StripeKit::drogon`,
  includes the fetched libsodium archive, and discovers enabled SQLite support.
- Real Stripe sandbox E2E remains required in trusted pipelines. It was not
  rerun as part of the final packaging and documentation work.
