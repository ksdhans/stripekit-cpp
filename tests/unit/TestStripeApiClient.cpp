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

TEST_F(StripeApiClientTest, DefaultApiVersionIsSentForAsyncAndCallbackRequests) {
    http_client.set_response({ .status_code = 200, .body = R"({"id":"cus_version"})" });
    client->customers().create_async({ .email = "version@example.com" }).result();
    client->customers().retrieve_async("cus_version").result();
    int calls = 0;
    const auto completion = [&](CallbackResult<Customer> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<Customer>(result));
    };
    client->customers().create({}, {}, completion);
    client->customers().retrieve("cus_version", {}, completion);
    EXPECT_EQ(calls, 2);
    ASSERT_EQ(http_client.get_recorded_requests().size(), 4U);
    for (const auto& request : http_client.get_recorded_requests())
        EXPECT_EQ(request.headers.at("Stripe-Version"), default_stripe_api_version);

    http_client.set_response(
        { .status_code = 200, .body = R"({"object":"invoice","id":"in_version","deleted":true})" });
    client->invoices().delete_async("in_version").result();
    EXPECT_EQ(http_client.get_recorded_requests().back().method, HttpMethod::Delete);
    EXPECT_EQ(http_client.get_recorded_requests().back().headers.at("Stripe-Version"),
              default_stripe_api_version);
}

TEST_F(StripeApiClientTest, ApiVersionOverridesAreCaseInsensitiveAndDoNotChangeTheDefault) {
    http_client.set_response({ .status_code = 200, .body = R"({"id":"cus_version"})" });
    for (const auto* name : { "Stripe-Version", "stripe-version", "STRIPE-VERSION" }) {
        const RequestOptions options{ .headers = { { name, "2024-06-20" } } };
        client->customers().retrieve_async("cus_version", options).result();
        const auto sent = http_client.get_recorded_requests().back();
        EXPECT_EQ(sent.headers.at("Stripe-Version"), "2024-06-20");
        EXPECT_EQ(sent.headers.size(), 2U);
    }
    client->customers().retrieve_async("cus_version").result();
    EXPECT_EQ(http_client.get_recorded_requests().back().headers.at("Stripe-Version"),
              default_stripe_api_version);
}

TEST_F(StripeApiClientTest, CheckoutPaymentMethodsUseEndiveParametersInBothForms) {
    http_client.set_response({ .status_code = 200, .body = R"({"id":"cs_version"})" });
    CreateCheckoutSessionRequest request;
    request.price_id = "price_version";
    request.payment_method_types = { "card" };
    client->checkout().sessions().create_async(request).result();
    int calls = 0;
    client->checkout().sessions().create(request, {}, [&](CallbackResult<CheckoutSession> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<CheckoutSession>(result));
    });
    EXPECT_EQ(calls, 1);
    for (const auto& sent : http_client.get_recorded_requests()) {
        EXPECT_EQ(sent.headers.at("Stripe-Version"), default_stripe_api_version);
        EXPECT_NE(sent.body.find("&allowed_payment_method_types%5B0%5D=card"), std::string::npos);
        EXPECT_EQ(sent.body.find("&payment_method_types"), std::string::npos);
    }
}

TEST_F(StripeApiClientTest, ExpireCheckoutPreservesOptionsAndEncodesIdentity) {
    http_client.set_response(
        { .status_code = 200,
          .body =
              R"({"object":"checkout.session","id":"cs_test","status":"expired","metadata":{"owner":"tenant"}})" });
    RequestOptions options;
    options.idempotency_key = "expiry-key";
    options.timeout = std::chrono::seconds(7);
    options.headers["Stripe-Version"] = "2024-06-20";
    const auto session = client->checkout().sessions().expire_async("cs/a?b", options).result();
    EXPECT_EQ(session.status, "expired");
    EXPECT_EQ(session.metadata.at("owner"), "tenant");
    const auto sent = http_client.get_recorded_requests().at(0);
    EXPECT_EQ(sent.method, HttpMethod::Post);
    EXPECT_EQ(sent.path, "/v1/checkout/sessions/cs%2Fa%3Fb/expire");
    EXPECT_TRUE(sent.body.empty());
    EXPECT_EQ(sent.options.timeout, options.timeout);
    EXPECT_EQ(sent.headers.at("Idempotency-Key"), "expiry-key");
    EXPECT_EQ(sent.headers.at("Stripe-Version"), "2024-06-20");
    int calls = 0;
    client->checkout().sessions().expire(
        "cs_test", options, [&](CallbackResult<CheckoutSession> result) {
            ++calls;
            ASSERT_TRUE(std::holds_alternative<CheckoutSession>(result));
            EXPECT_EQ(std::get<CheckoutSession>(result).status, "expired");
        });
    EXPECT_EQ(calls, 1);
}

TEST_F(StripeApiClientTest, CheckoutListingReturnsOnePageAndEncodesFilters) {
    http_client.set_response(
        { .status_code = 200,
          .body =
              R"({"object":"list","has_more":true,"data":[{"object":"checkout.session","id":"cs_one","status":"complete","mode":"payment","payment_intent":"pi_test"}]})" });
    ListCheckoutSessionsRequest request;
    request.payment_intent_id = "pi/a?b";
    request.starting_after = "cs &last";
    request.limit = 100;
    RequestOptions options;
    options.timeout = std::chrono::seconds(3);
    const auto page = client->checkout().sessions().list_async(request, options).result();
    ASSERT_EQ(page.data.size(), 1U);
    EXPECT_TRUE(page.has_more);
    EXPECT_EQ(page.data.front().payment_intent_id, "pi_test");
    const auto recorded = http_client.get_recorded_requests();
    ASSERT_EQ(recorded.size(), 1U);
    EXPECT_EQ(recorded[0].method, HttpMethod::Get);
    EXPECT_EQ(
        recorded[0].path,
        "/v1/checkout/sessions?limit=100&payment_intent=pi%2Fa%3Fb&starting_after=cs%20%26last");
    EXPECT_EQ(recorded[0].options.timeout, options.timeout);
    int calls = 0;
    client->checkout().sessions().list({}, {}, [&](CallbackResult<CheckoutSessionList> result) {
        ++calls;
        ASSERT_TRUE(std::holds_alternative<CheckoutSessionList>(result));
    });
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(http_client.get_recorded_requests().back().path, "/v1/checkout/sessions?limit=10");
    http_client.set_response(
        { .status_code = 200, .body = R"({"object":"list","has_more":false,"data":[]})" });
    EXPECT_TRUE(client->checkout().sessions().list_async().result().data.empty());
}

TEST_F(StripeApiClientTest, CheckoutManagementRejectsInvalidInputWithoutNetworkCalls) {
    EXPECT_THROW(client->checkout().sessions().expire_async("").result(), ConfigurationException);
    for (const auto limit : { 0, 101 }) {
        ListCheckoutSessionsRequest request;
        request.limit = limit;
        EXPECT_THROW(client->checkout().sessions().list_async(request).result(),
                     ConfigurationException);
    }
    int calls = 0;
    client->checkout().sessions().expire("", {}, [&](CallbackResult<CheckoutSession> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    });
    client->checkout().sessions().list(
        { .limit = 0 }, {}, [&](CallbackResult<CheckoutSessionList> result) {
            ++calls;
            EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
        });
    EXPECT_EQ(calls, 2);
    EXPECT_TRUE(http_client.get_recorded_requests().empty());
}

TEST_F(StripeApiClientTest, CheckoutListingFollowsExplicitCursorsAndParsesExpandedIdentities) {
    http_client.set_handler([](const HttpRequest& request) {
        if (request.path == "/v1/checkout/sessions?limit=1") {
            return HttpResponse{
                .status_code = 200,
                .body =
                    R"({"object":"list","has_more":true,"data":[{"object":"checkout.session","id":"cs_first","status":"complete","mode":"payment","customer":{"id":"cus_test"},"payment_intent":{"id":"pi_test"},"metadata":{"owner":"tenant"}}]})"
            };
        }
        EXPECT_EQ(request.path, "/v1/checkout/sessions?limit=1&starting_after=cs_first");
        return HttpResponse{
            .status_code = 200,
            .body =
                R"({"object":"list","has_more":false,"data":[{"object":"checkout.session","id":"cs_last","status":"open","mode":"subscription","subscription":{"id":"sub_test"},"expires_at":2000000000}]})"
        };
    });
    ListCheckoutSessionsRequest request;
    request.limit = 1;
    auto page = client->checkout().sessions().list_async(request).result();
    ASSERT_EQ(page.data.size(), 1U);
    EXPECT_TRUE(page.has_more);
    EXPECT_EQ(page.data[0].customer_id, "cus_test");
    EXPECT_EQ(page.data[0].payment_intent_id, "pi_test");
    EXPECT_EQ(page.data[0].metadata.at("owner"), "tenant");
    ASSERT_EQ(http_client.get_recorded_requests().size(), 1U);
    request.starting_after = page.data[0].id;
    page = client->checkout().sessions().list_async(request).result();
    ASSERT_EQ(page.data.size(), 1U);
    EXPECT_FALSE(page.has_more);
    EXPECT_EQ(page.data[0].id, "cs_last");
    EXPECT_EQ(page.data[0].subscription_id, "sub_test");
    EXPECT_EQ(page.data[0].expires_at_unix, 2000000000);
    EXPECT_EQ(http_client.get_recorded_requests().size(), 2U);
}

TEST_F(StripeApiClientTest, CheckoutListsRejectMalformedPages) {
    for (
        const auto* body :
        { "{}",
          R"({"object":"list","data":[],"has_more":true})",
          R"({"object":"list","data":{},"has_more":false})",
          R"({"object":"list","data":[]})",
          R"({"object":"list","data":[],"has_more":"false"})",
          R"({"object":"list","data":[{"object":"checkout.session","status":"open"}],"has_more":false})",
          R"({"object":"list","data":[{"object":"checkout.session","id":"cs_test"}],"has_more":false})",
          R"({"object":"list","data":[{"object":"customer","id":"cus_test","status":"open"}],"has_more":false})" }) {
        http_client.set_response({ .status_code = 200, .body = body });
        EXPECT_THROW(client->checkout().sessions().list_async().result(), ConfigurationException);
    }
}

TEST_F(StripeApiClientTest, CheckoutCallbacksPropagateParseProviderAndTransportFailuresOnce) {
    int calls = 0;
    const auto session_failure = [&](CallbackResult<CheckoutSession> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    };
    const auto list_failure = [&](CallbackResult<CheckoutSessionList> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    };
    http_client.set_response({ .status_code = 200, .body = "not json" });
    client->checkout().sessions().expire("cs_test", {}, session_failure);
    client->checkout().sessions().list({}, {}, list_failure);
    http_client.set_response(
        { .status_code = 429,
          .body = R"({"error":{"type":"rate_limit_error","message":"Slow down"}})" });
    client->checkout().sessions().list({}, {}, list_failure);
    http_client.set_exception(std::make_exception_ptr(std::runtime_error("Offline")));
    client->checkout().sessions().expire("cs_test", {}, session_failure);
    http_client.set_exception(std::make_exception_ptr(std::runtime_error("Offline")));
    EXPECT_THROW(client->checkout().sessions().list_async().result(), std::runtime_error);
    EXPECT_EQ(calls, 4);
    EXPECT_EQ(http_client.get_recorded_requests().size(), 5U);
}

TEST_F(StripeApiClientTest, CheckoutManagementPropagatesStripeAndTransportFailures) {
    http_client.set_response(
        { .status_code = 400,
          .headers = { { "request-id", "req_expire" } },
          .body =
              R"({"error":{"type":"invalid_request_error","message":"Session is complete"}})" });
    EXPECT_THROW(client->checkout().sessions().expire_async("cs_complete").result(),
                 StripeApiException);
    EXPECT_THROW(client->checkout().sessions().list_async().result(), StripeApiException);
    int calls = 0;
    client->checkout().sessions().expire(
        "cs_complete", {}, [&](CallbackResult<CheckoutSession> result) {
            ++calls;
            ASSERT_TRUE(std::holds_alternative<std::exception_ptr>(result));
            try {
                std::rethrow_exception(std::get<std::exception_ptr>(result));
            } catch (const StripeApiException& error) {
                EXPECT_EQ(error.payload().request_id, "req_expire");
            }
        });
    http_client.set_exception(std::make_exception_ptr(std::runtime_error("Offline")));
    client->checkout().sessions().list({}, {}, [&](CallbackResult<CheckoutSessionList> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    });
    EXPECT_EQ(calls, 2);
    http_client.set_exception(std::make_exception_ptr(std::runtime_error("Offline")));
    EXPECT_THROW(client->checkout().sessions().expire_async("cs_test").result(),
                 std::runtime_error);
}

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

        return HttpResponse{ .status_code = 200,
                             .headers = { { "request-id", "req_cust_1" } },
                             .body = R"({
                "id": "cus_new_1",
                "email": "test@example.com",
                "name": "Test User",
                "livemode": false,
                "metadata": {"user_id": "usr_999"}
            })" };
    });

    const CreateCustomerRequest request{ .email = "test@example.com",
                                         .name = "Test User",
                                         .metadata = { { "user_id", "usr_999" } } };

    const RequestOptions options{ .idempotency_key = "idem_key_123",
                                  .headers = { { "X-Custom-Header", "custom_val" } } };

    auto task = client->customers().create_async(request, options);
    const Customer customer = task.result();

    EXPECT_EQ(customer.id, "cus_new_1");
    EXPECT_EQ(customer.email, "test@example.com");
    EXPECT_EQ(customer.name, "Test User");
    EXPECT_EQ(customer.metadata.at("user_id"), "usr_999");
}

TEST_F(StripeApiClientTest, CreateCustomerCallbackSuccess) {
    http_client.set_response(
        HttpResponse{ .status_code = 200,
                      .headers = {},
                      .body = R"({ "id": "cus_cb_1", "email": "cb@example.com" })" });

    bool callback_invoked = false;
    client->customers().create(CreateCustomerRequest{ .email = "cb@example.com" },
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
            client.customers().create(CreateCustomerRequest{ .email = "deferred@example.com" },
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
    http_client.complete(
        HttpResponse{ .status_code = 200,
                      .headers = {},
                      .body = R"({"id":"cus_deferred_1","email":"deferred@example.com"})" });
    destroyer.join();

    EXPECT_TRUE(callback_invoked);
    EXPECT_TRUE(destruction_finished);
}

TEST(StripeApiClientLifetimeTest, CallbackCanDestroyClient) {
    DeferredHttpClient http_client;
    auto client = std::make_unique<StripeApiClient>(http_client);

    client->customers().create(CreateCustomerRequest{ .email = "destroy@example.com" },
                               RequestOptions{},
                               [&](CallbackResult<Customer> result) {
                                   ASSERT_TRUE(std::holds_alternative<Customer>(result));
                                   client.reset();
                               });

    http_client.wait_until_pending();
    http_client.complete(
        HttpResponse{ .status_code = 200,
                      .headers = {},
                      .body = R"({"id":"cus_destroy_1","email":"destroy@example.com"})" });

    EXPECT_EQ(client, nullptr);
}

TEST_F(StripeApiClientTest, RetrieveCustomerAsyncSuccess) {
    http_client.set_handler([](const HttpRequest& req) {
        EXPECT_EQ(req.method, HttpMethod::Get);
        EXPECT_EQ(req.path, "/v1/customers/cus_special%2Fid");
        return HttpResponse{ .status_code = 200,
                             .headers = {},
                             .body =
                                 R"({ "id": "cus_special/id", "email": "special@example.com" })" };
    });

    auto task = client->customers().retrieve_async("cus_special/id");
    const Customer customer = task.result();
    EXPECT_EQ(customer.id, "cus_special/id");
}

TEST_F(StripeApiClientTest, UpdateCustomerAddressUsesTypedPartialFields) {
    http_client.set_handler([](const HttpRequest& req) {
        EXPECT_EQ(req.method, HttpMethod::Post);
        EXPECT_EQ(req.path, "/v1/customers/cus_special%2Fid");
        EXPECT_EQ(req.body,
                  "tax%5Bvalidate_location%5D=immediately&address%5Bcity%5D=Wellington&address%"
                  "5Bpostal_code%5D=6011"
                  "&address%5Bcountry%5D=NZ");
        return HttpResponse{
            .status_code = 200,
            .body =
                R"({"id":"cus_special/id","address":{"city":"Wellington","country":"NZ","postal_code":"6011"},"tax":{"location":{"status":"verified","source":"billing_address"}}})"
        };
    });
    UpdateCustomerRequest request;
    request.address =
        BillingAddress{ .city = "Wellington", .postal_code = "6011", .country = "NZ" };
    request.validate_tax_location = TaxLocationValidation::Immediately;
    const auto customer = client->customers().update_async("cus_special/id", request).result();
    ASSERT_TRUE(customer.address.has_value());
    EXPECT_EQ(customer.address->country.value(), "NZ");
    EXPECT_EQ(customer.tax_location_status, "verified");
    EXPECT_THROW(client->customers().update_async("cus", {}).result(), ConfigurationException);
    UpdateCustomerRequest invalid;
    invalid.address = BillingAddress{ .country = "NZL" };
    EXPECT_THROW(client->customers().update_async("cus", invalid).result(), ConfigurationException);
    invalid.address.reset();
    invalid.validate_tax_location = static_cast<TaxLocationValidation>(99);
    EXPECT_THROW(client->customers().update_async("cus", invalid).result(), ConfigurationException);
    EXPECT_EQ(http_client.get_recorded_requests().size(), 1U);
}

TEST_F(StripeApiClientTest, CustomerTaxLocationValidationUsesStripePolicies) {
    http_client.set_handler([](const HttpRequest& req) {
        EXPECT_EQ(req.method, HttpMethod::Post);
        EXPECT_EQ(req.path, "/v1/customers/cus_tax");
        return HttpResponse{ .status_code = 200, .body = R"({"id":"cus_tax"})" };
    });
    const std::vector<std::pair<TaxLocationValidation, std::string>> policies = {
        { TaxLocationValidation::Automatic, "auto" },
        { TaxLocationValidation::Deferred, "deferred" },
        { TaxLocationValidation::Immediately, "immediately" }
    };
    for (const auto& [policy, value] : policies) {
        UpdateCustomerRequest request;
        request.validate_tax_location = policy;
        client->customers().update_async("cus_tax", request).result();
        EXPECT_EQ(http_client.get_recorded_requests().back().body,
                  "tax%5Bvalidate_location%5D=" + value);
    }
}

TEST_F(StripeApiClientTest, CreateCheckoutSessionAsyncSuccess) {
    http_client.set_handler([](const HttpRequest& req) {
        EXPECT_EQ(req.method, HttpMethod::Post);
        EXPECT_EQ(req.path, "/v1/checkout/sessions");
        EXPECT_NE(req.body.find("mode=subscription"), std::string::npos);
        EXPECT_NE(req.body.find("customer=cus_123"), std::string::npos);
        EXPECT_NE(req.body.find("line_items%5B0%5D%5Bprice%5D=price_abc"), std::string::npos);
        EXPECT_NE(req.body.find("line_items%5B0%5D%5Bquantity%5D=2"), std::string::npos);
        EXPECT_NE(req.body.find("success_url=https%3A%2F%2Fexample.com%2Fsuccess"),
                  std::string::npos);
        EXPECT_NE(req.body.find("cancel_url=https%3A%2F%2Fexample.com%2Fcancel"),
                  std::string::npos);
        EXPECT_NE(req.body.find("metadata%5Bplan%5D=starter"), std::string::npos);
        EXPECT_EQ(req.body.find("trial_period_days"), std::string::npos);
        EXPECT_EQ(req.body.find("subscription_data"), std::string::npos);
        EXPECT_EQ(req.body.find("payment_method_collection"), std::string::npos);
        EXPECT_EQ(req.body.find("payment_method_types"), std::string::npos);
        EXPECT_EQ(req.body.find("customer_email"), std::string::npos);
        EXPECT_EQ(req.body.find("expires_at"), std::string::npos);
        EXPECT_EQ(req.body.find("automatic_tax"), std::string::npos);
        EXPECT_EQ(req.body.find("billing_address_collection"), std::string::npos);
        EXPECT_EQ(req.body.find("customer_update"), std::string::npos);
        EXPECT_EQ(req.body.find("tax_id_collection"), std::string::npos);

        return HttpResponse{ .status_code = 200, .headers = {}, .body = R"({
                "id": "cs_test_session_1",
                "customer": "cus_123",
                "status": "open",
                "mode": "subscription",
                "url": "https://checkout.stripe.com/c/pay/cs_test_session_1"
            })" };
    });

    const CreateCheckoutSessionRequest request{ .customer_id = "cus_123",
                                                .price_id = "price_abc",
                                                .success_url = "https://example.com/success",
                                                .cancel_url = "https://example.com/cancel",
                                                .quantity = 2,
                                                .metadata = { { "plan", "starter" } } };

    auto task = client->checkout().sessions().create_async(request);
    const CheckoutSession session = task.result();
    EXPECT_EQ(session.id, "cs_test_session_1");
    EXPECT_EQ(session.customer_id, "cus_123");
    EXPECT_EQ(session.url, "https://checkout.stripe.com/c/pay/cs_test_session_1");
}

TEST_F(StripeApiClientTest, CheckoutTaxSettingsEncodeExplicitValuesForExistingCustomer) {
    http_client.set_handler([](const HttpRequest& req) {
        EXPECT_EQ(req.method, HttpMethod::Post);
        EXPECT_NE(req.body.find("automatic_tax%5Benabled%5D=true"), std::string::npos);
        EXPECT_NE(req.body.find("billing_address_collection=auto"), std::string::npos);
        EXPECT_NE(req.body.find("customer_update%5Baddress%5D=auto"), std::string::npos);
        EXPECT_NE(req.body.find("customer_update%5Bname%5D=auto"), std::string::npos);
        EXPECT_NE(req.body.find("tax_id_collection%5Benabled%5D=true"), std::string::npos);
        return HttpResponse{
            .status_code = 200,
            .body = R"({"id":"cs_tax","automatic_tax":{"enabled":true,"status":"complete"}})"
        };
    });
    CreateCheckoutSessionRequest request;
    request.customer_id = "cus_tax";
    request.price_id = "price_exclusive";
    request.automatic_tax_enabled = true;
    request.billing_address_collection = "auto";
    request.customer_update_address = true;
    request.customer_update_name = true;
    request.tax_id_collection_enabled = true;
    const auto session = client->checkout().sessions().create_async(request).result();
    ASSERT_TRUE(session.automatic_tax.has_value());
    EXPECT_TRUE(session.automatic_tax->enabled);
    EXPECT_EQ(session.automatic_tax->status, "complete");
}

TEST_F(StripeApiClientTest, CheckoutLineItemListSupportsPaginationAndOptions) {
    http_client.set_handler([](const HttpRequest& req) {
        EXPECT_EQ(req.method, HttpMethod::Get);
        EXPECT_EQ(req.path,
                  "/v1/checkout/sessions/cs%2Ftax/line_items?limit=1&starting_after=li%3F1");
        EXPECT_EQ(req.headers.at("Stripe-Version"), "2025-03-31.basil");
        return HttpResponse{
            .status_code = 200,
            .body =
                R"({"object":"list","has_more":true,"data":[{"id":"li_tax","pricing":{"price_details":{"price":"price_nz"}},"quantity":2,"amount_total":2500}]})"
        };
    });
    RequestOptions options{ .headers = { { "Stripe-Version", "2025-03-31.basil" } } };
    const auto page =
        client->checkout()
            .sessions()
            .list_line_items_async("cs/tax", { .limit = 1, .starting_after = "li?1" }, options)
            .result();
    ASSERT_EQ(page.data.size(), 1U);
    EXPECT_TRUE(page.has_more);
    EXPECT_EQ(page.data[0].id, "li_tax");
    EXPECT_EQ(page.data[0].price_id, "price_nz");
    EXPECT_EQ(page.data[0].quantity.value(), 2);
}

TEST_F(StripeApiClientTest, CheckoutLineItemPaginationRejectsInvalidRequestsAndResponses) {
    auto& checkout = client->checkout().sessions();
    EXPECT_THROW(checkout.list_line_items_async("").result(), ConfigurationException);
    for (const auto limit : { 0, 101 })
        EXPECT_THROW(checkout.list_line_items_async("cs_test", { .limit = limit }).result(),
                     ConfigurationException);
    int callbacks = 0;
    checkout.list_line_items("", {}, {}, [&](CallbackResult<PurchasedLineItemList> result) {
        ++callbacks;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    });
    EXPECT_EQ(callbacks, 1);
    EXPECT_TRUE(http_client.get_recorded_requests().empty());
    http_client.set_response(
        { .status_code = 200,
          .body =
              R"({"object":"list","has_more":false,"data":[{"pricing":{"price_details":{"price":"price_test"}},"quantity":1}]})" });
    EXPECT_THROW(checkout.list_line_items_async("cs_test").result(), ConfigurationException);
    EXPECT_EQ(http_client.get_recorded_requests().size(), 1U);
}

TEST_F(StripeApiClientTest, CreateCheckoutSessionWithInlinePriceData) {
    http_client.set_handler([](const HttpRequest& req) {
        EXPECT_EQ(req.method, HttpMethod::Post);
        EXPECT_EQ(req.path, "/v1/checkout/sessions");
        EXPECT_NE(req.body.find("mode=subscription"), std::string::npos);
        EXPECT_NE(req.body.find("customer=cus_123"), std::string::npos);
        EXPECT_EQ(req.body.find("line_items%5B0%5D%5Bprice%5D="), std::string::npos);
        EXPECT_NE(req.body.find("line_items%5B0%5D%5Bprice_data%5D%5Bcurrency%5D=usd"),
                  std::string::npos);
        EXPECT_NE(req.body.find("line_items%5B0%5D%5Bprice_data%5D%5Bunit_amount%5D=1200"),
                  std::string::npos);
        EXPECT_NE(
            req.body.find("line_items%5B0%5D%5Bprice_data%5D%5Brecurring%5D%5Binterval%5D=month"),
            std::string::npos);
        EXPECT_NE(req.body.find("line_items%5B0%5D%5Bprice_data%5D%5Bproduct_data%5D%5Bname%5D="
                                "StripeKit%20demo%20membership"),
                  std::string::npos);

        return HttpResponse{ .status_code = 200, .headers = {}, .body = R"({
                "id": "cs_test_inline_1",
                "customer": "cus_123",
                "status": "open",
                "mode": "subscription",
                "url": "https://checkout.stripe.com/c/pay/cs_test_inline_1"
            })" };
    });

    auto task = client->checkout().sessions().create_async(
        CreateCheckoutSessionRequest{ .customer_id = "cus_123",
                                      .product_name = "StripeKit demo membership",
                                      .currency = "usd",
                                      .unit_amount = 1200,
                                      .recurring_interval = "month",
                                      .success_url = "https://example.com/success",
                                      .cancel_url = "https://example.com/cancel" });
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
        EXPECT_NE(req.body.find("line_items%5B0%5D%5Bprice_data%5D%5Bcurrency%5D=usd"),
                  std::string::npos);
        EXPECT_NE(req.body.find("line_items%5B0%5D%5Bprice_data%5D%5Bunit_amount%5D=2999"),
                  std::string::npos);
        EXPECT_NE(
            req.body.find(
                "line_items%5B0%5D%5Bprice_data%5D%5Bproduct_data%5D%5Bname%5D=One-time%20Ebook"),
            std::string::npos);

        return HttpResponse{ .status_code = 200, .headers = {}, .body = R"({
                "id": "cs_test_payment_1",
                "customer": null,
                "status": "open",
                "mode": "payment",
                "payment_intent": "pi_test_12345",
                "url": "https://checkout.stripe.com/c/pay/cs_test_payment_1",
                "amount_total": 2999
            })" };
    });

    auto task = client->checkout().sessions().create_async(
        CreateCheckoutSessionRequest{ .mode = "payment",
                                      .product_name = "One-time Ebook",
                                      .currency = "usd",
                                      .unit_amount = 2999,
                                      .success_url = "https://example.com/success",
                                      .cancel_url = "https://example.com/cancel" });
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
        EXPECT_NE(req.body.find("line_items%5B0%5D%5Bprice%5D=price_onetime_456"),
                  std::string::npos);

        return HttpResponse{ .status_code = 200, .headers = {}, .body = R"({
                "id": "cs_test_payment_2",
                "customer": "cus_456",
                "status": "open",
                "mode": "payment",
                "payment_intent": "pi_test_67890",
                "url": "https://checkout.stripe.com/c/pay/cs_test_payment_2"
            })" };
    });

    auto task = client->checkout().sessions().create_async(
        CreateCheckoutSessionRequest{ .customer_id = "cus_456",
                                      .mode = "payment",
                                      .price_id = "price_onetime_456",
                                      .success_url = "https://example.com/success",
                                      .cancel_url = "https://example.com/cancel" });
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

TEST_F(StripeApiClientTest, MalformedErrorResponseThrowsStripeApiException) {
    http_client.set_response(HttpResponse{ .status_code = 502,
                                           .headers = { { "request-id", "req_gateway" } },
                                           .body = "<html>Bad gateway</html>" });
    try {
        client->customers().retrieve_async("cus_test").result();
        FAIL();
    } catch (const StripeApiException& error) {
        EXPECT_EQ(error.payload().http_status, 502);
        EXPECT_EQ(error.payload().request_id, "req_gateway");
        EXPECT_EQ(error.payload().type, StripeErrorType::Unknown);
        EXPECT_TRUE(error.payload().retryable);
    }

    int completions = 0;
    client->customers().retrieve("cus_test", {}, [&](CallbackResult<Customer> result) {
        ++completions;
        ASSERT_TRUE(std::holds_alternative<std::exception_ptr>(result));
        try {
            std::rethrow_exception(std::get<std::exception_ptr>(result));
            FAIL();
        } catch (const StripeApiException& error) {
            EXPECT_EQ(error.payload().http_status, 502);
            EXPECT_EQ(error.payload().request_id, "req_gateway");
            EXPECT_TRUE(error.payload().retryable);
        }
    });
    EXPECT_EQ(completions, 1);
}

TEST_F(StripeApiClientTest, SubscriptionsRetrieveAndCancelCallbacks) {
    // Retrieve via callback
    http_client.set_response(HttpResponse{
        .status_code = 200,
        .headers = {},
        .body = R"({ "id": "sub_cb_1", "customer": "cus_123", "status": "active" })" });

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
        .body = R"({ "id": "sub_cb_1", "customer": "cus_123", "status": "canceled" })" });

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
    http_client.set_response(
        HttpResponse{ .status_code = 200,
                      .headers = {},
                      .body = R"({ "id": "cs_cb_1", "customer": "cus_123", "status": "open" })" });

    bool checkout_cb_invoked = false;
    client->checkout().sessions().create(
        CreateCheckoutSessionRequest{ .customer_id = "cus_123",
                                      .price_id = "price_test",
                                      .success_url = "https://example.com/success",
                                      .cancel_url = "https://example.com/cancel" },
        {},
        [&](CallbackResult<CheckoutSession> result) {
            checkout_cb_invoked = true;
            ASSERT_TRUE(std::holds_alternative<CheckoutSession>(result));
            EXPECT_EQ(std::get<CheckoutSession>(result).id, "cs_cb_1");
        });
    EXPECT_TRUE(checkout_cb_invoked);
}

TEST_F(StripeApiClientTest, CreateCardRequiredSubscriptionTrial) {
    http_client.set_handler([](const HttpRequest& req) {
        EXPECT_EQ(req.path, "/v1/checkout/sessions");
        EXPECT_EQ(req.headers.at("Idempotency-Key"), "trial_attempt");
        EXPECT_NE(req.body.find("customer_email=buyer%40example.com"), std::string::npos);
        EXPECT_NE(req.body.find("subscription_data%5Btrial_period_days%5D=14"), std::string::npos);
        EXPECT_NE(req.body.find("subscription_data%5Bmetadata%5D%5Btenant_id%5D=tenant_1"),
                  std::string::npos);
        EXPECT_NE(req.body.find("payment_method_collection=always"), std::string::npos);
        EXPECT_NE(req.body.find("&allowed_payment_method_types%5B0%5D=card"), std::string::npos);
        EXPECT_EQ(req.body.find("&payment_method_types"), std::string::npos);
        EXPECT_NE(req.body.find("expires_at=1800000000"), std::string::npos);
        return HttpResponse{ .status_code = 200,
                             .body =
                                 R"({"id":"cs_trial","status":"open","expires_at":1800000000})" };
    });
    CreateCheckoutSessionRequest request;
    request.price_id = "price_monthly";
    request.mode.clear();
    request.customer_email = "buyer@example.com";
    request.trial_period_days = 14;
    request.subscription_metadata = { { "tenant_id", "tenant_1" } };
    request.payment_method_collection = "always";
    request.payment_method_types = { "card" };
    request.expires_at = 1800000000;
    const auto session =
        client->checkout()
            .sessions()
            .create_async(request, RequestOptions{ .idempotency_key = "trial_attempt" })
            .result();
    EXPECT_EQ(session.id, "cs_trial");
    EXPECT_EQ(session.expires_at_unix, 1800000000);
}

TEST_F(StripeApiClientTest, RetrieveCheckoutSessionEncodesIdentifier) {
    http_client.set_handler([](const HttpRequest& req) {
        EXPECT_EQ(req.method, HttpMethod::Get);
        EXPECT_EQ(req.path, "/v1/checkout/sessions/cs_special%2Fid");
        EXPECT_TRUE(req.body.empty());
        return HttpResponse{ .status_code = 200,
                             .body = R"({"id":"cs_special/id","status":"complete"})" };
    });
    const auto session = client->checkout().sessions().retrieve_async("cs_special/id").result();
    EXPECT_EQ(session.status, "complete");
}

TEST_F(StripeApiClientTest, RejectConflictingCheckoutTrialSettings) {
    CreateCheckoutSessionRequest request;
    request.customer_id = "cus_existing";
    request.customer_email = "buyer@example.com";
    EXPECT_THROW(client->checkout().sessions().create_async(request).result(),
                 ConfigurationException);
    request.customer_id.clear();
    request.mode = "payment";
    request.trial_period_days = 14;
    EXPECT_THROW(client->checkout().sessions().create_async(request).result(),
                 ConfigurationException);
    request.mode = "subscription";
    request.trial_period_days = 0;
    EXPECT_THROW(client->checkout().sessions().create_async(request).result(),
                 ConfigurationException);
    request.trial_period_days = 731;
    EXPECT_THROW(client->checkout().sessions().create_async(request).result(),
                 ConfigurationException);
    request.trial_period_days = -1;
    EXPECT_THROW(client->checkout().sessions().create_async(request).result(),
                 ConfigurationException);
    request.trial_period_days.reset();
    request.customer_update_address = true;
    EXPECT_THROW(client->checkout().sessions().create_async(request).result(),
                 ConfigurationException);
    request.customer_update_address.reset();
    request.customer_id = "cus_existing";
    request.customer_email.clear();
    request.billing_address_collection = "invalid";
    EXPECT_THROW(client->checkout().sessions().create_async(request).result(),
                 ConfigurationException);
    EXPECT_TRUE(http_client.get_recorded_requests().empty());
}

TEST_F(StripeApiClientTest, TrialBoundariesWorkForCallbacksAndCoroutines) {
    http_client.set_response({ .status_code = 200, .body = R"({"id":"cs_boundary"})" });
    CreateCheckoutSessionRequest request;
    request.customer_id = "cus_existing";
    request.price_id = "price_monthly";
    for (const auto days : { 1, 730 }) {
        request.trial_period_days = days;
        const auto session = client->checkout().sessions().create_async(request).result();
        EXPECT_EQ(session.id, "cs_boundary");
        int calls = 0;
        client->checkout().sessions().create(
            request, {}, [&](CallbackResult<CheckoutSession> result) {
                ++calls;
                EXPECT_TRUE(std::holds_alternative<CheckoutSession>(result));
            });
        EXPECT_EQ(calls, 1);
        for (const auto& sent : http_client.get_recorded_requests()) {
            EXPECT_NE(sent.body.find("trial_period_days%5D=" + std::to_string(days)),
                      std::string::npos);
            EXPECT_NE(sent.body.find("customer=cus_existing"), std::string::npos);
            EXPECT_EQ(sent.body.find("customer_email"), std::string::npos);
        }
        http_client.clear_recorded_requests();
    }
    request.customer_id.clear();
    request.trial_period_days = 14;
    client->checkout().sessions().create_async(request).result();
    const auto unprefilled = http_client.get_recorded_requests().front();
    EXPECT_EQ(unprefilled.body.find("customer="), std::string::npos);
    EXPECT_EQ(unprefilled.body.find("customer_email="), std::string::npos);
    http_client.clear_recorded_requests();
    request.trial_period_days = -1;
    int calls = 0;
    client->checkout().sessions().create(request, {}, [&](CallbackResult<CheckoutSession> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
        EXPECT_THROW(std::rethrow_exception(std::get<std::exception_ptr>(result)),
                     ConfigurationException);
    });
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(http_client.get_recorded_requests().empty());
}

TEST_F(StripeApiClientTest, RetrieveCheckoutPreservesReferencesAndExpiryStates) {
    for (const auto* status : { "open", "complete", "expired" }) {
        http_client.set_response(
            { .status_code = 200,
              .body = std::string(R"({"id":"cs_saved","status":")") + status +
                      R"(","customer":{"id":"cus_saved"},"subscription":{"id":"sub_saved"},
                         "expires_at":1800000000})" });
        const auto session = client->checkout().sessions().retrieve_async("cs_saved").result();
        EXPECT_EQ(session.status, status);
        EXPECT_EQ(session.customer_id, "cus_saved");
        EXPECT_EQ(session.subscription_id, "sub_saved");
        EXPECT_EQ(session.expires_at_unix, 1800000000);
    }
    http_client.set_response(
        { .status_code = 404,
          .body = R"({"error":{"type":"invalid_request_error","message":"No such session"}})" });
    EXPECT_THROW(client->checkout().sessions().retrieve_async("cs_missing").result(),
                 StripeApiException);
}

TEST_F(StripeApiClientTest, CreateBillingPortalSessionCallbackSuccess) {
    http_client.set_response(HttpResponse{
        .status_code = 200,
        .headers = {},
        .body =
            R"({ "id": "bps_cb_1", "customer": "cus_123", "url": "https://billing.stripe.com/session/bps_cb_1" })" });

    bool portal_cb_invoked = false;
    client->billing_portal().sessions().create(
        CreateBillingPortalSessionRequest{ .customer_id = "cus_123",
                                           .return_url = "https://example.com/return" },
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
        EXPECT_NE(req.body.find("return_url=https%3A%2F%2Fexample.com%2Freturn"),
                  std::string::npos);

        return HttpResponse{ .status_code = 200, .headers = {}, .body = R"({
                "id": "bps_portal_1",
                "customer": "cus_123",
                "url": "https://billing.stripe.com/session/bps_portal_1",
                "return_url": "https://example.com/return",
                "created": 1700000000
            })" };
    });

    const CreateBillingPortalSessionRequest request{ .customer_id = "cus_123",
                                                     .return_url = "https://example.com/return" };

    auto task = client->billing_portal().sessions().create_async(request);
    const BillingPortalSession session = task.result();
    EXPECT_EQ(session.id, "bps_portal_1");
    EXPECT_EQ(session.customer_id, "cus_123");
    EXPECT_EQ(session.url, "https://billing.stripe.com/session/bps_portal_1");
}

TEST_F(StripeApiClientTest, ApiErrorThrowsStripeApiException) {
    http_client.set_response(HttpResponse{
        .status_code = 402, .headers = { { "request-id", "req_card_declined" } }, .body = R"({
            "error": {
                "type": "card_error",
                "code": "card_declined",
                "message": "Your card was declined."
            }
        })" });

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
    http_client.set_response(HttpResponse{ .status_code = 400, .headers = {}, .body = R"({
            "error": {
                "type": "invalid_request_error",
                "message": "Invalid request parameter"
            }
        })" });

    bool callback_invoked = false;
    client->customers().retrieve(
        "cus_test", RequestOptions{}, [&](CallbackResult<Customer> result) {
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

    EXPECT_THROW(
        {
            auto task = client->customers().retrieve_async("cus_123");
            task.result();
        },
        std::runtime_error);

    bool callback_invoked = false;
    http_client.set_exception(std::make_exception_ptr(std::runtime_error("Connection refused")));
    client->customers().retrieve("cus_123", RequestOptions{}, [&](CallbackResult<Customer> result) {
        callback_invoked = true;
        ASSERT_TRUE(std::holds_alternative<std::exception_ptr>(result));
        EXPECT_THROW(std::rethrow_exception(std::get<std::exception_ptr>(result)),
                     std::runtime_error);
    });
    EXPECT_TRUE(callback_invoked);
}
