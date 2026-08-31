/** StripeApiClient.h
 *
 * Implementation of the StripeKit client API.
 *
 * @author Hans de Ruiter
 *
 * @License See LICENSE for details.
 */

#pragma once

#include "StripeKit/Client.h"
#include "StripeKit/Errors.h"
#include "StripeKit/HTTPClient.h"

namespace stripekit {

/**
 * Converts Stripe HTTP response payloads into StripeKit models.
 */
class StripeResponseParser {
public:
    /** Destroys the parser instance. */
    virtual ~StripeResponseParser() = default;

    /** Parses a successful customer response. */
    virtual Customer parse_customer(const HttpResponse& response) const = 0;

    /** Parses a successful Checkout Session response. */
    virtual CheckoutSession parse_checkout_session(const HttpResponse& response) const = 0;

    /** Parses a successful subscription response. */
    virtual Subscription parse_subscription(const HttpResponse& response) const = 0;

    /** Parses a successful Billing Portal session response. */
    virtual BillingPortalSession parse_billing_portal_session(const HttpResponse& response) const = 0;

    /** Parses an unsuccessful Stripe API response. */
    virtual StripeErrorPayload parse_error(const HttpResponse& response) const = 0;
};

/**
 * Default Stripe response parser using JsonCpp.
 */
class JsonStripeResponseParser final : public StripeResponseParser {
public:
    /** Parses a successful customer response. */
    Customer parse_customer(const HttpResponse& response) const override;

    /** Parses a successful Checkout Session response. */
    CheckoutSession parse_checkout_session(const HttpResponse& response) const override;

    /** Parses a successful subscription response. */
    Subscription parse_subscription(const HttpResponse& response) const override;

    /** Parses a successful Billing Portal session response. */
    BillingPortalSession parse_billing_portal_session(const HttpResponse& response) const override;

    /** Parses an unsuccessful Stripe API response. */
    StripeErrorPayload parse_error(const HttpResponse& response) const override;
};

/**
 * Concrete Stripe client that uses an application-provided HTTP client.
 *
 * By default, StripeKit decodes Stripe JSON responses with JsonCpp. Pass a
 * custom parser when the host application needs different response decoding.
 */
class StripeApiClient final : public StripeClient {
public:
     /** Creates a client using the supplied HTTP client and the default JsonCpp parser. */
     explicit StripeApiClient(HTTPClient& http_client);

    /** Creates a client using the supplied HTTP client and custom response parser. */
    StripeApiClient(HTTPClient& http_client, const StripeResponseParser& response_parser);

    /** Destroys the client and resource groups. */
    ~StripeApiClient() override;

    StripeApiClient(const StripeApiClient&) = delete;
    StripeApiClient& operator=(const StripeApiClient&) = delete;

    /** Returns customer operations. */
    Customers& customers() override;

    /** Returns Checkout Session operations. */
    Checkout& checkout() override;

    /** Returns subscription operations. */
    Subscriptions& subscriptions() override;

    /** Returns Billing Portal Session operations. */
    BillingPortal& billing_portal() override;

private:
    class CustomerOperations final : public Customers {
    public:
        explicit CustomerOperations(StripeApiClient& owner);
        Task<Customer> create_async(const CreateCustomerRequest& request,
                                    const RequestOptions& options = {}) override;
        void create(const CreateCustomerRequest& request,
                    const RequestOptions& options,
                    CreateCompletion completion) override;
        Task<Customer> retrieve_async(std::string customer_id,
                                      const RequestOptions& options = {}) override;
        void retrieve(std::string customer_id,
                      const RequestOptions& options,
                      RetrieveCompletion completion) override;

    private:
        StripeApiClient& owner;
    };

    class CheckoutSessionOperations final : public CheckoutSessions {
    public:
        explicit CheckoutSessionOperations(StripeApiClient& owner);
        Task<CheckoutSession> create_async(const CreateCheckoutSessionRequest& request,
                                           const RequestOptions& options = {}) override;
        void create(const CreateCheckoutSessionRequest& request,
                    const RequestOptions& options,
                    CreateCompletion completion) override;

    private:
        StripeApiClient& owner;
    };

    class CheckoutOperations final : public Checkout {
    public:
        explicit CheckoutOperations(StripeApiClient& owner);
        CheckoutSessions& sessions() override;

    private:
        StripeApiClient& owner;
    };

    class SubscriptionOperations final : public Subscriptions {
    public:
        explicit SubscriptionOperations(StripeApiClient& owner);
        Task<Subscription> retrieve_async(std::string subscription_id,
                                          const RequestOptions& options = {}) override;
        void retrieve(std::string subscription_id,
                      const RequestOptions& options,
                      RetrieveCompletion completion) override;
        Task<Subscription> cancel_async(std::string subscription_id,
                                        const RequestOptions& options = {}) override;
        void cancel(std::string subscription_id,
                    const RequestOptions& options,
                    CancelCompletion completion) override;

    private:
        StripeApiClient& owner;
    };

    class BillingPortalSessionOperations final : public BillingPortalSessions {
    public:
        explicit BillingPortalSessionOperations(StripeApiClient& owner);
        Task<BillingPortalSession> create_async(
            const CreateBillingPortalSessionRequest& request,
            const RequestOptions& options = {}) override;
        void create(const CreateBillingPortalSessionRequest& request,
                    const RequestOptions& options,
                    CreateCompletion completion) override;

    private:
        StripeApiClient& owner;
    };

    class BillingPortalOperations final : public BillingPortal {
    public:
        explicit BillingPortalOperations(StripeApiClient& owner);
        BillingPortalSessions& sessions() override;

    private:
        StripeApiClient& owner;
    };

    HTTPClient& http_client;
    JsonStripeResponseParser default_response_parser;
    const StripeResponseParser& response_parser;
    CustomerOperations customer_operations;
    CheckoutSessionOperations checkout_session_operations;
    CheckoutOperations checkout_operations;
    SubscriptionOperations subscription_operations;
    BillingPortalSessionOperations billing_portal_session_operations;
    BillingPortalOperations billing_portal_operations;
};

} // namespace stripekit