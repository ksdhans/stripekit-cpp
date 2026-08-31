/** TestModelsAndParser.cpp
 * 
 * Unit tests for Stripe data models and JSON response parsing.
 */

#include "TestFramework.h"
#include "StripeKit/Errors.h"
#include "StripeKit/HTTPClient.h"
#include "StripeKit/Models.h"
#include "StripeKit/StripeApiClient.h"

using namespace stripekit;

class ModelsAndParserTest : public ::testing::Test {
protected:
    JsonStripeResponseParser parser;
};

TEST_F(ModelsAndParserTest, ParseCustomerSuccess) {
    const HttpResponse response{
        .status_code = 200,
        .headers = {},
        .body = R"({
            "id": "cus_123456",
            "object": "customer",
            "email": "user@example.com",
            "name": "Jane Doe",
            "livemode": true,
            "metadata": {
                "user_id": "usr_789",
                "tier": "pro"
            }
        })"
    };

    const Customer customer = parser.parse_customer(response);
    EXPECT_EQ(customer.id, "cus_123456");
    EXPECT_EQ(customer.email, "user@example.com");
    EXPECT_EQ(customer.name, "Jane Doe");
    EXPECT_TRUE(customer.livemode);
    EXPECT_EQ(customer.metadata.size(), 2u);
    EXPECT_EQ(customer.metadata.at("user_id"), "usr_789");
    EXPECT_EQ(customer.metadata.at("tier"), "pro");
}

TEST_F(ModelsAndParserTest, ParseCustomerDefaultsOnMissingFields) {
    const HttpResponse response{
        .status_code = 200,
        .headers = {},
        .body = R"({ "id": "cus_minimal" })"
    };

    const Customer customer = parser.parse_customer(response);
    EXPECT_EQ(customer.id, "cus_minimal");
    EXPECT_EQ(customer.email, "");
    EXPECT_EQ(customer.name, "");
    EXPECT_FALSE(customer.livemode);
    EXPECT_TRUE(customer.metadata.empty());
}

TEST_F(ModelsAndParserTest, ParseCheckoutSessionSuccess) {
    const HttpResponse response{
        .status_code = 200,
        .headers = {},
        .body = R"({
            "id": "cs_test_123",
            "object": "checkout.session",
            "customer": "cus_456",
            "subscription": "sub_789",
            "status": "open",
            "mode": "subscription",
            "url": "https://checkout.stripe.com/c/pay/cs_test_123",
            "currency": "usd",
            "amount_total": 4999,
            "livemode": false,
            "metadata": {
                "order_ref": "ord_999"
            }
        })"
    };

    const CheckoutSession session = parser.parse_checkout_session(response);
    EXPECT_EQ(session.id, "cs_test_123");
    EXPECT_EQ(session.customer_id, "cus_456");
    EXPECT_EQ(session.subscription_id, "sub_789");
    EXPECT_EQ(session.status, "open");
    EXPECT_EQ(session.mode, "subscription");
    EXPECT_EQ(session.url, "https://checkout.stripe.com/c/pay/cs_test_123");
    EXPECT_EQ(session.currency, "usd");
    EXPECT_EQ(session.amount_total, 4999);
    EXPECT_FALSE(session.livemode);
    EXPECT_EQ(session.metadata.at("order_ref"), "ord_999");
}

TEST_F(ModelsAndParserTest, ParseSubscriptionSuccess) {
    const HttpResponse response{
        .status_code = 200,
        .headers = {},
        .body = R"({
            "id": "sub_123",
            "object": "subscription",
            "customer": "cus_456",
            "status": "active",
            "current_period_start": 1700000000,
            "current_period_end": 1702592000,
            "cancel_at_period_end": true,
            "livemode": true,
            "metadata": {
                "plan": "yearly"
            }
        })"
    };

    const Subscription sub = parser.parse_subscription(response);
    EXPECT_EQ(sub.id, "sub_123");
    EXPECT_EQ(sub.customer_id, "cus_456");
    EXPECT_EQ(sub.status, "active");
    EXPECT_EQ(sub.current_period_start_unix, 1700000000);
    EXPECT_EQ(sub.current_period_end_unix, 1702592000);
    EXPECT_TRUE(sub.cancel_at_period_end);
    EXPECT_TRUE(sub.livemode);
    EXPECT_EQ(sub.metadata.at("plan"), "yearly");
}

TEST_F(ModelsAndParserTest, ParseBillingPortalSessionSuccess) {
    const HttpResponse response{
        .status_code = 200,
        .headers = {},
        .body = R"({
            "id": "bps_123",
            "object": "billing_portal.session",
            "customer": "cus_456",
            "url": "https://billing.stripe.com/session/bps_123",
            "return_url": "https://example.com/account",
            "created": 1700000000,
            "livemode": false
        })"
    };

    const BillingPortalSession session = parser.parse_billing_portal_session(response);
    EXPECT_EQ(session.id, "bps_123");
    EXPECT_EQ(session.customer_id, "cus_456");
    EXPECT_EQ(session.url, "https://billing.stripe.com/session/bps_123");
    EXPECT_EQ(session.return_url, "https://example.com/account");
    EXPECT_EQ(session.created_unix, 1700000000);
    EXPECT_FALSE(session.livemode);
}

TEST_F(ModelsAndParserTest, ParseErrorDetails) {
    const HttpResponse response{
        .status_code = 400,
        .headers = {{"request-id", "req_err_123"}},
        .body = R"({
            "error": {
                "type": "invalid_request_error",
                "code": "parameter_missing",
                "message": "Missing required param: customer"
            }
        })"
    };

    const StripeErrorPayload error = parser.parse_error(response);
    EXPECT_EQ(error.type, StripeErrorType::InvalidRequest);
    EXPECT_EQ(error.code, "parameter_missing");
    EXPECT_EQ(error.message, "Missing required param: customer");
    EXPECT_EQ(error.http_status, 400);
    EXPECT_EQ(error.request_id, "req_err_123");
    EXPECT_FALSE(error.retryable);
}

TEST_F(ModelsAndParserTest, ParseErrorTypesAndRetryability) {
    auto test_error_type = [this](int status, const std::string& type_str, StripeErrorType expected_type, bool expected_retryable) {
        const HttpResponse resp{
            .status_code = status,
            .headers = {},
            .body = R"({ "error": { "type": ")" + type_str + R"(", "message": "error msg" } })"
        };
        const auto payload = parser.parse_error(resp);
        EXPECT_EQ(payload.type, expected_type);
        EXPECT_EQ(payload.retryable, expected_retryable);
    };

    test_error_type(402, "card_error", StripeErrorType::Card, false);
    test_error_type(401, "authentication_error", StripeErrorType::Authentication, false);
    test_error_type(403, "permission_error", StripeErrorType::Permission, false);
    test_error_type(409, "idempotency_error", StripeErrorType::Idempotency, true);
    test_error_type(429, "rate_limit_error", StripeErrorType::RateLimit, true);
    test_error_type(500, "api_error", StripeErrorType::Api, true);
    test_error_type(503, "api_connection_error", StripeErrorType::ApiConnection, true);
    test_error_type(418, "custom_unknown_error", StripeErrorType::Unknown, false);
}

TEST_F(ModelsAndParserTest, ParseInvalidJsonThrowsConfigurationException) {
    const HttpResponse bad_resp{
        .status_code = 200,
        .headers = {},
        .body = "{ invalid json..."
    };

    EXPECT_THROW({
        parser.parse_customer(bad_resp);
    }, ConfigurationException);

    EXPECT_THROW({
        parser.parse_checkout_session(bad_resp);
    }, ConfigurationException);

    EXPECT_THROW({
        parser.parse_subscription(bad_resp);
    }, ConfigurationException);

    EXPECT_THROW({
        parser.parse_billing_portal_session(bad_resp);
    }, ConfigurationException);

    EXPECT_THROW({
        parser.parse_error(bad_resp);
    }, ConfigurationException);
}
