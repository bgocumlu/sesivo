#include "http_json_fetch_job.h"

#include <spdlog/spdlog.h>

#include <exception>
#include <utility>

HttpJsonFetchJob::~HttpJsonFetchJob() {
    join();
}

void HttpJsonFetchJob::start(std::string url, int timeout_ms) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return;
        }
        running_ = true;
        finished_ = false;
        result_.reset();
    }

    join();

    thread_ = std::thread([this, url = std::move(url), timeout_ms]() {
        HttpJsonFetchResult result;
        result.url = url;
        spdlog::info("Fetching JSON from {}", result.url);

        try {
            auto stream = juce::URL(result.url)
                              .createInputStream(
                                  juce::URL::InputStreamOptions(
                                      juce::URL::ParameterHandling::inAddress)
                                      .withConnectionTimeoutMs(timeout_ms)
                                      .withStatusCode(&result.status_code));
            if (stream == nullptr || result.status_code != 200) {
                result.error = "HTTP " + std::to_string(result.status_code);
                spdlog::warn("JSON fetch failed for {}: {}", result.url, result.error);
            } else {
                result.json = juce::JSON::parse(stream->readEntireStreamAsString());
                if (result.json.isObject()) {
                    result.ok = true;
                    spdlog::info("Fetched JSON from {}", result.url);
                } else {
                    result.error = "invalid JSON";
                    spdlog::warn("JSON fetch failed for {}: {}", result.url, result.error);
                }
            }
        } catch (const std::exception& e) {
            result.error = e.what();
            spdlog::warn("JSON fetch failed for {}: {}", result.url, result.error);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        result_ = std::move(result);
        finished_ = true;
    });
}

std::optional<HttpJsonFetchResult> HttpJsonFetchJob::poll() {
    std::optional<HttpJsonFetchResult> result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!finished_) {
            return std::nullopt;
        }
        result = std::move(result_);
        result_.reset();
        finished_ = false;
        running_ = false;
    }

    join();
    return result;
}

bool HttpJsonFetchJob::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

void HttpJsonFetchJob::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}
