/** TestDrogonHTTPClient.cpp
 *
 * Integration tests for the Drogon HTTP client adapter and Stripe API client operations.
 */

#include "IntegrationTestServer.h"
#include "TestFramework.h"

#include "StripeKit/Adapters/Drogon.h"
#include "StripeKit/Errors.h"
#include "StripeKit/Models.h"
#include "StripeKit/StripeKit.h"

#include <chrono>
#include <future>
#include <memory>
#include <string>

using namespace stripekit;
using namespace stripekit::testing;

class DrogonHTTPClientIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        DrogonIntegrationEnvironment::get().start();
        base_url = DrogonIntegrationEnvironment::get().base_url;
        api_key = DrogonIntegrationEnvironment::get().api_key;

        client =
            stripekit::drogon::make_stripe_client({ .api_key = api_key, .api_base_url = base_url });
    }

    std::string base_url;
    std::string api_key;
    std::unique_ptr<StripeClient> client;
};

TEST_F(DrogonHTTPClientIntegrationTest, PriceLookupPreservesEncodedQueryValues) {
    ListPricesRequest request;
    request.active = false;
    request.lookup_keys = { "standard & monthly" };
    request.starting_after = "price+old";
    auto task = client->prices().list_async(request);
    const auto page = task.result();
    EXPECT_TRUE(page.data.empty());
    EXPECT_FALSE(page.has_more);
}

TEST_F(DrogonHTTPClientIntegrationTest, CustomerCreateAsyncSuccess) {
    const CreateCustomerRequest request{ .email = "integration@example.com",
                                         .name = "Integration User",
                                         .metadata = { { "source", "integration_test" } } };

    const RequestOptions options{ .idempotency_key = "idem_int_123",
                                  .headers = { { "X-Custom-Client-Header", "integration_val" } } };

    auto task = client->customers().create_async(request, options);
    const Customer customer = task.result();

    EXPECT_EQ(customer.id, "cus_drogon_123");
    EXPECT_EQ(customer.email, "integration@example.com");
    EXPECT_EQ(customer.name, "Integration User");
    EXPECT_FALSE(customer.livemode);
    EXPECT_EQ(customer.metadata.at("source"), "integration_test");
}

TEST_F(DrogonHTTPClientIntegrationTest, CustomerCreateSendsStripeVersionHeaderWhenRequested) {
    const CreateCustomerRequest request{ .email = "integration@example.com",
                                         .name = "Integration User" };

    const RequestOptions options{ .headers = { { "Stripe-Version", "2024-06-20" } } };

    auto task = client->customers().create_async(request, options);
    const Customer customer = task.result();

    EXPECT_EQ(customer.id, "cus_drogon_123");
    EXPECT_EQ(
        DrogonIntegrationEnvironment::get().telemetry.latest_api_request_header("stripe-version"),
        "2024-06-20");
}

TEST_F(DrogonHTTPClientIntegrationTest, CustomerCreateCallbackSuccess) {
    const CreateCustomerRequest request{ .email = "integration@example.com",
                                         .name = "Integration User" };

    std::promise<CallbackResult<Customer>> promise;
    auto future = promise.get_future();

    client->customers().create(request, {}, [&promise](CallbackResult<Customer> result) {
        promise.set_value(std::move(result));
    });

    ASSERT_TRUE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(std::holds_alternative<Customer>(result));
    const auto& customer = std::get<Customer>(result);
    EXPECT_EQ(customer.id, "cus_drogon_123");
}

TEST_F(DrogonHTTPClientIntegrationTest, CustomerRetrieveAsyncSuccess) {
    auto task = client->customers().retrieve_async("cus_drogon_123");
    const Customer customer = task.result();

    EXPECT_EQ(customer.id, "cus_drogon_123");
    EXPECT_EQ(customer.email, "retrieved@example.com");
    EXPECT_EQ(customer.name, "Retrieved Customer");
}

TEST_F(DrogonHTTPClientIntegrationTest, CustomerRetrieveCallbackSuccess) {
    std::promise<CallbackResult<Customer>> promise;
    auto future = promise.get_future();

    client->customers().retrieve("cus_drogon_123", {}, [&promise](CallbackResult<Customer> result) {
        promise.set_value(std::move(result));
    });

    ASSERT_TRUE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(std::holds_alternative<Customer>(result));
    EXPECT_EQ(std::get<Customer>(result).id, "cus_drogon_123");
}

TEST_F(DrogonHTTPClientIntegrationTest, CheckoutSessionCreateAsyncSuccess) {
    const CreateCheckoutSessionRequest request{ .customer_id = "cus_drogon_123",
                                                .price_id = "price_starter_monthly",
                                                .success_url =
                                                    "https://example.com/checkout/success",
                                                .cancel_url = "https://example.com/checkout/cancel",
                                                .quantity = 1 };

    auto task = client->checkout().sessions().create_async(request);
    const CheckoutSession session = task.result();

    EXPECT_EQ(session.id, "cs_drogon_session_1");
    EXPECT_EQ(session.customer_id, "cus_drogon_123");
    EXPECT_EQ(session.status, "open");
    EXPECT_EQ(session.mode, "subscription");
    EXPECT_EQ(session.url, "https://checkout.stripe.com/c/pay/cs_drogon_session_1");
    EXPECT_EQ(session.amount_total, 2000);
}

TEST_F(DrogonHTTPClientIntegrationTest, CheckoutSessionCreateOneTimePurchaseAsyncSuccess) {
    const CreateCheckoutSessionRequest request{ .customer_id = "cus_drogon_123",
                                                .mode = "payment",
                                                .product_name = "Lifetime License",
                                                .currency = "usd",
                                                .unit_amount = 9900,
                                                .success_url =
                                                    "https://example.com/checkout/success",
                                                .cancel_url = "https://example.com/cancel",
                                                .quantity = 1 };

    auto task = client->checkout().sessions().create_async(request);
    const CheckoutSession session = task.result();

    EXPECT_EQ(session.id, "cs_drogon_payment_1");
    EXPECT_EQ(session.customer_id, "cus_drogon_123");
    EXPECT_EQ(session.status, "open");
    EXPECT_EQ(session.mode, "payment");
    EXPECT_EQ(session.payment_intent_id, "pi_drogon_123");
    EXPECT_EQ(session.url, "https://checkout.stripe.com/c/pay/cs_drogon_payment_1");
    EXPECT_EQ(session.amount_total, 2000);
}

TEST_F(DrogonHTTPClientIntegrationTest, CheckoutSessionCreateAsyncUsesDrogonFrameworkLoop) {
    auto app_loop_client = stripekit::drogon::make_stripe_client(
        { .api_key = api_key,
          .api_base_url = base_url,
          .event_loop = stripekit::drogon::EventLoopOptions::drogon_framework() });
    const CreateCheckoutSessionRequest request{ .customer_id = "cus_drogon_123",
                                                .price_id = "price_starter_monthly",
                                                .success_url =
                                                    "https://example.com/checkout/success",
                                                .cancel_url = "https://example.com/checkout/cancel",
                                                .quantity = 1 };

    auto task = app_loop_client->checkout().sessions().create_async(request);
    const CheckoutSession session = task.result();

    EXPECT_EQ(session.id, "cs_drogon_session_1");
    EXPECT_EQ(session.url, "https://checkout.stripe.com/c/pay/cs_drogon_session_1");
}

TEST_F(DrogonHTTPClientIntegrationTest, SubscriptionRetrieveAndCancelAsyncSuccess) {
    // Retrieve
    auto retrieve_task = client->subscriptions().retrieve_async("sub_drogon_active");
    const Subscription active_sub = retrieve_task.result();

    EXPECT_EQ(active_sub.id, "sub_drogon_active");
    EXPECT_EQ(active_sub.customer_id, "cus_drogon_123");
    EXPECT_EQ(active_sub.status, "active");
    EXPECT_EQ(active_sub.current_period_start_unix, 1700000000);
    EXPECT_EQ(active_sub.current_period_end_unix, 1702592000);
    EXPECT_FALSE(active_sub.cancel_at_period_end);

    // Cancel
    auto cancel_task = client->subscriptions().cancel_async("sub_drogon_active");
    const Subscription canceled_sub = cancel_task.result();

    EXPECT_EQ(canceled_sub.id, "sub_drogon_active");
    EXPECT_EQ(canceled_sub.status, "canceled");
}

TEST_F(DrogonHTTPClientIntegrationTest, BillingPortalSessionCreateAsyncSuccess) {
    const CreateBillingPortalSessionRequest request{ .customer_id = "cus_drogon_123",
                                                     .return_url = "https://example.com/account" };

    auto task = client->billing_portal().sessions().create_async(request);
    const BillingPortalSession session = task.result();

    EXPECT_EQ(session.id, "bps_drogon_session_1");
    EXPECT_EQ(session.customer_id, "cus_drogon_123");
    EXPECT_EQ(session.url, "https://billing.stripe.com/session/bps_drogon_session_1");
    EXPECT_EQ(session.return_url, "https://example.com/account");
    EXPECT_EQ(session.created_unix, 1700000000);
}

TEST_F(DrogonHTTPClientIntegrationTest, ServerFault400BadRequestThrowsStripeApiException) {
    stripekit::drogon::HTTPClient http_adapter(api_key, base_url);
    const HttpRequest req{ .method = HttpMethod::Get, .path = "/fault/400-bad-request" };

    auto task = http_adapter.send_async(req);
    const HttpResponse resp = task.result();
    EXPECT_EQ(resp.status_code, 400);

    JsonStripeResponseParser parser;
    const StripeErrorPayload error = parser.parse_error(resp);
    EXPECT_EQ(error.type, StripeErrorType::InvalidRequest);
    EXPECT_EQ(error.http_status, 400);
    EXPECT_EQ(error.request_id, "req_fault_400");
    EXPECT_FALSE(error.retryable);
}

TEST_F(DrogonHTTPClientIntegrationTest, ServerFault401UnauthorizedThrowsStripeApiException) {
    // Client configured with wrong API key hitting authenticated endpoint
    auto bad_client = stripekit::drogon::make_stripe_client(
        { .api_key = "sk_test_wrong_key", .api_base_url = base_url });

    try {
        auto task = bad_client->customers().retrieve_async("cus_123");
        task.result();
        FAIL() << "Expected StripeApiException on 401";
    } catch (const StripeApiException& error) {
        EXPECT_EQ(error.payload().type, StripeErrorType::Authentication);
        EXPECT_EQ(error.payload().http_status, 401);
        EXPECT_FALSE(error.retryable());
    }
}

TEST_F(DrogonHTTPClientIntegrationTest, ServerFault402CardDeclinedThrowsStripeApiException) {
    stripekit::drogon::HTTPClient http_adapter(api_key, base_url);
    const HttpRequest req{ .method = HttpMethod::Get, .path = "/fault/402-card-declined" };

    auto task = http_adapter.send_async(req);
    const HttpResponse resp = task.result();
    EXPECT_EQ(resp.status_code, 402);

    JsonStripeResponseParser parser;
    const StripeErrorPayload error = parser.parse_error(resp);
    EXPECT_EQ(error.type, StripeErrorType::Card);
    EXPECT_EQ(error.code, "card_declined");
    EXPECT_EQ(error.http_status, 402);
    EXPECT_FALSE(error.retryable);
}

TEST_F(DrogonHTTPClientIntegrationTest, ServerFault429RateLimitThrowsRetryableStripeApiException) {
    stripekit::drogon::HTTPClient http_adapter(api_key, base_url);
    const HttpRequest req{ .method = HttpMethod::Get, .path = "/fault/429-rate-limit" };

    auto task = http_adapter.send_async(req);
    const HttpResponse resp = task.result();
    EXPECT_EQ(resp.status_code, 429);

    JsonStripeResponseParser parser;
    const StripeErrorPayload error = parser.parse_error(resp);
    EXPECT_EQ(error.type, StripeErrorType::RateLimit);
    EXPECT_EQ(error.http_status, 429);
    EXPECT_TRUE(error.retryable);
}

TEST_F(DrogonHTTPClientIntegrationTest,
       ServerFault500InternalErrorThrowsRetryableStripeApiException) {
    stripekit::drogon::HTTPClient http_adapter(api_key, base_url);
    const HttpRequest req{ .method = HttpMethod::Get, .path = "/fault/500-server-error" };

    auto task = http_adapter.send_async(req);
    const HttpResponse resp = task.result();
    EXPECT_EQ(resp.status_code, 500);

    JsonStripeResponseParser parser;
    const StripeErrorPayload error = parser.parse_error(resp);
    EXPECT_EQ(error.type, StripeErrorType::Api);
    EXPECT_EQ(error.http_status, 500);
    EXPECT_TRUE(error.retryable);
}

TEST_F(DrogonHTTPClientIntegrationTest,
       ServerFault503UnavailableThrowsRetryableStripeApiException) {
    stripekit::drogon::HTTPClient http_adapter(api_key, base_url);
    const HttpRequest req{ .method = HttpMethod::Get, .path = "/fault/503-unavailable" };

    auto task = http_adapter.send_async(req);
    const HttpResponse resp = task.result();
    EXPECT_EQ(resp.status_code, 503);

    JsonStripeResponseParser parser;
    const StripeErrorPayload error = parser.parse_error(resp);
    EXPECT_EQ(error.type, StripeErrorType::ApiConnection);
    EXPECT_EQ(error.http_status, 503);
    EXPECT_TRUE(error.retryable);
}

TEST_F(DrogonHTTPClientIntegrationTest, ServerFaultMalformedNonJsonThrowsConfigurationException) {
    stripekit::drogon::HTTPClient http_adapter(api_key, base_url);
    const HttpRequest req{ .method = HttpMethod::Get, .path = "/fault/malformed-non-json" };

    auto task = http_adapter.send_async(req);
    const HttpResponse resp = task.result();
    EXPECT_EQ(resp.status_code, 200);

    JsonStripeResponseParser parser;
    EXPECT_THROW({ parser.parse_customer(resp); }, ConfigurationException);
}

TEST_F(DrogonHTTPClientIntegrationTest, RequestTimeoutOverrideThrowsStripeKitException) {
    stripekit::drogon::HTTPClient http_adapter(api_key, base_url);
    const HttpRequest req{ .method = HttpMethod::Get,
                           .path = "/fault/timeout-delay",
                           .options = RequestOptions{
                               .timeout = std::chrono::milliseconds(
                                   50) // 50ms timeout against 400ms endpoint delay
                           } };

    EXPECT_THROW(
        {
            auto task = http_adapter.send_async(req);
            task.result();
        },
        StripeKitException);
}

TEST_F(DrogonHTTPClientIntegrationTest, UnreachableHostThrowsStripeKitException) {
    // Port 1 is reserved and closed
    stripekit::drogon::HTTPClient unreachable_client(api_key, "http://127.0.0.1:1");
    const HttpRequest req{ .method = HttpMethod::Get, .path = "/v1/customers/cus_123" };

    EXPECT_THROW(
        {
            auto task = unreachable_client.send_async(req);
            task.result();
        },
        StripeKitException);
}

TEST_F(DrogonHTTPClientIntegrationTest, EmptyApiKeyThrowsConfigurationException) {
    EXPECT_THROW(
        { stripekit::drogon::HTTPClient bad_client("", base_url); }, ConfigurationException);
}

TEST_F(DrogonHTTPClientIntegrationTest, RemoteCleartextBaseUrlIsRejected) {
    EXPECT_THROW(
        { stripekit::drogon::HTTPClient insecure_client(api_key, "http://example.com"); },
        ConfigurationException);
}
