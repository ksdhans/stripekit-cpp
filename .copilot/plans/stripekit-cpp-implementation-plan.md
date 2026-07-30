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
- Test pyramid including sandbox E2E.
- GitLab CI with protected secrets and abuse controls.
- Packaging and integration documentation.

Out of scope:
- App billing policies and UI logic.
- ZitaForge billing business state machine internals.
- Direct, broad PaymentIntent client surface in MVP (unless required by concrete product flow).
- Non-MVP webhook families outside checkout/subscription/invoice set.

## Suggested Repository Structure

- CMakeLists.txt
- LICENSE (BSD-3-Clause)
- README.md
- include/StripeKit/
- src/core/
- src/adapters/drogon/
- tests/unit/
- tests/integration/
- tests/e2e_sandbox/
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
4. Define transport interfaces for outbound HTTP and inbound event dispatch so future backends can be added without API breakage.
5. Define request options shared by outbound methods:
   - Idempotency key, timeout, and per-request header overrides.
6. Define exception taxonomy and payload for API failures:
   - Stripe error type/code/message, HTTP status, request id, retryability.
7. Define async interface pattern:
   - Coroutine-first methods plus callback compatibility overloads.

Deliverables:
- Stable public headers and core interfaces.

## Phase 3: Drogon MVP Adapter

1. Implement Drogon outbound adapter for Stripe API requests.
2. Implement Drogon webhook endpoint adapter for inbound events.
3. Implement adapter-level error mapping and retries policy hooks.
4. Keep API surface independent of Drogon types where possible in core headers.
5. Implement idempotency header propagation for outbound `POST` requests.
6. Implement webhook dispatch for required MVP event types and deterministic ack/retry outcomes.

Deliverables:
- Working Drogon adapter package as the only first-party runtime backend in MVP.

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
2. Integration tests:
   - Drogon outbound adapter behavior.
   - Drogon webhook endpoint processing and dispatch.
   - Callback compatibility wrappers for coroutine-backed outbound operations.
3. Sandbox E2E tests:
   - Real Stripe test-mode lifecycle flows.
   - Webhook delivery and replay behavior.
   - Subscription checkout success path with entitlement grant after `invoice.paid`.
   - Failed renewal/payment path with entitlement downgrade/hold after `invoice.payment_failed`.

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
- [ ] Core public API and models implemented (customers, checkout sessions, subscriptions, billing portal sessions).
- [ ] Webhook signature verification implemented.
- [ ] Drogon outbound and inbound adapters implemented (including idempotency propagation and required webhook dispatch).
- [ ] Dependency injection and fallback fetching wired.
- [ ] Unit/integration/sandbox E2E suites implemented (including invoice-based entitlement scenarios).
- [ ] GitLab CI gating and protected secret policy implemented.
- [ ] Packaging and consumer integration docs completed.
- [ ] README Status and Roadmap updated to reflect exact MVP-supported vs deferred API/event scope.
