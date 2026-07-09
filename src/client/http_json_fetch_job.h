#pragma once

#include <juce_core/juce_core.h>

#include <mutex>
#include <optional>
#include <string>
#include <thread>

struct HttpJsonFetchResult {
    bool ok = false;
    int status_code = 0;
    std::string url;
    std::string error;
    juce::var json;
};

class HttpJsonFetchJob {
public:
    ~HttpJsonFetchJob();

    void start(std::string url, int timeout_ms);
    std::optional<HttpJsonFetchResult> poll();
    bool running() const;
    void join();

private:
    mutable std::mutex mutex_;
    std::thread thread_;
    std::optional<HttpJsonFetchResult> result_;
    bool running_ = false;
    bool finished_ = false;
};
