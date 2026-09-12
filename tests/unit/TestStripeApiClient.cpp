/** TestStripeApiClient.cpp
 * 
 * Unit tests for StripeApiClient operations and request formatting.
 */

#include "TestFramework.h"
#include "TestHelpers.h"

#include "StripeKit/Client.h"
#include "StripeKit/Errors.h"
#include "StripeKit/StripeApiClient.h"

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace stripekit {

class DeferredHttpClient final : public HTTPClient {
public:
    Task<HttpResponse> send_async(const HttpRequest&) override {
        co_return HttpResponse{};
    }

    void send(const HttpRequest&, Completion completion) override {
        std::lock_guard<std::mutex> lock(mutex);
        pending = std::move(completion);
        ready.notify_all();
    }

    void wait_until_pending() {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock, [this]() { return pending != nullptr; });
    }

    void complete(HttpResponse response) {
        Completion completion;
        {
            std::lock_guard<std::mutex> lock(mutex);
            completion = std::move(pending);
        }
        ASSERT_TRUE(completion != nullptr);
        completion(std::move(response));
    }

private:
    std::mutex mutex;
    std::condition_variable ready;
    Completion pending;
};

} // namespace stripekit

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

TEST(StripeApiClientLifetimeTest, WaitsForCallbackBeforeDestroyingParser) {
    DeferredHttpClient http_client;
    bool callback_invoked = false;
    bool destruction_finished = false;

    std::thread destroyer([&]() {
        {
            StripeApiClient client(http_client);
            client.customers().create(
                CreateCustomerRequest{.email = "deferred@example.com"},
                RequestOptions{},
                [&](CallbackResult<Customer> result) {
                    callback_invoked = true;
                    ASSERT_TRUE(std::holds_alternative<Customer>(result));
                });
            http_client.wait_until_pending();
        }
        destruction_finished = true;
    });

    http_client.wait_until_pending();
    http_client.complete(HttpResponse{
        .status_code = 200,
        .headers = {},
        .body = R"({"id":"cus_deferred_1","email":"deferred@example.com"})"
    });
    destroyer.join();

    EXPECT_TRUE(callback_invoked);
    EXPECT_TRUE(destruction_finished);
}

TEST(StripeApiClientLifetimeTest, CallbackCanDestroyClient) {
    DeferredHttpClient http_client;
    auto client = std::make_unique<StripeApiClient>(http_client);

    client->customers().create(
        CreateCustomerRequest{.email = "destroy@example.com"},
        RequestOptions{},
        [&](CallbackResult<Customer> result) {
            ASSERT_TRUE(std::holds_alternative<Customer>(result));
            client.reset();
        });

    http_client.wait_until_pending();
    http_client.complete(HttpResponse{
        .status_code = 200,
        .headers = {},
        .body = R"({"id":"cus_destroy_1","email":"destroy@example.com"})"
    });

    EXPECT_EQ(client, nullptr);
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

TEST_F(StripeApiClientTest, CreateCheckoutSessionWithInlinePriceData) {
    http_client.set_handler([](const HttpRequest& req) {
        EXPECT_EQ(req.method, HttpMethod::Post);
        EXPECT_EQ(req.path, "/v1/checkout/sessions");
        EXPECT_NE(req.body.find("mode=subscription"), std::string::npos);
        EXPECT_NE(req.body.find("customer=cus_123"), std::string::npos);
        EXPECT_EQ(req.body.find("line_items%5B0%5D%5Bprice%5D="), std::string::npos);
        EXPECT_NE(req.body.find("line_items%5B0%5D%5Bprice_data%5D%5Bcurrency%5D=usd"), std::string::npos);
        EXPECT_NE(req.body.find("line_items%5B0%5D%5Bprice_data%5D%5Bunit_amount%5D=1200"), std::string::npos);
        EXPECT_NE(req.body.find("line_items%5B0%5D%5Bprice_data%5D%5Brecurring%5D%5Binterval%5D=month"), std::string::npos);
        EXPECT_NE(req.body.find("line_items%5B0%5D%5Bprice_data%5D%5Bproduct_data%5D%5Bname%5D=StripeKit%20demo%20membership"), std::string::npos);

        return HttpResponse{
            .status_code = 200,
            .headers = {},
            .body = R"({
                "id": "cs_test_inline_1",
                "customer": "cus_123",
                "status": "open",
                "mode": "subscription",
                "url": "https://checkout.stripe.com/c/pay/cs_test_inline_1"
            })"
        };
    });

    auto task = client->checkout().sessions().create_async(CreateCheckoutSessionRequest{
        .customer_id = "cus_123",
        .product_name = "StripeKit demo membership",
        .currency = "usd",
        .unit_amount = 1200,
        .recurring_interval = "month",
        .success_url = "https://example.com/success",
        .cancel_url = "https://example.com/cancel"
    });
    const CheckoutSession session = task.result();
    EXPECT_EQ(session.id, "cs_test_inline_1");
}

TEST_F(StripeApiClientTest, CreateCheckoutSessionOneTimePurchaseInlinePriceData) {
    http_client.set_handler([](const HttpRequest& req) {
        EXPECT_EQ(req.method, HttpMethod::Post);
        EXPECT_EQ(req.path, "/v1/checkout/sessions");
        EXPECT_NE(req.body.find("mode=payment"), std::string::npos);
        EXPECT_EQ(req.body.find("customer="), std::string::npos);
        EXPECT_EQ(req.body.find("recurring"), std::string::npos);
        EXPECT_NE(req.body.find("line_items%5B0%5D%5Bprice_data%5D%5Bcurrency%5D=usd"), std::string::npos);
        EXPECT_NE(req.body.find("line_items%5B0%5D%5Bprice_data%5D%5Bunit_amount%5D=2999"), std::string::npos);
        EXPECT_NE(req.body.find("line_items%5B0%5D%5Bprice_data%5D%5Bproduct_data%5D%5Bname%5D=One-time%20Ebook"), std::string::npos);

        return HttpResponse{
            .status_code = 200,
            .headers = {},
            .body = R"({
                "id": "cs_test_payment_1",
                "customer": null,
                "status": "open",
                "mode": "payment",
                "payment_intent": "pi_test_12345",
                "url": "https://checkout.stripe.com/c/pay/cs_test_payment_1",
                "amount_total": 2999
            })"
        };
    });

    auto task = client->checkout().sessions().create_async(CreateCheckoutSessionRequest{
        .mode = "payment",
        .product_name = "One-time Ebook",
        .currency = "usd",
        .unit_amount = 2999,
        .success_url = "https://example.com/success",
        .cancel_url = "https://example.com/cancel"
    });
    const CheckoutSession session = task.result();
    EXPECT_EQ(session.id, "cs_test_payment_1");
    EXPECT_EQ(session.mode, "payment");
    EXPECT_EQ(session.payment_intent_id, "pi_test_12345");
    EXPECT_EQ(session.amount_total, 2999);
}

TEST_F(StripeApiClientTest, CreateCheckoutSessionOneTimePurchaseWithPriceIdAndCustomer) {
    http_client.set_handler([](const HttpRequest& req) {
        EXPECT_EQ(req.method, HttpMethod::Post);
        EXPECT_EQ(req.path, "/v1/checkout/sessions");
        EXPECT_NE(req.body.find("mode=payment"), std::string::npos);
        EXPECT_NE(req.body.find("customer=cus_456"), std::string::npos);
        EXPECT_NE(req.body.find("line_items%5B0%5D%5Bprice%5D=price_onetime_456"), std::string::npos);

        return HttpResponse{
            .status_code = 200,
            .headers = {},
            .body = R"({
                "id": "cs_test_payment_2",
                "customer": "cus_456",
                "status": "open",
                "mode": "payment",
                "payment_intent": "pi_test_67890",
                "url": "https://checkout.stripe.com/c/pay/cs_test_payment_2"
            })"
        };
    });

    auto task = client->checkout().sessions().create_async(CreateCheckoutSessionRequest{
        .customer_id = "cus_456",
        .mode = "payment",
        .price_id = "price_onetime_456",
        .success_url = "https://example.com/success",
        .cancel_url = "https://example.com/cancel"
    });
    const CheckoutSession session = task.result();
    EXPECT_EQ(session.id, "cs_test_payment_2");
    EXPECT_EQ(session.customer_id, "cus_456");
    EXPECT_EQ(session.mode, "payment");
    EXPECT_EQ(session.payment_intent_id, "pi_test_67890");
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

TEST_F(StripeApiClientTest, SubscriptionsRetrieveAndCancelCallbacks) {
    // Retrieve via callback
    http_client.set_response(HttpResponse{
        .status_code = 200,
        .headers = {},
        .body = R"({ "id": "sub_cb_1", "customer": "cus_123", "status": "active" })"
    });

    bool retrieve_cb_invoked = false;
    client->subscriptions().retrieve("sub_cb_1", {}, [&](CallbackResult<Subscription> result) {
        retrieve_cb_invoked = true;
        ASSERT_TRUE(std::holds_alternative<Subscription>(result));
        EXPECT_EQ(std::get<Subscription>(result).id, "sub_cb_1");
        EXPECT_EQ(std::get<Subscription>(result).status, "active");
    });
    EXPECT_TRUE(retrieve_cb_invoked);

    // Cancel via callback
    http_client.set_response(HttpResponse{
        .status_code = 200,
        .headers = {},
        .body = R"({ "id": "sub_cb_1", "customer": "cus_123", "status": "canceled" })"
    });

    bool cancel_cb_invoked = false;
    client->subscriptions().cancel("sub_cb_1", {}, [&](CallbackResult<Subscription> result) {
        cancel_cb_invoked = true;
        ASSERT_TRUE(std::holds_alternative<Subscription>(result));
        EXPECT_EQ(std::get<Subscription>(result).id, "sub_cb_1");
        EXPECT_EQ(std::get<Subscription>(result).status, "canceled");
    });
    EXPECT_TRUE(cancel_cb_invoked);
}

TEST_F(StripeApiClientTest, CreateCheckoutSessionCallbackSuccess) {
    http_client.set_response(HttpResponse{
        .status_code = 200,
        .headers = {},
        .body = R"({ "id": "cs_cb_1", "customer": "cus_123", "status": "open" })"
    });

    bool checkout_cb_invoked = false;
    client->checkout().sessions().create(
        CreateCheckoutSessionRequest{
            .customer_id = "cus_123",
            .price_id = "price_test",
            .success_url = "https://example.com/success",
            .cancel_url = "https://example.com/cancel"
        },
        {},
        [&](CallbackResult<CheckoutSession> result) {
            checkout_cb_invoked = true;
            ASSERT_TRUE(std::holds_alternative<CheckoutSession>(result));
            EXPECT_EQ(std::get<CheckoutSession>(result).id, "cs_cb_1");
        });
    EXPECT_TRUE(checkout_cb_invoked);
}

TEST_F(StripeApiClientTest, CreateBillingPortalSessionCallbackSuccess) {
    http_client.set_response(HttpResponse{
        .status_code = 200,
        .headers = {},
        .body = R"({ "id": "bps_cb_1", "customer": "cus_123", "url": "https://billing.stripe.com/session/bps_cb_1" })"
    });

    bool portal_cb_invoked = false;
    client->billing_portal().sessions().create(
        CreateBillingPortalSessionRequest{
            .customer_id = "cus_123",
            .return_url = "https://example.com/return"
        },
        {},
        [&](CallbackResult<BillingPortalSession> result) {
            portal_cb_invoked = true;
            ASSERT_TRUE(std::holds_alternative<BillingPortalSession>(result));
            EXPECT_EQ(std::get<BillingPortalSession>(result).id, "bps_cb_1");
        });
    EXPECT_TRUE(portal_cb_invoked);
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
