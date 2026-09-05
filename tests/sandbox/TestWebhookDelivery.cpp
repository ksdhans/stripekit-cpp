/** TestWebhookDelivery.cpp
 *
 * Sandbox end-to-end tests for webhook delivery, replay, and recovery using
 * genuinely Stripe-signed payloads.
 *
 * Every payload and signature used here was produced by Stripe: each test
 * first makes a real API call and waits for the Stripe CLI to forward the
 * resulting live event (see deliver_customer_created below). Only the
 * duplicate/retry/tamper scenarios then re-post that captured delivery
 * directly, because Stripe's own redelivery (`stripe events resend`) happens
 * on Stripe's schedule and cannot be triggered deterministically inside a
 * test's timeout. Re-posting the exact bytes Stripe already sent exercises
 * the same signature and de-duplication code paths without that flakiness.
 */

#include "SandboxEnvironment.h"
#include "TestFramework.h"

#include <string>
#include <utility>

using namespace stripekit;
using namespace stripekit::testing;

namespace {

/**
 * Creates a customer and returns the delivery Stripe made for the resulting event.
 *
 * Replaying a captured delivery keeps the payload and its signature exactly as
 * Stripe produced them, so signature verification and de-duplication are
 * exercised for real rather than against locally forged data.
 */
std::pair<ReceivedEvent, CapturedDelivery> deliver_customer_created(SandboxEnvironment& environment,
                                                                    std::string_view label) {
    const StripeApiResult customer = environment.stripe_api().require_post("/v1/customers", {
        {"email", sandbox_email(environment, label)},
        {"metadata[stripekit_run_id]", environment.run_id()}
    });

    const auto event = environment.wait_for_event("customer.created", customer.id());
    if (!event) {
        FAIL() << "Stripe did not deliver customer.created for customer " << customer.id();
    }

    const auto delivery = environment.delivery_for(event->id);
    if (!delivery) {
        FAIL() << "The local endpoint did not capture a raw delivery for event " << event->id;
    }

    return {*event, *delivery};
}

} // namespace

class SandboxWebhookDeliveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        environment = &SandboxEnvironment::get();
    }

    SandboxEnvironment* environment = nullptr;
};

TEST_F(SandboxWebhookDeliveryTest, LiveDeliveryIsVerifiedAndDispatchedOnce) {
    const auto [event, delivery] = deliver_customer_created(*environment, "delivery");

    EXPECT_EQ(delivery.event_id, event.id);
    EXPECT_FALSE(delivery.signature_header.empty());
    EXPECT_EQ(environment->dispatch_count(event.id), 1);
    EXPECT_EQ(json_path(event.json(), "type").asString(), "customer.created");
}

TEST_F(SandboxWebhookDeliveryTest, ReplayedDeliveryIsAcknowledgedWithoutReprocessing) {
    const auto [event, delivery] = deliver_customer_created(*environment, "replay");

    EXPECT_EQ(environment->dispatch_count(event.id), 1);

    const int status = environment->post_webhook(SandboxEnvironment::main_webhook_path,
                                                 delivery.payload,
                                                 delivery.signature_header);

    EXPECT_EQ(status, 200);
    EXPECT_EQ(environment->dispatch_count(event.id), 1);
}

TEST_F(SandboxWebhookDeliveryTest, RetryableFailureIsRetriedAndThenRecovers) {
    const auto [event, delivery] = deliver_customer_created(*environment, "retry");

    const int first = environment->post_webhook(SandboxEnvironment::retry_webhook_path,
                                                delivery.payload,
                                                delivery.signature_header);
    EXPECT_EQ(first, 500);
    EXPECT_EQ(environment->retry_route_attempts(event.id), 1);

    const int second = environment->post_webhook(SandboxEnvironment::retry_webhook_path,
                                                 delivery.payload,
                                                 delivery.signature_header);
    EXPECT_EQ(second, 200);
    EXPECT_EQ(environment->retry_route_attempts(event.id), 2);

    const int third = environment->post_webhook(SandboxEnvironment::retry_webhook_path,
                                                delivery.payload,
                                                delivery.signature_header);
    EXPECT_EQ(third, 200);
    EXPECT_EQ(environment->retry_route_attempts(event.id), 2);
}

TEST_F(SandboxWebhookDeliveryTest, TamperedPayloadIsRejected) {
    const auto [event, delivery] = deliver_customer_created(*environment, "tampered");

    const int status = environment->post_webhook(SandboxEnvironment::main_webhook_path,
                                                 delivery.payload + " ",
                                                 delivery.signature_header);

    EXPECT_EQ(status, 400);
    EXPECT_EQ(environment->dispatch_count(event.id), 1);
}
