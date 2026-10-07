# Changelog

Notable changes for users of StripeKit. Unreleased changes move into a dated
version section when a release is tagged.

## Unreleased


## 0.1.0 2026-10-07

### Initial Release

- Stripe-hosted Checkout for subscriptions and one-time card payments.
- Customer creation and retrieval, subscription retrieval and cancellation,
  and Stripe-hosted billing portal sessions.
- Coroutine APIs and callback overloads, with request timeouts, custom headers,
  and idempotency keys.
- Webhook signature verification and parsing for checkout, subscription,
  invoice payment, dispute, and refund events.
- Webhook de-duplication with an application-provided store, an in-memory store
  for development, and optional SQLite storage. Failed attempts can retry
  immediately; interrupted attempts can retry after a configurable timeout.
- Drogon as the default backend (adaptor), with interfaces for other frameworks.
- Installable CMake package, examples, and API documentation.

### Current Limitations

- Card-based flows only; direct PaymentIntent APIs and embedded/custom payment
  forms are not supported.
- Covers a subset of Stripe's API, not all Stripe products or request options.
- Applications handle billing policies, access control, and other business logic.
- Webhook handlers must be safe to retry after failures or crashes.
