#include "client_config_store.h"

#include <spdlog/spdlog.h>

#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

namespace {

juce::var empty_config_object() {
    return juce::var(new juce::DynamicObject());
}

bool write_client_config_root(const std::filesystem::path& path,
                              const juce::var& root) {
    if (path.empty()) {
        return false;
    }

    std::error_code create_error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), create_error);
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        spdlog::warn("Could not write client config: {}", path.string());
        return false;
    }

    output << juce::JSON::toString(root, false).toStdString() << '\n';
    return true;
}

class ClientConfigWriteQueue {
public:
    ClientConfigWriteQueue() : worker_([this]() { run(); }) {}

    ~ClientConfigWriteQueue() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    bool enqueue(std::filesystem::path path,
                 std::function<void(juce::DynamicObject&)> mutation,
                 std::string log_message) {
        if (path.empty()) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(
                Request{std::move(path), std::move(mutation), std::move(log_message)});
        }
        spdlog::info("Queued client config write: {}", log_message);
        cv_.notify_one();
        return true;
    }

private:
    struct Request {
        std::filesystem::path path;
        std::function<void(juce::DynamicObject&)> mutation;
        std::string log_message;
    };

    void run() {
        for (;;) {
            Request request;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&]() { return stopping_ || !requests_.empty(); });
                if (stopping_ && requests_.empty()) {
                    return;
                }
                request = std::move(requests_.front());
                requests_.pop_front();
            }

            spdlog::info("{}: writing {}", request.log_message, request.path.string());
            auto root = read_client_config_root(request.path);
            auto* object = client_config_root_object(root);
            request.mutation(*object);

            const bool saved = write_client_config_root(request.path, root);
            if (saved) {
                spdlog::info("{}: {}", request.log_message, request.path.string());
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> requests_;
    std::thread worker_;
    bool stopping_ = false;
};

ClientConfigWriteQueue& config_write_queue() {
    static ClientConfigWriteQueue queue;
    return queue;
}

}  // namespace

juce::var read_client_config_root(const std::filesystem::path& path) {
    if (path.empty()) {
        spdlog::info("Client config path is empty; using defaults");
        return empty_config_object();
    }

    const juce::File file{path.string()};
    if (!file.existsAsFile()) {
        spdlog::info("Client config not found, using defaults: {}", path.string());
        return empty_config_object();
    }

    auto root = juce::JSON::parse(file);
    if (!root.isObject()) {
        spdlog::warn("Client config is invalid JSON, using defaults: {}", path.string());
        return empty_config_object();
    }
    spdlog::info("Loaded client config: {}", path.string());
    return root;
}

juce::DynamicObject* client_config_root_object(juce::var& root) {
    if (!root.isObject()) {
        root = empty_config_object();
    }
    return root.getDynamicObject();
}

bool enqueue_client_config_write(
    std::filesystem::path path,
    std::function<void(juce::DynamicObject&)> mutation,
    std::string log_message) {
    return config_write_queue().enqueue(std::move(path), std::move(mutation),
                                        std::move(log_message));
}

ClientMixerUiState load_client_mixer_ui_state(const std::filesystem::path& path) {
    ClientMixerUiState state;
    auto root = read_client_config_root(path);
    const auto* root_object = root.getDynamicObject();
    if (root_object == nullptr) {
        return state;
    }

    const auto mixer_value = root_object->getProperty("mixer");
    const auto* mixer = mixer_value.getDynamicObject();
    if (mixer != nullptr) {
        state.advanced_latency_open =
            static_cast<bool>(mixer->getProperty("advancedLatencyOpen"));
    }
    return state;
}

bool save_client_mixer_ui_state(const std::filesystem::path& path,
                                const ClientMixerUiState& state) {
    return enqueue_client_config_write(
        path,
        [state](juce::DynamicObject& object) {
            const auto mixer_value = object.getProperty("mixer");
            auto* mixer = mixer_value.getDynamicObject();
            if (mixer == nullptr) {
                mixer = new juce::DynamicObject();
                object.setProperty("mixer", juce::var(mixer));
            }
            mixer->setProperty("advancedLatencyOpen", state.advanced_latency_open);
        },
        "Save mixer UI state");
}
