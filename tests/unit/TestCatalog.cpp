/** TestCatalog.cpp
 *
 * Tests for catalog requests, models, and failures without contacting Stripe.
 */

#include "TestFramework.h"
#include "TestHelpers.h"
#include "StripeKit/StripeApiClient.h"

using namespace stripekit;
using namespace stripekit::testing;

namespace {

const std::string product_json = R"({
    "object":"product","id":"prod_test","name":"Standard & More",
    "description":null,"active":true,"livemode":false,"metadata":{"plan":"standard"}
})";
const std::string price_json = R"({
    "object":"price","id":"price_test","product":"prod_test","currency":"usd",
    "unit_amount":2500,"unit_amount_decimal":"2500","type":"recurring",
    "billing_scheme":"per_unit","tax_behavior":"unspecified","active":true,
    "livemode":false,"lookup_key":"standard_monthly",
    "recurring":{"interval":"month","interval_count":2,"usage_type":"licensed"},
    "transform_quantity":null,"custom_unit_amount":null,"metadata":{"version":"one"}
})";

/** Replace a fixture fragment, failing explicitly if the fixture changed. */
std::string replacing(std::string text, const std::string& from, const std::string& to) {
    const auto position = text.find(from);
    if (position == std::string::npos) throw std::runtime_error("Missing fixture fragment");
    text.replace(position, from.size(), to);
    return text;
}

/** A realistic price creation request with explicit billing terms. */
CreatePriceRequest price_request() {
    return { .product_id = "prod_test",
             .currency = "usd",
             .unit_amount = 2500,
             .recurring_interval = "month",
             .recurring_interval_count = 2,
             .lookup_key = "standard & monthly",
             .tax_behavior = "exclusive",
             .metadata = { { "version", "one" } } };
}

} // namespace

class CatalogTest : public ::testing::Test {
protected:
    MockHttpClient http;
    StripeApiClient client{ http };
};

TEST_F(CatalogTest, ProductCreateEncodesFieldsAndOptions) {
    http.set_response({ .status_code = 200, .body = product_json });
    const CreateProductRequest request{ .id = "stable_id",
                                        .name = "Standard & More",
                                        .description = "A=B + C",
                                        .active = false,
                                        .metadata = { { "plan", "standard" } } };
    const RequestOptions options{ .idempotency_key = "product-key",
                                  .timeout = std::chrono::seconds(4),
                                  .headers = { { "Stripe-Version", "2024-06-20" } } };
    auto task = client.products().create_async(request, options);
    const auto result = task.result();
    EXPECT_EQ(result.id, "prod_test");
    EXPECT_EQ(result.metadata.at("plan"), "standard");
    const auto sent = http.get_recorded_requests().at(0);
    EXPECT_EQ(sent.path, "/v1/products");
    EXPECT_EQ(sent.method, HttpMethod::Post);
    EXPECT_NE(sent.body.find("name=Standard%20%26%20More"), std::string::npos);
    EXPECT_NE(sent.body.find("description=A%3DB%20%2B%20C"), std::string::npos);
    EXPECT_NE(sent.body.find("id=stable_id"), std::string::npos);
    EXPECT_NE(sent.body.find("active=false"), std::string::npos);
    EXPECT_NE(sent.body.find("metadata%5Bplan%5D=standard"), std::string::npos);
    EXPECT_EQ(sent.headers.at("Idempotency-Key"), "product-key");
    EXPECT_EQ(sent.headers.at("Stripe-Version"), "2024-06-20");
    EXPECT_EQ(sent.options.timeout, options.timeout);
}

TEST_F(CatalogTest, ProductUpdateDistinguishesUnsetEmptyAndFalse) {
    http.set_response({ .status_code = 200, .body = product_json });
    UpdateProductRequest request;
    request.description = "";
    request.active = false;
    request.metadata["remove"] = "";
    auto task = client.products().update_async("prod/a?b", request);
    task.result();
    const auto sent = http.get_recorded_requests().at(0);
    EXPECT_EQ(sent.path, "/v1/products/prod%2Fa%3Fb");
    EXPECT_EQ(sent.method, HttpMethod::Post);
    EXPECT_NE(sent.body.find("description="), std::string::npos);
    EXPECT_NE(sent.body.find("active=false"), std::string::npos);
    EXPECT_NE(sent.body.find("metadata%5Bremove%5D="), std::string::npos);
    EXPECT_EQ(sent.body.find("name="), std::string::npos);
    request.name = "Updated";
    auto renamed = client.products().update_async("prod_test", request);
    renamed.result();
    EXPECT_NE(http.get_recorded_requests().back().body.find("name=Updated"), std::string::npos);
}

TEST_F(CatalogTest, ProductRetrieveHandlesDeletedObjects) {
    http.set_response({ .status_code = 200,
                        .body = R"({"object":"product","id":"prod_deleted","deleted":true})" });
    auto task = client.products().retrieve_async("prod_deleted");
    const auto result = task.result();
    EXPECT_TRUE(result.deleted);
    EXPECT_FALSE(result.active);
    EXPECT_EQ(result.id, "prod_deleted");
    EXPECT_EQ(http.get_recorded_requests().at(0).method, HttpMethod::Get);
}

TEST_F(CatalogTest, PriceCreateEncodesRecurrenceLookupAndIdempotency) {
    http.set_response({ .status_code = 200, .body = price_json });
    const auto request = price_request();
    auto task = client.prices().create_async(request, { .idempotency_key = "price-key" });
    const auto result = task.result();
    EXPECT_EQ(result.unit_amount.value(), 2500);
    EXPECT_EQ(result.recurring->interval_count, 2);
    EXPECT_EQ(result.metadata.at("version"), "one");
    const auto sent = http.get_recorded_requests().at(0);
    EXPECT_EQ(sent.path, "/v1/prices");
    EXPECT_EQ(sent.method, HttpMethod::Post);
    EXPECT_EQ(sent.headers.at("Content-Type"), "application/x-www-form-urlencoded");
    EXPECT_EQ(sent.headers.at("Idempotency-Key"), "price-key");
    EXPECT_NE(sent.body.find("unit_amount=2500"), std::string::npos);
    EXPECT_NE(sent.body.find("recurring%5Binterval%5D=month"), std::string::npos);
    EXPECT_NE(sent.body.find("recurring%5Binterval_count%5D=2"), std::string::npos);
    EXPECT_NE(sent.body.find("recurring%5Busage_type%5D=licensed"), std::string::npos);
    EXPECT_NE(sent.body.find("lookup_key=standard%20%26%20monthly"), std::string::npos);
    EXPECT_NE(sent.body.find("tax_behavior=exclusive"), std::string::npos);
}

TEST_F(CatalogTest, OneTimePriceOmitsRecurrenceAndSupportsZero) {
    http.set_response(
        { .status_code = 200,
          .body = replacing(
              replacing(price_json, R"("type":"recurring")", R"("type":"one_time")"),
              R"("recurring":{"interval":"month","interval_count":2,"usage_type":"licensed"})",
              R"("recurring":null)") });
    const CreatePriceRequest request{ .product_id = "prod_test", .currency = "usd" };
    auto task = client.prices().create_async(request);
    EXPECT_FALSE(task.result().recurring.has_value());
    const auto body = http.get_recorded_requests().at(0).body;
    EXPECT_EQ(body.find("recurring"), std::string::npos);
    EXPECT_NE(body.find("unit_amount=0"), std::string::npos);
}

TEST_F(CatalogTest, PriceListEncodesLookupFiltersAndPagination) {
    http.set_response(
        { .status_code = 200,
          .body = R"({"object":"list","has_more":true,"data":[)" + price_json + "]}" });
    ListPricesRequest request;
    request.product_id = "prod_test";
    request.active = false;
    request.lookup_keys = { "first & key", "second/key" };
    request.limit = 2;
    request.starting_after = "price+old";
    auto task = client.prices().list_async(request);
    const auto result = task.result();
    EXPECT_TRUE(result.has_more);
    EXPECT_EQ(result.data.size(), 1u);
    EXPECT_EQ(result.data.at(0).id, "price_test");
    const auto sent = http.get_recorded_requests().at(0);
    EXPECT_EQ(sent.method, HttpMethod::Get);
    EXPECT_TRUE(sent.body.empty());
    EXPECT_EQ(sent.path,
              "/v1/prices?limit=2&product=prod_test&active=false&starting_after=price%2Bold"
              "&lookup_keys%5B%5D=first%20%26%20key&lookup_keys%5B%5D=second%2Fkey");
}

TEST_F(CatalogTest, PriceListDefaultsDoNotExcludeArchivedPrices) {
    http.set_response(
        { .status_code = 200, .body = R"({"object":"list","has_more":false,"data":[]})" });
    auto task = client.prices().list_async();
    EXPECT_TRUE(task.result().data.empty());
    EXPECT_EQ(http.get_recorded_requests().at(0).path, "/v1/prices?limit=10");
}

TEST_F(CatalogTest, AllCatalogCallbacksReturnParsedResultsOnce) {
    http.set_response({ .status_code = 200, .body = product_json });
    int products = 0;
    const auto product = [&](CallbackResult<Product> result) {
        ++products;
        EXPECT_EQ(std::get<Product>(result).id, "prod_test");
    };
    client.products().create({ .name = "Standard" }, {}, product);
    client.products().retrieve("prod_test", {}, product);
    client.products().update("prod_test", { .name = "Updated" }, {}, product);
    EXPECT_EQ(products, 3);
    http.set_response({ .status_code = 200, .body = price_json });
    int prices = 0;
    const auto price = [&](CallbackResult<Price> result) {
        ++prices;
        EXPECT_EQ(std::get<Price>(result).id, "price_test");
    };
    client.prices().create(price_request(), {}, price);
    client.prices().retrieve("price_test", {}, price);
    EXPECT_EQ(prices, 2);
    http.set_response(
        { .status_code = 200, .body = R"({"object":"list","has_more":false,"data":[]})" });
    int pages = 0;
    client.prices().list({}, {}, [&](CallbackResult<PriceList> result) {
        ++pages;
        EXPECT_TRUE(std::get<PriceList>(result).data.empty());
    });
    EXPECT_EQ(pages, 1);
}

TEST_F(CatalogTest, InvalidInputsFailWithoutNetworkCalls) {
    auto invalid = price_request();
    invalid.unit_amount = -1;
    EXPECT_THROW(client.prices().create_async(invalid).result(), ConfigurationException);
    invalid = price_request();
    invalid.recurring_interval_count = 0;
    EXPECT_THROW(client.prices().create_async(invalid).result(), ConfigurationException);
    invalid = price_request();
    invalid.recurring_interval = "fortnight";
    EXPECT_THROW(client.prices().create_async(invalid).result(), ConfigurationException);
    invalid = price_request();
    invalid.currency = "USD";
    EXPECT_THROW(client.prices().create_async(invalid).result(), ConfigurationException);
    EXPECT_THROW(client.products().create_async({}).result(), ConfigurationException);
    EXPECT_THROW(client.products().retrieve_async("").result(), ConfigurationException);
    EXPECT_THROW(client.products().update_async("prod_test", { .name = "" }).result(),
                 ConfigurationException);
    EXPECT_THROW(client.prices().retrieve_async("").result(), ConfigurationException);
    EXPECT_THROW(client.prices().list_async({ .limit = 101 }).result(), ConfigurationException);
    ListPricesRequest list;
    list.lookup_keys.resize(11, "key");
    EXPECT_THROW(client.prices().list_async(list).result(), ConfigurationException);
    list.lookup_keys = { "" };
    EXPECT_THROW(client.prices().list_async(list).result(), ConfigurationException);
    EXPECT_TRUE(http.get_recorded_requests().empty());
}

TEST_F(CatalogTest, CallbackValidationErrorsAreDeliveredOnce) {
    int calls = 0;
    const auto product = [&](CallbackResult<Product> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    };
    client.products().create({}, {}, product);
    client.products().retrieve("", {}, product);
    client.products().update("", {}, {}, product);
    const auto price = [&](CallbackResult<Price> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    };
    client.prices().create({}, {}, price);
    client.prices().retrieve("", {}, price);
    client.prices().list({ .limit = 0 }, {}, [&](CallbackResult<PriceList> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    });
    EXPECT_EQ(calls, 6);
    EXPECT_TRUE(http.get_recorded_requests().empty());
}

TEST_F(CatalogTest, CatalogErrorsRetainStripeStatusAndRequestId) {
    http.set_response(
        { .status_code = 429,
          .headers = { { "request-id", "req_catalog" } },
          .body =
              R"({"error":{"type":"rate_limit_error","message":"Slow down","code":"rate_limit"}})" });
    try {
        client.prices().retrieve_async("price_test").result();
        FAIL();
    } catch (const StripeApiException& error) {
        EXPECT_EQ(error.payload().http_status, 429);
        EXPECT_EQ(error.payload().request_id, "req_catalog");
        EXPECT_TRUE(error.payload().retryable);
    }
    int calls = 0;
    client.products().retrieve("prod_test", {}, [&](CallbackResult<Product> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
        try {
            std::rethrow_exception(std::get<std::exception_ptr>(result));
        } catch (const StripeApiException& error) {
            EXPECT_EQ(error.payload().request_id, "req_catalog");
        }
    });
    EXPECT_EQ(calls, 1);
}

TEST_F(CatalogTest, TransportAndMalformedResponseFailuresPropagate) {
    http.set_exception(std::make_exception_ptr(std::runtime_error("Offline")));
    EXPECT_THROW(client.prices().retrieve_async("price_test").result(), std::runtime_error);
    http.set_response({ .status_code = 200, .body = "{}" });
    EXPECT_THROW(client.products().retrieve_async("prod_test").result(), ConfigurationException);
    EXPECT_THROW(client.prices().retrieve_async("price_test").result(), ConfigurationException);
    int calls = 0;
    client.prices().list({}, {}, [&](CallbackResult<PriceList> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    });
    EXPECT_EQ(calls, 1);
}

TEST_F(CatalogTest, MissingProductsAndCallbackTransportFailuresRemainDistinguishable) {
    http.set_response(
        { .status_code = 404,
          .headers = { { "request-id", "req_missing" } },
          .body =
              R"({"error":{"type":"invalid_request_error","code":"resource_missing","message":"No such product"}})" });
    try {
        client.products().retrieve_async("prod_missing").result();
        FAIL();
    } catch (const StripeApiException& error) {
        EXPECT_EQ(error.payload().http_status, 404);
        EXPECT_EQ(error.payload().code, "resource_missing");
        EXPECT_FALSE(error.retryable());
    }
    http.set_exception(std::make_exception_ptr(std::runtime_error("Offline")));
    int calls = 0;
    client.prices().retrieve("price_test", {}, [&](CallbackResult<Price> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
        try {
            std::rethrow_exception(std::get<std::exception_ptr>(result));
        } catch (const std::runtime_error& error) {
            EXPECT_EQ(std::string(error.what()), "Offline");
        }
    });
    EXPECT_EQ(calls, 1);
}

TEST(CatalogParser, PreservesNonstandardPricingWithoutInventingFixedAmounts) {
    JsonStripeResponseParser parser;
    const auto body = replacing(
        replacing(replacing(replacing(price_json, R"("unit_amount":2500)", R"("unit_amount":null)"),
                            R"("usage_type":"licensed")",
                            R"("usage_type":"metered")"),
                  R"("transform_quantity":null)",
                  R"("transform_quantity":{"divide_by":5,"round":"up"})"),
        R"("custom_unit_amount":null)",
        R"("custom_unit_amount":{"minimum":100})");
    const auto price = parser.parse_price({ .status_code = 200, .body = body });
    EXPECT_FALSE(price.unit_amount.has_value());
    EXPECT_EQ(price.unit_amount_decimal.value(), "2500");
    EXPECT_EQ(price.recurring->usage_type, "metered");
    EXPECT_EQ(price.transform_quantity->divide_by, 5);
    EXPECT_EQ(price.transform_quantity->round, "up");
    EXPECT_TRUE(price.custom_unit_amount);
}

TEST(CatalogParser, HandlesExpandedProductsArchivedPricesAndNullLookup) {
    JsonStripeResponseParser parser;
    const auto body =
        replacing(replacing(replacing(price_json,
                                      R"("product":"prod_test")",
                                      R"("product":{"id":"prod_test","object":"product"})"),
                            R"("active":true)",
                            R"("active":false)"),
                  R"("lookup_key":"standard_monthly")",
                  R"("lookup_key":null)");
    const auto price = parser.parse_price({ .status_code = 200, .body = body });
    EXPECT_EQ(price.product_id, "prod_test");
    EXPECT_FALSE(price.active);
    EXPECT_TRUE(price.lookup_key.empty());
}

TEST(CatalogParser, RejectsMalformedFieldsAndIncompletePages) {
    JsonStripeResponseParser parser;
    for (const auto& body :
         { replacing(price_json, R"("unit_amount":2500)", R"("unit_amount":"2500")"),
           replacing(price_json, R"("interval_count":2)", R"("interval_count":0)"),
           replacing(price_json, R"("active":true)", R"("active":"true")"),
           replacing(price_json, R"("product":"prod_test")", R"("product":null)"),
           replacing(price_json, R"("object":"price")", R"("object":"product")"),
           std::string("not-json") }) {
        EXPECT_THROW(parser.parse_price({ .status_code = 200, .body = body }),
                     ConfigurationException);
    }
    EXPECT_THROW(parser.parse_price_list({ .body = R"({"object":"list","data":[]})" }),
                 ConfigurationException);
    EXPECT_THROW(
        parser.parse_price_list({ .body = R"({"object":"list","data":[{}],"has_more":false})" }),
        ConfigurationException);
    EXPECT_THROW(
        parser.parse_product({ .body = R"({"object":"product","id":"prod_test","active":true})" }),
        ConfigurationException);
}
