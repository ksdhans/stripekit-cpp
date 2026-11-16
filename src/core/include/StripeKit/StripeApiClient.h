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

#include <memory>

namespace stripekit {

struct CallbackLifetime;

/**
 * @addtogroup adapter_interfaces
 * @{
 */

/**
 * Turns Stripe's responses into StripeKit models.
 *
 * StripeKit decodes Stripe's JSON with JsonStripeResponseParser by default.
 * Implement this only when the application needs different decoding, such as
 * to reuse a JSON library it already links.
 */
class StripeResponseParser {
public:
    /** Destroys the parser instance. */
    virtual ~StripeResponseParser() = default;

    /**
     * Decode a schedule using Stripe's JSON format by default.
     *
     * @param response Successful response to decode.
     *
     * @return Schedule lifecycle, references, phases, and metadata.
     *
     * @throws ConfigurationException If dates, references, or required fields are malformed.
     */
    virtual SubscriptionSchedule parse_subscription_schedule(const HttpResponse& response) const;

    /**
     * Decode a refund using Stripe's JSON format by default.
     *
     * @param response Successful response to decode.
     *
     * @return Refund identity, amount, payment references, status, and metadata.
     *
     * @throws ConfigurationException If required fields or references are malformed.
     */
    virtual Refund parse_refund(const HttpResponse& response) const;
    /**
     * Decode one page using this parser's refund decoder.
     *
     * @param response Successful list response.
     *
     * @return Refunds and whether more pages exist.
     *
     * @throws ConfigurationException If the page or an entry is malformed.
     */
    virtual RefundList parse_refund_list(const HttpResponse& response) const;

    /**
     * Decode a charge using Stripe's JSON format by default.
     *
     * @param response Successful response to decode.
     *
     * @return Charge with nullable references preserved as empty IDs.
     *
     * @throws ConfigurationException If required fields or references are malformed.
     */
    virtual Charge parse_charge(const HttpResponse& response) const;
    /**
     * Decode one page using this parser's charge decoder.
     *
     * @param response Successful list response.
     *
     * @return Charges and whether more pages exist.
     *
     * @throws ConfigurationException If the page or an entry is malformed.
     */
    virtual ChargeList parse_charge_list(const HttpResponse& response) const;
    /**
     * Decode an invoice allocation using Stripe's JSON format by default.
     *
     * @param response Successful response to decode.
     *
     * @return Invoice and payment references, amounts and status.
     *
     * @throws ConfigurationException If required fields or references are malformed.
     */
    virtual InvoicePayment parse_invoice_payment(const HttpResponse& response) const;
    /**
     * Decode one page using this parser's invoice payment decoder.
     *
     * @param response Successful list response.
     *
     * @return Allocations and whether more pages exist.
     *
     * @throws ConfigurationException If the page or an entry is malformed.
     */
    virtual InvoicePaymentList parse_invoice_payment_list(const HttpResponse& response) const;

    /**
     * Reads a product out of a successful response.
     *
     * Deleted products contain only their ID and deleted flag.
     *
     * @param response The response body and status from Stripe.
     *
     * @return The parsed product.
     *
     * @throws ConfigurationException The body was not a valid product object.
     */
    virtual Product parse_product(const HttpResponse& response) const;

    /**
     * Reads a price out of a successful response.
     *
     * Preserves absent amounts and recurrence details so callers can distinguish
     * fixed prices from other pricing types. Active and archived prices are accepted.
     *
     * @param response The response body and status from Stripe.
     *
     * @return The parsed price.
     *
     * @throws ConfigurationException The body was not a valid price object.
     */
    virtual Price parse_price(const HttpResponse& response) const;

    /**
     * Reads one page of prices out of a successful response.
     *
     * Does not fetch further pages.
     *
     * @param response The response body and status from Stripe.
     *
     * @return The parsed prices and whether further pages are available.
     *
     * @throws ConfigurationException The body was not a valid price list or
     * contained an invalid price object.
     */
    virtual PriceList parse_price_list(const HttpResponse& response) const;

    /**
     * Reads a customer out of a successful response.
     *
     * @param response The response body and status from Stripe.
     * @return The parsed customer.
     * @throws ConfigurationException The body was not a valid customer object.
     */
    virtual Customer parse_customer(const HttpResponse& response) const = 0;

    /**
     * Reads a Checkout session out of a successful response.
     *
     * @param response The response body and status from Stripe.
     * @return The parsed session.
     * @throws ConfigurationException The body was not a valid session object.
     */
    virtual CheckoutSession parse_checkout_session(const HttpResponse& response) const = 0;
    /**
     * Decode a page using this parser's Checkout session decoder.
     *
     * @param response Successful Stripe list response.
     *
     * @return One validated page.
     *
     * @throws ConfigurationException If the list or a session identity is malformed.
     */
    virtual CheckoutSessionList parse_checkout_session_list(const HttpResponse& response) const;

    /**
     * Decode one page of Checkout prices and quantities using Stripe's JSON format.
     *
     * @param response Successful line-item list response.
     *
     * @return Purchased items and whether another page exists.
     *
     * @throws ConfigurationException If a line item is malformed.
     */
    virtual PurchasedLineItemList parse_line_item_list(const HttpResponse& response) const;

    /**
     * Reads a subscription out of a successful response.
     *
     * @param response The response body and status from Stripe.
     * @return The parsed subscription.
     * @throws ConfigurationException The body was not a valid subscription object.
     */
    virtual Subscription parse_subscription(const HttpResponse& response) const = 0;

    /**
     * Reads a billing portal session out of a successful response.
     *
     * @param response The response body and status from Stripe.
     * @return The parsed session.
     * @throws ConfigurationException The body was not a valid session object.
     */
    virtual BillingPortalSession
    parse_billing_portal_session(const HttpResponse& response) const = 0;

    /**
     * Reads an invoice out of a successful response.
     *
     * The default decodes Stripe's JSON with JsonStripeResponseParser, so
     * parsers written before invoices were supported keep working.
     *
     * @param response The response body and status from Stripe.
     *
     * @return The parsed invoice.
     *
     * @throws ConfigurationException The body was not a valid invoice object.
     */
    virtual Invoice parse_invoice(const HttpResponse& response) const;
    /**
     * Decode one page using this parser's invoice decoder.
     *
     * @param response Successful invoice list response.
     *
     * @return One validated page.
     *
     * @throws ConfigurationException If a page or invoice identity is malformed.
     */
    virtual InvoiceList parse_invoice_list(const HttpResponse& response) const;

    /**
     * Decode Stripe's confirmation that a draft invoice was deleted.
     *
     * @param response Successful deletion response.
     *
     * @return Deleted invoice ID and confirmation.
     *
     * @throws ConfigurationException If confirmation is absent or malformed.
     */
    virtual DeletedInvoice parse_deleted_invoice(const HttpResponse& response) const;

    /**
     * Reads a billing portal configuration out of a successful response.
     *
     * The default decodes Stripe's JSON with JsonStripeResponseParser, so
     * parsers written before configurations were supported keep working.
     *
     * @param response The response body and status from Stripe.
     *
     * @return The parsed configuration.
     *
     * @throws ConfigurationException The body was not a valid configuration object.
     */
    virtual BillingPortalConfiguration
    parse_billing_portal_configuration(const HttpResponse& response) const;

    /**
     * Reads the error details out of a failed response.
     *
     * Must not throw: it is already handling a failure. Fall back to a
     * StripeErrorType::Unknown payload when the body cannot be parsed.
     *
     * @param response The response body and status from Stripe.
     * @return The error details, used to build a StripeApiException.
     */
    virtual StripeErrorPayload parse_error(const HttpResponse& response) const = 0;
};

/**
 * The response parser StripeKit uses by default.
 *
 * Decodes Stripe's JSON with JsonCpp. Applications get this automatically and
 * only need to think about it when supplying a parser of their own.
 */
class JsonStripeResponseParser final : public StripeResponseParser {
public:
    /**
     * Reads a product out of a successful JSON response.
     *
     * Deleted products contain only their ID and deleted flag.
     *
     * @param response The response body and status from Stripe.
     *
     * @return The parsed product.
     *
     * @throws ConfigurationException The body was not valid JSON or contained
     * a malformed product object.
     */
    Product parse_product(const HttpResponse& response) const override;

    /**
     * Reads a price out of a successful JSON response.
     *
     * Preserves absent amounts and recurrence details. Active and archived prices
     * are accepted, and expanded products are returned as product IDs.
     *
     * @param response The response body and status from Stripe.
     *
     * @return The parsed price.
     *
     * @throws ConfigurationException The body was not valid JSON or contained
     * a malformed price object.
     */
    Price parse_price(const HttpResponse& response) const override;

    /**
     * Reads one page of prices out of a successful JSON response.
     *
     * Does not fetch further pages.
     *
     * @param response The response body and status from Stripe.
     *
     * @return The parsed prices and whether further pages are available.
     *
     * @throws ConfigurationException The body was not valid JSON, was not a
     * price list, or contained a malformed price object.
     */
    PriceList parse_price_list(const HttpResponse& response) const override;

    /**
     * Reads a customer out of a successful response.
     *
     * @param response The response body and status from Stripe.
     * @return The parsed customer.
     * @throws ConfigurationException The body was not valid JSON, or not a customer.
     */
    Customer parse_customer(const HttpResponse& response) const override;

    /**
     * Reads a Checkout session out of a successful response.
     *
     * @param response The response body and status from Stripe.
     * @return The parsed session.
     * @throws ConfigurationException The body was not valid JSON, or not a session.
     */
    CheckoutSession parse_checkout_session(const HttpResponse& response) const override;

    /**
     * Reads a subscription out of a successful response.
     *
     * @param response The response body and status from Stripe.
     * @return The parsed subscription.
     * @throws ConfigurationException The body was not valid JSON, or not a subscription.
     */
    Subscription parse_subscription(const HttpResponse& response) const override;

    /**
     * Reads a billing portal session out of a successful response.
     *
     * @param response The response body and status from Stripe.
     * @return The parsed session.
     * @throws ConfigurationException The body was not valid JSON, or not a session.
     */
    BillingPortalSession parse_billing_portal_session(const HttpResponse& response) const override;

    /**
     * Reads an invoice out of a successful JSON response.
     *
     * Reads subscription references from the invoice parent and derives the
     * paid state from the invoice status.
     *
     * @param response The response body and status from Stripe.
     *
     * @return The parsed invoice.
     *
     * @throws ConfigurationException The body was not valid JSON, or not an invoice.
     */
    Invoice parse_invoice(const HttpResponse& response) const override;

    /**
     * Reads a billing portal configuration out of a successful JSON response.
     *
     * @param response The response body and status from Stripe.
     *
     * @return The parsed configuration.
     *
     * @throws ConfigurationException The body was not valid JSON, or not a configuration.
     */
    BillingPortalConfiguration
    parse_billing_portal_configuration(const HttpResponse& response) const override;

    /**
     * Reads the error details out of a failed response.
     *
     * @param response The response body and status from Stripe.
     * @return The error details, falling back to StripeErrorType::Unknown when
     *         the body cannot be parsed.
     */
    StripeErrorPayload parse_error(const HttpResponse& response) const override;
};

/**
 * The StripeClient implementation, built on any HTTPClient.
 *
 * This is what turns an adapter's transport into a working Stripe client: it
 * builds the requests, sends them through the supplied HTTPClient, parses the
 * responses, and raises StripeApiException on failure. An adapter's factory
 * function constructs one of these, so applications rarely need to.
 *
 * Construct it directly when you need to supply a custom response parser:
 *
 * @code
 * stripekit::drogon::HTTPClient http_client("sk_test_...");
 * stripekit::StripeApiClient stripe(http_client, custom_parser);
 * @endcode
 *
 * The HTTP client and parser are referenced, not owned, so both must outlive
 * the client. Safe to use from several threads.
 */
class StripeApiClient final : public StripeClient {
public:
    /** Return schedule operations owned by this client. */
    SubscriptionSchedules& subscription_schedules() override;
    /** Return refund operations owned by this client. */
    Refunds& refunds() override;

    /** Return charge operations owned by this client. */
    Charges& charges() override;

    /** Return invoice allocation operations owned by this client. */
    InvoicePayments& invoice_payments() override;
    /**
     * Creates a client that decodes responses with JsonCpp.
     *
     * @param http_client Transport to send requests through. Must outlive the client.
     */
    explicit StripeApiClient(HTTPClient& http_client);

    /**
     * Creates a client that decodes responses with a parser of your own.
     *
     * @param http_client Transport to send requests through. Must outlive the client.
     * @param response_parser Decoder for Stripe's responses. Must outlive the client.
     */
    StripeApiClient(HTTPClient& http_client, const StripeResponseParser& response_parser);

    /** Destroys the client and its operation groups. */
    ~StripeApiClient() override;

    StripeApiClient(const StripeApiClient&) = delete;
    StripeApiClient& operator=(const StripeApiClient&) = delete;

    /**
     * Returns the customer operations.
     *
     * @return A reference valid for the lifetime of this client.
     */
    Customers& customers() override;

    /**
     * Returns the Checkout operations.
     *
     * @return A reference valid for the lifetime of this client.
     */
    Checkout& checkout() override;

    /**
     * Returns the subscription operations.
     *
     * @return A reference valid for the lifetime of this client.
     */
    Subscriptions& subscriptions() override;

    /**
     * Returns the billing portal operations.
     *
     * @return A reference valid for the lifetime of this client.
     */
    BillingPortal& billing_portal() override;

    /**
     * Returns the invoice operations.
     *
     * @return A reference valid for the lifetime of this client.
     */
    Invoices& invoices() override;

    /**
     * Returns the product operations.
     *
     * @return A reference valid for the lifetime of this client.
     */
    Products& products() override;

    /**
     * Returns the price operations.
     *
     * @return A reference valid for the lifetime of this client.
     */
    Prices& prices() override;

private:
    /** Request refunds and read their history. */
    class RefundOperations final : public Refunds {
    public:
        /** Retain the client that owns these operations. */
        explicit RefundOperations(StripeApiClient& owner) : owner(owner) {}
        /** Request a refund using the saved retry settings. */
        Task<Refund> create_async(const CreateRefundRequest&, const RequestOptions&) override;
        /** Deliver a refund or failure exactly once. */
        void create(const CreateRefundRequest&, const RequestOptions&, Completion) override;
        /** Retrieve one page without automatic traversal. */
        Task<RefundList> list_async(const ListRefundsRequest&, const RequestOptions&) override;
        /** Deliver one page or failure exactly once. */
        void list(const ListRefundsRequest&, const RequestOptions&, ListCompletion) override;

    private:
        StripeApiClient& owner;
    };

    /** Retrieve payment attempts and list them by customer. */
    class ChargeOperations final : public Charges {
    public:
        /** Retain the client that owns these operations. */
        explicit ChargeOperations(StripeApiClient& owner) : owner(owner) {}
        /** Retrieve a charge with per-request settings. */
        Task<Charge> retrieve_async(std::string, const RequestOptions&) override;
        /** Deliver a charge or failure exactly once. */
        void retrieve(std::string, const RequestOptions&, Completion) override;
        /** Retrieve one page without automatic traversal. */
        Task<ChargeList> list_async(const ListChargesRequest&, const RequestOptions&) override;
        /** Deliver one page or failure exactly once. */
        void list(const ListChargesRequest&, const RequestOptions&, ListCompletion) override;

    private:
        StripeApiClient& owner;
    };

    /** Read the invoices allocated to a payment. */
    class InvoicePaymentOperations final : public InvoicePayments {
    public:
        /** Retain the client that owns these operations. */
        explicit InvoicePaymentOperations(StripeApiClient& owner) : owner(owner) {}
        /** Retrieve one allocation page without automatic traversal. */
        Task<InvoicePaymentList> list_async(const ListInvoicePaymentsRequest&,
                                            const RequestOptions&) override;
        /** Deliver one page or failure exactly once. */
        void
        list(const ListInvoicePaymentsRequest&, const RequestOptions&, ListCompletion) override;

    private:
        StripeApiClient& owner;
    };

    /** Creates, retrieves, and updates Stripe products. */
    class ProductOperations final : public Products {
    public:
        /** Retain the owning client for the lifetime of the group. */
        explicit ProductOperations(StripeApiClient& owner);

        /** Create a product with the supplied request settings. */
        Task<Product> create_async(const CreateProductRequest&, const RequestOptions&) override;

        /** Create a product and deliver its result once. */
        void create(const CreateProductRequest&, const RequestOptions&, Completion) override;

        /** Retrieve a product by ID. */
        Task<Product> retrieve_async(std::string, const RequestOptions&) override;

        /** Retrieve a product and deliver its result once. */
        void retrieve(std::string, const RequestOptions&, Completion) override;

        /** Update the specified product. */
        Task<Product>
        update_async(std::string, const UpdateProductRequest&, const RequestOptions&) override;

        /** Update a product and deliver its result once. */
        void update(std::string,
                    const UpdateProductRequest&,
                    const RequestOptions&,
                    Completion) override;

    private:
        StripeApiClient& owner;
    };

    /** Creates and retrieves Stripe prices, including filtered lists. */
    class PriceOperations final : public Prices {
    public:
        /** Retain the owning client for the lifetime of the group. */
        explicit PriceOperations(StripeApiClient& owner);

        /** Create a fixed per-unit price. */
        Task<Price> create_async(const CreatePriceRequest&, const RequestOptions&) override;

        /** Create a price and deliver its result once. */
        void create(const CreatePriceRequest&, const RequestOptions&, Completion) override;

        /** Update mutable price fields or archive a price. */
        Task<Price>
        update_async(std::string, const UpdatePriceRequest&, const RequestOptions&) override;

        /** Update a price and deliver the result once. */
        void
        update(std::string, const UpdatePriceRequest&, const RequestOptions&, Completion) override;

        /** Retrieve an existing price by ID. */
        Task<Price> retrieve_async(std::string, const RequestOptions&) override;

        /** Retrieve a price and deliver its result once. */
        void retrieve(std::string, const RequestOptions&, Completion) override;

        /** Retrieve one filtered page of prices. */
        Task<PriceList> list_async(const ListPricesRequest&, const RequestOptions&) override;

        /** Retrieve a page and deliver its result once. */
        void list(const ListPricesRequest&, const RequestOptions&, ListCompletion) override;

    private:
        StripeApiClient& owner;
    };

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
        Task<Customer> update_async(std::string customer_id,
                                    const UpdateCustomerRequest& request,
                                    const RequestOptions& options = {}) override;
        void update(std::string customer_id,
                    const UpdateCustomerRequest& request,
                    const RequestOptions& options,
                    RetrieveCompletion completion) override;

    private:
        StripeApiClient& owner;
    };

    class CheckoutSessionOperations final : public CheckoutSessions {
    public:
        explicit CheckoutSessionOperations(StripeApiClient& owner);

        /** Expire an open session, preserving per-request settings. */
        Task<CheckoutSession> expire_async(std::string, const RequestOptions&) override;

        /** Report expiry or failure exactly once. */
        void expire(std::string, const RequestOptions&, CreateCompletion) override;

        /** Retrieve one filtered page without automatically fetching more. */
        Task<CheckoutSessionList> list_async(const ListCheckoutSessionsRequest&,
                                             const RequestOptions&) override;

        /** Report a page or failure exactly once. */
        void
        list(const ListCheckoutSessionsRequest&, const RequestOptions&, ListCompletion) override;
        Task<PurchasedLineItemList> list_line_items_async(std::string,
                                                          const ListPurchasedLineItemsRequest&,
                                                          const RequestOptions&) override;
        void list_line_items(std::string,
                             const ListPurchasedLineItemsRequest&,
                             const RequestOptions&,
                             LineItemsCompletion) override;

        /** Retrieve the current state of a checkout session. */
        Task<CheckoutSession> retrieve_async(std::string session_id,
                                             const RequestOptions& options = {}) override;

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
        /** Pause service and invoice generation. */
        Task<Subscription>
        pause_async(std::string, const PauseSubscriptionRequest&, const RequestOptions&) override;
        /** Deliver a pause result or failure once. */
        void pause(std::string,
                   const PauseSubscriptionRequest&,
                   const RequestOptions&,
                   UpdateCompletion) override;
        /** Start a resume attempt. */
        Task<Subscription>
        resume_async(std::string, const ResumeSubscriptionRequest&, const RequestOptions&) override;
        /** Deliver a resume result or failure once. */
        void resume(std::string,
                    const ResumeSubscriptionRequest&,
                    const RequestOptions&,
                    UpdateCompletion) override;
        /** Explicitly upgrade billing mode. */
        Task<Subscription> migrate_async(std::string,
                                         const MigrateSubscriptionRequest&,
                                         const RequestOptions&) override;
        /** Deliver a migration result or failure once. */
        void migrate(std::string,
                     const MigrateSubscriptionRequest&,
                     const RequestOptions&,
                     UpdateCompletion) override;
        /** Set or clear cancellation at period end. */
        Task<Subscription>
        set_cancel_at_period_end_async(std::string, bool, const RequestOptions&) override;
        /** Deliver the cancellation change or failure once. */
        void set_cancel_at_period_end(std::string,
                                      bool,
                                      const RequestOptions&,
                                      UpdateCompletion) override;
        /** Pause collection according to the requested invoice behavior. */
        Task<Subscription> pause_collection_async(std::string,
                                                  const PauseSubscriptionCollectionRequest&,
                                                  const RequestOptions&) override;
        /** Deliver the collection pause or failure once. */
        void pause_collection(std::string,
                              const PauseSubscriptionCollectionRequest&,
                              const RequestOptions&,
                              UpdateCompletion) override;
        /** Clear a collection pause without collecting existing invoices. */
        Task<Subscription> resume_collection_async(std::string, const RequestOptions&) override;
        /** Deliver the resumed subscription or failure once. */
        void resume_collection(std::string, const RequestOptions&, UpdateCompletion) override;
        explicit SubscriptionOperations(StripeApiClient& owner);
        /** Update subscription items, billing settings, cancellation, or metadata. */
        Task<Subscription>
        update_async(std::string, const UpdateSubscriptionRequest&, const RequestOptions&) override;
        /** Deliver the updated subscription or its failure once. */
        void update(std::string,
                    const UpdateSubscriptionRequest&,
                    const RequestOptions&,
                    UpdateCompletion) override;
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
        Task<BillingPortalSession> create_async(const CreateBillingPortalSessionRequest& request,
                                                const RequestOptions& options = {}) override;
        void create(const CreateBillingPortalSessionRequest& request,
                    const RequestOptions& options,
                    CreateCompletion completion) override;

    private:
        StripeApiClient& owner;
    };

    /** Retrieves Stripe invoices. */
    class InvoiceOperations final : public Invoices {
    public:
        /** Read one page of subscription invoices. */
        Task<InvoiceList> list_async(const ListInvoicesRequest&, const RequestOptions&) override;
        /** Deliver a page or failure once. */
        void list(const ListInvoicesRequest&, const RequestOptions&, ListCompletion) override;
        /** Read one page of purchased invoice lines. */
        Task<PurchasedLineItemList> list_line_items_async(std::string,
                                                          const ListPurchasedLineItemsRequest&,
                                                          const RequestOptions&) override;
        /** Deliver a purchased invoice line page or failure once. */
        void list_line_items(std::string,
                             const ListPurchasedLineItemsRequest&,
                             const RequestOptions&,
                             LineItemsCompletion) override;
        /** Finalize a draft without automatic collection. */
        Task<Invoice> finalize_async(std::string, const RequestOptions&) override;
        /** Deliver the finalized invoice or failure once. */
        void finalize(std::string, const RequestOptions&, Completion) override;
        /** Update automatic tax or collection settings on a draft invoice. */
        Task<Invoice>
        update_async(std::string, const UpdateInvoiceRequest&, const RequestOptions&) override;
        /** Deliver the updated invoice or failure once. */
        void update(std::string,
                    const UpdateInvoiceRequest&,
                    const RequestOptions&,
                    Completion) override;
        /** Void a finalized unpaid invoice. */
        Task<Invoice> void_async(std::string, const RequestOptions&) override;
        /** Deliver the voided invoice or failure once. */
        void void_invoice(std::string, const RequestOptions&, Completion) override;
        /** Delete a draft invoice. */
        Task<DeletedInvoice> delete_async(std::string, const RequestOptions&) override;
        /** Deliver deletion confirmation or failure once. */
        void delete_invoice(std::string, const RequestOptions&, DeleteCompletion) override;
        /** Retain the owning client for the lifetime of the group. */
        explicit InvoiceOperations(StripeApiClient& owner);

        /** Retrieve an invoice by ID. */
        Task<Invoice> retrieve_async(std::string, const RequestOptions&) override;

        /** Retrieve an invoice and deliver its result once. */
        void retrieve(std::string, const RequestOptions&, Completion) override;

    private:
        StripeApiClient& owner;
    };

    /** Creates, retrieves, and updates billing portal configurations. */
    class BillingPortalConfigurationOperations final : public BillingPortalConfigurations {
    public:
        /** Retain the owning client for the lifetime of the group. */
        explicit BillingPortalConfigurationOperations(StripeApiClient& owner);

        /** Create a configuration from explicit feature settings. */
        Task<BillingPortalConfiguration>
        create_async(const CreateBillingPortalConfigurationRequest&,
                     const RequestOptions&) override;

        /** Create a configuration and deliver its result once. */
        void create(const CreateBillingPortalConfigurationRequest&,
                    const RequestOptions&,
                    Completion) override;

        /** Retrieve a configuration by ID. */
        Task<BillingPortalConfiguration> retrieve_async(std::string,
                                                        const RequestOptions&) override;

        /** Retrieve a configuration and deliver its result once. */
        void retrieve(std::string, const RequestOptions&, Completion) override;

        /** Update or deactivate a configuration. */
        Task<BillingPortalConfiguration>
        update_async(std::string,
                     const UpdateBillingPortalConfigurationRequest&,
                     const RequestOptions&) override;

        /** Update a configuration and deliver its result once. */
        void update(std::string,
                    const UpdateBillingPortalConfigurationRequest&,
                    const RequestOptions&,
                    Completion) override;

    private:
        StripeApiClient& owner;
    };

    class BillingPortalOperations final : public BillingPortal {
    public:
        explicit BillingPortalOperations(StripeApiClient& owner);
        BillingPortalSessions& sessions() override;
        BillingPortalConfigurations& configurations() override;

    private:
        StripeApiClient& owner;
    };

    /** Creates and changes subscription schedules. */
    class SubscriptionScheduleOperations final : public SubscriptionSchedules {
    public:
        /** Retain the client that owns these operations. */
        explicit SubscriptionScheduleOperations(StripeApiClient& owner) : owner(owner) {}
        /** Read a schedule by ID. */
        Task<SubscriptionSchedule> retrieve_async(std::string, const RequestOptions&) override;
        /** Deliver a schedule or failure once. */
        void retrieve(std::string, const RequestOptions&, Completion) override;
        /** Create a schedule from an existing subscription's billing settings. */
        Task<SubscriptionSchedule> create_async(const CreateSubscriptionScheduleRequest&,
                                                const RequestOptions&) override;
        /** Deliver the created schedule or failure once. */
        void create(const CreateSubscriptionScheduleRequest&,
                    const RequestOptions&,
                    Completion) override;
        /** Replace current and future phases. */
        Task<SubscriptionSchedule> update_async(std::string,
                                                const UpdateSubscriptionScheduleRequest&,
                                                const RequestOptions&) override;
        /** Deliver the updated schedule or failure once. */
        void update(std::string,
                    const UpdateSubscriptionScheduleRequest&,
                    const RequestOptions&,
                    Completion) override;

    private:
        StripeApiClient& owner;
    };

    HTTPClient& http_client;
    JsonStripeResponseParser default_response_parser;
    const StripeResponseParser& response_parser;
    std::shared_ptr<CallbackLifetime> callback_lifetime;
    CustomerOperations customer_operations;
    CheckoutSessionOperations checkout_session_operations;
    CheckoutOperations checkout_operations;
    SubscriptionOperations subscription_operations;
    BillingPortalSessionOperations billing_portal_session_operations;
    BillingPortalOperations billing_portal_operations;
    ProductOperations product_operations;
    PriceOperations price_operations;
    InvoiceOperations invoice_operations;
    BillingPortalConfigurationOperations billing_portal_configuration_operations;
    ChargeOperations charge_operations;
    InvoicePaymentOperations invoice_payment_operations;
    RefundOperations refund_operations;
    SubscriptionScheduleOperations subscription_schedule_operations;
};

/** @} */

} // namespace stripekit