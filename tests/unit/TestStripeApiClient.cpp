/** TestStripeApiClient.cpp
 * 
 * Unit tests for StripeApiClient operations and request formatting.
 */

#include "TestHelpers.h"

#include "StripeKit/Client.h"
#include "StripeKit/Errors.h"
#include "StripeKit/StripeApiClient.h"

#include <gtest/gtest.h>

#include <string>

using namespace stripekit;
using namespace stripekit::testing;

class StripeApiClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        client = std::make_unique<StripeApiClient>(http_client);
    }

    MockHttpClient http_client;
    std::unique_ptr<StripeApiClient> client;
};

TEST_F(StripeApiClientTest, CreateCustomerAsyncSuccess) {
    http_client.set_handler([](const HttpRequest& req) {
        EXPECT_EQ(req.method, HttpMethod::Post);
        EXPECT_EQ(req.path, "/v1/customers");
        EXPECT_EQ(req.headers.at("Accept"), "application/json");
        EXPECT_EQ(req.headers.at("Content-Type"), "application/x-www-form-urlencoded");
        EXPECT_EQ(req.headers.at("Idempotency-Key"), "idem_key_123");
        EXPECT_EQ(req.headers.at("X-Custom-Header"), "custom_val");

        // Verify body contains form-encoded parameters
        EXPECT_NE(req.body.find("email=test%40example.com"), std::string::npos);
        EXPECT_NE(req.body.find("name=Test%20User"), std::string::npos);
        EXPECT_NE(req.body.find("metadata%5Buser_id%5D=usr_999"), std::string::npos);

        return HttpResponse{
            .status_code = 200,
            .headers = {{"request-id", "req_cust_1"}},
            .body = R"({
                "id": "cus_new_1",
                "email": "test@example.com",
                "name": "Test User",
                "livemode": false,
                "metadata": {"user_id": "usr_999"}
            })"
        };
    });

    const CreateCustomerRequest request{
        .email = "test@example.com",
        .name = "Test User",
        .metadata = {{"user_id", "usr_999"}}
    };

    const RequestOptions options{
        .idempotency_key = "idem_key_123",
        .headers = {{"X-Custom-Header", "custom_val"}}
    };

    auto task = client->customers().create_async(request, options);
    const Customer customer = task.result();

    EXPECT_EQ(customer.id, "cus_new_1");
    EXPECT_EQ(customer.email, "test@example.com");
    EXPECT_EQ(customer.name, "Test User");
    EXPECT_EQ(customer.metadata.at("user_id"), "usr_999");
}

TEST_F(StripeApiClientTest, CreateCustomerCallbackSuccess) {
    http_client.set_response(HttpResponse{
        .status_code = 200,
        .headers = {},
        .body = R"({ "id": "cus_cb_1", "email": "cb@example.com" })"
    });

    bool callback_invoked = false;
    client->customers().create(
        CreateCustomerRequest{.email = "cb@example.com"},
        RequestOptions{},
        [&](CallbackResult<Customer> result) {
            callback_invoked = true;
            ASSERT_TRUE(std::holds_alternative<Customer>(result));
            EXPECT_EQ(std::get<Customer>(result).id, "cus_cb_1");
        });

    EXPECT_TRUE(callback_invoked);
}

TEST_F(StripeApiClientTest, RetrieveCustomerAsyncSuccess) {
    http_client.set_handler([](const HttpRequest& req) {
        EXPECT_EQ(req.method, HttpMethod::Get);
        EXPECT_EQ(req.path, "/v1/customers/cus_special%2Fid");
        return HttpResponse{
            .status_code = 200,
            .headers = {},
            .body = R"({ "id": "cus_special/id", "email": "special@example.com" })"
        };
    });

    auto task = client->customers().retrieve_async("cus_special/id");
    const Customer customer = task.result();
    EXPECT_EQ(customer.id, "cus_special/id");
}

TEST_F(StripeApiClientTest, CreateCheckoutSessionAsyncSuccess) {
    http_client.set_handler([](const HttpRequest& req) {
        EXPECT_EQ(req.method, HttpMethod::Post);
        EXPECT_EQ(req.path, "/v1/checkout/sessions");
        EXPECT_NE(req.body.find("mode=subscription"), std::string::npos);
        EXPECT_NE(req.body.find("customer=cus_123"), std::string::npos);
        EXPECT_NE(req.body.find("line_items%5B0%5D%5Bprice%5D=price_abc"), std::string::npos);
        EXPECT_NE(req.body.find("line_items%5B0%5D%5Bquantity%5D=2"), std::string::npos);
        EXPECT_NE(req.body.find("success_url=https%3A%2F%2Fexample.com%2Fsuccess"), std::string::npos);
        EXPECT_NE(req.body.find("cancel_url=https%3A%2F%2Fexample.com%2Fcancel"), std::string::npos);
        EXPECT_NE(req.body.find("metadata%5Bplan%5D=starter"), std::string::npos);

        return HttpResponse{
            .status_code = 200,
            .headers = {},
            .body = R"({
                "id": "cs_test_session_1",
                "customer": "cus_123",
                "status": "open",
                "mode": "subscription",
                "url": "https://checkout.stripe.com/c/pay/cs_test_session_1"
            })"
        };
    });

    const CreateCheckoutSessionRequest request{
        .customer_id = "cus_123",
        .price_id = "price_abc",
        .success_url = "https://example.com/success",
        .cancel_url = "https://example.com/cancel",
        .quantity = 2,
        .metadata = {{"plan", "starter"}}
    };

    auto task = client->checkout().sessions().create_async(request);
    const CheckoutSession session = task.result();
    EXPECT_EQ(session.id, "cs_test_session_1");
    EXPECT_EQ(session.customer_id, "cus_123");
    EXPECT_EQ(session.url, "https://checkout.stripe.com/c/pay/cs_test_session_1");
}

TEST_F(StripeApiClientTest, SubscriptionsRetrieveAndCancel) {
    // Retrieve
    http_client.set_handler([](const HttpRequest& req) {
        EXPECT_EQ(req.method, HttpMethod::Get);
        EXPECT_EQ(req.path, "/v1/subscriptions/sub_test_1");
        return HttpResponse{
            .status_code = 200,
            .headers = {},
            .body = R"({ "id": "sub_test_1", "customer": "cus_123", "status": "active" })"
        };
    });

    auto retrieve_task = client->subscriptions().retrieve_async("sub_test_1");
    const Subscription sub = retrieve_task.result();
    EXPECT_EQ(sub.id, "sub_test_1");
    EXPECT_EQ(sub.status, "active");

    // Cancel
    http_client.set_handler([](const HttpRequest& req) {
        EXPECT_EQ(req.method, HttpMethod::Delete);
        EXPECT_EQ(req.path, "/v1/subscriptions/sub_test_1");
        return HttpResponse{
            .status_code = 200,
            .headers = {},
            .body = R"({ "id": "sub_test_1", "customer": "cus_123", "status": "canceled" })"
        };
    });

    auto cancel_task = client->subscriptions().cancel_async("sub_test_1");
    const Subscription canceled_sub = cancel_task.result();
    EXPECT_EQ(canceled_sub.id, "sub_test_1");
    EXPECT_EQ(canceled_sub.status, "canceled");
}

TEST_F(StripeApiClientTest, CreateBillingPortalSessionAsyncSuccess) {
    http_client.set_handler([](const HttpRequest& req) {
        EXPECT_EQ(req.method, HttpMethod::Post);
        EXPECT_EQ(req.path, "/v1/billing_portal/sessions");
        EXPECT_NE(req.body.find("customer=cus_123"), std::string::npos);
        EXPECT_NE(req.body.find("return_url=https%3A%2F%2Fexample.com%2Freturn"), std::string::npos);

        return HttpResponse{
            .status_code = 200,
            .headers = {},
            .body = R"({
                "id": "bps_portal_1",
                "customer": "cus_123",
                "url": "https://billing.stripe.com/session/bps_portal_1",
                "return_url": "https://example.com/return",
                "created": 1700000000
            })"
        };
    });

    const CreateBillingPortalSessionRequest request{
        .customer_id = "cus_123",
        .return_url = "https://example.com/return"
    };

    auto task = client->billing_portal().sessions().create_async(request);
    const BillingPortalSession session = task.result();
    EXPECT_EQ(session.id, "bps_portal_1");
    EXPECT_EQ(session.customer_id, "cus_123");
    EXPECT_EQ(session.url, "https://billing.stripe.com/session/bps_portal_1");
}

TEST_F(StripeApiClientTest, ApiErrorThrowsStripeApiException) {
    http_client.set_response(HttpResponse{
        .status_code = 402,
        .headers = {{"request-id", "req_card_declined"}},
        .body = R"({
            "error": {
                "type": "card_error",
                "code": "card_declined",
                "message": "Your card was declined."
            }
        })"
    });

    try {
        auto task = client->customers().retrieve_async("cus_invalid");
        task.result();
        FAIL() << "Expected StripeApiException";
    } catch (const StripeApiException& error) {
        EXPECT_EQ(error.payload().type, StripeErrorType::Card);
        EXPECT_EQ(error.payload().code, "card_declined");
        EXPECT_EQ(error.payload().message, "Your card was declined.");
        EXPECT_EQ(error.payload().http_status, 402);
        EXPECT_EQ(error.payload().request_id, "req_card_declined");
        EXPECT_FALSE(error.retryable());
    }
}

TEST_F(StripeApiClientTest, ApiErrorPassedToCallback) {
    http_client.set_response(HttpResponse{
        .status_code = 400,
        .headers = {},
        .body = R"({
            "error": {
                "type": "invalid_request_error",
                "message": "Invalid request parameter"
            }
        })"
    });

    bool callback_invoked = false;
    client->customers().retrieve(
        "cus_test",
        RequestOptions{},
        [&](CallbackResult<Customer> result) {
            callback_invoked = true;
            ASSERT_TRUE(std::holds_alternative<std::exception_ptr>(result));
            try {
                std::rethrow_exception(std::get<std::exception_ptr>(result));
            } catch (const StripeApiException& error) {
                EXPECT_EQ(error.payload().type, StripeErrorType::InvalidRequest);
                EXPECT_EQ(error.payload().message, "Invalid request parameter");
            }
        });

    EXPECT_TRUE(callback_invoked);
}

TEST_F(StripeApiClientTest, TransportExceptionPropagates) {
    http_client.set_exception(std::make_exception_ptr(std::runtime_error("Connection refused")));

    EXPECT_THROW({
        auto task = client->customers().retrieve_async("cus_123");
        task.result();
    }, std::runtime_error);

    bool callback_invoked = false;
    http_client.set_exception(std::make_exception_ptr(std::runtime_error("Connection refused")));
    client->customers().retrieve(
        "cus_123",
        RequestOptions{},
        [&](CallbackResult<Customer> result) {
            callback_invoked = true;
            ASSERT_TRUE(std::holds_alternative<std::exception_ptr>(result));
            EXPECT_THROW(std::rethrow_exception(std::get<std::exception_ptr>(result)), std::runtime_error);
        });
    EXPECT_TRUE(callback_invoked);
}
