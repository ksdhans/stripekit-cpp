/** TestWebhookProcessor.cpp
 *
 * Unit tests for webhook processor orchestration and event dispatching.
 */

#include "TestFramework.h"
#include "TestHelpers.h"

#include "StripeKit/Errors.h"
#include "StripeKit/WebhookProcessor.h"

#include <chrono>
#include <string>
#include <type_traits>

static_assert(std::is_same_v<stripekit::webhooks::ProcessResult, stripekit::EventDispatchResult>);

using namespace stripekit;
using namespace stripekit::testing;

class WebhookProcessorTest : public ::testing::Test {
protected:
    void SetUp() override {
        secret = "whsec_proc_test_secret_1234567890abcdef";
        now = std::chrono::system_clock::from_time_t(1700000000);
        timestamp_unix = 1700000000;

        payload = R"({
            "id": "evt_processor_1",
            "object": "event",
            "type": "customer.subscription.created",
            "api_version": "2026-10-28.endive",
            "created": 1700000000,
            "livemode": false,
            "request": {
                "id": "req_proc_abc"
            }
        })";

        valid_header = make_stripe_signature_header(timestamp_unix, payload, secret);
    }

    std::string secret;
    std::chrono::system_clock::time_point now;
    std::int64_t timestamp_unix = 0;
    std::string payload;
    std::string valid_header;
};

TEST_F(WebhookProcessorTest, ProcessValidDeliveryHandled) {
    InMemoryWebhookDedupStore store;
    MockEventDispatcher dispatcher;
    dispatcher.set_result(EventDispatchResult::Handled);
    TestLogger logger;

    WebhookProcessor processor(store, dispatcher, &logger);

    const WebhookVerificationInput input{ .payload = payload,
                                          .stripe_signature_header = valid_header,
                                          .endpoint_secret = secret,
                                          .now = now };

    const WebhookResponseAction action = processor.process(input, "deliv_123");
    EXPECT_EQ(action, WebhookResponseAction::AcknowledgeSuccess);

    const auto signals = dispatcher.get_recorded_signals();
    ASSERT_EQ(signals.size(), 1u);
    EXPECT_EQ(signals[0].envelope.id, "evt_processor_1");
    EXPECT_EQ(signals[0].envelope.type, "customer.subscription.created");
    EXPECT_EQ(signals[0].delivery_id, "deliv_123");
    EXPECT_EQ(signals[0].received_at, now);
}

TEST_F(WebhookProcessorTest, ProcessValidDeliveryIgnored) {
    InMemoryWebhookDedupStore store;
    MockEventDispatcher dispatcher;
    dispatcher.set_result(EventDispatchResult::Ignored);

    WebhookProcessor processor(store, dispatcher);

    const WebhookVerificationInput input{ .payload = payload,
                                          .stripe_signature_header = valid_header,
                                          .endpoint_secret = secret,
                                          .now = now };

    const WebhookResponseAction action = processor.process(input);
    EXPECT_EQ(action, WebhookResponseAction::AcknowledgeSuccess);
    EXPECT_EQ(dispatcher.get_recorded_signals().size(), 1u);
}

TEST_F(WebhookProcessorTest, ProcessValidDeliveryRetryableFailure) {
    InMemoryWebhookDedupStore store;
    MockEventDispatcher dispatcher;
    dispatcher.set_result(EventDispatchResult::RetryableFailure);

    WebhookProcessor processor(store, dispatcher);

    const WebhookVerificationInput input{ .payload = payload,
                                          .stripe_signature_header = valid_header,
                                          .endpoint_secret = secret,
                                          .now = now };

    const WebhookResponseAction action = processor.process(input);
    EXPECT_EQ(action, WebhookResponseAction::RetryLater);
}

TEST_F(WebhookProcessorTest, ProcessValidDeliveryFatalFailure) {
    InMemoryWebhookDedupStore store;
    MockEventDispatcher dispatcher;
    dispatcher.set_result(EventDispatchResult::FatalFailure);

    WebhookProcessor processor(store, dispatcher);

    const WebhookVerificationInput input{ .payload = payload,
                                          .stripe_signature_header = valid_header,
                                          .endpoint_secret = secret,
                                          .now = now };

    const WebhookResponseAction action = processor.process(input);
    EXPECT_EQ(action, WebhookResponseAction::RetryLater);
}

TEST_F(WebhookProcessorTest, ProcessDuplicateDeliveryDoesNotDispatch) {
    InMemoryWebhookDedupStore store;
    MockEventDispatcher dispatcher;
    dispatcher.set_result(EventDispatchResult::Handled);

    WebhookProcessor processor(store, dispatcher);

    const WebhookVerificationInput input{ .payload = payload,
                                          .stripe_signature_header = valid_header,
                                          .endpoint_secret = secret,
                                          .now = now };

    // First call: handled and marked processed
    EXPECT_EQ(processor.process(input), WebhookResponseAction::AcknowledgeSuccess);
    EXPECT_EQ(dispatcher.get_recorded_signals().size(), 1u);

    // Second call: duplicate delivery returns success without dispatching
    EXPECT_EQ(processor.process(input), WebhookResponseAction::AcknowledgeSuccess);
    EXPECT_EQ(dispatcher.get_recorded_signals().size(), 1u);
}

TEST_F(WebhookProcessorTest, ProcessInvalidSignatureThrows) {
    InMemoryWebhookDedupStore store;
    MockEventDispatcher dispatcher;

    WebhookProcessor processor(store, dispatcher);

    const WebhookVerificationInput input{ .payload = payload,
                                          .stripe_signature_header =
                                              "t=1700000000,v1=invalid_signature",
                                          .endpoint_secret = secret,
                                          .now = now };

    EXPECT_THROW({ processor.process(input); }, SignatureVerificationException);

    EXPECT_TRUE(dispatcher.get_recorded_signals().empty());
}
