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

## Scope

In scope:
- Framework-agnostic core models, contracts, and verification logic.
- Drogon adapter for outbound API and inbound webhook handling.
- Test pyramid including sandbox E2E.
- GitLab CI with protected secrets and abuse controls.
- Packaging and integration documentation.

Out of scope:
- App billing policies and UI logic.
- ZitaForge billing business state machine internals.

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
   - Customer, CheckoutSession, Subscription, Invoice, Event envelope.
2. Implement client API contracts for Stripe operations.
3. Implement webhook signature verification core using libsodium.
4. Define transport interfaces for outbound HTTP and inbound event dispatch so future backends can be added without API breakage.

Deliverables:
- Stable public headers and core interfaces.

## Phase 3: Drogon MVP Adapter

1. Implement Drogon outbound adapter for Stripe API requests.
2. Implement Drogon webhook endpoint adapter for inbound events.
3. Implement adapter-level error mapping and retries policy hooks.
4. Keep API surface independent of Drogon types where possible in core headers.

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
2. Integration tests:
   - Drogon outbound adapter behavior.
   - Drogon webhook endpoint processing and dispatch.
3. Sandbox E2E tests:
   - Real Stripe test-mode lifecycle flows.
   - Webhook delivery and replay behavior.

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
- [ ] Core public API and models implemented.
- [ ] Webhook signature verification implemented.
- [ ] Drogon outbound and inbound adapters implemented.
- [ ] Dependency injection and fallback fetching wired.
- [ ] Unit/integration/sandbox E2E suites implemented.
- [ ] GitLab CI gating and protected secret policy implemented.
- [ ] Packaging and consumer integration docs completed.
