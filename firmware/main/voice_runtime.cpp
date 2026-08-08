#include "voice_runtime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>
#include <optional>
#include <string_view>
#include <utility>

#include "audio_capture.hpp"
#include "audio_playback.hpp"
#include "ble_provisioning.hpp"
#include "bsp/esp-bsp.h"
#include "chatesp/agent_loop.hpp"
#include "chatesp/app_mode.hpp"
#include "chatesp/audio_spectrum.hpp"
#include "chatesp/ble_settings.hpp"
#include "chatesp/interaction_state.hpp"
#include "chatesp/quick_controls.hpp"
#include "chatesp/runtime_control.hpp"
#include "chatesp/speech_segmenter.hpp"
#include "chatesp/turn_timing.hpp"
#include "chatesp/user_error_message.hpp"
#include "cloud_providers.hpp"
#include "device_control.hpp"
#include "device_memory_store.hpp"
#include "device_settings.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "http_transport.hpp"
#include "image_fetch_provider.hpp"
#include "jpeg_image_sink.hpp"
#include "micropython_executor.hpp"
#include "network_manager.hpp"
#include "network_context_provider.hpp"
#include "pcm_playback_sink.hpp"
#include "power_control.hpp"
#include "settings_store.hpp"
#include "speech_segment_channel.hpp"
#include "ui.hpp"

#ifndef CHATESP_DEVELOPMENT_MODE
#define CHATESP_DEVELOPMENT_MODE 0
#endif

namespace chatesp {
namespace {

constexpr bool kDevelopmentMode = CHATESP_DEVELOPMENT_MODE != 0;
constexpr char kTag[] = "voice_runtime";
constexpr std::uint32_t kLevelRefreshMs = 80;
constexpr std::uint32_t kSettingsRefreshMs = 500;
constexpr std::uint32_t kEarlyConnectRetryMs = 10'000;
constexpr std::uint32_t kDisplayWakeRetryMs = 100;
constexpr std::uint32_t kDisplaySleepRetryMs = 100;
constexpr std::uint32_t kFooterRefreshMs = 100;
constexpr std::uint32_t kBatteryRefreshMs = 30'000;
constexpr std::uint32_t kAnswerStreamRefreshMs = 60;
constexpr std::uint32_t kPoweroffGraceMs = 250;
constexpr std::uint32_t kInteractionTimeoutMs = 180'000;
constexpr std::uint32_t kClockReturnDelayMs = 30'000;
constexpr std::uint32_t kClockTimeSyncLimitMs = 15'000;
constexpr std::uint32_t kModeDisplayRetryMs = 100;
constexpr std::uint32_t kBleRestartAfterWorkerMs = 250;
constexpr std::uint32_t kBleStopTimeoutMs = 1'000;
constexpr std::size_t kMinimumRecordingSamples =
    AudioCapture::kSampleRateHz / 10;
static_assert(
    AudioCapture::kSampleRateHz == kAudioSpectrumSampleRateHz,
    "The spectrum bin frequencies must match the capture sample rate");
constexpr UBaseType_t kRuntimePriority = 5;
constexpr std::uint32_t kRuntimeStackBytes = 32 * 1024;
constexpr UBaseType_t kPasskeyPriority = 6;
constexpr std::uint32_t kPasskeyStackBytes = 8 * 1024;
constexpr UBaseType_t kSpeechPriority = 5;
constexpr std::uint32_t kSpeechStackBytes = 16 * 1024;
constexpr EventBits_t kSpeechDoneBit = BIT0;
constexpr EventBits_t kNetworkWarmDoneBit = BIT1;
constexpr EventBits_t kImageDoneBit = BIT2;
constexpr EventBits_t kNetworkContextDoneBit = BIT3;
constexpr UBaseType_t kNetworkWarmPriority = 3;
constexpr std::uint32_t kNetworkWarmStackBytes = 6 * 1024;
constexpr UBaseType_t kImagePriority = 3;
constexpr std::uint32_t kImageStackBytes = 20 * 1024;
constexpr UBaseType_t kNetworkContextPriority = 3;
constexpr std::uint32_t kNetworkContextStackBytes = 20 * 1024;
constexpr std::uint64_t kMinimumValidEpochSeconds = 1'577'836'800ULL;
constexpr char kNtpServer[] = "time.cloudflare.com";

std::uint32_t monotonic_ms() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1'000ULL);
}

void secure_wipe(void *data, std::size_t size) {
    auto *bytes = static_cast<volatile std::uint8_t *>(data);
    while (size-- != 0) {
        *bytes++ = 0;
    }
}

template <std::size_t Capacity>
class SecureTextGuard {
public:
    explicit SecureTextGuard(agent::FixedText<Capacity> &text) : text_(text) {}
    ~SecureTextGuard() {
        secure_wipe(text_.data(), text_.capacity() + 1);
        text_.clear();
    }

private:
    agent::FixedText<Capacity> &text_;
};

struct RequestScratch {
    agent::FixedText<agent::Limits::max_transcript_bytes> transcript;
    agent::FixedText<agent::Limits::max_answer_bytes> answer;
    agent::FixedText<agent::Limits::max_answer_bytes> stream_answer;

    void clear() {
        transcript.clear();
        answer.clear();
        stream_answer.clear();
    }
};

class SpeechStartSink final : public agent::PcmSink {
public:
    SpeechStartSink(PcmPlaybackSink &sink, agent::AgentLoop &agent_loop)
        : sink_(sink), agent_loop_(agent_loop) {}

    agent::Error begin(
        std::uint32_t sample_rate_hz, std::uint8_t channels,
        std::uint8_t bits_per_sample) override {
        const agent::Error result = sink_.begin(
            sample_rate_hz, channels, bits_per_sample);
        if (result == agent::Error::none) {
            agent_loop_.report_speech_start();
        }
        return result;
    }

    agent::Error write(
        const std::uint8_t *data, std::size_t size) override {
        return sink_.write(data, size);
    }

    agent::Error finish() override { return sink_.finish(); }
    void cancel() override { sink_.cancel(); }

private:
    PcmPlaybackSink &sink_;
    agent::AgentLoop &agent_loop_;
};

class AtomicCancellation final : public agent::CancellationToken {
public:
    [[nodiscard]] bool cancelled() const override {
        return cancelled_.load(std::memory_order_acquire);
    }

    void cancel() { cancelled_.store(true, std::memory_order_release); }
    void reset() { cancelled_.store(false, std::memory_order_release); }

private:
    std::atomic<bool> cancelled_{false};
};

class DeadlineCancellation final : public agent::CancellationToken {
public:
    DeadlineCancellation(
        AtomicCancellation &source, std::uint32_t started_ms,
        std::uint32_t timeout_ms)
        : source_(source), deadline_(started_ms, timeout_ms) {}

    [[nodiscard]] bool cancelled() const override {
        return source_.cancelled() || expired();
    }

    [[nodiscard]] bool expired() const {
        return deadline_.expired(monotonic_ms());
    }

    [[nodiscard]] agent::Error normalize(agent::Error error) const {
        return expired() ? agent::Error::total_timeout : error;
    }

private:
    AtomicCancellation &source_;
    runtime::MonotonicDeadline deadline_;
};

class NetworkRequestGuard {
public:
    explicit NetworkRequestGuard(network::NetworkManager &network)
        : network_(network), active_(network_.set_request_active(true) == ESP_OK) {}

    ~NetworkRequestGuard() {
        if (active_) {
            const esp_err_t error = network_.set_request_active(false);
            if (error != ESP_OK) {
                ESP_LOGW(
                    kTag,
                    "Wi-Fi modem-save restore failed (category %s)",
                    esp_err_to_name(error));
            }
        }
    }

    NetworkRequestGuard(const NetworkRequestGuard &) = delete;
    NetworkRequestGuard &operator=(const NetworkRequestGuard &) = delete;

private:
    network::NetworkManager &network_;
    bool active_ = false;
};

template <std::size_t Capacity>
bool assign_setting(
    provisioning::BoundedSetting<Capacity> &destination,
    std::string_view source) {
    return destination.assign(source);
}

struct RuntimeSettings {
    RuntimeSettings() { load_local(); }
    ~RuntimeSettings() { clear(); }

    void clear() {
        endpoint.clear();
        openrouter_key.clear();
        brave_key.clear();
        wifi_ssid.clear();
        wifi_password.clear();
        chat_model.clear();
        transcription_model.clear();
        speech_model.clear();
        approximate_location.clear();
        configured = false;
    }

    bool load_local() {
        clear();
        const DeviceSettingsView local = device_settings();
        if (!local.configured) {
            return false;
        }
        configured =
            assign_setting(endpoint, local.chat_endpoint) &&
            assign_setting(openrouter_key, local.openrouter_api_key) &&
            assign_setting(brave_key, local.brave_api_key) &&
            assign_setting(wifi_ssid, local.wifi_ssid) &&
            assign_setting(wifi_password, local.wifi_password) &&
            chat_model.assign("~deepseek/deepseek-v4-flash-latest") &&
            transcription_model.assign("openai/whisper-large-v3-turbo") &&
            speech_model.assign("hexgrad/kokoro-82m") &&
            approximate_location.assign("");
        if (!configured) {
            clear();
        }
        return configured;
    }

    bool load(const provisioning::SettingsRecord &record) {
        RuntimeSettings next;
        next.clear();
        next.configured =
            next.endpoint.assign(record.chat_endpoint.view()) &&
            next.openrouter_key.assign(record.openrouter_key.view()) &&
            next.brave_key.assign(record.brave_key.view()) &&
            next.wifi_ssid.assign(record.wifi_ssid.view()) &&
            next.wifi_password.assign(record.wifi_password.view()) &&
            next.chat_model.assign(record.chat_model.view()) &&
            next.transcription_model.assign(
                record.transcription_model.view()) &&
            next.speech_model.assign(record.speech_model.view()) &&
            next.approximate_location.assign(
                record.approximate_location.view());
        if (!next.configured) {
            next.clear();
            return false;
        }
        clear();
        *this = next;
        next.clear();
        return true;
    }

    cloud::OpenRouterConnectionView openrouter() const {
        const std::string_view endpoint_view = endpoint.view();
        const std::string_view key_view = openrouter_key.view();
        return {
            endpoint_view.data(),
            endpoint_view.size(),
            {key_view.data(), key_view.size()},
            {
                chat_model.view().data(),
                transcription_model.view().data(),
                speech_model.view().data(),
                "af_heart",
            },
        };
    }

    provider::SecretView brave() const {
        const std::string_view value = brave_key.view();
        return {value.data(), value.size()};
    }

    network::WifiCredentials wifi() const {
        const std::string_view ssid = wifi_ssid.view();
        const std::string_view password = wifi_password.view();
        return {
            ssid.data(), ssid.size(), password.data(), password.size()};
    }

    std::string_view location() const {
        return approximate_location.view();
    }

    [[nodiscard]] bool has_wifi_credentials() const {
        return configured && !wifi_ssid.view().empty();
    }

    [[nodiscard]] bool has_service_credentials() const {
        return configured && !endpoint.view().empty() &&
            !openrouter_key.view().empty() && !chat_model.view().empty() &&
            !transcription_model.view().empty() &&
            !speech_model.view().empty();
    }

    provisioning::BoundedSetting<192> endpoint;
    provisioning::BoundedSetting<256> openrouter_key;
    provisioning::BoundedSetting<128> brave_key;
    provisioning::BoundedSetting<32> wifi_ssid;
    provisioning::BoundedSetting<63> wifi_password;
    provisioning::BoundedSetting<96> chat_model;
    provisioning::BoundedSetting<96> transcription_model;
    provisioning::BoundedSetting<96> speech_model;
    provisioning::BoundedSetting<96> approximate_location;
    bool configured = false;
};

enum class CommandKind : std::uint8_t {
    button_down,
    wake_button_down,
    button_up,
    wake_from_poweroff,
    poweroff_failed,
    provisioning_activity,
    toggle_mode,
};

struct Command {
    CommandKind kind = CommandKind::button_down;
    std::uint32_t at_ms = 0;
};

struct PasskeyEvent {
    std::uint32_t passkey = 0;
    bool visible = false;
};

struct DeviceContextEvent {
    std::uint64_t epoch_seconds = 0;
    std::int16_t utc_offset_minutes = 0;
    std::uint32_t observed_at_ms = 0;
    std::uint8_t approximate_location_size = 0;
    std::array<char, provisioning::kMaximumApproximateLocationSize + 1> approximate_location{};
};

}  // namespace

class VoiceRuntime::Impl final : public agent::AgentProgressObserver,
                                 public agent::ChatTextSink {
public:
    Impl(
        DevicePreferencesStore &device_preferences_store,
        DeviceMemoryStore &device_memory_store)
        : device_control_(
              device_preferences_store,
              device_preferences_store.preferences(),
              kDevelopmentMode),
          memory_store_(device_memory_store),
          network_context_provider_(network_context_transport_),
          image_fetch_provider_(image_transport_),
          openrouter_connection_(settings_.openrouter()),
          brave_key_(settings_.brave()),
          web_provider_(search_transport_, network_, brave_key_),
          image_provider_(search_transport_, network_, brave_key_),
          web_tool_(web_provider_),
          image_tool_(image_provider_),
          python_tool_(python_executor_),
          device_status_tool_(device_control_),
          brightness_tool_(device_control_),
          volume_tool_(device_control_),
          power_off_tool_(device_control_),
          remember_memory_tool_(memory_store_),
          forget_memory_tool_(memory_store_),
          clear_memories_tool_(memory_store_),
          compact_memories_tool_(memory_store_),
          chat_provider_(
              openrouter_control_transport_, network_, openrouter_connection_,
              tools_, memory_store_, utc_clock_, settings_.location()),
          transcription_provider_(
              openrouter_control_transport_, network_, openrouter_connection_,
              utc_clock_),
          speech_provider_(
              openrouter_audio_transport_, network_, openrouter_connection_),
          pcm_sink_(playback_, device_control_),
          interaction_(interaction_config_for_mode(kDevelopmentMode)) {
        tools_ready_ = tools_.add(web_tool_) == agent::Error::none &&
            tools_.add(image_tool_) == agent::Error::none &&
            (!python_executor_.available() ||
             tools_.add(python_tool_) == agent::Error::none) &&
            tools_.add(device_status_tool_) == agent::Error::none &&
            tools_.add(brightness_tool_) == agent::Error::none &&
            tools_.add(volume_tool_) == agent::Error::none &&
            tools_.add(power_off_tool_) == agent::Error::none &&
            tools_.add(remember_memory_tool_) == agent::Error::none &&
            tools_.add(forget_memory_tool_) == agent::Error::none &&
            tools_.add(clear_memories_tool_) == agent::Error::none &&
            tools_.add(compact_memories_tool_) == agent::Error::none;
        void *agent_memory = heap_caps_malloc(
            sizeof(agent::AgentLoop), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (agent_memory != nullptr) {
            agent_loop_ = new (agent_memory)
                agent::AgentLoop(chat_provider_, tools_, this, this);
        }
        void *scratch_memory = heap_caps_malloc(
            sizeof(RequestScratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (scratch_memory != nullptr) {
            request_scratch_ = new (scratch_memory) RequestScratch();
        }
        tools_ready_ = tools_ready_ && agent_loop_ != nullptr &&
            request_scratch_ != nullptr;
    }

    ~Impl() override {
        cancellation_.cancel();
        context_cancellation_.cancel();
        speech_channel_.cancel();
        cancel_transports();
        stop_network_time_sync();
        capture_.cancel();
        pcm_sink_.cancel();
        if (quick_controls_enabled_ && bsp_display_lock(100)) {
            ui::disable_quick_controls();
            bsp_display_unlock();
            quick_controls_enabled_ = false;
        }
        if (speech_events_ != nullptr) {
            vEventGroupDelete(speech_events_);
            speech_events_ = nullptr;
        }
        if (ble_started_) {
            (void)ble_provisioning::stop(kBleStopTimeoutMs);
        }
        if (agent_loop_ != nullptr) {
            agent_loop_->clear_thread();
            agent_loop_->~AgentLoop();
            secure_wipe(agent_loop_, sizeof(agent::AgentLoop));
            heap_caps_free(agent_loop_);
            agent_loop_ = nullptr;
        }
        if (request_scratch_ != nullptr) {
            request_scratch_->clear();
            request_scratch_->~RequestScratch();
            secure_wipe(request_scratch_, sizeof(RequestScratch));
            heap_caps_free(request_scratch_);
            request_scratch_ = nullptr;
        }
    }

    [[nodiscard]] bool started() const {
        return started_.load(std::memory_order_acquire);
    }

    esp_err_t start(bool startup_button_down, std::uint32_t startup_at_ms) {
        if (!tools_ready_) {
            return ESP_FAIL;
        }
        if (!python_executor_.available()) {
            ESP_LOGW(kTag, "The optional Python tool is not available");
        }
        // Confirm NVS through the settings layer before Wi-Fi or NimBLE can
        // use it. The device-preference start in app_main can start NVS first
        // so the saved display brightness is available before the splash.
        const esp_err_t settings_result = settings_store_.initialize();
        if (settings_result != ESP_OK) {
            return settings_result;
        }
        // Reserve the internal I2S DMA path before Wi-Fi and Bluetooth use
        // the remaining internal memory.
        const esp_err_t audio_result = capture_.initialize();
        if (audio_result != ESP_OK) {
            ESP_LOGE(kTag, "Audio initialization failed: %s",
                     esp_err_to_name(audio_result));
            return audio_result;
        }
        command_queue_ = xQueueCreate(16, sizeof(Command));
        passkey_queue_ = xQueueCreate(1, sizeof(PasskeyEvent));
        device_context_queue_ = xQueueCreate(1, sizeof(DeviceContextEvent));
        speech_events_ = xEventGroupCreate();
        if (command_queue_ == nullptr || passkey_queue_ == nullptr ||
            device_context_queue_ == nullptr || speech_events_ == nullptr) {
            if (command_queue_ != nullptr) {
                vQueueDelete(command_queue_);
                command_queue_ = nullptr;
            }
            if (passkey_queue_ != nullptr) {
                vQueueDelete(passkey_queue_);
                passkey_queue_ = nullptr;
            }
            if (device_context_queue_ != nullptr) {
                vQueueDelete(device_context_queue_);
                device_context_queue_ = nullptr;
            }
            if (speech_events_ != nullptr) {
                vEventGroupDelete(speech_events_);
                speech_events_ = nullptr;
            }
            return ESP_ERR_NO_MEM;
        }
        const BaseType_t passkey_task_result = xTaskCreatePinnedToCore(
            passkey_task_entry,
            "ble_passkey_ui",
            kPasskeyStackBytes,
            this,
            kPasskeyPriority,
            &passkey_task_,
            1);
        if (passkey_task_result != pdPASS) {
            vQueueDelete(command_queue_);
            vQueueDelete(passkey_queue_);
            vQueueDelete(device_context_queue_);
            command_queue_ = nullptr;
            passkey_queue_ = nullptr;
            device_context_queue_ = nullptr;
            vEventGroupDelete(speech_events_);
            speech_events_ = nullptr;
            return ESP_ERR_NO_MEM;
        }
        if (startup_button_down) {
            button_pressed_.store(true, std::memory_order_release);
            voice_priority_.store(true, std::memory_order_release);
            cancellation_.cancel();
            context_cancellation_.cancel();
            const Command command{
                CommandKind::wake_button_down, startup_at_ms};
            queue_command(command);
        }
        const BaseType_t task_result = xTaskCreatePinnedToCore(
            task_entry,
            "voice_runtime",
            kRuntimeStackBytes,
            this,
            kRuntimePriority,
            &task_,
            1);
        if (task_result != pdPASS) {
            vTaskDelete(passkey_task_);
            passkey_task_ = nullptr;
            vQueueDelete(command_queue_);
            vQueueDelete(passkey_queue_);
            vQueueDelete(device_context_queue_);
            command_queue_ = nullptr;
            passkey_queue_ = nullptr;
            device_context_queue_ = nullptr;
            vEventGroupDelete(speech_events_);
            speech_events_ = nullptr;
            return ESP_ERR_NO_MEM;
        }

        pending_brightness_percent_.store(
            device_control_.brightness_percent(), std::memory_order_release);
        pending_volume_percent_.store(
            device_control_.volume_percent(), std::memory_order_release);
        if (bsp_display_lock(100)) {
            quick_controls_enabled_ = ui::enable_quick_controls(
                device_control_.brightness_percent(),
                device_control_.volume_percent(),
                quick_controls_callback,
                this);
            lv_refr_now(nullptr);
            bsp_display_unlock();
        }
        if (!quick_controls_enabled_) {
            ESP_LOGW(kTag, "Touch controls are not available");
        }
        started_.store(true, std::memory_order_release);
        return ESP_OK;
    }

    void action_button_edge(bool pressed, std::uint32_t at_ms) {
        if (command_queue_ == nullptr) {
            return;
        }
        button_pressed_.store(pressed, std::memory_order_release);
        if (pressed) {
            device_control_.cancel_power_off();
            voice_priority_.store(true, std::memory_order_release);
            cancellation_.cancel();
            context_cancellation_.cancel();
            speech_channel_.cancel();
            capture_.cancel();
            pcm_sink_.cancel();
            cancel_transports();
        }
        const runtime::ButtonRoute route = pressed
            ? poweroff_gate_.button_down()
            : runtime::ButtonRoute::normal;
        const Command command{
            pressed
                ? (route == runtime::ButtonRoute::wake
                       ? CommandKind::wake_from_poweroff
                       : CommandKind::button_down)
                : CommandKind::button_up,
            at_ms,
        };
        queue_command(command);
    }

    void mode_button_short_press(std::uint32_t at_ms) {
        if (command_queue_ == nullptr ||
            !display_available_.load(std::memory_order_acquire)) {
            return;
        }
        if (app_mode_.load(std::memory_order_acquire) == AppMode::chat) {
            cancellation_.cancel();
            context_cancellation_.cancel();
            speech_channel_.cancel();
            capture_.cancel();
            pcm_sink_.cancel();
            cancel_transports();
        }
        queue_command({CommandKind::toggle_mode, at_ms});
    }

    [[nodiscard]] bool mode_button_available() const {
        return display_available_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool poweroff_ready() const {
        return poweroff_gate_.poweroff_ready();
    }

    void poweroff_failed() {
        poweroff_gate_.recover();
        const Command command{CommandKind::poweroff_failed, monotonic_ms()};
        queue_command(command);
    }

    void on_agent_progress(agent::AgentProgressEvent event) override {
        const std::uint32_t now_ms = monotonic_ms();
        switch (event) {
            case agent::AgentProgressEvent::transcription_complete:
                interaction_.transcription_ready(now_ms);
                show_model_progress("PREPARING THE REQUEST");
                break;
            case agent::AgentProgressEvent::model_start:
                timing_.mark(runtime::TurnPhase::route_start, now_ms);
                show_model_progress("ASKING THE MODEL");
                break;
            case agent::AgentProgressEvent::tool_start:
                timing_.note_tool_round();
                interaction_.tool_started(now_ms);
                show_state(interaction_.state());
                break;
            case agent::AgentProgressEvent::answer_start:
                prepare_visual();
                show_model_progress("GENERATING THE ANSWER");
                break;
            case agent::AgentProgressEvent::answer_ready:
                show_model_progress("ANSWER READY");
                break;
            case agent::AgentProgressEvent::speech_start:
                timing_.mark(runtime::TurnPhase::playback_start, now_ms);
                interaction_.speech_started(now_ms);
                show_state(interaction_.state());
                start_image_worker();
                break;
        }
    }

    agent::Error write_chat_text(
        const char *text, std::size_t size) override {
        if (cancellation_.cancelled()) {
            return agent::Error::cancelled;
        }
        if ((size != 0 && text == nullptr) ||
            size > agent::Limits::max_answer_bytes) {
            return agent::Error::invalid_argument;
        }

        const std::uint32_t now_ms = monotonic_ms();
        timing_.mark(runtime::TurnPhase::first_answer_text, now_ms);
        if (request_scratch_ == nullptr ||
            !request_scratch_->stream_answer.assign(text, size) ||
            !speech_segmenter_.update(text, size, speech_channel_)) {
            return cancellation_.cancelled() ? agent::Error::cancelled
                                             : agent::Error::model_failed;
        }
        if (size == 0 || !stream_text_shown_ ||
            now_ms - stream_text_refreshed_at_ms_ >=
                kAnswerStreamRefreshMs) {
            if (display_available_.load(std::memory_order_acquire) &&
                bsp_display_lock(10)) {
                ui::show_answer_stream({text, size});
                bsp_display_unlock();
            }
            stream_text_shown_ = true;
            stream_text_refreshed_at_ms_ = now_ms;
        }
        return agent::Error::none;
    }

    agent::Error set_speech_language(
        agent::SpeechLanguage language) override {
        if (cancellation_.cancelled()) {
            return agent::Error::cancelled;
        }
        speech_provider_.set_language(language);
        return agent::Error::none;
    }

private:
    void cancel_transports() {
        openrouter_control_transport_.cancel_active();
        openrouter_audio_transport_.cancel_active();
        search_transport_.cancel_active();
        image_transport_.cancel_active();
        network_context_transport_.cancel_active();
    }

    void reset_transports() {
        openrouter_control_transport_.reset_session();
        openrouter_audio_transport_.reset_session();
        search_transport_.reset_session();
        image_transport_.reset_session();
        network_context_transport_.reset_session();
    }

    static void task_entry(void *context) {
        static_cast<Impl *>(context)->run();
    }

    static void passkey_task_entry(void *context) {
        static_cast<Impl *>(context)->run_passkey_ui();
    }

    static void speech_task_entry(void *context) {
        static_cast<Impl *>(context)->run_speech_worker();
    }

    static void network_warm_task_entry(void *context) {
        static_cast<Impl *>(context)->run_network_warm_worker();
    }

    static void image_task_entry(void *context) {
        static_cast<Impl *>(context)->run_image_worker();
    }

    static void network_context_task_entry(void *context) {
        static_cast<Impl *>(context)->run_network_context_worker();
    }

    void start_image_worker() {
        if (image_task_ != nullptr || speech_cancellation_ == nullptr ||
            speech_events_ == nullptr ||
            selected_image_result_.thumbnail_url.empty()) {
            return;
        }
        image_frame_.reset();
        image_cancellation_ = speech_cancellation_;
        image_error_.store(agent::Error::none, std::memory_order_release);
        xEventGroupClearBits(speech_events_, kImageDoneBit);
        const BaseType_t created = xTaskCreatePinnedToCore(
            image_task_entry, "image_download", kImageStackBytes, this,
            kImagePriority, &image_task_, 0);
        if (created != pdPASS) {
            image_task_ = nullptr;
            image_cancellation_ = nullptr;
            selected_image_result_.clear();
        }
    }

    void prepare_visual() {
        pending_plot_.clear();
        selected_image_result_.clear();
        image_frame_.reset();
        if (python_tool_.take_plot(pending_plot_)) {
            return;
        }
        (void)image_tool_.take_selected(selected_image_result_);
    }

    void run_image_worker() {
        agent::Error error = agent::Error::model_failed;
        if (image_cancellation_ != nullptr) {
            image::JpegImageSink sink(*image_cancellation_);
            const agent::ImageFetchRequest request{
                selected_image_result_.thumbnail_url.c_str(),
                agent::Limits::max_image_download_bytes,
                agent::Limits::max_image_dimension,
                0,
                agent::image_fetch_policy(),
            };
            error = image_fetch_provider_.fetch(
                request, sink, *image_cancellation_);
            if (error == agent::Error::none && sink.ready()) {
                image_frame_ = sink.take_frame();
            } else if (error == agent::Error::none) {
                error = agent::Error::malformed_response;
            }
        }
        selected_image_result_.clear();
        image_error_.store(error, std::memory_order_release);
        xEventGroupSetBits(speech_events_, kImageDoneBit);
        vTaskDelete(nullptr);
    }

    void join_image_worker() {
        if (image_task_ == nullptr) {
            return;
        }
        xEventGroupWaitBits(
            speech_events_, kImageDoneBit, pdFALSE, pdTRUE, portMAX_DELAY);
        image_task_ = nullptr;
        image_cancellation_ = nullptr;
        const agent::Error error =
            image_error_.load(std::memory_order_acquire);
        if (image_frame_.available()) {
            timing_.mark(runtime::TurnPhase::image_ready, monotonic_ms());
        } else if (error != agent::Error::cancelled) {
            ESP_LOGW(
                kTag, "Optional image was not available (category %u)",
                static_cast<unsigned>(error));
        }
    }

    void start_network_during_recording() {
        if (!settings_.has_wifi_credentials() || network_initialized_ ||
            network_warm_task_ != nullptr || speech_events_ == nullptr) {
            return;
        }
        if (network_.connected() || network_.connecting()) {
            return;
        }
        xEventGroupClearBits(speech_events_, kNetworkWarmDoneBit);
        network_warm_initialized_.store(false, std::memory_order_release);
        const BaseType_t created = xTaskCreatePinnedToCore(
            network_warm_task_entry, "wifi_recording",
            kNetworkWarmStackBytes, this, kNetworkWarmPriority,
            &network_warm_task_, 0);
        if (created != pdPASS) {
            network_warm_task_ = nullptr;
        }
    }

    void run_network_warm_worker() {
        const bool initialized = network_.initialize() == ESP_OK;
        network_warm_initialized_.store(initialized, std::memory_order_release);
        if (initialized) {
            const agent::Error error =
                network_.start_connect(settings_.wifi());
            if (error != agent::Error::none) {
                ESP_LOGW(
                    kTag,
                    "Recording-time Wi-Fi connect did not start (category %u)",
                    static_cast<unsigned>(error));
            }
        }
        xEventGroupSetBits(speech_events_, kNetworkWarmDoneBit);
        vTaskDelete(nullptr);
    }

    void join_network_warm_worker() {
        if (network_warm_task_ == nullptr) {
            return;
        }
        xEventGroupWaitBits(
            speech_events_, kNetworkWarmDoneBit, pdFALSE, pdTRUE,
            portMAX_DELAY);
        network_warm_task_ = nullptr;
        if (network_warm_initialized_.load(std::memory_order_acquire)) {
            network_initialized_ = true;
        }
    }

    void start_network_time_sync() {
        if (sntp_started_ || sntp_complete_ || sntp_start_attempted_ ||
            !network_.connected()) {
            return;
        }
        sntp_start_attempted_ = true;
        esp_sntp_config_t config =
            ESP_NETIF_SNTP_DEFAULT_CONFIG(kNtpServer);
        const esp_err_t result = esp_netif_sntp_init(&config);
        if (result == ESP_OK) {
            sntp_started_ = true;
        } else {
            ESP_LOGW(
                kTag, "NTP start failed (category %s)",
                esp_err_to_name(result));
        }
    }

    void poll_network_time_sync(std::uint32_t now_ms) {
        if (!sntp_started_ ||
            esp_netif_sntp_sync_wait(0) != ESP_OK) {
            return;
        }
        const std::time_t current = std::time(nullptr);
        if (current >= 0 &&
            static_cast<std::uint64_t>(current) >=
                kMinimumValidEpochSeconds &&
            utc_clock_.update_from_epoch_seconds_utc(
                static_cast<std::uint64_t>(current), now_ms)) {
            sntp_complete_ = true;
            ESP_LOGI(kTag, "NTP time accepted");
            refresh_clock(now_ms, true);
        }
        esp_netif_sntp_deinit();
        sntp_started_ = false;
    }

    void stop_network_time_sync() {
        if (sntp_started_) {
            esp_netif_sntp_deinit();
            sntp_started_ = false;
        }
    }

    void start_network_context_lookup() {
        if (network_context_task_ != nullptr ||
            context_lookup_attempted_ || !network_.connected() ||
            !live_approximate_location_.view().empty() ||
            voice_priority_.load(std::memory_order_acquire) ||
            speech_events_ == nullptr) {
            return;
        }
        context_lookup_attempted_ = true;
        context_cancellation_.reset();
        network_context_.clear();
        network_context_date_.clear();
        network_context_result_.store(
            agent::Error::model_failed, std::memory_order_release);
        xEventGroupClearBits(speech_events_, kNetworkContextDoneBit);

        // TLS needs the controller memory. A phone can reconnect after this
        // one short, optional lookup.
        if (!stop_ble()) {
            ESP_LOGW(
                kTag,
                "Optional IP context skipped because Bluetooth stayed active");
            return;
        }
        // This optional HTTPS worker does not use DMA from its stack. Keep its
        // fixed stack in PSRAM so Wi-Fi, I2S, and task control data keep enough
        // internal RAM for the worker to start after Bluetooth stops.
        const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
            network_context_task_entry, "network_context",
            kNetworkContextStackBytes, this, kNetworkContextPriority,
            &network_context_task_, 0,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (created != pdPASS) {
            network_context_task_ = nullptr;
            network_context_finished_at_ms_ = monotonic_ms();
            ESP_LOGW(
                kTag,
                "Optional IP context worker could not start; "
                "the lookup will not retry in this settings session");
            ensure_ble_started();
        }
    }

    void run_network_context_worker() {
        NetworkRequestGuard request_guard(network_);
        const agent::Error result = network_context_provider_.lookup(
            network_context_, network_context_date_, context_cancellation_);
        network_context_result_.store(result, std::memory_order_release);
        xEventGroupSetBits(speech_events_, kNetworkContextDoneBit);
        // The runtime task owns this WithCaps task and reclaims its PSRAM
        // stack after it observes the completion bit.
        vTaskSuspend(nullptr);
    }

    void finish_network_context_worker(bool wait) {
        if (network_context_task_ == nullptr) {
            return;
        }
        const EventBits_t bits = xEventGroupWaitBits(
            speech_events_, kNetworkContextDoneBit, pdFALSE, pdTRUE,
            wait ? portMAX_DELAY : 0);
        if ((bits & kNetworkContextDoneBit) == 0) {
            return;
        }
        TaskHandle_t completed_task = network_context_task_;
        network_context_task_ = nullptr;
        vTaskDeleteWithCaps(completed_task);
        network_context_finished_at_ms_ = monotonic_ms();
        network_context_transport_.reset_session();
        const agent::Error result = network_context_result_.load(
            std::memory_order_acquire);
        if (result == agent::Error::none &&
            live_approximate_location_.view().empty()) {
            if (!network_context_date_.value.empty()) {
                (void)utc_clock_.update_from_http_date(
                    network_context_date_.value.data(),
                    network_context_date_.value.size(),
                    network_context_date_.observed_at_ms);
            }
            (void)utc_clock_.set_utc_offset_minutes(
                network_context_.utc_offset_minutes);
            ESP_LOGI(kTag, "Optional network time context accepted");
            if (live_approximate_location_.assign(
                    std::string_view{
                        network_context_.approximate_location.data(),
                        network_context_.approximate_location.size()})) {
                chat_provider_.set_approximate_location(
                    live_approximate_location_.view());
            }
            refresh_clock(monotonic_ms(), true);
        } else if (result == agent::Error::cancelled) {
            context_lookup_attempted_ = false;
        } else if (result != agent::Error::none) {
            ESP_LOGW(
                kTag,
                "Optional IP context was not available (category %u)",
                static_cast<unsigned>(result));
        }
        network_context_.clear();
        network_context_date_.clear();
        // Let the idle task reclaim the worker stack before BLE starts again.
        ble_start_attempted_ = false;
    }

    void cancel_network_context_worker() {
        if (network_context_task_ == nullptr) {
            return;
        }
        context_cancellation_.cancel();
        network_context_transport_.cancel_active();
        finish_network_context_worker(true);
    }

    bool start_speech_worker(DeadlineCancellation &cancellation) {
        if (speech_events_ == nullptr || speech_task_ != nullptr ||
            !speech_channel_.start(cancellation)) {
            return false;
        }
        speech_segmenter_.reset();
        xEventGroupClearBits(speech_events_, kSpeechDoneBit);
        speech_cancellation_ = &cancellation;
        speech_result_.store(
            agent::Error::model_failed, std::memory_order_release);
        const BaseType_t created = xTaskCreatePinnedToCore(
            speech_task_entry, "tts_requests", kSpeechStackBytes, this,
            kSpeechPriority, &speech_task_, 1);
        if (created != pdPASS) {
            speech_task_ = nullptr;
            speech_cancellation_ = nullptr;
            speech_channel_.cancel();
            return false;
        }
        return true;
    }

    void run_speech_worker() {
        agent::Error result = agent::Error::model_failed;
        if (speech_cancellation_ != nullptr) {
            SpeechStartSink speech_sink(pcm_sink_, *agent_loop_);
            result = speech_provider_.speak_segments(
                speech_channel_, speech_sink, *speech_cancellation_);
        }
        speech_result_.store(result, std::memory_order_release);
        xEventGroupSetBits(speech_events_, kSpeechDoneBit);
        vTaskDelete(nullptr);
    }

    agent::Error wait_for_speech_worker() {
        if (speech_task_ == nullptr) {
            return speech_result_.load(std::memory_order_acquire);
        }
        while (true) {
            const EventBits_t bits = xEventGroupWaitBits(
                speech_events_, kSpeechDoneBit, pdFALSE, pdFALSE,
                pdMS_TO_TICKS(20));
            if ((bits & kSpeechDoneBit) != 0) {
                break;
            }
            if (cancellation_.cancelled()) {
                speech_channel_.cancel();
            }
        }
        speech_task_ = nullptr;
        speech_cancellation_ = nullptr;
        speech_channel_.reset();
        return speech_result_.load(std::memory_order_acquire);
    }

    static void quick_controls_callback(
        const ui::QuickControlsUpdate &update, void *context) {
        auto *self = static_cast<Impl *>(context);
        const runtime::DevicePreferences preferences{
            update.brightness_percent, update.volume_percent};
        if (self == nullptr || !preferences.valid()) {
            return;
        }
        if (update.brightness_changed) {
            self->pending_brightness_percent_.store(
                update.brightness_percent, std::memory_order_release);
            self->quick_controls_brightness_pending_.store(
                true, std::memory_order_release);
        }
        if (update.volume_changed) {
            // Volume updates use atomic state only. Keep them immediate while
            // a model or speech operation owns the main runtime task.
            if (self->device_control_.preview_volume(update.volume_percent) ==
                agent::Error::none) {
                (void)self->playback_.set_volume(update.volume_percent);
            }
            self->pending_volume_percent_.store(
                update.volume_percent, std::memory_order_release);
            self->quick_controls_volume_pending_.store(
                true, std::memory_order_release);
        }
        if (update.commit) {
            self->quick_controls_commit_pending_.store(
                true, std::memory_order_release);
        }
        self->quick_controls_update_pending_.store(
            true, std::memory_order_release);
    }

    static void passkey_callback(
        std::uint32_t passkey, bool visible, void *context) {
        auto *self = static_cast<Impl *>(context);
        if (self == nullptr || self->passkey_queue_ == nullptr) {
            return;
        }
        const PasskeyEvent event{passkey, visible};
        xQueueOverwrite(self->passkey_queue_, &event);
        if (visible) {
            const Command activity{
                CommandKind::provisioning_activity, monotonic_ms()};
            self->queue_command(activity);
        }
    }

    static void device_context_callback(
        std::uint64_t epoch_seconds,
        std::int16_t utc_offset_minutes,
        const char *approximate_location,
        std::size_t approximate_location_size,
        void *context) {
        auto *self = static_cast<Impl *>(context);
        if (self == nullptr || self->device_context_queue_ == nullptr ||
            approximate_location_size > provisioning::kMaximumApproximateLocationSize ||
            (approximate_location == nullptr && approximate_location_size != 0)) {
            return;
        }
        DeviceContextEvent event;
        event.epoch_seconds = epoch_seconds;
        event.utc_offset_minutes = utc_offset_minutes;
        event.observed_at_ms = monotonic_ms();
        event.approximate_location_size =
            static_cast<std::uint8_t>(approximate_location_size);
        if (approximate_location_size != 0) {
            std::memcpy(
                event.approximate_location.data(), approximate_location,
                approximate_location_size);
        }
        xQueueOverwrite(self->device_context_queue_, &event);
    }

    template <typename Callback>
    bool with_display(Callback callback) {
        if (bsp_display_lock(100)) {
            callback();
            lv_refr_now(nullptr);
            bsp_display_unlock();
            return true;
        }
        return false;
    }

    void show_state(InteractionState state) {
        with_display([this, state]() {
            ui::sync_quick_controls(
                device_control_.brightness_percent(),
                device_control_.volume_percent());
            ui::show_state(state);
        });
    }

    void refresh_clock(std::uint32_t now_ms, bool force = false) {
        if (app_mode_.load(std::memory_order_acquire) != AppMode::clock ||
            !display_available_.load(std::memory_order_acquire)) {
            return;
        }
        agent::LocalTimeOfDay local;
        const bool available = utc_clock_.current_local_time(now_ms, local);
        if (!force && available == clock_time_available_ &&
            (!available || local.second == clock_refreshed_second_)) {
            return;
        }
        clock_time_available_ = available;
        clock_refreshed_second_ = available ? local.second : 0xff;
        with_display([available, local]() {
            ui::show_clock_time(
                available,
                ClockTime{local.hour, local.minute, local.second});
        });
    }

    bool apply_mode_display(AppMode mode, InteractionState chat_state) {
        const bool applied = with_display([this, mode, chat_state]() {
            ui::show_app_mode(mode, chat_state);
            ui::sync_quick_controls(
                device_control_.brightness_percent(),
                device_control_.volume_percent());
        });
        mode_display_pending_ = !applied;
        return applied;
    }

    void retry_mode_display(std::uint32_t now_ms) {
        if (!mode_display_pending_ ||
            !display_available_.load(std::memory_order_acquire) ||
            now_ms - mode_display_attempted_at_ms_ < kModeDisplayRetryMs) {
            return;
        }
        mode_display_attempted_at_ms_ = now_ms;
        (void)apply_mode_display(
            app_mode_.load(std::memory_order_acquire), interaction_.state());
    }

    void enter_clock_mode(std::uint32_t now_ms) {
        if (!display_available_.load(std::memory_order_acquire)) {
            return;
        }
        app_mode_.store(AppMode::clock, std::memory_order_release);
        clock_return_pending_ = false;
        interaction_.ready(now_ms);
        previous_state_ = interaction_.state();
        agent_loop_->clear_thread();
        image_tool_.clear_results();
        python_tool_.clear_plot();
        selected_image_result_.clear();
        image_frame_.reset();
        pending_plot_.clear();
        clock_network_stop_pending_ =
            !utc_clock_.has_local_time() &&
            settings_.has_wifi_credentials();
        clock_network_stop_started_at_ms_ = now_ms;
        if (clock_network_stop_pending_) {
            // Clock can become active before the asynchronous startup
            // connection has supplied UTC and a timezone. Keep that bounded
            // acquisition alive, then stop Wi-Fi from the runtime loop.
            start_network_early(true);
        } else {
            stop_clock_network();
        }
        footer_shown_ = false;
        clock_refreshed_second_ = 0xff;
        clock_time_available_ = false;
        mode_display_attempted_at_ms_ = now_ms;
        (void)apply_mode_display(AppMode::clock, interaction_.state());
        refresh_clock(now_ms, true);
        ESP_LOGI(kTag, "Clock mode is active");
    }

    void enter_chat_mode(std::uint32_t now_ms, bool return_to_clock) {
        if (!display_available_.load(std::memory_order_acquire)) {
            return;
        }
        app_mode_.store(AppMode::chat, std::memory_order_release);
        clock_network_stop_pending_ = false;
        clock_return_pending_ = return_to_clock;
        interaction_.ready(now_ms);
        previous_state_ = interaction_.state();
        mode_display_attempted_at_ms_ = now_ms;
        (void)apply_mode_display(AppMode::chat, interaction_.state());
        footer_shown_ = false;
        refresh_footer(now_ms, true);
        if (!return_to_clock) {
            start_network_early(true);
        }
        ESP_LOGI(kTag, "ChatESP mode is active");
    }

    void show_error(const char *message) {
        with_display([message]() { ui::show_error(message); });
    }

    void show_model_progress(const char *message) {
        with_display(
            [message]() { ui::show_model_progress(message); });
    }

    void hide_visual() {
        with_display([]() { ui::hide_fullscreen_visual(); });
    }

    static ui::WifiIndicator wifi_indicator(
        network::NetworkState state, bool configured) {
        if (!configured) {
            return ui::WifiIndicator::setup;
        }
        switch (state) {
            case network::NetworkState::off:
                return ui::WifiIndicator::off;
            case network::NetworkState::connecting:
                return ui::WifiIndicator::connecting;
            case network::NetworkState::connected:
                return ui::WifiIndicator::online;
            case network::NetworkState::failed:
                return ui::WifiIndicator::failed;
        }
        return ui::WifiIndicator::failed;
    }

    void draw_footer(ui::WifiIndicator wifi) {
        if (!display_available_.load(std::memory_order_acquire)) {
            return;
        }
        if (!with_display([this, wifi]() {
            ui::show_footer(
                wifi,
                battery_percent_.has_value(),
                battery_percent_.value_or(0));
        })) {
            return;
        }
        shown_wifi_ = wifi;
        shown_battery_percent_ = battery_percent_;
        footer_shown_ = true;
    }

    void refresh_footer(std::uint32_t now_ms, bool force) {
        const bool battery_due = force || !battery_checked_ ||
            now_ms - battery_checked_at_ms_ >= kBatteryRefreshMs;
        if (battery_due &&
            !button_pressed_.load(std::memory_order_acquire) &&
            !voice_priority_.load(std::memory_order_acquire)) {
            battery_percent_ = power::battery_percent();
            battery_checked_ = true;
            battery_checked_at_ms_ = now_ms;
        }
        const ui::WifiIndicator wifi = wifi_indicator(
            network_.state(), settings_.has_wifi_credentials());
        if (force || !footer_shown_ || wifi != shown_wifi_ ||
            battery_percent_ != shown_battery_percent_) {
            draw_footer(wifi);
        }
    }

    void queue_command(const Command &command) {
        if (command_queue_ != nullptr &&
            xQueueSend(command_queue_, &command, 0) != pdTRUE) {
            queue_overflow_.store(true, std::memory_order_release);
        }
    }

    void run_passkey_ui() {
        PasskeyEvent current{};
        PasskeyEvent shown{};
        bool has_shown = false;
        while (true) {
            PasskeyEvent event;
            if (xQueueReceive(
                    passkey_queue_, &event, pdMS_TO_TICKS(40)) == pdTRUE) {
                current = event;
            }
            const bool visible = current.visible &&
                display_available_.load(std::memory_order_acquire) &&
                !voice_priority_.load(std::memory_order_acquire);
            const PasskeyEvent desired{current.passkey, visible};
            if (!has_shown || desired.passkey != shown.passkey ||
                desired.visible != shown.visible) {
                if (bsp_display_lock(100)) {
                    ui::show_ble_passkey(
                        desired.passkey, desired.visible);
                    bsp_display_unlock();
                    shown = desired;
                    has_shown = true;
                }
            }
        }
    }

    void process_quick_controls(std::uint32_t now_ms) {
        const bool update = quick_controls_update_pending_.exchange(
            false, std::memory_order_acq_rel);
        const bool brightness_update =
            quick_controls_brightness_pending_.exchange(
                false, std::memory_order_acq_rel);
        const bool volume_update = quick_controls_volume_pending_.exchange(
            false, std::memory_order_acq_rel);
        const bool commit_requested =
            quick_controls_commit_pending_.load(std::memory_order_acquire);
        if (!update && !brightness_update && !volume_update &&
            !commit_requested) {
            return;
        }
        const bool can_commit = quick_controls_can_persist(
            voice_priority_.load(std::memory_order_acquire),
            button_pressed_.load(std::memory_order_acquire),
            interaction_.state() == InteractionState::sleep_pending);
        if (!update && !brightness_update && !volume_update && !can_commit) {
            return;
        }

        const std::uint8_t brightness =
            pending_brightness_percent_.load(std::memory_order_acquire);
        const std::uint8_t volume =
            pending_volume_percent_.load(std::memory_order_acquire);
        bool values_match = true;
        bool brightness_deferred = false;
        const runtime::DevicePreferences requested{brightness, volume};
        values_match = requested.valid();
        if (values_match && brightness_update) {
            bool brightness_applied = false;
            if (bsp_display_lock(100)) {
                brightness_applied =
                    device_control_.preview_brightness(brightness) ==
                    agent::Error::none;
                bsp_display_unlock();
            } else {
                // A display refresh can own the lock during a drag. Keep the
                // latest request and try it again. Do not move the control
                // back to its old value for this temporary condition.
                quick_controls_brightness_pending_.store(
                    true, std::memory_order_release);
                brightness_deferred = true;
            }
            if (!brightness_applied && !brightness_deferred) {
                values_match = false;
            }
        }
        if (values_match && volume_update) {
            if (device_control_.preview_volume(volume) != agent::Error::none ||
                playback_.set_volume(volume) != ESP_OK) {
                values_match = false;
            }
        }
        if (update) {
            interaction_.note_idle_activity(now_ms);
        }

        if (!values_match) {
            with_display([this]() {
                ui::sync_quick_controls(
                    device_control_.brightness_percent(),
                    device_control_.volume_percent());
            });
        }
        if (commit_requested && values_match && !brightness_deferred &&
            can_commit &&
            quick_controls_commit_pending_.exchange(
                false, std::memory_order_acq_rel)) {
            (void)device_control_.persist_preferences();
        }
    }

    void run() {
        // Replace the full-boot splash when this task can accept input. Do not
        // add a splash timer because it would delay hold-to-talk.
        interaction_.ready(monotonic_ms());
        previous_state_ = interaction_.state();
        if (button_pressed_.load(std::memory_order_acquire)) {
            Command startup_command;
            if (xQueueReceive(
                    command_queue_, &startup_command, 0) == pdTRUE) {
                process_command(startup_command);
            } else {
                wake_for_button(monotonic_ms());
            }
        } else {
            show_state(previous_state_);
            refresh_footer(monotonic_ms(), true);
            start_network_early(true);
        }

        while (true) {
            if (queue_overflow_.exchange(false, std::memory_order_acq_rel)) {
                fail("BUTTON INPUT WAS TOO FAST");
            }

            Command command;
            if (xQueueReceive(
                    command_queue_, &command, pdMS_TO_TICKS(5)) == pdTRUE) {
                process_command(command);
            }

            DeviceContextEvent device_context;
            if (xQueueReceive(
                    device_context_queue_, &device_context, 0) == pdTRUE) {
                apply_device_context(device_context);
            }

            const std::uint32_t now_ms = monotonic_ms();
            process_quick_controls(now_ms);
            retry_mode_display(now_ms);
            const network::NetworkState network_state = network_.state();
            if (network_state != last_network_state_) {
                if (last_network_state_ ==
                        network::NetworkState::connected &&
                    network_state != network::NetworkState::connected) {
                    cancel_network_context_worker();
                    stop_network_time_sync();
                    reset_transports();
                }
                last_network_state_ = network_state;
            }
            if (network_state == network::NetworkState::connected) {
                start_network_time_sync();
                start_network_context_lookup();
            }
            poll_network_time_sync(now_ms);
            finish_network_context_worker(false);
            const AppMode app_mode =
                app_mode_.load(std::memory_order_acquire);
            if (app_mode == AppMode::clock) {
                // Clock mode is a continuous display mode. It does not use the
                // normal ChatESP idle sleep timer.
                interaction_.note_idle_activity(now_ms);
                refresh_clock(now_ms);
                if (clock_network_shutdown_due(
                        clock_network_stop_pending_,
                        utc_clock_.has_local_time(),
                        now_ms - clock_network_stop_started_at_ms_,
                        kClockTimeSyncLimitMs)) {
                    stop_clock_network();
                }
            } else if (clock_return_due(
                           app_mode, clock_return_pending_,
                           interaction_.state() == InteractionState::idle,
                           interaction_.inactivity_ms(now_ms),
                           kClockReturnDelayMs)) {
                enter_clock_mode(now_ms);
            }
            interaction_.tick(now_ms);
            process_state_change(now_ms);
            const bool recording =
                interaction_.state() == InteractionState::recording;
            if (recording) {
                capture_audio(now_ms);
            }
            if (!voice_priority_.load(std::memory_order_acquire)) {
                retry_display_wake(now_ms);
            }
            retry_display_sleep(now_ms);
            if (app_mode_.load(std::memory_order_acquire) == AppMode::chat &&
                !voice_priority_.load(std::memory_order_acquire) &&
                now_ms - footer_checked_at_ms_ >= kFooterRefreshMs) {
                footer_checked_at_ms_ = now_ms;
                refresh_footer(now_ms, false);
            }
            if (!recording &&
                interaction_.state() == InteractionState::idle &&
                now_ms - settings_checked_at_ms_ >=
                    kSettingsRefreshMs) {
                settings_checked_at_ms_ = now_ms;
                if (ble_started_ && !ble_provisioning::running()) {
                    ble_started_ = false;
                    ble_start_attempted_ = false;
                }
                if (!interaction_.button_is_down() && !ble_started_ &&
                    !ble_start_attempted_ &&
                    network_context_task_ == nullptr &&
                    now_ms - network_context_finished_at_ms_ >=
                        kBleRestartAfterWorkerMs) {
                    ensure_ble_started();
                }
                refresh_settings();
                if (!interaction_.button_is_down() &&
                    app_mode_.load(std::memory_order_acquire) ==
                        AppMode::chat) {
                    start_network_early(false);
                }
            }
        }
    }

    void process_command(const Command &command) {
        if (command.kind == CommandKind::poweroff_failed) {
            recover_poweroff(command.at_ms);
            return;
        }
        if (command.kind == CommandKind::provisioning_activity) {
            if (!display_available_.load(std::memory_order_acquire)) {
                display_available_.store(true, std::memory_order_release);
                display_sleep_pending_ = false;
                poweroff_gate_.recover();
                interaction_.ready(command.at_ms);
                previous_state_ = interaction_.state();
                request_display_wake(command.at_ms);
            }
            interaction_.note_idle_activity(command.at_ms);
            return;
        }
        if (command.kind == CommandKind::toggle_mode) {
            if (!display_available_.load(std::memory_order_acquire) ||
                interaction_.state() == InteractionState::sleep_pending) {
                return;
            }
            if (app_mode_.load(std::memory_order_acquire) == AppMode::clock) {
                enter_chat_mode(command.at_ms, false);
            } else {
                if (interaction_.state() != InteractionState::idle) {
                    cancel_current();
                }
                enter_clock_mode(command.at_ms);
            }
            return;
        }
        if ((command.kind == CommandKind::button_down ||
             command.kind == CommandKind::wake_button_down ||
             command.kind == CommandKind::wake_from_poweroff) &&
            app_mode_.load(std::memory_order_acquire) == AppMode::clock) {
            // The bottom button always returns to ChatESP before its normal
            // short-press or hold-to-talk state handling continues.
            enter_chat_mode(command.at_ms, true);
        }
        if (command.kind == CommandKind::wake_button_down ||
            command.kind == CommandKind::wake_from_poweroff ||
            (command.kind == CommandKind::button_down &&
             interaction_.state() == InteractionState::sleep_pending)) {
            ESP_LOGI(kTag, "Action button requested display wake");
            wake_for_button(command.at_ms);
            return;
        }
        if (command.kind == CommandKind::button_down) {
            ESP_LOGI(kTag, "Action button started a voice hold");
            interaction_.button_down(command.at_ms);
            // Reassert only the panel command here. Full display work must not
            // delay the button state machine or microphone capture.
            const esp_err_t panel_error = ui::reassert_panel(
                device_control_.brightness_percent());
            if (panel_error != ESP_OK) {
                display_wake_pending_ = true;
                display_wake_attempted_at_ms_ = command.at_ms;
            }
        } else if (command.kind == CommandKind::button_up) {
            ESP_LOGI(kTag, "Action button was released");
            timing_.reset(command.at_ms);
            interaction_.button_up(command.at_ms);
            if (interaction_.state() == InteractionState::idle) {
                voice_priority_.store(false, std::memory_order_release);
            }
        }
    }

    void apply_device_context(const DeviceContextEvent &context) {
        if (!utc_clock_.update_from_epoch_seconds(
                context.epoch_seconds, context.utc_offset_minutes,
                context.observed_at_ms) ||
            !live_approximate_location_.assign(
                std::string_view{
                    context.approximate_location.data(),
                    context.approximate_location_size})) {
            return;
        }
        const std::string_view location = live_approximate_location_.view();
        chat_provider_.set_approximate_location(
            location.empty() ? settings_.location() : location);
        refresh_clock(context.observed_at_ms, true);
    }

    void start_network_early(bool force) {
        if (!settings_.has_wifi_credentials() || network_.connected() ||
            network_.connecting()) {
            return;
        }
        const std::uint32_t now_ms = monotonic_ms();
        if (!force && early_connect_attempted_at_ms_ != 0 &&
            now_ms - early_connect_attempted_at_ms_ <
                kEarlyConnectRetryMs) {
            return;
        }
        early_connect_attempted_at_ms_ = now_ms;
        if (!network_initialized_) {
            const esp_err_t initialize_error = network_.initialize();
            if (initialize_error != ESP_OK) {
                ESP_LOGE(
                    kTag,
                    "Early Wi-Fi initialize failed (category %s)",
                    esp_err_to_name(initialize_error));
                return;
            }
            network_initialized_ = true;
        }
        const agent::Error connect_error =
            network_.start_connect(settings_.wifi());
        if (connect_error != agent::Error::none) {
            ESP_LOGW(
                kTag,
                "Early Wi-Fi connect did not start (category %u)",
                static_cast<unsigned>(connect_error));
        }
    }

    void wake_for_button(std::uint32_t now_ms) {
        display_available_.store(true, std::memory_order_release);
        display_sleep_pending_ = false;
        poweroff_gate_.recover();
        interaction_.ready(now_ms);
        interaction_.wake_button_down(now_ms);
        previous_state_ = interaction_.state();
        request_display_wake(now_ms);
    }

    void request_display_wake(std::uint32_t now_ms) {
        const esp_err_t result = ui::wake(
            interaction_.state(), device_control_.brightness_percent(),
            app_mode_.load(std::memory_order_acquire));
        display_wake_pending_ = result != ESP_OK;
        display_wake_attempted_at_ms_ = now_ms;
        if (display_wake_pending_) {
            ESP_LOGW(
                kTag,
                "Display wake will retry (category %s)",
                esp_err_to_name(result));
        } else {
            footer_shown_ = false;
            refresh_footer(now_ms, true);
        }
    }

    void retry_display_wake(std::uint32_t now_ms) {
        if (!display_wake_pending_ ||
            now_ms - display_wake_attempted_at_ms_ < kDisplayWakeRetryMs) {
            return;
        }
        request_display_wake(now_ms);
    }

    void retry_display_sleep(std::uint32_t now_ms) {
        if (!display_sleep_pending_ ||
            now_ms - display_sleep_attempted_at_ms_ <
                kDisplaySleepRetryMs) {
            return;
        }
        display_sleep_attempted_at_ms_ = now_ms;
        if (ui::sleep() == ESP_OK) {
            display_sleep_pending_ = false;
        }
    }

    void process_state_change(std::uint32_t now_ms) {
        if (interaction_.state() == previous_state_) {
            return;
        }
        const InteractionState prior = previous_state_;
        previous_state_ = interaction_.state();
        if (previous_state_ == InteractionState::sleep_pending) {
            if (request_active_) {
                ESP_LOGW(kTag, "Sleep was blocked during active work");
                interaction_.ready(now_ms);
                previous_state_ = interaction_.state();
                return;
            }
            poweroff_gate_.begin_sleep();
            enter_sleep();
            return;
        }

        if (previous_state_ == InteractionState::recording) {
            begin_recording(now_ms);
            if (interaction_.state() == InteractionState::recording) {
                show_state(previous_state_);
            }
            return;
        }

        show_state(previous_state_);
        if (prior == InteractionState::recording &&
            previous_state_ == InteractionState::transcribing) {
            request_active_ = true;
            finish_recording_and_request();
            request_active_ = false;
        }
    }

    void begin_recording(std::uint32_t now_ms) {
        // Every completed voice attempt returns to the travel clock after the
        // final 30-second follow-up window.
        clock_return_pending_ = true;
        pcm_sink_.cancel_and_stop();
        capture_.discard();
        python_tool_.clear_plot();
        pending_plot_.clear();
        cancellation_.reset();
        if (capture_.start() != ESP_OK) {
            fail("MICROPHONE COULD NOT START");
            return;
        }
        // Do not start radio work between microphone start and audio reads.
        // A held cold start connects after release. Normal awake sessions
        // already have an asynchronous connection.
        level_refreshed_at_ms_ = now_ms;
    }

    void capture_audio(std::uint32_t now_ms) {
        const esp_err_t result = capture_.capture_chunk();
        if (result != ESP_OK) {
            if (cancellation_.cancelled()) {
                cancel_current();
            } else if (capture_.sample_count() >=
                       AudioCapture::kMaximumSamples) {
                timing_.reset(now_ms);
                interaction_.button_up(now_ms);
                process_state_change(now_ms);
            } else {
                fail("MICROPHONE READ FAILED");
            }
            return;
        }
        if (capture_.sample_count() >= kMinimumRecordingSamples) {
            start_network_during_recording();
        }
        if (now_ms - level_refreshed_at_ms_ >= kLevelRefreshMs) {
            level_refreshed_at_ms_ = now_ms;
            const AudioSpectrum spectrum = pcm_frequency_spectrum(
                capture_.samples(),
                capture_.sample_count(),
                AudioCapture::kChunkSamples);
            with_display(
                [&spectrum]() { ui::show_recording_spectrum(spectrum); });
        }
    }

    void finish_recording_and_request() {
        if (capture_.stop() != ESP_OK) {
            capture_.discard();
            fail("MICROPHONE STOP FAILED");
            return;
        }
        if (capture_.sample_count() < kMinimumRecordingSamples) {
            capture_.discard();
            fail("HOLD THE BUTTON A LITTLE LONGER");
            return;
        }
        if (!settings_.has_wifi_credentials()) {
            capture_.discard();
            fail("WI-FI IS NOT CONFIGURED");
            return;
        }
        if (!settings_.has_service_credentials()) {
            capture_.discard();
            fail("THE SERVICE KEY IS NOT CONFIGURED");
            return;
        }
        DeadlineCancellation request_cancellation(
            cancellation_, monotonic_ms(), kInteractionTimeoutMs);

        join_network_warm_worker();
        finish_network_context_worker(true);
        if (!stop_ble_for_request()) {
            capture_.discard();
            fail("BLUETOOTH COULD NOT STOP");
            return;
        }

        if (!network_initialized_) {
            const esp_err_t network_result = network_.initialize();
            if (network_result != ESP_OK) {
                ESP_LOGE(
                    kTag,
                    "Wi-Fi initialize failed (category %s)",
                    esp_err_to_name(network_result));
                capture_.discard();
                fail("WI-FI COULD NOT START");
                return;
            }
            network_initialized_ = true;
        }
        NetworkRequestGuard network_request(network_);
        if (!network_.connected()) {
            draw_footer(ui::WifiIndicator::connecting);
        }
        agent::Error error = network_.connect(
            settings_.wifi(), request_cancellation);
        refresh_footer(monotonic_ms(), true);
        error = request_cancellation.normalize(error);
        if (error != agent::Error::none) {
            capture_.discard();
            finish_with_error(error);
            return;
        }
        timing_.mark(runtime::TurnPhase::network_ready, monotonic_ms());
        timing_.set_rssi_band(network_.rssi_band());
        show_state(InteractionState::transcribing);

        auto &transcript = request_scratch_->transcript;
        transcript.clear();
        SecureTextGuard transcript_guard(transcript);
        const agent::AudioView audio{
            reinterpret_cast<const std::uint8_t *>(capture_.samples()),
            capture_.sample_count() * sizeof(std::int16_t),
            AudioCapture::kSampleRateHz,
            AudioCapture::kChannelCount,
            AudioCapture::kBitsPerSample,
        };
        timing_.mark(runtime::TurnPhase::stt_start, monotonic_ms());
        error = transcription_provider_.transcribe(
            audio, transcript, request_cancellation);
        timing_.mark(runtime::TurnPhase::stt_finish, monotonic_ms());
        error = request_cancellation.normalize(error);
        capture_.discard();
        if (error != agent::Error::none) {
            finish_with_error(error);
            return;
        }
        if (transcript.empty()) {
            fail("I DID NOT HEAR SPEECH");
            return;
        }

        with_display([&transcript]() {
            ui::show_transcript(
                {transcript.data(), transcript.size()});
        });

        auto &answer = request_scratch_->answer;
        answer.clear();
        request_scratch_->stream_answer.clear();
        stream_text_shown_ = false;
        stream_text_refreshed_at_ms_ = 0;
        SecureTextGuard answer_guard(answer);
        SecureTextGuard stream_answer_guard(
            request_scratch_->stream_answer);
        speech_provider_.set_language(agent::SpeechLanguage::english);
        if (!start_speech_worker(request_cancellation)) {
            fail("SPEECH PIPELINE COULD NOT START");
            return;
        }
        memory_store_.clear_turn_state();
        error = agent_loop_->run(
            transcript.data(), transcript.size(), answer,
            request_cancellation);
        memory_store_.clear_turn_state();
        image_tool_.clear_results();
        python_tool_.clear_plot();
        error = request_cancellation.normalize(error);
        if (error != agent::Error::none) {
            image_transport_.cancel_active();
            speech_segmenter_.reset();
            speech_channel_.discard_pending_and_finish();
            const agent::Error speech_error = wait_for_speech_worker();
            (void)speech_error;
            join_image_worker();
            image_frame_.reset();
            pending_plot_.clear();
            if (device_control_.power_off_pending() &&
                !cancellation_.cancelled()) {
                finish_model_power_off();
                return;
            }
            if (error == agent::Error::cancelled ||
                cancellation_.cancelled()) {
                cancel_current();
                return;
            }
            auto &partial = request_scratch_->stream_answer;
            if (!partial.empty()) {
                with_display([&partial]() {
                    ui::show_answer_notice(
                        {partial.data(), partial.size()},
                        "ANSWER INTERRUPTED");
                });
                interaction_.interaction_finished(monotonic_ms());
                previous_state_ = interaction_.state();
                voice_priority_.store(false, std::memory_order_release);
                timing_.mark(
                    runtime::TurnPhase::completion, monotonic_ms());
                log_turn_timing();
            } else {
                finish_with_error(error);
            }
            return;
        }

        if (!speech_segmenter_.finish(speech_channel_)) {
            speech_channel_.cancel();
            const agent::Error speech_error = wait_for_speech_worker();
            (void)speech_error;
            if (device_control_.power_off_pending() &&
                !cancellation_.cancelled()) {
                finish_model_power_off();
                return;
            }
            fail("SPEECH PIPELINE FAILED");
            return;
        }
        speech_channel_.finish();

        with_display([&answer]() {
            ui::show_answer({answer.data(), answer.size()});
        });

        error = request_cancellation.normalize(wait_for_speech_worker());
        timing_.mark(runtime::TurnPhase::playback_finish, monotonic_ms());
        join_image_worker();
        bool speech_failed = false;
        if (error != agent::Error::none) {
            pcm_sink_.cancel_and_stop();
            if (error == agent::Error::cancelled ||
                cancellation_.cancelled()) {
                cancel_current();
                return;
            }
            speech_failed = true;
            ESP_LOGW(
                kTag,
                "Speech output failed (category %u)",
                static_cast<unsigned>(error));
            const char *speech_notice = speech_error_message(error);
            with_display([&answer, speech_notice]() {
                ui::show_answer_notice(
                    {answer.data(), answer.size()}, speech_notice);
            });
        }

        if (cancellation_.cancelled()) {
            cancel_current();
            return;
        }

        if (device_control_.power_off_pending()) {
            finish_model_power_off();
            return;
        }

        interaction_.interaction_finished(monotonic_ms());
        previous_state_ = interaction_.state();
        if (!speech_failed) {
            show_state(previous_state_);
        }
        publish_selected_visual(image_frame_, pending_plot_);
        timing_.mark(runtime::TurnPhase::completion, monotonic_ms());
        log_turn_timing();
        voice_priority_.store(false, std::memory_order_release);
        ESP_LOGI(kTag, "Interaction complete; idle timer started");
        ESP_LOGI(
            kTag,
            "Runtime stack minimum free bytes: %u",
            static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    }

    void log_turn_timing() {
        std::array<char, 256> summary{};
        if (timing_.format_summary(summary.data(), summary.size())) {
            ESP_LOGI(kTag, "%s", summary.data());
        }
    }

    void publish_selected_visual(
        image::Rgb565Frame &frame, agent::PlotData &plot) {
        if (plot.ready()) {
            bool shown = false;
            with_display([&plot, &shown]() {
                shown = ui::show_fullscreen_plot(plot);
            });
            plot.clear();
            frame.reset();
            if (!shown) {
                ESP_LOGW(kTag, "The bounded plot could not be shown");
            } else {
                ESP_LOGI(kTag, "The bounded plot is on the display");
            }
            return;
        }
        if (!frame.available()) {
            return;
        }
        bool shown = false;
        with_display([&frame, &shown]() {
            shown = ui::show_fullscreen_image(std::move(frame));
        });
        if (!shown) {
            ESP_LOGW(kTag, "Optional image could not be shown");
        } else {
            ESP_LOGI(kTag, "The bounded image is on the display");
        }
    }

    void finish_model_power_off() {
        image_transport_.cancel_active();
        join_image_worker();
        selected_image_result_.clear();
        image_frame_.reset();
        python_tool_.clear_plot();
        pending_plot_.clear();
        with_display([]() {
            ui::show_answer_notice("TURNING OFF", "POWER OFF");
        });
        interaction_.cancel_for_sleep();
        timing_.mark(runtime::TurnPhase::completion, monotonic_ms());
        log_turn_timing();
        voice_priority_.store(false, std::memory_order_release);
        ESP_LOGI(kTag, "The model scheduled device power-off");
    }

    void finish_with_error(agent::Error error) {
        if (error == agent::Error::cancelled || cancellation_.cancelled()) {
            cancel_current();
        } else if (
            (error == agent::Error::connect_timeout ||
             error == agent::Error::disconnected) &&
            network_.connected()) {
            fail("SERVICE CONNECTION FAILED");
        } else {
            fail(request_error_message(error));
        }
    }

    void cancel_current() {
        device_control_.cancel_power_off();
        memory_store_.clear_turn_state();
        capture_.discard();
        speech_channel_.cancel();
        pcm_sink_.cancel_and_stop();
        image_tool_.clear_results();
        python_tool_.clear_plot();
        selected_image_result_.clear();
        image_frame_.reset();
        pending_plot_.clear();
        hide_visual();
        interaction_.ready(monotonic_ms());
        previous_state_ = interaction_.state();
        show_state(previous_state_);
        voice_priority_.store(false, std::memory_order_release);
    }

    void fail(const char *message) {
        device_control_.cancel_power_off();
        memory_store_.clear_turn_state();
        capture_.discard();
        speech_channel_.cancel();
        pcm_sink_.cancel_and_stop();
        image_tool_.clear_results();
        python_tool_.clear_plot();
        selected_image_result_.clear();
        image_frame_.reset();
        pending_plot_.clear();
        interaction_.fail(monotonic_ms());
        previous_state_ = interaction_.state();
        show_error(message);
        voice_priority_.store(false, std::memory_order_release);
    }

    void refresh_settings() {
        const provisioning::StoredVersion version =
            settings_store_.stored_version();
        if (!version.present || version.revision == applied_revision_) {
            return;
        }
        provisioning::SettingsRecord record;
        if (!settings_store_.read(&record) || !settings_.load(record)) {
            return;
        }
        applied_revision_ = version.revision;
        openrouter_connection_ = settings_.openrouter();
        brave_key_ = settings_.brave();
        chat_provider_.set_connection(openrouter_connection_);
        const std::string_view live_location = live_approximate_location_.view();
        chat_provider_.set_approximate_location(
            live_location.empty() ? settings_.location() : live_location);
        transcription_provider_.set_connection(openrouter_connection_);
        speech_provider_.set_connection(openrouter_connection_);
        web_provider_.set_api_key(brave_key_);
        image_provider_.set_api_key(brave_key_);
        cancel_network_context_worker();
        stop_network_time_sync();
        context_lookup_attempted_ = false;
        if (!sntp_complete_) {
            sntp_start_attempted_ = false;
        }
        if (network_initialized_) {
            network_.disconnect();
        }
        reset_transports();
        agent_loop_->clear_thread();
        image_tool_.clear_results();
        python_tool_.clear_plot();
        selected_image_result_.clear();
        image_frame_.reset();
        pending_plot_.clear();
        hide_visual();
        show_state(interaction_.state());
        if (!interaction_.button_is_down() &&
            app_mode_.load(std::memory_order_acquire) == AppMode::chat) {
            start_network_early(true);
        }
    }

    void enter_sleep() {
        device_control_.cancel_power_off();
        memory_store_.clear_turn_state();
        display_available_.store(false, std::memory_order_release);
        app_mode_.store(AppMode::chat, std::memory_order_release);
        clock_network_stop_pending_ = false;
        clock_return_pending_ = false;
        mode_display_pending_ = false;
        display_wake_pending_ = false;
        footer_shown_ = false;
        // Turn the AMOLED off before network and peripheral cleanup.
        esp_err_t display_sleep_result = ESP_ERR_TIMEOUT;
        for (std::uint8_t attempt = 0;
             attempt < 3 && display_sleep_result != ESP_OK;
             ++attempt) {
            display_sleep_result = ui::sleep();
            if (display_sleep_result != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        if (display_sleep_result != ESP_OK) {
            ESP_LOGE(
                kTag,
                "Display sleep failed (category %s)",
                esp_err_to_name(display_sleep_result));
        }
        display_sleep_pending_ = display_sleep_result != ESP_OK;
        display_sleep_attempted_at_ms_ = monotonic_ms();
        if (quick_controls_commit_pending_.exchange(
                false, std::memory_order_acq_rel)) {
            (void)device_control_.persist_preferences();
        }
        cancellation_.cancel();
        context_cancellation_.cancel();
        speech_channel_.cancel();
        cancel_transports();
        cancel_network_context_worker();
        stop_network_time_sync();
        capture_.discard();
        pcm_sink_.cancel_and_stop();
        agent_loop_->clear_thread();
        image_tool_.clear_results();
        python_tool_.clear_plot();
        pending_plot_.clear();
        hide_visual();
        if (network_initialized_) {
            network_.shutdown();
            network_initialized_ = false;
        }
        reset_transports();
        early_connect_attempted_at_ms_ = 0;
        const std::uint32_t grace_started_ms = monotonic_ms();
        while (monotonic_ms() - grace_started_ms < kPoweroffGraceMs) {
            if (button_pressed_.load(std::memory_order_acquire)) {
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        stop_ble();

        if (kDevelopmentMode) {
            if (!poweroff_gate_.mark_soft_sleep()) {
                display_available_.store(true, std::memory_order_release);
            }
            return;
        }
        if (!poweroff_gate_.mark_poweroff_ready()) {
            display_available_.store(true, std::memory_order_release);
        }
    }

    bool stop_ble_for_request() {
        return stop_ble();
    }

    void stop_clock_network() {
        clock_network_stop_pending_ = false;
        cancel_network_context_worker();
        stop_network_time_sync();
        if (network_initialized_) {
            network_.shutdown();
            network_initialized_ = false;
        }
        reset_transports();
        early_connect_attempted_at_ms_ = 0;
    }

    bool stop_ble() {
        if (!ble_started_) {
            return true;
        }
        const esp_err_t result =
            ble_provisioning::stop(kBleStopTimeoutMs);
        if (result != ESP_OK) {
            ESP_LOGE(
                kTag,
                "BLE provisioning stop failed (category %s)",
                esp_err_to_name(result));
        }
        ble_started_ = ble_provisioning::running();
        ble_start_attempted_ = ble_started_;
        return result == ESP_OK && !ble_started_;
    }

    bool ensure_ble_started() {
        if (ble_started_ && ble_provisioning::running()) {
            return true;
        }
        ble_start_attempted_ = true;
        const esp_err_t result = ble_provisioning::start(
            &settings_store_, &memory_store_, passkey_callback,
            device_context_callback, this);
        ble_started_ = result == ESP_OK;
        if (!ble_started_) {
            ESP_LOGE(kTag, "BLE provisioning restart failed");
        }
        return ble_started_;
    }

    void recover_poweroff(std::uint32_t now_ms) {
        device_control_.cancel_power_off();
        display_available_.store(true, std::memory_order_release);
        display_sleep_pending_ = false;
        ensure_ble_started();
        interaction_.fail(now_ms);
        previous_state_ = interaction_.state();
        request_display_wake(now_ms);
        show_error("CHATESP COULD NOT TURN OFF");
    }

    RuntimeSettings settings_;
    SettingsStore settings_store_;
    DeviceControl device_control_;
    DeviceMemoryStore &memory_store_;
    network::NetworkManager network_;
    transport::HttpTransport network_context_transport_;
    transport::HttpTransport openrouter_control_transport_;
    transport::HttpTransport openrouter_audio_transport_;
    transport::HttpTransport search_transport_;
    transport::HttpTransport image_transport_;
    network::NetworkContextProvider network_context_provider_;
    image::HttpImageFetchProvider image_fetch_provider_;
    AudioCapture capture_;
    AudioPlayback playback_;
    MicroPythonExecutor python_executor_;
    AtomicCancellation cancellation_;
    AtomicCancellation context_cancellation_;
    agent::ToolRegistry tools_;
    agent::UtcClock utc_clock_;
    agent::IpLocationContext network_context_;
    transport::HttpResponseDate network_context_date_;
    provisioning::BoundedSetting<provisioning::kMaximumApproximateLocationSize>
        live_approximate_location_;
    cloud::OpenRouterConnectionView openrouter_connection_;
    provider::SecretView brave_key_;
    cloud::BraveWebSearchProvider web_provider_;
    cloud::BraveImageSearchProvider image_provider_;
    agent::SearchWebTool web_tool_;
    agent::SearchImagesTool image_tool_;
    agent::RunPythonTool python_tool_;
    agent::GetDeviceStatusTool device_status_tool_;
    agent::SetBrightnessTool brightness_tool_;
    agent::SetVolumeTool volume_tool_;
    agent::PowerOffTool power_off_tool_;
    agent::RememberMemoryTool remember_memory_tool_;
    agent::ForgetMemoryTool forget_memory_tool_;
    agent::ClearMemoriesTool clear_memories_tool_;
    agent::CompactMemoriesTool compact_memories_tool_;
    cloud::OpenRouterChatProvider chat_provider_;
    cloud::OpenRouterTranscriptionProvider transcription_provider_;
    cloud::OpenRouterSpeechProvider speech_provider_;
    agent::AgentLoop *agent_loop_ = nullptr;
    RequestScratch *request_scratch_ = nullptr;
    PcmPlaybackSink pcm_sink_;
    runtime::SpeechSegmenter speech_segmenter_;
    runtime::TurnTiming timing_;
    SpeechSegmentChannel speech_channel_;
    agent::ImageResult selected_image_result_;
    image::Rgb565Frame image_frame_;
    agent::PlotData pending_plot_;
    InteractionStateMachine interaction_;
    InteractionState previous_state_ = InteractionState::booting;
    network::NetworkState last_network_state_ =
        network::NetworkState::off;
    QueueHandle_t command_queue_ = nullptr;
    QueueHandle_t passkey_queue_ = nullptr;
    QueueHandle_t device_context_queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    TaskHandle_t passkey_task_ = nullptr;
    TaskHandle_t speech_task_ = nullptr;
    TaskHandle_t network_warm_task_ = nullptr;
    TaskHandle_t image_task_ = nullptr;
    TaskHandle_t network_context_task_ = nullptr;
    EventGroupHandle_t speech_events_ = nullptr;
    DeadlineCancellation *speech_cancellation_ = nullptr;
    DeadlineCancellation *image_cancellation_ = nullptr;
    std::atomic<agent::Error> speech_result_{agent::Error::model_failed};
    std::atomic<bool> network_warm_initialized_{false};
    std::atomic<agent::Error> image_error_{agent::Error::none};
    std::atomic<agent::Error> network_context_result_{
        agent::Error::model_failed};
    std::atomic<bool> queue_overflow_{false};
    std::atomic<bool> voice_priority_{false};
    std::atomic<bool> display_available_{true};
    std::atomic<bool> button_pressed_{false};
    std::atomic<bool> started_{false};
    std::atomic<AppMode> app_mode_{AppMode::chat};
    std::atomic<std::uint8_t> pending_brightness_percent_{
        runtime::DevicePreferences::default_brightness_percent};
    std::atomic<std::uint8_t> pending_volume_percent_{
        runtime::DevicePreferences::default_volume_percent};
    std::atomic<bool> quick_controls_update_pending_{false};
    std::atomic<bool> quick_controls_brightness_pending_{false};
    std::atomic<bool> quick_controls_volume_pending_{false};
    std::atomic<bool> quick_controls_commit_pending_{false};
    runtime::PoweroffGate poweroff_gate_;
    std::uint32_t level_refreshed_at_ms_ = 0;
    std::uint32_t settings_checked_at_ms_ = 0;
    std::uint32_t early_connect_attempted_at_ms_ = 0;
    std::uint32_t display_wake_attempted_at_ms_ = 0;
    std::uint32_t display_sleep_attempted_at_ms_ = 0;
    std::uint32_t footer_checked_at_ms_ = 0;
    std::uint32_t battery_checked_at_ms_ = 0;
    std::uint32_t stream_text_refreshed_at_ms_ = 0;
    std::uint32_t applied_revision_ = 0;
    std::uint32_t mode_display_attempted_at_ms_ = 0;
    std::uint32_t network_context_finished_at_ms_ = 0;
    std::uint8_t clock_refreshed_second_ = 0xff;
    bool tools_ready_ = false;
    bool ble_started_ = false;
    bool ble_start_attempted_ = false;
    bool network_initialized_ = false;
    bool context_lookup_attempted_ = false;
    bool sntp_started_ = false;
    bool sntp_complete_ = false;
    bool sntp_start_attempted_ = false;
    bool display_wake_pending_ = false;
    bool display_sleep_pending_ = false;
    bool battery_checked_ = false;
    bool footer_shown_ = false;
    bool stream_text_shown_ = false;
    bool request_active_ = false;
    bool quick_controls_enabled_ = false;
    bool clock_return_pending_ = false;
    bool clock_time_available_ = false;
    bool clock_network_stop_pending_ = false;
    std::uint32_t clock_network_stop_started_at_ms_ = 0;
    bool mode_display_pending_ = false;
    ui::WifiIndicator shown_wifi_ = ui::WifiIndicator::off;
    std::optional<std::uint8_t> battery_percent_;
    std::optional<std::uint8_t> shown_battery_percent_;
};

VoiceRuntime::VoiceRuntime() = default;

VoiceRuntime::~VoiceRuntime() {
    if (impl_ != nullptr && !impl_->started()) {
        delete impl_;
    }
    impl_ = nullptr;
}

esp_err_t VoiceRuntime::start(
    bool startup_button_down, std::uint32_t startup_at_ms,
    DevicePreferencesStore &device_preferences_store,
    DeviceMemoryStore &device_memory_store) {
    if (impl_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    impl_ = new (std::nothrow) Impl(
        device_preferences_store, device_memory_store);
    if (impl_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t result =
        impl_->start(startup_button_down, startup_at_ms);
    if (result != ESP_OK) {
        delete impl_;
        impl_ = nullptr;
    }
    return result;
}

void VoiceRuntime::action_button_edge(bool pressed, std::uint32_t at_ms) {
    if (impl_ != nullptr) {
        impl_->action_button_edge(pressed, at_ms);
    }
}

bool VoiceRuntime::mode_button_available() const {
    return impl_ != nullptr && impl_->mode_button_available();
}

void VoiceRuntime::mode_button_short_press(std::uint32_t at_ms) {
    if (impl_ != nullptr) {
        impl_->mode_button_short_press(at_ms);
    }
}

bool VoiceRuntime::poweroff_ready() const {
    return impl_ != nullptr && impl_->poweroff_ready();
}

void VoiceRuntime::poweroff_failed() {
    if (impl_ != nullptr) {
        impl_->poweroff_failed();
    }
}

}  // namespace chatesp
