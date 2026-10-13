/** TestDrogonWebhookEndpoint.cpp
 *
 * Integration tests for the Drogon webhook endpoint adapter, signature verification,
 * duplicate handling, error recovery, and malicious attack rejection.
 */

#include "IntegrationTestServer.h"
#include "TestFramework.h"

#include "StripeKit/Adapters/Drogon.h"
#include "StripeKit/Errors.h"
#include "StripeKit/Webhooks.h"

#include <chrono>
#include <string>

using namespace stripekit;
using namespace stripekit::testing;

class DrogonWebhookEndpointIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        DrogonIntegrationEnvironment::get().start();
        DrogonIntegrationEnvironment::get().telemetry.reset();
        DrogonIntegrationEnvironment::get().webhook_logger.clear();
        webhook_secret = DrogonIntegrationEnvironment::get().webhook_secret;
        now_unix = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
    }

    std::string webhook_secret;
    std::int64_t now_unix = 0;

    std::string make_event_payload(const std::string& event_id, const std::string& type) {
        return R"({
            "id": ")" +
               event_id + R"(",
            "object": "event",
            "type": ")" +
               type + R"(",
            "api_version": "2024-06-20",
            "created": )" +
               std::to_string(now_unix) + R"(,
            "livemode": false,
            "request": {
                "id": "req_hook_123"
            }
        })";
    }

    std::string make_legacy_event_payload(const std::string& event_id,
                                          const std::string& type,
                                          const std::string& request_id) {
        return R"({
            "id": ")" +
               event_id + R"(",
            "object": "event",
            "type": ")" +
               type + R"(",
            "api_version": "2017-02-14",
            "created": )" +
               std::to_string(now_unix) + R"(,
            "livemode": false,
            "request": ")" +
               request_id + R"("
        })";
    }
};

TEST_F(DrogonWebhookEndpointIntegrationTest, ValidInvoicePaidReturns200AndInvokesHandler) {
    const std::string payload = make_event_payload("evt_int_paid_1", "invoice.paid");
    const std::string sig_header = make_stripe_signature_header(now_unix, payload, webhook_secret);

    auto [result, resp] = DrogonIntegrationEnvironment::get().send_raw_post(
        "/test/webhook/standard",
        payload,
        { { "stripe-signature", sig_header }, { "Content-Type", "application/json" } });

    EXPECT_EQ(result, ::drogon::ReqResult::Ok);
    ASSERT_TRUE(resp != nullptr);
    EXPECT_EQ(resp->statusCode(), ::drogon::k200OK);
    EXPECT_EQ(DrogonIntegrationEnvironment::get().telemetry.handled_events_count.load(), 1);
}

TEST_F(DrogonWebhookEndpointIntegrationTest, ValidIgnoredEventReturns200AndInvokesHandler) {
    const std::string payload = make_event_payload("evt_int_cust_1", "customer.created");
    const std::string sig_header = make_stripe_signature_header(now_unix, payload, webhook_secret);

    auto [result, resp] = DrogonIntegrationEnvironment::get().send_raw_post(
        "/test/webhook/standard",
        payload,
        { { "stripe-signature", sig_header }, { "Content-Type", "application/json" } });

    EXPECT_EQ(result, ::drogon::ReqResult::Ok);
    ASSERT_TRUE(resp != nullptr);
    EXPECT_EQ(resp->statusCode(), ::drogon::k200OK);
    EXPECT_EQ(DrogonIntegrationEnvironment::get().telemetry.ignored_events_count.load(), 1);
}

// The standard handler suspends on Drogon's event loop for this event type.
// A successful response verifies the webhook route awaits coroutine handlers
// instead of blocking on their tasks.
TEST_F(DrogonWebhookEndpointIntegrationTest, CoroutineHandlerCanSuspendOnDrogonEventLoop) {
    const std::string payload = make_event_payload("evt_int_async_1", "invoice.async_test");
    const std::string signature = make_stripe_signature_header(now_unix, payload, webhook_secret);

    auto [result, response] = DrogonIntegrationEnvironment::get().send_raw_post(
        "/test/webhook/standard", payload, { { "stripe-signature", signature } });

    EXPECT_EQ(result, ::drogon::ReqResult::Ok);
    ASSERT_TRUE(response != nullptr);
    EXPECT_EQ(response->statusCode(), ::drogon::k200OK);
    EXPECT_EQ(DrogonIntegrationEnvironment::get().telemetry.handled_events_count.load(), 1);
}

TEST_F(DrogonWebhookEndpointIntegrationTest, LegacyStringRequestIdReturns200AndRecordsRequestId) {
    ASSERT_TRUE(DrogonIntegrationEnvironment::get().class_endpoint_dispatcher != nullptr);
    DrogonIntegrationEnvironment::get().class_endpoint_dispatcher->clear();
    DrogonIntegrationEnvironment::get().class_endpoint_dispatcher->set_result(
        EventDispatchResult::Handled);

    const std::string payload = make_legacy_event_payload(
        "evt_legacy_string_request_1", "charge.refunded", "req_legacy_string_123");
    const std::string sig_header = make_stripe_signature_header(now_unix, payload, webhook_secret);

    auto [result, resp] = DrogonIntegrationEnvironment::get().send_raw_post(
        "/test/webhook/class-endpoint", payload, { { "stripe-signature", sig_header } });

    EXPECT_EQ(result, ::drogon::ReqResult::Ok);
    ASSERT_TRUE(resp != nullptr);
    EXPECT_EQ(resp->statusCode(), ::drogon::k200OK);

    const auto signals =
        DrogonIntegrationEnvironment::get().class_endpoint_dispatcher->get_recorded_signals();
    ASSERT_EQ(signals.size(), 1u);
    EXPECT_EQ(signals[0].envelope.id, "evt_legacy_string_request_1");
    EXPECT_EQ(signals[0].envelope.type, "charge.refunded");
    EXPECT_EQ(signals[0].envelope.request_id, "req_legacy_string_123");
}

TEST_F(DrogonWebhookEndpointIntegrationTest,
       ReplayedProcessedWebhookReturns200WithoutReexecutingHandler) {
    const std::string payload = make_event_payload("evt_int_dedup_1", "invoice.paid");
    const std::string sig_header = make_stripe_signature_header(now_unix, payload, webhook_secret);

    // First delivery
    auto [result1, resp1] = DrogonIntegrationEnvironment::get().send_raw_post(
        "/test/webhook/standard", payload, { { "stripe-signature", sig_header } });
    EXPECT_EQ(result1, ::drogon::ReqResult::Ok);
    EXPECT_EQ(resp1->statusCode(), ::drogon::k200OK);
    EXPECT_EQ(DrogonIntegrationEnvironment::get().telemetry.handled_events_count.load(), 1);

    // Second delivery (duplicate re-transmission by Stripe)
    auto [result2, resp2] = DrogonIntegrationEnvironment::get().send_raw_post(
        "/test/webhook/standard", payload, { { "stripe-signature", sig_header } });
    EXPECT_EQ(result2, ::drogon::ReqResult::Ok);
    EXPECT_EQ(resp2->statusCode(), ::drogon::k200OK);
    // Handler count MUST remain 1 because it was de-duplicated!
    EXPECT_EQ(DrogonIntegrationEnvironment::get().telemetry.handled_events_count.load(), 1);
}

TEST_F(DrogonWebhookEndpointIntegrationTest,
       HandlerRetryableFailureReturns500ToTriggerStripeRetry) {
    const std::string payload = make_event_payload("evt_int_retryable_1", "invoice.payment_failed");
    const std::string sig_header = make_stripe_signature_header(now_unix, payload, webhook_secret);

    auto [result, resp] = DrogonIntegrationEnvironment::get().send_raw_post(
        "/test/webhook/retryable", payload, { { "stripe-signature", sig_header } });

    EXPECT_EQ(result, ::drogon::ReqResult::Ok);
    ASSERT_TRUE(resp != nullptr);
    EXPECT_EQ(resp->statusCode(), ::drogon::k500InternalServerError);
}

TEST_F(DrogonWebhookEndpointIntegrationTest, HandlerFatalFailureReturns500) {
    const std::string payload = make_event_payload("evt_int_fatal_1", "invoice.payment_failed");
    const std::string sig_header = make_stripe_signature_header(now_unix, payload, webhook_secret);

    auto [result, resp] = DrogonIntegrationEnvironment::get().send_raw_post(
        "/test/webhook/fatal", payload, { { "stripe-signature", sig_header } });

    EXPECT_EQ(result, ::drogon::ReqResult::Ok);
    ASSERT_TRUE(resp != nullptr);
    EXPECT_EQ(resp->statusCode(), ::drogon::k500InternalServerError);
}

TEST_F(DrogonWebhookEndpointIntegrationTest, HandlerExceptionReturns500) {
    const std::string payload = make_event_payload("evt_int_throwing_1", "invoice.paid");
    const std::string sig_header = make_stripe_signature_header(now_unix, payload, webhook_secret);

    auto [result, resp] = DrogonIntegrationEnvironment::get().send_raw_post(
        "/test/webhook/throwing", payload, { { "stripe-signature", sig_header } });

    EXPECT_EQ(result, ::drogon::ReqResult::Ok);
    ASSERT_TRUE(resp != nullptr);
    EXPECT_EQ(resp->statusCode(), ::drogon::k500InternalServerError);
}

TEST_F(DrogonWebhookEndpointIntegrationTest, FailedDeliveriesCanRecoverWithoutStaleTimeout) {
    auto& environment = DrogonIntegrationEnvironment::get();
    for (const std::string failure : { "retryable", "fatal", "throwing" }) {
        const auto payload = make_event_payload("evt_recover_" + failure, "invoice.paid");
        const auto signature = make_stripe_signature_header(now_unix, payload, webhook_secret);
        auto [failed_result, failed_response] = environment.send_raw_post(
            "/test/webhook/" + failure, payload, { { "stripe-signature", signature } });
        EXPECT_EQ(failed_result, ::drogon::ReqResult::Ok);
        ASSERT_TRUE(failed_response != nullptr);
        EXPECT_EQ(failed_response->statusCode(), ::drogon::k500InternalServerError);

        // The standard route shares the store and represents the recovered handler.
        auto [retry_result, retry_response] = environment.send_raw_post(
            "/test/webhook/standard", payload, { { "stripe-signature", signature } });
        EXPECT_EQ(retry_result, ::drogon::ReqResult::Ok);
        ASSERT_TRUE(retry_response != nullptr);
        EXPECT_EQ(retry_response->statusCode(), ::drogon::k200OK);

        auto [duplicate_result, duplicate_response] = environment.send_raw_post(
            "/test/webhook/standard", payload, { { "stripe-signature", signature } });
        EXPECT_EQ(duplicate_result, ::drogon::ReqResult::Ok);
        ASSERT_TRUE(duplicate_response != nullptr);
        EXPECT_EQ(duplicate_response->statusCode(), ::drogon::k200OK);
    }
    EXPECT_EQ(environment.telemetry.handled_events_count.load(), 3);
}

TEST_F(DrogonWebhookEndpointIntegrationTest, HandlerExceptionLogRedactsSecretTokens) {
    const std::string payload = make_event_payload("evt_int_secret_throwing_1", "invoice.paid");
    const std::string sig_header = make_stripe_signature_header(now_unix, payload, webhook_secret);

    auto [result, resp] = DrogonIntegrationEnvironment::get().send_raw_post(
        "/test/webhook/throwing-secret", payload, { { "stripe-signature", sig_header } });

    EXPECT_EQ(result, ::drogon::ReqResult::Ok);
    ASSERT_TRUE(resp != nullptr);
    EXPECT_EQ(resp->statusCode(), ::drogon::k500InternalServerError);

    const auto entries = DrogonIntegrationEnvironment::get().webhook_logger.get_entries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].level, LogLevel::Error);
    EXPECT_TRUE(entries[0].message.find("whsec_...") != std::string::npos);
    EXPECT_TRUE(entries[0].message.find("sk_test_...") != std::string::npos);
    EXPECT_TRUE(entries[0].message.find("Bearer <redacted>") != std::string::npos);
    EXPECT_TRUE(entries[0].message.find("whsec_handler_secret") == std::string::npos);
    EXPECT_TRUE(entries[0].message.find("sk_test_handler_key") == std::string::npos);
    EXPECT_TRUE(entries[0].message.find("handler-token") == std::string::npos);
}

TEST_F(DrogonWebhookEndpointIntegrationTest, HackerForgedSignatureWithFakeSecretRejectedWith400) {
    const std::string payload = make_event_payload("evt_hacker_1", "invoice.paid");
    const std::string hacker_fake_secret = "whsec_attacker_controlled_secret_key_999";
    const std::string forged_signature =
        make_stripe_signature_header(now_unix, payload, hacker_fake_secret);

    auto [result, resp] = DrogonIntegrationEnvironment::get().send_raw_post(
        "/test/webhook/standard", payload, { { "stripe-signature", forged_signature } });

    EXPECT_EQ(result, ::drogon::ReqResult::Ok);
    ASSERT_TRUE(resp != nullptr);
    EXPECT_EQ(resp->statusCode(), ::drogon::k400BadRequest);
    EXPECT_EQ(DrogonIntegrationEnvironment::get().telemetry.handled_events_count.load(), 0);
}

TEST_F(DrogonWebhookEndpointIntegrationTest, HackerTamperedBodyRejectedWith400) {
    const std::string original_payload = make_event_payload("evt_tamper_1", "invoice.paid");
    const std::string valid_sig_for_original =
        make_stripe_signature_header(now_unix, original_payload, webhook_secret);

    // Attacker alters payload (e.g. changing amount or customer ID) while sending original
    // signature
    const std::string tampered_payload = make_event_payload("evt_tamper_1", "charge.refunded");

    auto [result, resp] = DrogonIntegrationEnvironment::get().send_raw_post(
        "/test/webhook/standard",
        tampered_payload,
        { { "stripe-signature", valid_sig_for_original } });

    EXPECT_EQ(result, ::drogon::ReqResult::Ok);
    ASSERT_TRUE(resp != nullptr);
    EXPECT_EQ(resp->statusCode(), ::drogon::k400BadRequest);
    EXPECT_EQ(DrogonIntegrationEnvironment::get().telemetry.handled_events_count.load(), 0);
}

TEST_F(DrogonWebhookEndpointIntegrationTest, HackerReplayAttackExpiredTimestampRejectedWith400) {
    const std::string payload = make_event_payload("evt_replay_attack_1", "invoice.paid");
    // Timestamp from 1 hour ago (3600 seconds)
    const std::int64_t expired_timestamp = now_unix - 3600;
    const std::string old_signature =
        make_stripe_signature_header(expired_timestamp, payload, webhook_secret);

    auto [result, resp] = DrogonIntegrationEnvironment::get().send_raw_post(
        "/test/webhook/standard", payload, { { "stripe-signature", old_signature } });

    EXPECT_EQ(result, ::drogon::ReqResult::Ok);
    ASSERT_TRUE(resp != nullptr);
    EXPECT_EQ(resp->statusCode(), ::drogon::k400BadRequest);
    EXPECT_EQ(DrogonIntegrationEnvironment::get().telemetry.handled_events_count.load(), 0);
}

TEST_F(DrogonWebhookEndpointIntegrationTest, HackerFutureTimestampReplayRejectedWith400) {
    const std::string payload = make_event_payload("evt_future_attack_1", "invoice.paid");
    // Timestamp 1 hour into the future
    const std::int64_t future_timestamp = now_unix + 3600;
    const std::string future_signature =
        make_stripe_signature_header(future_timestamp, payload, webhook_secret);

    auto [result, resp] = DrogonIntegrationEnvironment::get().send_raw_post(
        "/test/webhook/standard", payload, { { "stripe-signature", future_signature } });

    EXPECT_EQ(result, ::drogon::ReqResult::Ok);
    ASSERT_TRUE(resp != nullptr);
    EXPECT_EQ(resp->statusCode(), ::drogon::k400BadRequest);
    EXPECT_EQ(DrogonIntegrationEnvironment::get().telemetry.handled_events_count.load(), 0);
}

TEST_F(DrogonWebhookEndpointIntegrationTest, HackerInjectionInHeaderRejectedWith400) {
    const std::string payload = make_event_payload("evt_header_inject_1", "invoice.paid");
    const std::string malformed_header = "t=999999999999999999999999999999999999,v1=' OR '1'='1";

    auto [result, resp] = DrogonIntegrationEnvironment::get().send_raw_post(
        "/test/webhook/standard", payload, { { "stripe-signature", malformed_header } });

    EXPECT_EQ(result, ::drogon::ReqResult::Ok);
    ASSERT_TRUE(resp != nullptr);
    EXPECT_EQ(resp->statusCode(), ::drogon::k400BadRequest);
    EXPECT_EQ(DrogonIntegrationEnvironment::get().telemetry.handled_events_count.load(), 0);
}

TEST_F(DrogonWebhookEndpointIntegrationTest, HackerTruncatedSignatureRejectedWith400) {
    const std::string payload = make_event_payload("evt_trunc_1", "invoice.paid");
    const std::string valid_sig = make_stripe_signature_header(now_unix, payload, webhook_secret);
    // Truncate signature to test constant-time comparison safety against partial prefix match
    const std::string truncated_sig = valid_sig.substr(0, valid_sig.size() - 8);

    auto [result, resp] = DrogonIntegrationEnvironment::get().send_raw_post(
        "/test/webhook/standard", payload, { { "stripe-signature", truncated_sig } });

    EXPECT_EQ(result, ::drogon::ReqResult::Ok);
    ASSERT_TRUE(resp != nullptr);
    EXPECT_EQ(resp->statusCode(), ::drogon::k400BadRequest);
    EXPECT_EQ(DrogonIntegrationEnvironment::get().telemetry.handled_events_count.load(), 0);
}

TEST_F(DrogonWebhookEndpointIntegrationTest, MissingSignatureHeaderRejectedWith400) {
    const std::string payload = make_event_payload("evt_no_sig_1", "invoice.paid");

    auto [result, resp] =
        DrogonIntegrationEnvironment::get().send_raw_post("/test/webhook/standard", payload, {});

    EXPECT_EQ(result, ::drogon::ReqResult::Ok);
    ASSERT_TRUE(resp != nullptr);
    EXPECT_EQ(resp->statusCode(), ::drogon::k400BadRequest);
    EXPECT_EQ(DrogonIntegrationEnvironment::get().telemetry.handled_events_count.load(), 0);
}

TEST_F(DrogonWebhookEndpointIntegrationTest, CorruptNonJsonPayloadWithValidHmacRejectedWith400) {
    const std::string non_json_body = "This is not valid JSON at all!";
    const std::string valid_hmac_header =
        make_stripe_signature_header(now_unix, non_json_body, webhook_secret);

    auto [result, resp] = DrogonIntegrationEnvironment::get().send_raw_post(
        "/test/webhook/standard", non_json_body, { { "stripe-signature", valid_hmac_header } });

    EXPECT_EQ(result, ::drogon::ReqResult::Ok);
    ASSERT_TRUE(resp != nullptr);
    EXPECT_EQ(resp->statusCode(), ::drogon::k400BadRequest);
    EXPECT_EQ(DrogonIntegrationEnvironment::get().telemetry.handled_events_count.load(), 0);
}

TEST_F(DrogonWebhookEndpointIntegrationTest, PayloadMissingEventIdRejectedWith400) {
    const std::string payload_missing_id = R"({ "object": "event", "type": "invoice.paid" })";
    const std::string valid_sig =
        make_stripe_signature_header(now_unix, payload_missing_id, webhook_secret);

    auto [result, resp] = DrogonIntegrationEnvironment::get().send_raw_post(
        "/test/webhook/standard", payload_missing_id, { { "stripe-signature", valid_sig } });

    EXPECT_EQ(result, ::drogon::ReqResult::Ok);
    ASSERT_TRUE(resp != nullptr);
    EXPECT_EQ(resp->statusCode(), ::drogon::k400BadRequest);
    EXPECT_EQ(DrogonIntegrationEnvironment::get().telemetry.handled_events_count.load(), 0);
}

TEST_F(DrogonWebhookEndpointIntegrationTest, GetMethodOnWebhookEndpointReturns405) {
    auto [result, resp] =
        DrogonIntegrationEnvironment::get().send_raw_get("/test/webhook/standard");
    EXPECT_EQ(result, ::drogon::ReqResult::Ok);
    ASSERT_TRUE(resp != nullptr);
    EXPECT_EQ(resp->statusCode(), ::drogon::k405MethodNotAllowed);
}

TEST_F(DrogonWebhookEndpointIntegrationTest, WebhookEndpointClassWrapperDispatchesCorrectly) {
    ASSERT_TRUE(DrogonIntegrationEnvironment::get().class_endpoint_dispatcher != nullptr);
    DrogonIntegrationEnvironment::get().class_endpoint_dispatcher->clear();
    DrogonIntegrationEnvironment::get().class_endpoint_dispatcher->set_result(
        EventDispatchResult::Handled);

    const std::string payload = make_event_payload("evt_class_endpoint_1", "invoice.paid");
    const std::string sig_header = make_stripe_signature_header(now_unix, payload, webhook_secret);

    auto [result, resp] = DrogonIntegrationEnvironment::get().send_raw_post(
        "/test/webhook/class-endpoint",
        payload,
        { { "stripe-signature", sig_header }, { "stripe-webhook-id", "wh_deliv_789" } });

    EXPECT_EQ(result, ::drogon::ReqResult::Ok);
    ASSERT_TRUE(resp != nullptr);
    EXPECT_EQ(resp->statusCode(), ::drogon::k200OK);

    const auto signals =
        DrogonIntegrationEnvironment::get().class_endpoint_dispatcher->get_recorded_signals();
    ASSERT_EQ(signals.size(), 1u);
    EXPECT_EQ(signals[0].envelope.id, "evt_class_endpoint_1");
    EXPECT_EQ(signals[0].envelope.type, "invoice.paid");
    EXPECT_EQ(signals[0].delivery_id, "wh_deliv_789");
}
