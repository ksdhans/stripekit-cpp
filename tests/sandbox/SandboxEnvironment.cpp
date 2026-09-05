/** SandboxEnvironment.cpp
 *
 * See header file for details.
 */

#include "SandboxEnvironment.h"

#include "JsonValue.h"

#include "StripeKit/WebhookDedupStore.h"
#include "StripeKit/Webhooks.h"

#include <drogon/drogon.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace stripekit::testing {

namespace {

/** Stripe event types the CLI listener forwards to the local endpoint. */
constexpr const char* forwarded_event_types =
    "customer.created,"
    "customer.subscription.created,"
    "customer.subscription.updated,"
    "invoice.paid,"
    "invoice.payment_failed,"
    "charge.refunded,"
    "charge.dispute.created,"
    "charge.dispute.closed";

/** Reads an environment variable, falling back to a default when unset or empty. */
std::string env_or(const char* name, std::string fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    return std::string(value);
}

/** Interprets an environment variable as a boolean opt-in flag. */
bool env_flag(const char* name) {
    std::string value = env_or(name, "");
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

/** Returns a display name for a Stripe REST method. */
std::string_view method_name(HttpMethod method) {
    switch (method) {
    case HttpMethod::Get:
        return "GET";
    case HttpMethod::Post:
        return "POST";
    case HttpMethod::Delete:
        return "DELETE";
    }
    return "UNKNOWN";
}

/** Shortens an identifier or match fragment for single-line progress output. */
std::string progress_value(std::string_view value) {
    constexpr std::size_t max_length = 80;
    if (value.size() <= max_length) {
        return std::string(value);
    }
    return std::string(value.substr(0, max_length - 3)) + "...";
}

/** Prints a sandbox progress line immediately. */
void sandbox_progress(std::string_view message) {
    const std::string line = "[SANDBOX] " + std::string(message);
    std::cout << line << std::endl;

    const char* path = std::getenv("STRIPEKIT_TEST_PROGRESS_LOG");
    if (path == nullptr || *path == '\0') {
        return;
    }

    std::ofstream log(path, std::ios::app);
    if (log) {
        log << line << '\n';
    }
}

/** Clears the sandbox progress log at the start of a run. */
void reset_sandbox_progress_log() {
    const char* path = std::getenv("STRIPEKIT_TEST_PROGRESS_LOG");
    if (path == nullptr || *path == '\0') {
        return;
    }

    std::ofstream log(path, std::ios::trunc);
}

/** Reads an environment variable as a positive integer, falling back on any parse failure. */
int env_int(const char* name, int fallback) {
    const std::string value = env_or(name, "");
    if (value.empty()) {
        return fallback;
    }
    try {
        const int parsed = std::stoi(value);
        return parsed > 0 ? parsed : fallback;
    } catch (const std::exception&) {
        return fallback;
    }
}

/** Percent-encodes a string for use in a form-encoded request body. */
std::string form_encode(std::string_view value) {
    std::ostringstream encoded;
    encoded << std::hex << std::uppercase << std::setfill('0');
    for (const unsigned char character : value) {
        if (std::isalnum(character) != 0 || character == '-' || character == '_' ||
            character == '.' || character == '~') {
            encoded << static_cast<char>(character);
        } else {
            encoded << '%' << std::setw(2) << static_cast<int>(character);
        }
    }
    return encoded.str();
}

/** Joins form parameters into a request body. */
std::string encode_form(const FormParams& params) {
    std::ostringstream body;
    bool first = true;
    for (const auto& [key, value] : params) {
        if (!first) {
            body << '&';
        }
        first = false;
        body << form_encode(key) << '=' << form_encode(value);
    }
    return body.str();
}

/** Finds an unused TCP port on the loopback interface. */
std::uint16_t find_free_port() {
    const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return 18432;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    std::uint16_t port = 18432;
    if (::bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        socklen_t length = sizeof(address);
        if (::getsockname(sock, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
            port = ntohs(address.sin_port);
        }
    }
    ::close(sock);
    return port;
}

/** Builds a short identifier that distinguishes this run from concurrent ones. */
std::string make_run_id() {
    std::random_device source;
    std::uniform_int_distribution<int> distribution(0, 15);
    std::ostringstream id;
    id << std::hex;
    for (int i = 0; i < 12; ++i) {
        id << distribution(source);
    }
    return id.str();
}

/** Extracts a Stripe webhook signing secret from Stripe CLI output. */
std::string find_webhook_secret(const std::string& text) {
    const auto start = text.find("whsec_");
    if (start == std::string::npos) {
        return {};
    }

    auto end = start;
    while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) != 0 || text[end] == '_')) {
        ++end;
    }
    return text.substr(start, end - start);
}

/**
 * Parses JSON the same way StripeKit's core does, but returns an empty value
 * instead of throwing.
 *
 * Used only where the text may be a deliberately malformed test payload, so a
 * parse failure must not be treated as a harness bug.
 */
Json::Value try_parse_json(std::string_view text) {
    if (text.empty()) {
        return Json::Value();
    }
    try {
        return json_value::parse(text);
    } catch (const ConfigurationException&) {
        return Json::Value();
    }
}

/**
 * Runs `stripe listen` as a child process and forwards events to a local URL.
 *
 * The Stripe CLI connects outbound to Stripe, so no public hostname or inbound
 * port is needed. That is what lets the same suite run behind NAT and on
 * ephemeral CI runners.
 */
class StripeCliListener {
public:
    ~StripeCliListener() {
        stop();
    }

    /** Starts the listener and blocks until it reports readiness. */
    void start(const SandboxConfig& config, const std::string& forward_url) {
        int pipe_fds[2] = {-1, -1};
        if (::pipe(pipe_fds) != 0) {
            throw std::runtime_error("Could not create a pipe for the Stripe CLI listener");
        }

        const pid_t child = ::fork();
        if (child < 0) {
            ::close(pipe_fds[0]);
            ::close(pipe_fds[1]);
            throw std::runtime_error("Could not fork the Stripe CLI listener");
        }

        if (child == 0) {
            ::close(pipe_fds[0]);
            ::dup2(pipe_fds[1], STDOUT_FILENO);
            ::dup2(pipe_fds[1], STDERR_FILENO);
            ::close(pipe_fds[1]);

#if defined(__linux__)
            ::prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif

            // The key is passed through the environment rather than argv so it
            // does not show up in the machine's process list.
            ::setenv("STRIPE_API_KEY", config.api_key.c_str(), 1);
            ::setenv("STRIPE_CLI_TELEMETRY_OPTOUT", "1", 1);

            const std::string cli = config.cli_path;
            const std::string events = forwarded_event_types;
            std::vector<char*> args{
                const_cast<char*>(cli.c_str()),
                const_cast<char*>("listen"),
                const_cast<char*>("--forward-to"),
                const_cast<char*>(forward_url.c_str()),
                const_cast<char*>("--events"),
                const_cast<char*>(events.c_str()),
                nullptr
            };

            ::execvp(cli.c_str(), args.data());
            const std::string failure = "Could not start the Stripe CLI: " + std::string(std::strerror(errno));
            [[maybe_unused]] const ssize_t ignored = ::write(STDERR_FILENO, failure.c_str(), failure.size());
            ::_exit(127);
        }

        ::close(pipe_fds[1]);
        pid = child;
        read_fd = pipe_fds[0];

        reader = std::thread([this]() {
            char buffer[1024];
            while (true) {
                const ssize_t count = ::read(read_fd, buffer, sizeof(buffer));
                if (count <= 0) {
                    break;
                }
                std::lock_guard<std::mutex> lock(mutex);
                captured_output.append(buffer, static_cast<std::size_t>(count));
            }
        });

        wait_until_ready();
    }

    /** Stops the listener and reaps the child process. */
    void stop() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
        if (read_fd >= 0) {
            ::close(read_fd);
            read_fd = -1;
        }
        if (reader.joinable()) {
            reader.join();
        }
    }

    /** Returns the signing secret the listener uses for forwarded events. */
    const std::string& webhook_secret() const {
        return secret;
    }

    /** Returns everything the Stripe CLI has printed so far. */
    std::string output() const {
        std::lock_guard<std::mutex> lock(mutex);
        return captured_output;
    }

private:
    void wait_until_ready() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
        while (std::chrono::steady_clock::now() < deadline) {
            const std::string text = output();
            if (text.find("Ready!") != std::string::npos) {
                secret = find_webhook_secret(text);
                if (!secret.empty()) {
                    return;
                }
            }

            int status = 0;
            if (pid > 0 && ::waitpid(pid, &status, WNOHANG) == pid) {
                pid = -1;
                throw std::runtime_error("The Stripe CLI listener exited during startup:\n" + text);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        throw std::runtime_error(
            "The Stripe CLI listener did not become ready. Check that `stripe` is installed and that "
            "STRIPE_API_KEY is a valid sandbox key. Output so far:\n" + output());
    }

    pid_t pid = -1;
    int read_fd = -1;
    std::thread reader;
    mutable std::mutex mutex;
    std::string captured_output;
    std::string secret;
};

} // namespace

SandboxConfig SandboxConfig::from_environment() {
    SandboxConfig config;
    config.enabled = env_flag("STRIPEKIT_SANDBOX_ENABLED");
    config.required = env_flag("STRIPEKIT_SANDBOX_REQUIRED");
    config.api_key = env_or("STRIPE_API_KEY", "");
    config.cli_path = env_or("STRIPEKIT_SANDBOX_STRIPE_CLI", "stripe");
    config.event_timeout = std::chrono::seconds(env_int("STRIPEKIT_SANDBOX_EVENT_TIMEOUT_SECONDS", 90));
    config.max_api_calls = env_int("STRIPEKIT_SANDBOX_MAX_API_CALLS", 400);
    return config;
}

std::string SandboxConfig::unavailable_reason() const {
    if (!enabled) {
        return "STRIPEKIT_SANDBOX_ENABLED is not set";
    }
    if (api_key.empty()) {
        return "STRIPE_API_KEY is not set; provide the sandbox secret API key (sk_test_...)";
    }
    if (api_key.rfind("pk_test", 0) == 0 || api_key.rfind("pk_live", 0) == 0) {
        return "STRIPE_API_KEY is a publishable key; provide the sandbox secret API key (sk_test_...) instead";
    }
    if (is_unsafe()) {
        return "STRIPE_API_KEY is a live-mode key; the sandbox suite refuses to run against live mode";
    }
    return {};
}

bool SandboxConfig::is_unsafe() const {
    return api_key.rfind("sk_live", 0) == 0 || api_key.rfind("rk_live", 0) == 0;
}

bool StripeApiResult::ok() const {
    return status_code >= 200 && status_code < 300;
}

std::string StripeApiResult::id() const {
    return body.isObject() ? body.get("id", "").asString() : std::string();
}

std::string StripeApiResult::describe() const {
    std::ostringstream summary;
    summary << "HTTP " << status_code;
    const Json::Value message = json_path(body, "error.message");
    if (message.isString()) {
        summary << ": " << message.asString();
    } else if (!raw_body.empty()) {
        summary << ": " << raw_body.substr(0, 300);
    }
    return summary.str();
}

SandboxStripeApi::SandboxStripeApi(std::string api_key, int max_calls)
    : http_client(std::move(api_key)), max_calls(max_calls) {}

SandboxStripeApi::~SandboxStripeApi() = default;

StripeApiResult SandboxStripeApi::post(std::string_view path, const FormParams& params) {
    return send(HttpMethod::Post, path, params);
}

StripeApiResult SandboxStripeApi::get(std::string_view path) {
    return send(HttpMethod::Get, path, {});
}

StripeApiResult SandboxStripeApi::require_post(std::string_view path, const FormParams& params) {
    StripeApiResult result = post(path, params);
    if (!result.ok()) {
        throw std::runtime_error("Stripe rejected POST " + std::string(path) + " - " + result.describe());
    }
    return result;
}

int SandboxStripeApi::call_count() const {
    return calls.load();
}

StripeApiResult SandboxStripeApi::send(HttpMethod method,
                                       std::string_view path,
                                       const FormParams& params) {
    const int call_number = calls.fetch_add(1) + 1;
    if (call_number > max_calls) {
        throw std::runtime_error(
            "The sandbox suite exceeded its Stripe API call budget. Raise "
            "STRIPEKIT_SANDBOX_MAX_API_CALLS only if the suite legitimately needs more calls.");
    }

    sandbox_progress("Stripe API call " + std::to_string(call_number) + ": " +
                     std::string(method_name(method)) + " " + std::string(path));

    stripekit::HttpRequest request;
    request.method = method;
    request.path = std::string(path);
    request.body = encode_form(params);
    request.headers.emplace("Content-Type", "application/x-www-form-urlencoded");

    HttpResponse response = http_client.send_async(request).result();

    StripeApiResult result;
    result.status_code = response.status_code;
    result.raw_body = std::move(response.body);
    result.body = try_parse_json(result.raw_body);
    sandbox_progress("Stripe API response " + std::to_string(call_number) + ": HTTP " +
                     std::to_string(result.status_code) + " for " + std::string(path));
    return result;
}

Json::Value ReceivedEvent::json() const {
    return json_value::parse(payload_json);
}

/** Everything the environment owns between start and stop. */
struct SandboxEnvironment::State {
    SandboxConfig config;
    std::string reason;
    std::string run_id;
    bool running = false;

    std::uint16_t port = 0;
    std::string base_url;
    std::thread server_thread;

    std::unique_ptr<StripeCliListener> listener;
    std::unique_ptr<SandboxStripeApi> api;
    std::unique_ptr<StripeClient> client;

    InMemoryWebhookDedupStore main_dedup_store;
    InMemoryWebhookDedupStore retry_dedup_store;

    mutable std::mutex mutex;
    std::condition_variable arrived;
    std::vector<ReceivedEvent> events;
    std::vector<CapturedDelivery> deliveries;
    std::map<std::string, int> retry_attempts;

    void record_event(const webhooks::Event& event) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(ReceivedEvent{
                .id = event.id,
                .type = event.type,
                .payload_json = event.payload_json
            });
        }
        sandbox_progress("Received webhook " + event.type + " (" + event.id + ")");
        arrived.notify_all();
    }

    void capture_delivery(std::string payload, std::string signature_header) {
        const Json::Value parsed = try_parse_json(payload);
        CapturedDelivery delivery{
            .event_id = parsed.isObject() ? parsed.get("id", "").asString() : std::string(),
            .payload = std::move(payload),
            .signature_header = std::move(signature_header)
        };

        std::lock_guard<std::mutex> lock(mutex);
        deliveries.push_back(std::move(delivery));
    }

    int note_retry_attempt(const std::string& event_id) {
        std::lock_guard<std::mutex> lock(mutex);
        return ++retry_attempts[event_id];
    }
};

SandboxEnvironment::SandboxEnvironment() : state(std::make_unique<State>()) {}

SandboxEnvironment::~SandboxEnvironment() {
    stop();
}

SandboxEnvironment& SandboxEnvironment::get() {
    static SandboxEnvironment environment;
    return environment;
}

bool SandboxEnvironment::start() {
    if (state->running) {
        return true;
    }

    reset_sandbox_progress_log();

    state->config = SandboxConfig::from_environment();
    state->reason = state->config.unavailable_reason();
    if (!state->reason.empty()) {
        return false;
    }

    state->run_id = make_run_id();
    state->port = find_free_port();
    state->base_url = "http://127.0.0.1:" + std::to_string(state->port);

    ::drogon::app().setLogLevel(trantor::Logger::kError);

    State* shared = state.get();

    ::drogon::app().registerPreRoutingAdvice(
        std::function<void(const ::drogon::HttpRequestPtr&)>(
            [shared](const ::drogon::HttpRequestPtr& request) {
                if (request->path() != std::string(main_webhook_path)) {
                    return;
                }
                shared->capture_delivery(std::string(request->body()),
                                         request->getHeader("stripe-signature"));
            }));

    state->listener = std::make_unique<StripeCliListener>();
    state->listener->start(state->config, state->base_url + main_webhook_path);

    stripekit::drogon::register_webhook_handler({
        .path = main_webhook_path,
        .endpoint_secret = state->listener->webhook_secret(),
        .dedup_store = state->main_dedup_store,
        .handler = [shared](const webhooks::Event& event)
            -> Task<webhooks::ProcessResult> {
            shared->record_event(event);
            co_return webhooks::ProcessResult::Handled;
        }
    });

    // Zero timeout so a replayed delivery can reclaim the in-flight record
    // immediately, instead of waiting out the production reclaim window.
    stripekit::drogon::register_webhook_handler({
        .path = retry_webhook_path,
        .endpoint_secret = state->listener->webhook_secret(),
        .dedup_store = state->retry_dedup_store,
        .handler = [shared](const webhooks::Event& event)
            -> Task<webhooks::ProcessResult> {
            if (shared->note_retry_attempt(event.id) == 1) {
                co_return webhooks::ProcessResult::RetryableFailure;
            }
            co_return webhooks::ProcessResult::Handled;
        },
        .stale_inflight_timeout = std::chrono::minutes(0)
    });

    ::drogon::app().addListener("127.0.0.1", state->port);
    state->server_thread = std::thread([]() {
        ::drogon::app().run();
    });

    for (int i = 0; i < 300 && !::drogon::app().isRunning(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!::drogon::app().isRunning()) {
        state->reason = "The local webhook server did not start";
        state->listener->stop();
        return false;
    }

    state->api = std::make_unique<SandboxStripeApi>(state->config.api_key, state->config.max_api_calls);
    state->client = stripekit::drogon::make_stripe_client({.api_key = state->config.api_key});

    state->running = true;
    std::cout << "Stripe sandbox run " << state->run_id
              << " listening on " << state->base_url << main_webhook_path << std::endl;
    return true;
}

void SandboxEnvironment::stop() {
    if (!state->running) {
        return;
    }

    if (state->listener) {
        state->listener->stop();
    }

    if (::drogon::app().isRunning()) {
        ::drogon::app().quit();
    }
    if (state->server_thread.joinable()) {
        state->server_thread.join();
    }

    state->running = false;
}

const SandboxConfig& SandboxEnvironment::config() const {
    return state->config;
}

const std::string& SandboxEnvironment::unavailable_reason() const {
    return state->reason;
}

SandboxStripeApi& SandboxEnvironment::stripe_api() {
    return *state->api;
}

StripeClient& SandboxEnvironment::stripe_client() {
    return *state->client;
}

const std::string& SandboxEnvironment::run_id() const {
    return state->run_id;
}

std::optional<ReceivedEvent> SandboxEnvironment::wait_for_event(std::string_view type,
                                                                std::string_view must_contain) {
    const auto matches = [&]() -> std::optional<ReceivedEvent> {
        for (const auto& event : state->events) {
            if (event.type != type) {
                continue;
            }
            if (!must_contain.empty() && event.payload_json.find(must_contain) == std::string::npos) {
                continue;
            }
            return event;
        }
        return std::nullopt;
    };

    std::unique_lock<std::mutex> lock(state->mutex);
    const auto deadline = std::chrono::steady_clock::now() + state->config.event_timeout;
    const auto timeout_seconds = state->config.event_timeout.count();
    const std::string event_type(type);
    const std::string match_value = progress_value(must_contain);
    sandbox_progress("Waiting up to " + std::to_string(timeout_seconds) + "s for webhook " + event_type +
                     (match_value.empty() ? std::string() : " matching " + match_value));
    while (true) {
        if (auto found = matches()) {
            sandbox_progress("Matched webhook " + event_type + " (" + found->id + ")");
            return found;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            auto final_match = matches();
            if (!final_match) {
                sandbox_progress("Timed out waiting for webhook " + event_type +
                                 (match_value.empty() ? std::string() : " matching " + match_value));
            }
            return final_match;
        }

        const auto next_progress = std::min(deadline, now + std::chrono::seconds(5));
        if (state->arrived.wait_until(lock, next_progress) == std::cv_status::timeout) {
            const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(deadline - next_progress).count();
            sandbox_progress("Still waiting for webhook " + event_type + "; " +
                             std::to_string(remaining) + "s remaining");
        }
    }
}

std::optional<CapturedDelivery> SandboxEnvironment::delivery_for(std::string_view event_id) const {
    std::lock_guard<std::mutex> lock(state->mutex);
    for (const auto& delivery : state->deliveries) {
        if (delivery.event_id == event_id) {
            return delivery;
        }
    }
    return std::nullopt;
}

int SandboxEnvironment::dispatch_count(std::string_view event_id) const {
    std::lock_guard<std::mutex> lock(state->mutex);
    int count = 0;
    for (const auto& event : state->events) {
        if (event.id == event_id) {
            ++count;
        }
    }
    return count;
}

int SandboxEnvironment::retry_route_attempts(std::string_view event_id) const {
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto it = state->retry_attempts.find(std::string(event_id));
    return it == state->retry_attempts.end() ? 0 : it->second;
}

int SandboxEnvironment::post_webhook(std::string_view path,
                                     std::string_view payload,
                                     std::string_view signature_header) const {
    auto client = ::drogon::HttpClient::newHttpClient(state->base_url);
    auto request = ::drogon::HttpRequest::newHttpRequest();
    request->setMethod(::drogon::Post);
    request->setPath(std::string(path));
    request->addHeader("Stripe-Signature", std::string(signature_header));
    request->setBody(std::string(payload));

    const auto [result, response] = client->sendRequest(request, 30.0);
    if (result != ::drogon::ReqResult::Ok || response == nullptr) {
        throw std::runtime_error("Could not deliver a replayed webhook to the local endpoint");
    }
    return static_cast<int>(response->statusCode());
}

Json::Value json_path(const Json::Value& root, std::string_view dotted_path) {
    Json::Value current = root;
    std::size_t start = 0;
    while (start <= dotted_path.size()) {
        const std::size_t separator = dotted_path.find('.', start);
        const std::string_view segment = dotted_path.substr(
            start, separator == std::string_view::npos ? std::string_view::npos : separator - start);

        if (!current.isObject() || !current.isMember(std::string(segment))) {
            return Json::Value();
        }
        current = current[std::string(segment)];

        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1;
    }
    return current;
}

std::string sandbox_email(const SandboxEnvironment& environment, std::string_view label) {
    return "stripekit+" + std::string(label) + "-" + environment.run_id() + "@example.com";
}

std::string create_customer_with_card(SandboxEnvironment& environment,
                                      std::string_view test_payment_method,
                                      std::string_view label) {
    const StripeApiResult result = environment.stripe_api().require_post("/v1/customers", {
        {"email", sandbox_email(environment, label)},
        {"name", "StripeKit E2E " + std::string(label)},
        {"payment_method", std::string(test_payment_method)},
        {"invoice_settings[default_payment_method]", std::string(test_payment_method)},
        {"metadata[stripekit_run_id]", environment.run_id()}
    });
    return result.id();
}

std::string create_monthly_price(SandboxEnvironment& environment, std::int64_t amount_cents) {
    const StripeApiResult product = environment.stripe_api().require_post("/v1/products", {
        {"name", "StripeKit E2E Plan " + environment.run_id()},
        {"metadata[stripekit_run_id]", environment.run_id()}
    });

    const StripeApiResult price = environment.stripe_api().require_post("/v1/prices", {
        {"product", product.id()},
        {"currency", "usd"},
        {"unit_amount", std::to_string(amount_cents)},
        {"recurring[interval]", "month"},
        {"metadata[stripekit_run_id]", environment.run_id()}
    });
    return price.id();
}

std::string create_confirmed_charge(SandboxEnvironment& environment,
                                    std::string_view test_payment_method,
                                    std::int64_t amount_cents) {
    const StripeApiResult intent = environment.stripe_api().require_post("/v1/payment_intents", {
        {"amount", std::to_string(amount_cents)},
        {"currency", "usd"},
        {"payment_method", std::string(test_payment_method)},
        {"confirm", "true"},
        {"automatic_payment_methods[enabled]", "true"},
        {"automatic_payment_methods[allow_redirects]", "never"},
        {"metadata[stripekit_run_id]", environment.run_id()}
    });

    const Json::Value charge = json_path(intent.body, "latest_charge");
    if (!charge.isString() || charge.asString().empty()) {
        throw std::runtime_error("Stripe did not return a charge for the confirmed payment: " +
                                 intent.describe());
    }
    return charge.asString();
}

} // namespace stripekit::testing
