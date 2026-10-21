# Changelog

Notable changes for users of StripeKit. Unreleased changes move into a dated
version section when a release is tagged.

## Unreleased

- Add invoice retrieval, with more invoice fields (e.g., hosted payment link)
- Billing portal sessions can set the configuration and language, and open
  directly at a specific task (e.g., updating the payment method)
- Add billing portal configuration management
- Custom StripeClient and BillingPortal implementations now need `invoices()`
  and `configurations()`
- Add a Billing Portal and Invoices guide
- BillingPortalCli can now open the portal at a specific task
- Added a new InvoiceStatusCli example shows what an invoice still owes


## 0.2.1 2026-10-15

- Added some missing fields to Subscription and SubscriptionItem (e.g., 
  trial_end_unix)


## 0.2.0 2026-10-15

- Added info about Stripe sandbox's test credit card numbers, including the
  "always authenticate" number that shows a 3D secure challenge page
- Add product creation, retrieval, and updates, plus fixed-price creation,
  retrieval, and paginated listing with lookup-key filters.
- Preserve nullable price amounts and recurrence/quantity details when reading
  prices, and preserve encoded paths and query values in the Drogon transport.
- Custom StripeClient implementations now need products() and prices() groups.
  Existing custom response parsers inherit JSON catalog parsing by default.
- Add ProductCatalogCli, demonstrating reusable catalog creation, product updates,
  lookup-key recovery, default-price selection, and old-price archiving.
- Add product default-price selection, price archiving and mutable detail updates,
  and immediate subscription price changes with explicit proration and payment
  policies. Subscription responses expose item quantities, latest invoice IDs,
  and pending changes. Custom Prices and Subscriptions implementations must
  implement the new update methods.
- Refactored code to use one universal default_api_base_url instead of having 
  "https://api.stripe.com" duplicated in multiple places.

## 0.1.2 2026-10-13

- The drogon backend's webhook HTTP request handler was doing a synchronous 
  wait (via .result()). Fixed, so that it's fully asynchronous, using 
  co-routines (as it should).

## 0.1.1 2026-10-08

- The Stripe CLI tool now requires setting which events you want to receive.
  Updated the Stripe Checkout Demo and related docs
- Fixes to StripeCheckoutDemo:
    - Fixed a mutex locking issue that could cause the demo to hang after 
      "Save setup" is clicked
    - Waiting for the listener's signing secret no longer blocks the server's 
      event loop
    - Successful setup and listener actions now redirect to the home page

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
