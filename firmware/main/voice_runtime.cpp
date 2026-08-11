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
#include "chatesp/ble_controller.hpp"
#include "chatesp/ble_settings.hpp"
#include "chatesp/clock_network_transition.hpp"
#include "chatesp/clock_power_policy.hpp"
#include "chatesp/interaction_state.hpp"
#include "chatesp/quick_controls.hpp"
#include "chatesp/runtime_control.hpp"
#include "chatesp/speech_segmenter.hpp"
#include "chatesp/turn_timing.hpp"
#include "chatesp/user_error_message.hpp"
#include "cloud_providers.hpp"
#include "crash_diagnostics.hpp"
#include "device_control.hpp"
#include "device_memory_store.hpp"
#include "device_settings.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
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
constexpr std::uint32_t kRuntimeHeartbeatMs = 5'000;
constexpr std::uint32_t kBatteryRefreshMs = 30'000;
constexpr std::uint32_t kSignalRefreshMs = 2'000;
constexpr std::uint32_t kAnswerStreamRefreshMs = 60;
constexpr std::uint32_t kPoweroffGraceMs = 250;
constexpr std::uint32_t kInteractionTimeoutMs = 180'000;
constexpr std::uint32_t kClockTimeSyncLimitMs = 15'000;
constexpr std::uint32_t kModeDisplayRetryMs = 100;
constexpr std::uint32_t kBleRestartAfterWorkerMs = 250;
constexpr std::uint32_t kBleStopTimeoutMs = 1'000;
constexpr std::uint32_t kControllerWaitMs = 5'000;
constexpr std::uint32_t kStartupServicesSleepWaitMs = 5'000;
constexpr std::uint32_t kPhoneProxyConnectGraceMs = 2'000;
constexpr std::size_t kBleControllerMinimumLargestBlockBytes = 30 * 1024;
constexpr std::size_t kBleControllerMinimumFreeInternalBytes = 48 * 1024;
constexpr std::size_t kMinimumRecordingSamples =
    AudioCapture::kSampleRateHz / 10;
static_assert(
    AudioCapture::kSampleRateHz == kAudioSpectrumSampleRateHz,
    "The spectrum bin frequencies must match the capture sample rate");
constexpr UBaseType_t kRuntimePriority = 5;
constexpr std::uint32_t kRuntimeStackBytes = 28 * 1024;
constexpr UBaseType_t kDisplayControllerPriority = 4;
constexpr std::uint32_t kDisplayControllerStackBytes = 8 * 1024;
constexpr UBaseType_t kBleControllerPriority = 4;
constexpr std::uint32_t kBleControllerStackBytes = 16 * 1024;
constexpr UBaseType_t kStartupServicesPriority = 2;
constexpr std::uint32_t kStartupServicesStackBytes = 12 * 1024;
constexpr UBaseType_t kDeferredUiPriority = 1;
constexpr std::uint32_t kDeferredUiStackBytes = 10 * 1024;
constexpr UBaseType_t kPasskeyPriority = 6;
constexpr std::uint32_t kPasskeyStackBytes = 8 * 1024;
constexpr UBaseType_t kSpeechPriority = 5;
constexpr std::uint32_t kSpeechStackBytes = 16 * 1024;
constexpr EventBits_t kSpeechDoneBit = BIT0;
constexpr EventBits_t kNetworkWarmDoneBit = BIT1;
constexpr EventBits_t kImageDoneBit = BIT2;
constexpr EventBits_t kNetworkContextDoneBit = BIT3;
constexpr EventBits_t kStartupServicesDoneBit = BIT4;
constexpr EventBits_t kDeferredUiDoneBit = BIT5;
constexpr UBaseType_t kNetworkWarmPriority = 3;
constexpr std::uint32_t kNetworkWarmStackBytes = 6 * 1024;
constexpr UBaseType_t kImagePriority = 3;
constexpr std::uint32_t kImageStackBytes = 20 * 1024;
constexpr std::size_t kMaximumImageCandidateAttempts = 3;
constexpr std::uint32_t kImageCandidateBudgetMs = 20'000;
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
        agent::CancellationToken &source, std::uint32_t started_ms,
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
    agent::CancellationToken &source_;
    runtime::MonotonicDeadline deadline_;
};

class NetworkRequestGuard {
public:
    explicit NetworkRequestGuard(
        network::NetworkManager &network, bool enabled = true)
        : network_(network),
          active_(enabled && network_.set_request_active(true) == ESP_OK) {}

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
        english_speech_voice.clear();
        french_speech_voice.clear();
        approximate_location.clear();
        chat_font_scale_percent =
            provisioning::kDefaultChatFontScalePercent;
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
            english_speech_voice.assign("af_heart") &&
            french_speech_voice.assign("ff_siwis") &&
            approximate_location.assign("");
        chat_font_scale_percent =
            provisioning::kDefaultChatFontScalePercent;
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
            next.english_speech_voice.assign(
                record.english_speech_voice.view()) &&
            next.french_speech_voice.assign(
                record.french_speech_voice.view()) &&
            next.approximate_location.assign(
                record.approximate_location.view());
        next.chat_font_scale_percent = record.chat_font_scale_percent;
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
                english_speech_voice.view().data(),
                french_speech_voice.view().data(),
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
            !speech_model.view().empty() &&
            !english_speech_voice.view().empty() &&
            !french_speech_voice.view().empty();
    }

    provisioning::BoundedSetting<192> endpoint;
    provisioning::BoundedSetting<256> openrouter_key;
    provisioning::BoundedSetting<128> brave_key;
    provisioning::BoundedSetting<32> wifi_ssid;
    provisioning::BoundedSetting<63> wifi_password;
    provisioning::BoundedSetting<96> chat_model;
    provisioning::BoundedSetting<96> transcription_model;
    provisioning::BoundedSetting<96> speech_model;
    provisioning::BoundedSetting<96> english_speech_voice;
    provisioning::BoundedSetting<96> french_speech_voice;
    provisioning::BoundedSetting<96> approximate_location;
    std::uint16_t chat_font_scale_percent =
        provisioning::kDefaultChatFontScalePercent;
    bool configured = false;
};

enum class CommandKind : std::uint8_t {
    button_down,
    startup_button_down,
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
    bool short_press_confirmed = false;
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
          restart_device_tool_(device_control_),
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
          pcm_sink_(playback_, device_control_) {
        tools_ready_ = tools_.add(web_tool_) == agent::Error::none &&
            tools_.add(image_tool_) == agent::Error::none &&
            (!python_executor_.available() ||
             tools_.add(python_tool_) == agent::Error::none) &&
            tools_.add(device_status_tool_) == agent::Error::none &&
            tools_.add(brightness_tool_) == agent::Error::none &&
            tools_.add(volume_tool_) == agent::Error::none &&
            tools_.add(power_off_tool_) == agent::Error::none &&
            tools_.add(restart_device_tool_) == agent::Error::none &&
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
        if (ble_actual_running_.load(std::memory_order_acquire)) {
            (void)stop_ble();
        }
        stop_control_tasks();
        if (deferred_ui_task_ != nullptr) {
            vTaskDeleteWithCaps(deferred_ui_task_);
            deferred_ui_task_ = nullptr;
        }
        if (speech_events_ != nullptr) {
            vEventGroupDelete(speech_events_);
            speech_events_ = nullptr;
        }
        release_ble_restart_memory();
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
        start_control_tasks();
        start_startup_services();
        // Reclaim the completed internal stack before the two persistent
        // runtime tasks need contiguous internal memory. The result stays in
        // the atomics and is applied on the runtime task.
        if (!release_startup_services_stack()) {
            ESP_LOGE(kTag, "Startup services did not finish before start");
            stop_control_tasks();
            vQueueDelete(command_queue_);
            vQueueDelete(passkey_queue_);
            vQueueDelete(device_context_queue_);
            command_queue_ = nullptr;
            passkey_queue_ = nullptr;
            device_context_queue_ = nullptr;
            return ESP_ERR_TIMEOUT;
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
            stop_startup_services();
            stop_control_tasks();
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
                CommandKind::startup_button_down, startup_at_ms};
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
            stop_startup_services();
            stop_control_tasks();
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
        started_.store(true, std::memory_order_release);
        return ESP_OK;
    }

    void action_button_edge(
        bool pressed, std::uint32_t at_ms,
        bool short_press_confirmed) {
        if (command_queue_ == nullptr) {
            return;
        }
        button_pressed_.store(pressed, std::memory_order_release);
        if (pressed) {
            device_control_.cancel_pending_action();
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
            short_press_confirmed,
        };
        queue_command(command);
    }

    void mode_button_short_press(std::uint32_t at_ms) {
        if (command_queue_ == nullptr ||
            !display_available_.load(std::memory_order_acquire)) {
            return;
        }
        // A mode change has priority over optional Clock time work as well as
        // active ChatESP work. Cancel it before the runtime task reads the
        // command so a bounded network wait cannot delay the visible change.
        cancellation_.cancel();
        context_cancellation_.cancel();
        speech_channel_.cancel();
        capture_.cancel();
        pcm_sink_.cancel();
        cancel_transports();
        queue_command({CommandKind::toggle_mode, at_ms});
    }

    void mode_button_edge(bool pressed) {
        mode_button_pressed_.store(pressed, std::memory_order_release);
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
                speech_started_for_turn_.store(
                    true, std::memory_order_release);
                show_state(interaction_.state());
                if (speech_cancellation_ != nullptr) {
                    start_image_worker(*speech_cancellation_);
                }
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
    enum class BleRestartFailurePolicy : std::uint8_t {
        restart_device,
        return_error,
    };

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

    static void display_controller_task_entry(void *context) {
        static_cast<Impl *>(context)->run_display_controller();
    }

    static void ble_controller_task_entry(void *context) {
        static_cast<Impl *>(context)->run_ble_controller();
    }

    static void startup_services_task_entry(void *context) {
        static_cast<Impl *>(context)->run_startup_services();
    }

    static void deferred_ui_task_entry(void *context) {
        static_cast<Impl *>(context)->run_deferred_ui();
    }

    static bool deferred_ui_cancelled(void *context) {
        auto *self = static_cast<Impl *>(context);
        return self->voice_priority_.load(std::memory_order_acquire) ||
            self->button_pressed_.load(std::memory_order_acquire);
    }

    void start_control_tasks() {
        const BaseType_t display_created = xTaskCreatePinnedToCoreWithCaps(
            display_controller_task_entry, "display_control",
            kDisplayControllerStackBytes, this,
            kDisplayControllerPriority, &display_controller_task_, 0,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (display_created != pdPASS) {
            display_controller_task_ = nullptr;
            ESP_LOGW(
                kTag,
                "Display controller task could not start; using the "
                "synchronous recovery path");
        }

        // BLE start restores the NimBLE bond store from NVS. Keep this task
        // stack in internal RAM because flash work can disable the PSRAM
        // cache. The task exists before the controller headroom check, so its
        // fixed cost is included in that decision.
        const BaseType_t ble_created = xTaskCreatePinnedToCore(
            ble_controller_task_entry, "ble_control",
            kBleControllerStackBytes, this, kBleControllerPriority,
            &ble_controller_task_, 0);
        if (ble_created != pdPASS) {
            ble_controller_task_ = nullptr;
            ESP_LOGW(
                kTag,
                "BLE controller task could not start; using the "
                "synchronous recovery path");
        }
    }

    void stop_control_tasks() {
        if (display_controller_task_ != nullptr) {
            vTaskDeleteWithCaps(display_controller_task_);
            display_controller_task_ = nullptr;
        }
        if (ble_controller_task_ != nullptr) {
            vTaskDelete(ble_controller_task_);
            ble_controller_task_ = nullptr;
        }
    }

    void start_startup_services() {
        startup_settings_result_.store(
            ESP_ERR_INVALID_STATE, std::memory_order_release);
        startup_memory_result_.store(
            ESP_ERR_INVALID_STATE, std::memory_order_release);
        xEventGroupClearBits(speech_events_, kStartupServicesDoneBit);
        const BaseType_t created = xTaskCreatePinnedToCore(
            startup_services_task_entry, "startup_services",
            kStartupServicesStackBytes, this, kStartupServicesPriority,
            &startup_services_task_, 0);
        if (created == pdPASS) {
            return;
        }

        startup_services_task_ = nullptr;
        ESP_LOGW(
            kTag,
            "Startup services task could not start; using the synchronous "
            "recovery path");
        run_startup_services_inline();
    }

    void run_startup_services_inline() {
        const esp_err_t settings_result = settings_store_.initialize();
        startup_settings_result_.store(
            settings_result, std::memory_order_release);
        const esp_err_t memory_result = memory_store_.initialize();
        startup_memory_result_.store(memory_result, std::memory_order_release);
        xEventGroupSetBits(speech_events_, kStartupServicesDoneBit);
    }

    void run_startup_services() {
        run_startup_services_inline();
        // The start path reclaims this short-lived internal stack before it
        // creates the input tasks.
        vTaskSuspend(nullptr);
    }

    bool release_startup_services_stack() {
        const EventBits_t bits = xEventGroupWaitBits(
            speech_events_, kStartupServicesDoneBit, pdFALSE, pdTRUE,
            pdMS_TO_TICKS(kControllerWaitMs));
        if ((bits & kStartupServicesDoneBit) == 0) {
            return false;
        }
        if (startup_services_task_ != nullptr) {
            TaskHandle_t completed = startup_services_task_;
            startup_services_task_ = nullptr;
            vTaskDelete(completed);
        }
        return true;
    }

    void stop_startup_services() {
        if (speech_events_ == nullptr) {
            return;
        }
        (void)xEventGroupWaitBits(
            speech_events_, kStartupServicesDoneBit, pdFALSE, pdTRUE,
            portMAX_DELAY);
        if (startup_services_task_ != nullptr) {
            TaskHandle_t completed = startup_services_task_;
            startup_services_task_ = nullptr;
            vTaskDelete(completed);
        }
    }

    bool finish_startup_services(bool wait) {
        if (startup_services_applied_) {
            return startup_settings_result_.load(std::memory_order_acquire) ==
                ESP_OK;
        }
        const EventBits_t bits = xEventGroupWaitBits(
            speech_events_, kStartupServicesDoneBit, pdFALSE, pdTRUE,
            wait ? pdMS_TO_TICKS(kControllerWaitMs) : 0);
        if ((bits & kStartupServicesDoneBit) == 0) {
            return false;
        }
        if (startup_services_task_ != nullptr) {
            TaskHandle_t completed = startup_services_task_;
            startup_services_task_ = nullptr;
            vTaskDelete(completed);
        }
        const bool settings_ready =
            startup_settings_result_.load(std::memory_order_acquire) ==
            ESP_OK;
        const bool memory_ready =
            startup_memory_result_.load(std::memory_order_acquire) == ESP_OK;
        startup_services_applied_ = true;
        if (settings_ready) {
            refresh_settings(
                !voice_priority_.load(std::memory_order_acquire) &&
                !button_pressed_.load(std::memory_order_acquire));
            crash_diagnostics::mark(
                runtime::CrashEvent::startup_services_ready);
        }
        if (!memory_ready) {
            ESP_LOGW(kTag, "Saved memories are not available");
        }
        return settings_ready;
    }

    [[nodiscard]] bool startup_services_ready() const {
        return startup_services_applied_ &&
            startup_settings_result_.load(std::memory_order_acquire) ==
                ESP_OK;
    }

    std::uint32_t request_display(
        bool on, bool keep_panel_ready, std::uint32_t now_ms) {
        display_requested_on_.store(on, std::memory_order_relaxed);
        display_requested_keep_ready_.store(
            keep_panel_ready, std::memory_order_relaxed);
        display_requested_state_.store(
            interaction_.state(), std::memory_order_relaxed);
        display_requested_mode_.store(
            app_mode_.load(std::memory_order_acquire),
            std::memory_order_relaxed);
        display_requested_brightness_.store(
            device_control_.brightness_percent(),
            std::memory_order_relaxed);
        display_request_at_ms_.store(now_ms, std::memory_order_relaxed);
        const std::uint32_t generation =
            display_requested_generation_.fetch_add(
                1, std::memory_order_acq_rel) +
            1;
        if (display_controller_task_ != nullptr) {
            xTaskNotifyGive(display_controller_task_);
        } else if (button_pressed_.load(std::memory_order_acquire)) {
            // Do not move panel recovery onto the PWR hold path when the
            // optional controller task could not start. Retry after release.
            display_result_.store(
                ESP_ERR_INVALID_STATE, std::memory_order_release);
            display_completed_generation_.store(
                generation, std::memory_order_release);
        } else {
            perform_display_request(generation);
        }
        return generation;
    }

    void perform_display_request(std::uint32_t generation) {
        const bool on = display_requested_on_.load(std::memory_order_acquire);
        const std::uint32_t requested_at_ms =
            display_request_at_ms_.load(std::memory_order_acquire);
        esp_err_t result = ESP_FAIL;
        if (on) {
            crash_diagnostics::mark(
                runtime::CrashEvent::display_wake_begin);
            result = ui::wake(
                display_requested_state_.load(std::memory_order_acquire),
                display_requested_brightness_.load(
                    std::memory_order_acquire),
                display_requested_mode_.load(std::memory_order_acquire));
            crash_diagnostics::mark(
                result == ESP_OK
                    ? runtime::CrashEvent::display_wake_complete
                    : runtime::CrashEvent::display_wake_failed);
        } else {
            result = ui::sleep(
                display_requested_keep_ready_.load(
                    std::memory_order_acquire));
            if (result == ESP_OK) {
                crash_diagnostics::mark(
                    runtime::CrashEvent::display_sleep_complete);
            }
        }
        if (generation != display_requested_generation_.load(
                              std::memory_order_acquire)) {
            return;
        }
        display_result_.store(result, std::memory_order_release);
        display_completed_at_ms_.store(
            requested_at_ms, std::memory_order_release);
        display_completed_generation_.store(
            generation, std::memory_order_release);
    }

    void run_display_controller() {
        while (true) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            while (display_completed_generation_.load(
                       std::memory_order_acquire) !=
                   display_requested_generation_.load(
                       std::memory_order_acquire)) {
                const std::uint32_t generation =
                    display_requested_generation_.load(
                        std::memory_order_acquire);
                perform_display_request(generation);
            }
        }
    }

    bool display_request_complete(std::uint32_t generation) const {
        return display_requested_generation_.load(
                   std::memory_order_acquire) == generation &&
            display_completed_generation_.load(
                std::memory_order_acquire) == generation;
    }

    bool wait_for_display(std::uint32_t generation) {
        const std::uint32_t started_ms = monotonic_ms();
        while (!display_request_complete(generation)) {
            if (button_pressed_.load(std::memory_order_acquire)) {
                return false;
            }
            if (display_requested_generation_.load(
                    std::memory_order_acquire) != generation) {
                return false;
            }
            if (monotonic_ms() - started_ms >= kControllerWaitMs) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        return display_result_.load(std::memory_order_acquire) == ESP_OK;
    }

    void poll_display_controller() {
        const std::uint32_t completed =
            display_completed_generation_.load(std::memory_order_acquire);
        if (completed == display_completion_seen_) {
            return;
        }
        display_completion_seen_ = completed;
        const bool succeeded =
            display_result_.load(std::memory_order_acquire) == ESP_OK;
        display_wake_pending_ =
            display_requested_on_.load(std::memory_order_acquire) &&
            !succeeded;
        display_sleep_pending_ =
            !display_requested_on_.load(std::memory_order_acquire) &&
            !succeeded;
        if (succeeded &&
            display_requested_on_.load(std::memory_order_acquire)) {
            footer_shown_ = false;
            display_refresh_pending_ = true;
        }
    }

    std::uint32_t request_ble(bool running) {
        const bool prior = ble_requested_running_.exchange(
            running, std::memory_order_acq_rel);
        std::uint32_t generation =
            ble_requested_generation_.load(std::memory_order_acquire);
        const bool completed = ble_completed_generation_.load(
                                   std::memory_order_acquire) == generation;
        const bool succeeded =
            ble_result_.load(std::memory_order_acquire) == ESP_OK;
        const bool actual_matches =
            ble_actual_running_.load(std::memory_order_acquire) == running;
        if (prior != running ||
            (completed && (!succeeded || !actual_matches))) {
            generation = ble_requested_generation_.fetch_add(
                             1, std::memory_order_acq_rel) +
                1;
        }
        if (ble_controller_task_ != nullptr) {
            xTaskNotifyGive(ble_controller_task_);
        } else if (
            button_pressed_.load(std::memory_order_acquire) ||
            interaction_.state() == InteractionState::recording) {
            // Do not run NimBLE inline with microphone priority. A later
            // explicit request retries this failed generation.
            ble_result_.store(
                ESP_ERR_INVALID_STATE, std::memory_order_release);
            ble_actual_running_.store(
                ble_provisioning::running(), std::memory_order_release);
            ble_completed_generation_.store(
                generation, std::memory_order_release);
        } else {
            perform_ble_request(generation);
        }
        return generation;
    }

    void perform_ble_request(std::uint32_t generation) {
        const bool running =
            ble_requested_running_.load(std::memory_order_acquire);
        const bool actual_before = ble_provisioning::running();
        if (!ble_controller_planner_.operation_active() &&
            ble_controller_planner_.actual_running() != actual_before) {
            ble_controller_planner_ =
                runtime::BleControllerPlanner(actual_before);
        }
        (void)ble_controller_planner_.request(
            running ? runtime::BleControllerTarget::running
                    : runtime::BleControllerTarget::stopped);
        const runtime::BleControllerWork work =
            ble_controller_planner_.begin_next();
        esp_err_t result = ESP_OK;
        if (work.operation == runtime::BleControllerOperation::start) {
            // Keep the reserved contiguous block until this sole owner is
            // ready to enter NimBLE. No runtime allocation can split the
            // handoff between the final check and controller start.
            release_ble_restart_memory();
            const std::size_t free_internal = heap_caps_get_free_size(
                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            const std::size_t largest_internal =
                heap_caps_get_largest_free_block(
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            if (free_internal < kBleControllerMinimumFreeInternalBytes ||
                largest_internal <
                    kBleControllerMinimumLargestBlockBytes) {
                result = ESP_ERR_NO_MEM;
            } else {
                crash_diagnostics::mark(
                    runtime::CrashEvent::ble_start_begin);
                result = ble_provisioning::start(
                    &settings_store_, &memory_store_, passkey_callback,
                    device_context_callback, this);
            }
            if (result == ESP_OK) {
                crash_diagnostics::mark(
                    runtime::CrashEvent::ble_start_complete);
            }
        } else if (
            work.operation == runtime::BleControllerOperation::stop &&
            ble_provisioning::running()) {
            result = ble_provisioning::stop(kBleStopTimeoutMs);
        }
        const bool actual_running = ble_provisioning::running();
        if (work.valid()) {
            (void)ble_controller_planner_.complete(
                work,
                result == ESP_OK && running == actual_running);
        }
        ble_actual_running_.store(
            actual_running, std::memory_order_release);
        if (running != actual_running && result == ESP_OK) {
            result = ESP_FAIL;
        }
        if (generation != ble_requested_generation_.load(
                              std::memory_order_acquire)) {
            return;
        }
        ble_result_.store(result, std::memory_order_release);
        ble_completed_generation_.store(
            generation, std::memory_order_release);
    }

    void run_ble_controller() {
        while (true) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            while (ble_completed_generation_.load(
                       std::memory_order_acquire) !=
                   ble_requested_generation_.load(
                       std::memory_order_acquire)) {
                const std::uint32_t generation =
                    ble_requested_generation_.load(
                        std::memory_order_acquire);
                perform_ble_request(generation);
            }
        }
    }

    bool ble_request_complete(std::uint32_t generation) const {
        return ble_requested_generation_.load(
                   std::memory_order_acquire) == generation &&
            ble_completed_generation_.load(
                std::memory_order_acquire) == generation;
    }

    bool wait_for_ble(std::uint32_t generation) {
        return wait_for_ble_until(
            generation, monotonic_ms(), kControllerWaitMs);
    }

    bool wait_for_ble_until(
        std::uint32_t generation, std::uint32_t period_started_ms,
        std::uint32_t period_ms) {
        while (!ble_request_complete(generation)) {
            if (button_pressed_.load(std::memory_order_acquire)) {
                return false;
            }
            if (ble_requested_generation_.load(
                    std::memory_order_acquire) != generation) {
                return false;
            }
            if (monotonic_ms() - period_started_ms >= period_ms) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        poll_ble_controller();
        return ble_result_.load(std::memory_order_acquire) == ESP_OK;
    }

    void poll_ble_controller() {
        const std::uint32_t completed =
            ble_completed_generation_.load(std::memory_order_acquire);
        if (completed == ble_completion_seen_) {
            return;
        }
        ble_completion_seen_ = completed;
        ble_started_ = ble_actual_running_.load(std::memory_order_acquire);
        const bool succeeded =
            ble_result_.load(std::memory_order_acquire) == ESP_OK;
        ble_start_attempted_ =
            ble_requested_running_.load(std::memory_order_acquire);
        if (!succeeded) {
            ESP_LOGW(kTag, "The radio controller request failed");
        }
    }

    void run_deferred_ui() {
        deferred_ui_result_.store(
            ui::prepare_deferred_views(deferred_ui_cancelled, this),
            std::memory_order_release);
        xEventGroupSetBits(speech_events_, kDeferredUiDoneBit);
        // The runtime reclaims this WithCaps task and its PSRAM stack.
        vTaskSuspend(nullptr);
    }

    void start_image_worker(DeadlineCancellation &cancellation) {
        if (image_task_ != nullptr || speech_events_ == nullptr ||
            image_candidates_.size == 0) {
            return;
        }
        image_frame_.reset();
        image_cancellation_ = &cancellation;
        image_error_.store(agent::Error::none, std::memory_order_release);
        xEventGroupClearBits(speech_events_, kImageDoneBit);
        const BaseType_t created = xTaskCreatePinnedToCore(
            image_task_entry, "image_download", kImageStackBytes, this,
            kImagePriority, &image_task_, 0);
        if (created != pdPASS) {
            image_task_ = nullptr;
            image_cancellation_ = nullptr;
            image_candidates_.clear();
            image_error_.store(
                agent::Error::model_failed, std::memory_order_release);
            image_unavailable_.store(true, std::memory_order_release);
        }
    }

    void clear_runtime_image_state() {
        image_candidates_.clear();
        image_frame_.reset();
        image_error_.store(agent::Error::none, std::memory_order_release);
        image_requested_.store(false, std::memory_order_release);
        image_unavailable_.store(false, std::memory_order_release);
    }

    void clear_all_image_state() {
        image_transport_.cancel_active();
        join_image_worker();
        image_tool_.clear_results();
        clear_runtime_image_state();
    }

    void prepare_visual() {
        pending_plot_.clear();
        clear_runtime_image_state();
        if (python_tool_.take_plot(pending_plot_)) {
            return;
        }
        const bool requested =
            image_tool_.take_selected_or_first_candidates(image_candidates_);
        image_requested_.store(requested, std::memory_order_release);
    }

    void run_image_worker() {
        agent::Error error = agent::Error::model_failed;
        if (image_cancellation_ != nullptr) {
            const std::uint32_t started_ms = monotonic_ms();
            DeadlineCancellation image_deadline(
                *image_cancellation_, started_ms,
                kImageCandidateBudgetMs);
            const std::size_t attempt_count = std::min(
                image_candidates_.size, kMaximumImageCandidateAttempts);
            for (std::size_t index = 0; index < attempt_count; ++index) {
                if (image_deadline.cancelled()) {
                    error = image_deadline.normalize(
                        agent::Error::cancelled);
                    break;
                }
                const std::uint32_t elapsed_ms =
                    monotonic_ms() - started_ms;
                if (elapsed_ms >= kImageCandidateBudgetMs) {
                    error = agent::Error::total_timeout;
                    break;
                }
                const std::uint32_t remaining_budget_ms =
                    kImageCandidateBudgetMs - elapsed_ms;
                agent::RequestPolicy policy = agent::image_fetch_policy();
                policy.connect_timeout_ms = std::min(
                    policy.connect_timeout_ms, remaining_budget_ms);
                policy.first_byte_timeout_ms = std::min(
                    policy.first_byte_timeout_ms, remaining_budget_ms);
                policy.idle_timeout_ms = std::min(
                    policy.idle_timeout_ms, remaining_budget_ms);
                policy.total_timeout_ms = remaining_budget_ms;
                policy.max_attempts = 1;

                image::JpegImageSink sink(image_deadline);
                const agent::ImageFetchRequest request{
                    image_candidates_.items[index].thumbnail_url.c_str(),
                    agent::Limits::max_image_download_bytes,
                    agent::Limits::max_image_dimension,
                    0,
                    policy,
                };
                error = image_fetch_provider_.fetch(
                    request, sink, image_deadline);
                if (error == agent::Error::none &&
                    image_deadline.cancelled()) {
                    error = image_deadline.normalize(
                        agent::Error::cancelled);
                }
                if (error == agent::Error::none && sink.ready()) {
                    image_frame_ = sink.take_frame();
                    break;
                }
                if (error == agent::Error::none) {
                    error = agent::Error::malformed_response;
                }
                if (error == agent::Error::cancelled ||
                    image_deadline.cancelled()) {
                    error = image_deadline.normalize(
                        agent::Error::cancelled);
                    break;
                }
            }
        }
        image_candidates_.clear();
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
        } else if (
            image_requested_.load(std::memory_order_acquire) &&
            error != agent::Error::cancelled) {
            image_unavailable_.store(true, std::memory_order_release);
            ESP_LOGW(
                kTag, "Optional image was not available (category %u)",
                static_cast<unsigned>(error));
        }
    }

    void start_network_during_recording() {
        if (!startup_services_applied_ ||
            ble_provisioning::http_proxy_available() ||
            !settings_.has_wifi_credentials() || network_initialized_ ||
            network_warm_task_ != nullptr || speech_events_ == nullptr) {
            return;
        }
        const std::uint32_t now_ms = monotonic_ms();
        const std::uint32_t requested_generation =
            ble_requested_generation_.load(std::memory_order_acquire);
        const bool ble_start_pending =
            ble_requested_running_.load(std::memory_order_acquire) &&
            !ble_request_complete(requested_generation);
        if (ble_start_pending &&
            now_ms - recording_started_at_ms_ <
                kPhoneProxyConnectGraceMs) {
            return;
        }
        if (runtime::keep_ble_during_recording(
                ble_provisioning::running(),
                ble_provisioning::bond_available(),
                recording_started_at_ms_, now_ms,
                kPhoneProxyConnectGraceMs)) {
            return;
        }
        if (network_.connected() || network_.connecting()) {
            return;
        }
        if (!recording_ble_stop_pending_) {
            recording_ble_stop_generation_ = request_ble(false);
            recording_ble_stop_pending_ = true;
        }
        if (!ble_request_complete(recording_ble_stop_generation_)) {
            return;
        }
        poll_ble_controller();
        recording_ble_stop_pending_ = false;
        if (ble_actual_running_.load(std::memory_order_acquire) ||
            ble_result_.load(std::memory_order_acquire) != ESP_OK ||
            !reserve_ble_restart_memory()) {
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
            ble_started_ || ble_provisioning::running() ||
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

        // Do not stop an active BLE host for this optional TLS request. A
        // phone can be connecting or restoring notifications even when the
        // firmware has not received a GATT event yet. Run the lookup only
        // during another operation that already owns the network and has
        // stopped BLE.
        crash_diagnostics::mark(runtime::CrashEvent::network_context_begin);
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
        crash_diagnostics::mark(
            runtime::CrashEvent::network_context_complete);
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
        speech_started_for_turn_.store(false, std::memory_order_release);
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
        const std::uint32_t lock_ms =
            voice_priority_.load(std::memory_order_acquire) ? 0 : 100;
        if (bsp_display_lock(lock_ms)) {
            callback();
            if (!voice_priority_.load(std::memory_order_acquire)) {
                lv_refr_now(nullptr);
            }
            bsp_display_unlock();
            return true;
        }
        display_refresh_pending_ = true;
        return false;
    }

    void show_state(InteractionState state) {
        if (with_display([this, state]() {
            ui::sync_quick_controls(
                device_control_.brightness_percent(),
                device_control_.volume_percent());
            ui::show_state(state);
        })) {
            display_refresh_pending_ = false;
        }
    }

    void prepare_deferred_ui() {
        if (deferred_views_ready_.load(std::memory_order_acquire)) {
            if (!quick_controls_enabled_ &&
                !quick_controls_attempted_ &&
                !voice_priority_.load(std::memory_order_acquire) &&
                bsp_display_lock(100)) {
                quick_controls_attempted_ = true;
                quick_controls_enabled_ = ui::enable_quick_controls(
                    device_control_.brightness_percent(),
                    device_control_.volume_percent(),
                    quick_controls_callback, this);
                if (quick_controls_enabled_) {
                    lv_refr_now(nullptr);
                }
                bsp_display_unlock();
                if (!quick_controls_enabled_) {
                    ESP_LOGW(kTag, "Touch controls are not available");
                }
            }
            return;
        }

        if (deferred_ui_task_ != nullptr) {
            if ((xEventGroupGetBits(speech_events_) &
                 kDeferredUiDoneBit) == 0) {
                return;
            }
            TaskHandle_t completed = deferred_ui_task_;
            deferred_ui_task_ = nullptr;
            vTaskDeleteWithCaps(completed);
            if (deferred_ui_result_.load(std::memory_order_acquire)) {
                deferred_views_ready_.store(
                    true, std::memory_order_release);
                mode_display_pending_ = true;
                mode_display_attempted_at_ms_ = 0;
            }
            return;
        }

        const std::uint32_t ble_generation =
            ble_requested_generation_.load(std::memory_order_acquire);
        if (!display_available_.load(std::memory_order_acquire) ||
            interaction_.state() != InteractionState::idle ||
            voice_priority_.load(std::memory_order_acquire) ||
            deferred_ui_task_create_failed_ ||
            !startup_services_ready() ||
            !ble_request_complete(ble_generation)) {
            return;
        }
        xEventGroupClearBits(speech_events_, kDeferredUiDoneBit);
        deferred_ui_result_.store(false, std::memory_order_release);
        const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
            deferred_ui_task_entry, "deferred_ui", kDeferredUiStackBytes,
            this, kDeferredUiPriority, &deferred_ui_task_, 0,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (created != pdPASS) {
            deferred_ui_task_ = nullptr;
            deferred_ui_task_create_failed_ = true;
            ESP_LOGW(kTag, "Optional touch setup task could not start");
        }
    }

    void prepare_deferred_ui_for_request() {
        if (!ui::prepare_visual_views()) {
            ESP_LOGW(kTag, "Visual display setup is not available");
        }
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
                ClockTime{
                    local.hour, local.minute, local.second,
                    local.millisecond});
        });
    }

    bool apply_mode_display(AppMode mode, InteractionState chat_state) {
        if (mode == AppMode::clock && !ui::prepare_clock_view()) {
            mode_display_pending_ = true;
            return false;
        }
        const bool applied = with_display([this, mode, chat_state]() {
            ui::show_app_mode(mode, chat_state);
            if (quick_controls_enabled_) {
                ui::sync_quick_controls(
                    device_control_.brightness_percent(),
                    device_control_.volume_percent());
            }
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
        interaction_.ready(now_ms);
        previous_state_ = interaction_.state();
        agent_loop_->clear_thread();
        clear_all_image_state();
        python_tool_.clear_plot();
        pending_plot_.clear();
        footer_shown_ = false;
        clock_refreshed_second_ = 0xff;
        clock_time_available_ = false;

        const bool local_time_ready = utc_clock_.has_local_time();
        const bool wifi_configured = settings_.has_wifi_credentials();
        const runtime::ClockNetworkTransition network_transition =
            runtime::clock_network_transition(
                network_warm_task_ != nullptr, local_time_ready,
                wifi_configured);
        clock_network_attempted_ = false;
        clock_ble_time_sync_requested_ = false;
        clock_network_stop_pending_ = false;
        clock_network_stop_started_at_ms_ = now_ms;
        clock_unpowered_since_ms_ = now_ms;
        clock_unpowered_timer_running_ = false;
        // Reserve the Bluetooth restart block before LVGL creates its Clock
        // objects. Small internal LVGL allocations can otherwise fragment the
        // block that the controller released.
        for (std::size_t index = 0;
             index < network_transition.size; ++index) {
            switch (network_transition.steps[index]) {
                case runtime::ClockNetworkTransitionStep::
                    join_recording_worker:
                    join_network_warm_worker();
                    break;
                case runtime::ClockNetworkTransitionStep::
                    acquire_local_time:
                    // Clock can become active before the asynchronous startup
                    // connection has supplied UTC and a timezone. Keep that
                    // bounded acquisition alive, then stop Wi-Fi from the
                    // runtime loop.
                    clock_network_attempted_ = true;
                    clock_network_stop_pending_ = true;
                    clock_network_stop_started_at_ms_ = now_ms;
                    if (stop_ble_for_request(
                            BleRestartFailurePolicy::return_error)) {
                        start_network_early(true);
                    } else {
                        ESP_LOGW(
                            kTag,
                            "Clock time sync skipped because radio memory "
                            "is unavailable");
                        stop_clock_network();
                    }
                    break;
                case runtime::ClockNetworkTransitionStep::
                    stop_network_and_restart_ble:
                    stop_clock_network();
                    break;
            }
        }
        mode_display_attempted_at_ms_ = now_ms;
        (void)apply_mode_display(AppMode::clock, interaction_.state());
        refresh_clock(now_ms, true);
        refresh_battery(now_ms, true);
        ESP_LOGI(kTag, "Clock mode is active");
    }

    void start_clock_network_after_settings(std::uint32_t now_ms) {
        if (app_mode_.load(std::memory_order_acquire) != AppMode::clock ||
            utc_clock_.has_local_time() || clock_network_attempted_ ||
            clock_network_stop_pending_ ||
            voice_priority_.load(std::memory_order_acquire) ||
            button_pressed_.load(std::memory_order_acquire)) {
            return;
        }
        if (!settings_.has_wifi_credentials()) {
            // An immediate Clock entry can happen before saved settings are
            // ready. In this case the earlier BLE start request is absent.
            // Start BLE now so the authenticated phone can supply time.
            if (!clock_ble_time_sync_requested_) {
                clock_ble_time_sync_requested_ = ensure_ble_started();
            }
            return;
        }
        clock_network_attempted_ = true;
        clock_network_stop_pending_ = true;
        clock_network_stop_started_at_ms_ = now_ms;
        if (stop_ble_for_request(BleRestartFailurePolicy::return_error)) {
            start_network_early(true);
        } else {
            ESP_LOGW(
                kTag,
                "Clock time sync skipped because radio memory is "
                "unavailable");
            stop_clock_network();
        }
    }

    void enter_chat_mode(std::uint32_t now_ms) {
        if (!display_available_.load(std::memory_order_acquire)) {
            return;
        }
        app_mode_.store(AppMode::chat, std::memory_order_release);
        clock_network_attempted_ = false;
        clock_ble_time_sync_requested_ = false;
        clock_network_stop_pending_ = false;
        interaction_.ready(now_ms);
        previous_state_ = interaction_.state();
        mode_display_attempted_at_ms_ = now_ms;
        (void)apply_mode_display(AppMode::chat, interaction_.state());
        footer_shown_ = false;
        refresh_footer(now_ms, true);
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

    static ui::RadioIndicator radio_indicator(
        network::NetworkState state, bool configured,
        bool ble_connected) {
        if (state == network::NetworkState::off && ble_connected) {
            return ui::RadioIndicator::ble_online;
        }
        if (!configured) {
            return ui::RadioIndicator::setup;
        }
        switch (state) {
            case network::NetworkState::off:
                return ui::RadioIndicator::off;
            case network::NetworkState::connecting:
                return ui::RadioIndicator::wifi_connecting;
            case network::NetworkState::connected:
                return ui::RadioIndicator::wifi_online;
            case network::NetworkState::failed:
                return ui::RadioIndicator::failed;
        }
        return ui::RadioIndicator::failed;
    }

    void draw_footer(
        ui::RadioIndicator radio, std::uint8_t signal_band) {
        if (!display_available_.load(std::memory_order_acquire)) {
            return;
        }
        if (!with_display([this, radio, signal_band]() {
            ui::show_footer(
                radio,
                signal_band,
                battery_status_.has_value() && battery_status_->available,
                battery_status_.has_value() && battery_status_->available
                    ? battery_status_->percent
                    : 0,
                battery_status_.has_value() &&
                    power::connected_to_external_power(*battery_status_));
        })) {
            return;
        }
        shown_radio_ = radio;
        shown_signal_band_ = signal_band;
        shown_battery_status_ = battery_status_;
        footer_shown_ = true;
    }

    bool refresh_battery(std::uint32_t now_ms, bool force) {
        if (!display_available_.load(std::memory_order_acquire)) {
            return false;
        }
        const std::uint32_t source_revision =
            power::power_source_revision();
        const bool battery_due = force || battery_wake_refresh_pending_ ||
            !battery_checked_ ||
            now_ms - battery_checked_at_ms_ >= kBatteryRefreshMs ||
            source_revision != battery_power_source_revision_;
        if (battery_due &&
            !button_pressed_.load(std::memory_order_acquire) &&
            !voice_priority_.load(std::memory_order_acquire)) {
            const std::optional<power::BatteryStatus> sample =
                power::battery_status();
            battery_checked_ = true;
            battery_checked_at_ms_ = now_ms;
            battery_power_source_revision_ = source_revision;
            battery_wake_refresh_pending_ = false;
            battery_status_sample_valid_ = sample.has_value();
            if (sample.has_value()) {
                battery_status_ = sample;
            }
            if (sample.has_value() &&
                power::low_battery_requires_shutdown(*sample) &&
                !low_battery_poweroff_pending_) {
                low_battery_poweroff_pending_ = true;
                ESP_LOGW(kTag, "Low battery system-off requested");
                interaction_.cancel_for_sleep();
            }
        }
        return battery_status_sample_valid_;
    }

    void refresh_footer(std::uint32_t now_ms, bool force) {
        refresh_battery(now_ms, force);
        const ui::RadioIndicator radio = radio_indicator(
            network_.state(), settings_.has_wifi_credentials(),
            ble_provisioning::secure_link_connected());
        const bool signal_due = force || radio != radio_indicator_ ||
            now_ms - signal_checked_at_ms_ >= kSignalRefreshMs;
        if (signal_due) {
            radio_indicator_ = radio;
            signal_checked_at_ms_ = now_ms;
            if (radio == ui::RadioIndicator::wifi_online) {
                signal_band_ = network_.rssi_band();
            } else {
                signal_band_ = 0;
            }
        }
        if (force || !footer_shown_ || radio != shown_radio_ ||
            signal_band_ != shown_signal_band_ ||
            battery_status_ != shown_battery_status_) {
            draw_footer(radio, signal_band_);
        }
    }

    void queue_command(const Command &command) {
        if (command_queue_ != nullptr &&
            xQueueSend(command_queue_, &command, 0) != pdTRUE) {
            queue_overflow_.store(true, std::memory_order_release);
        }
    }

    void run_passkey_ui() {
        const bool watchdog_active = esp_task_wdt_add(nullptr) == ESP_OK;
        PasskeyEvent current{};
        PasskeyEvent shown{};
        bool has_shown = false;
        while (true) {
            if (watchdog_active) {
                (void)esp_task_wdt_reset();
            }
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
                crash_diagnostics::mark(
                    runtime::CrashEvent::ble_passkey_begin);
                if (bsp_display_lock(100)) {
                    ui::show_ble_passkey(
                        desired.passkey, desired.visible);
                    bsp_display_unlock();
                    shown = desired;
                    has_shown = true;
                }
                crash_diagnostics::mark(
                    runtime::CrashEvent::ble_passkey_complete);
                ESP_LOGI(
                    kTag, "BLE passkey stack minimum free bytes: %u",
                    static_cast<unsigned>(
                        uxTaskGetStackHighWaterMark(nullptr)));
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
        }

        std::uint32_t heartbeat_at_ms = monotonic_ms();
        crash_diagnostics::heartbeat();
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
            poll_display_controller();
            poll_ble_controller();
            if (image_task_ != nullptr &&
                (xEventGroupGetBits(speech_events_) & kImageDoneBit) != 0) {
                join_image_worker();
            }
            if (network_warm_task_ != nullptr &&
                (xEventGroupGetBits(speech_events_) &
                 kNetworkWarmDoneBit) != 0) {
                join_network_warm_worker();
            }
            if (!startup_services_applied_) {
                if (finish_startup_services(false)) {
                    if (interaction_.state() !=
                        InteractionState::sleep_pending) {
                        if (app_mode_.load(std::memory_order_acquire) ==
                            AppMode::clock) {
                            start_clock_network_after_settings(
                                monotonic_ms());
                        } else {
                            (void)ensure_ble_started();
                        }
                    }
                } else if (
                    (xEventGroupGetBits(speech_events_) &
                     kStartupServicesDoneBit) != 0 &&
                    !startup_services_failed_reported_) {
                    startup_services_failed_reported_ = true;
                    fail("DEVICE STORAGE COULD NOT START");
                }
            }
            if (now_ms - heartbeat_at_ms >= kRuntimeHeartbeatMs) {
                heartbeat_at_ms = now_ms;
                crash_diagnostics::heartbeat();
            }
            if (ble_started_ &&
                interaction_.state() == InteractionState::idle &&
                !voice_priority_.load(std::memory_order_acquire) &&
                ble_provisioning::advertising_recovery_requested()) {
                ESP_LOGW(
                    kTag,
                    "Restarting BLE after advertising retries ended");
                if (stop_ble()) {
                    ble_start_attempted_ = false;
                    (void)ensure_ble_started();
                }
            }
            prepare_deferred_ui();
            apply_pending_settings_display();
            process_quick_controls(now_ms);
            retry_mode_display(now_ms);
            const network::NetworkState network_state = network_.state();
            if (network_state != last_network_state_) {
                if (network_state == network::NetworkState::connected) {
                    crash_diagnostics::mark(
                        runtime::CrashEvent::wifi_connected);
                }
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
            if (app_mode == AppMode::chat &&
                mode_button_pressed_.load(std::memory_order_acquire)) {
                // A valid short press can cross the ChatESP idle boundary.
                // Keep the device active until the gesture is resolved.
                interaction_.note_idle_activity(now_ms);
            }
            if (app_mode == AppMode::clock) {
                start_clock_network_after_settings(now_ms);
                // Apply a pending VBUS event before the timeout decision. A
                // cable inserted at the five-minute boundary must keep Clock
                // active instead of using the prior battery-only sample.
                const bool power_status_available =
                    refresh_battery(now_ms, false);
                const bool external_power_connected =
                    power_status_available && battery_status_.has_value() &&
                    power::connected_to_external_power(*battery_status_);
                if (!power_status_available || external_power_connected) {
                    clock_unpowered_since_ms_ = now_ms;
                    clock_unpowered_timer_running_ = false;
                } else if (!clock_unpowered_timer_running_) {
                    clock_unpowered_since_ms_ = now_ms;
                    clock_unpowered_timer_running_ = true;
                } else if (power::clock_unpowered_sleep_due(
                               true,
                               false,
                               now_ms - clock_unpowered_since_ms_)) {
                    ESP_LOGI(
                        kTag,
                        "Clock battery timeout requested sleep");
                    interaction_.cancel_for_sleep();
                }
                refresh_clock(now_ms);
                if (clock_network_shutdown_due(
                        clock_network_stop_pending_,
                        utc_clock_.has_local_time(),
                        now_ms - clock_network_stop_started_at_ms_,
                        kClockTimeSyncLimitMs)) {
                    stop_clock_network();
                }
            }
            // Clock has its own five-minute battery-only policy. Disable the
            // ChatESP 30-second idle gate at the state-machine boundary.
            interaction_.tick(now_ms, app_mode != AppMode::clock);
            process_state_change(now_ms);
            const bool recording =
                interaction_.state() == InteractionState::recording;
            if (recording) {
                capture_audio(now_ms);
            }
            if (!voice_priority_.load(std::memory_order_acquire)) {
                retry_display_wake(now_ms);
                if (display_refresh_pending_ &&
                    display_available_.load(std::memory_order_acquire)) {
                    show_state(interaction_.state());
                }
            }
            retry_display_sleep(now_ms);
            if (display_available_.load(std::memory_order_acquire) &&
                !voice_priority_.load(std::memory_order_acquire) &&
                now_ms - footer_checked_at_ms_ >= kFooterRefreshMs) {
                footer_checked_at_ms_ = now_ms;
                if (app_mode_.load(std::memory_order_acquire) ==
                    AppMode::chat) {
                    refresh_footer(now_ms, false);
                } else {
                    refresh_battery(now_ms, false);
                }
            }
            if (!recording &&
                interaction_.state() == InteractionState::idle &&
                !voice_priority_.load(std::memory_order_acquire) &&
                !button_pressed_.load(std::memory_order_acquire) &&
                now_ms - settings_checked_at_ms_ >=
                    kSettingsRefreshMs) {
                settings_checked_at_ms_ = now_ms;
                if (ble_started_ && !ble_provisioning::running()) {
                    ble_started_ = false;
                    ble_start_attempted_ = false;
                    ble_actual_running_.store(
                        false, std::memory_order_release);
                }
                if (!interaction_.button_is_down() && !ble_started_ &&
                    !ble_start_attempted_ &&
                    network_context_task_ == nullptr &&
                    now_ms - network_context_finished_at_ms_ >=
                        kBleRestartAfterWorkerMs) {
                    (void)ensure_ble_started();
                }
                if (startup_services_applied_) {
                    refresh_settings();
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
                enter_chat_mode(command.at_ms);
            } else {
                if (interaction_.state() != InteractionState::idle) {
                    cancel_current();
                }
                enter_clock_mode(command.at_ms);
            }
            return;
        }
        if ((command.kind == CommandKind::button_down ||
             command.kind == CommandKind::startup_button_down ||
             command.kind == CommandKind::wake_button_down ||
             command.kind == CommandKind::wake_from_poweroff) &&
            app_mode_.load(std::memory_order_acquire) == AppMode::clock) {
            // The bottom button always returns to ChatESP before its normal
            // short-press or hold-to-talk state handling continues.
            prepare_recording();
            enter_chat_mode(command.at_ms);
        }
        if (command.kind == CommandKind::startup_button_down ||
            command.kind == CommandKind::wake_button_down ||
            command.kind == CommandKind::wake_from_poweroff ||
            (command.kind == CommandKind::button_down &&
             interaction_.state() == InteractionState::sleep_pending)) {
            ESP_LOGI(kTag, "Action button requested display wake");
            const bool display_already_ready =
                command.kind == CommandKind::startup_button_down;
            wake_for_button(command.at_ms, display_already_ready);
            return;
        }
        if (command.kind == CommandKind::button_down) {
            ESP_LOGI(kTag, "Action button started a voice hold");
            display_recovery_requested_for_press_ = false;
            interaction_.button_down(command.at_ms);
            prepare_recording();
        } else if (command.kind == CommandKind::button_up) {
            ESP_LOGI(kTag, "Action button was released");
            if (button_pressed_.load(std::memory_order_acquire)) {
                // A newer physical press superseded this queued release.
                // Cancel the old hold without scheduling sleep or submission.
                cancel_current();
                return;
            }
            timing_.reset(command.at_ms);
            // Resolve a hold that crossed the threshold before this queued
            // release. Do not depend on the periodic tick running first.
            interaction_.tick(command.at_ms);
            process_state_change(command.at_ms);
            interaction_.button_up(
                command.at_ms, command.short_press_confirmed);
            if (interaction_.state() == InteractionState::sleep_pending) {
                crash_diagnostics::mark(
                    runtime::CrashEvent::sleep_button_request);
            }
            if (interaction_.state() == InteractionState::idle ||
                interaction_.state() == InteractionState::sleep_pending) {
                capture_.discard();
                recording_prepared_for_press_ = false;
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

    void wake_for_button(
        std::uint32_t now_ms, bool display_already_ready = false) {
        low_battery_poweroff_pending_ = false;
        // The display wake runs while PWR and voice work have priority, so its
        // forced footer refresh cannot read the PMIC. Keep one request until
        // that priority ends. This also catches a charge-direction change that
        // does not produce a new VBUS event while USB stays connected.
        battery_wake_refresh_pending_ = true;
        display_available_.store(true, std::memory_order_release);
        display_sleep_pending_ = false;
        poweroff_gate_.recover();
        interaction_.ready(now_ms);
        interaction_.wake_button_down(now_ms);
        previous_state_ = interaction_.state();
        prepare_recording();
        display_recovery_requested_for_press_ = true;
        if (!display_already_ready) {
            request_display_wake(now_ms);
        }
        crash_diagnostics::mark(runtime::CrashEvent::phone_proxy_wake_start);
        if (startup_services_applied_ && !ensure_ble_started()) {
            ESP_LOGW(kTag, "BLE start could not be scheduled during wake");
        }
    }

    void request_display_wake(std::uint32_t now_ms) {
        display_wake_generation_ = request_display(true, true, now_ms);
        display_wake_pending_ = true;
        display_wake_attempted_at_ms_ = now_ms;
    }

    void retry_display_wake(std::uint32_t now_ms) {
        if (!display_wake_pending_ ||
            now_ms - display_wake_attempted_at_ms_ < kDisplayWakeRetryMs) {
            return;
        }
        if (!display_request_complete(display_wake_generation_)) {
            return;
        }
        poll_display_controller();
        if (!display_wake_pending_) {
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
        if (display_request_complete(display_sleep_generation_)) {
            poll_display_controller();
            if (display_sleep_pending_) {
                display_sleep_generation_ = request_display(
                    false, display_sleep_keep_panel_ready_, now_ms);
            }
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

    void prepare_recording() {
        if (recording_prepared_for_press_) {
            return;
        }
        recording_prepared_for_press_ = true;
        capture_.discard();
        crash_diagnostics::mark(runtime::CrashEvent::audio_prepare_begin);
        capture_prepare_result_ = capture_.prepare();
        if (capture_prepare_result_ == ESP_OK) {
            crash_diagnostics::mark(
                runtime::CrashEvent::audio_prepare_complete);
        }
    }

    void begin_recording(std::uint32_t now_ms) {
        pcm_sink_.cancel_and_stop();
        cancellation_.reset();
        if (capture_prepare_result_ != ESP_OK ||
            capture_.start() != ESP_OK) {
            fail("MICROPHONE COULD NOT START");
            return;
        }
        crash_diagnostics::mark(runtime::CrashEvent::audio_capture_open);
        recording_prepared_for_press_ = false;
        // Do not reinitialize the active panel for a short sleep press. If the
        // press becomes a voice hold, start audio first and then request the
        // bounded display recovery. A wake press requested recovery earlier.
        if (!display_recovery_requested_for_press_) {
            display_recovery_requested_for_press_ = true;
            request_display_wake(now_ms);
        }
        // The button-wake path starts BLE before the recording threshold.
        // Do not start more radio work between microphone start and reads.
        recording_started_at_ms_ = now_ms;
        level_refreshed_at_ms_ = now_ms;
        audio_capture_read_marked_ = false;
        audio_first_chunk_marked_ = false;
        recording_ble_stop_pending_ = false;
    }

    void capture_audio(std::uint32_t now_ms) {
        if (!audio_capture_read_marked_) {
            audio_capture_read_marked_ = true;
            crash_diagnostics::mark(
                runtime::CrashEvent::audio_capture_read_begin);
        }
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
        if (!audio_first_chunk_marked_) {
            audio_first_chunk_marked_ = true;
            crash_diagnostics::mark(runtime::CrashEvent::audio_first_chunk);
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
        const std::uint32_t released_at_ms = monotonic_ms();
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
        if (!finish_startup_services(true)) {
            capture_.discard();
            fail("DEVICE STORAGE COULD NOT START");
            return;
        }
        clear_all_image_state();
        python_tool_.clear_plot();
        pending_plot_.clear();
        const bool wifi_path_requested =
            recording_ble_stop_pending_ || network_warm_task_ != nullptr ||
            network_initialized_;
        if (recording_ble_stop_pending_) {
            if (!stop_ble_for_request()) {
                capture_.discard();
                fail("BLUETOOTH COULD NOT STOP");
                return;
            }
            recording_ble_stop_pending_ = false;
        }
        join_network_warm_worker();
        if (!wifi_path_requested) {
            if (!ensure_ble_started() ||
                !wait_for_ble_until(
                    ble_active_request_generation_, released_at_ms,
                    kPhoneProxyConnectGraceMs)) {
                ESP_LOGW(kTag, "BLE was not ready after button release");
            }
        }
        if (ble_provisioning::running() &&
            !ble_provisioning::http_proxy_available()) {
            crash_diagnostics::mark(
                runtime::CrashEvent::phone_proxy_grace_begin);
            ESP_LOGI(kTag, "Waiting for the saved phone proxy after release");
        }
        while (ble_provisioning::running() &&
               !ble_provisioning::http_proxy_available() &&
               !cancellation_.cancelled() &&
               monotonic_ms() - released_at_ms <
                   kPhoneProxyConnectGraceMs) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        const bool use_phone_proxy =
            ble_provisioning::http_proxy_available();
        if (!use_phone_proxy) {
            // The phone-proxy choice ends two seconds after release. Post the
            // radio stop at once so Wi-Fi can use the controller memory.
            (void)request_ble(false);
        }
        crash_diagnostics::mark(
            use_phone_proxy
                ? runtime::CrashEvent::phone_proxy_ready
                : runtime::CrashEvent::phone_proxy_fallback);
        if (!use_phone_proxy && !settings_.has_wifi_credentials()) {
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

        finish_network_context_worker(true);
        if (!use_phone_proxy) {
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
        }
        NetworkRequestGuard network_request(network_, !use_phone_proxy);
        agent::Error error = agent::Error::none;
        if (!use_phone_proxy) {
            if (!network_.connected()) {
                draw_footer(ui::RadioIndicator::wifi_connecting, 0);
            }
            error = network_.connect(
                settings_.wifi(), request_cancellation);
            refresh_footer(monotonic_ms(), true);
            error = request_cancellation.normalize(error);
            if (error != agent::Error::none) {
                capture_.discard();
                finish_with_error(error);
                return;
            }
        } else {
            ESP_LOGI(kTag, "Using the secure iPhone network proxy");
        }
        timing_.mark(runtime::TurnPhase::network_ready, monotonic_ms());
        if (!use_phone_proxy) {
            timing_.set_rssi_band(network_.rssi_band());
        }
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

        // Visual results are optional and are not needed for recording. Build
        // their views after microphone and transport setup, before the first
        // model result can publish an image or plot.
        with_display([&transcript]() {
            ui::show_transcript(
                {transcript.data(), transcript.size()});
        });
        prepare_deferred_ui_for_request();

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
            fail("THE DEVICE COULD NOT START ANSWER SPEECH");
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
            speech_segmenter_.reset();
            speech_channel_.discard_pending_and_finish();
            const agent::Error speech_error = wait_for_speech_worker();
            (void)speech_error;
            clear_all_image_state();
            pending_plot_.clear();
            if (device_control_.power_off_pending() &&
                !cancellation_.cancelled()) {
                finish_model_power_off();
                return;
            }
            if (device_control_.restart_pending() &&
                !cancellation_.cancelled()) {
                finish_model_restart();
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
                voice_priority_.store(
                    button_pressed_.load(std::memory_order_acquire),
                    std::memory_order_release);
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
            clear_all_image_state();
            if (device_control_.power_off_pending() &&
                !cancellation_.cancelled()) {
                finish_model_power_off();
                return;
            }
            if (device_control_.restart_pending() &&
                !cancellation_.cancelled()) {
                finish_model_restart();
                return;
            }
            fail("THE DEVICE COULD NOT QUEUE THE COMPLETE ANSWER");
            return;
        }
        speech_channel_.finish();

        with_display([&answer]() {
            ui::show_answer({answer.data(), answer.size()});
        });

        error = request_cancellation.normalize(wait_for_speech_worker());
        timing_.mark(runtime::TurnPhase::playback_finish, monotonic_ms());
        if (error != agent::Error::none &&
            !speech_started_for_turn_.load(std::memory_order_acquire) &&
            image_requested_.load(std::memory_order_acquire) &&
            !request_cancellation.cancelled()) {
            // The speech task has released its stack and codec resources.
            // The optional image can now use the remaining memory.
            start_image_worker(request_cancellation);
        }
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

        join_image_worker();

        if (cancellation_.cancelled()) {
            cancel_current();
            return;
        }

        if (device_control_.power_off_pending()) {
            finish_model_power_off();
            return;
        }
        if (device_control_.restart_pending()) {
            finish_model_restart();
            return;
        }

        const bool visual_ready =
            pending_plot_.ready() || image_frame_.available();
        if (!speech_failed) {
            if (image_unavailable_.load(std::memory_order_acquire)) {
                with_display([&answer]() {
                    ui::show_answer_notice(
                        {answer.data(), answer.size()},
                        "IMAGE UNAVAILABLE");
                });
            } else if (!visual_ready) {
                show_state(InteractionState::idle);
            }
        }
        const bool visual_published =
            publish_selected_visual(image_frame_, pending_plot_);
        if (!visual_published && !speech_failed) {
            image_unavailable_.store(true, std::memory_order_release);
            with_display([&answer]() {
                ui::show_answer_notice(
                    {answer.data(), answer.size()},
                    "IMAGE UNAVAILABLE");
            });
        }
        const std::uint32_t completed_at_ms = monotonic_ms();
        timing_.mark(runtime::TurnPhase::completion, completed_at_ms);
        interaction_.interaction_finished(completed_at_ms);
        previous_state_ = interaction_.state();
        ESP_LOGI(kTag, "Interaction complete; idle timer started");
        log_turn_timing();
        voice_priority_.store(
            button_pressed_.load(std::memory_order_acquire),
            std::memory_order_release);
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

    bool publish_selected_visual(
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
            return true;
        }
        if (!frame.available()) {
            return true;
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
        return shown;
    }

    void finish_model_power_off() {
        if (cancellation_.cancelled() ||
            button_pressed_.load(std::memory_order_acquire)) {
            cancel_current();
            return;
        }
        crash_diagnostics::mark(
            runtime::CrashEvent::sleep_model_request);
        clear_all_image_state();
        python_tool_.clear_plot();
        pending_plot_.clear();
        with_display([]() {
            ui::show_answer_notice("TURNING OFF", "POWER OFF");
        });
        interaction_.cancel_for_sleep();
        timing_.mark(runtime::TurnPhase::completion, monotonic_ms());
        log_turn_timing();
        voice_priority_.store(
            button_pressed_.load(std::memory_order_acquire),
            std::memory_order_release);
        ESP_LOGI(kTag, "The model scheduled device power-off");
    }

    void finish_model_restart() {
        if (cancellation_.cancelled() ||
            button_pressed_.load(std::memory_order_acquire)) {
            cancel_current();
            return;
        }
        crash_diagnostics::mark(
            runtime::CrashEvent::restart_model_request);
        timing_.mark(runtime::TurnPhase::completion, monotonic_ms());
        log_turn_timing();
        voice_priority_.store(
            button_pressed_.load(std::memory_order_acquire),
            std::memory_order_release);
        ESP_LOGI(kTag, "The model scheduled a device restart");
        (void)finish_startup_services(true);
        if ((xEventGroupGetBits(speech_events_) &
             kStartupServicesDoneBit) == 0) {
            fail("DEVICE STORAGE COULD NOT STOP");
            return;
        }
        (void)stop_ble();
        if (cancellation_.cancelled() ||
            button_pressed_.load(std::memory_order_acquire)) {
            cancel_current();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    }

    void finish_with_error(agent::Error error) {
        if (error == agent::Error::cancelled || cancellation_.cancelled()) {
            cancel_current();
        } else {
            fail(request_error_message(error));
        }
    }

    void cancel_current() {
        device_control_.cancel_pending_action();
        if (startup_memory_result_.load(std::memory_order_acquire) == ESP_OK) {
            memory_store_.clear_turn_state();
        }
        capture_.discard();
        recording_prepared_for_press_ = false;
        speech_channel_.cancel();
        pcm_sink_.cancel_and_stop();
        clear_all_image_state();
        python_tool_.clear_plot();
        pending_plot_.clear();
        hide_visual();
        interaction_.ready(monotonic_ms());
        previous_state_ = interaction_.state();
        show_state(previous_state_);
        voice_priority_.store(
            button_pressed_.load(std::memory_order_acquire),
            std::memory_order_release);
    }

    void fail(const char *message) {
        device_control_.cancel_pending_action();
        if (startup_memory_result_.load(std::memory_order_acquire) == ESP_OK) {
            memory_store_.clear_turn_state();
        }
        capture_.discard();
        recording_prepared_for_press_ = false;
        speech_channel_.cancel();
        pcm_sink_.cancel_and_stop();
        clear_all_image_state();
        python_tool_.clear_plot();
        pending_plot_.clear();
        show_error(message);
        interaction_.fail(monotonic_ms());
        previous_state_ = interaction_.state();
        voice_priority_.store(
            button_pressed_.load(std::memory_order_acquire),
            std::memory_order_release);
    }

    void apply_pending_settings_display() {
        if (!settings_display_pending_ ||
            voice_priority_.load(std::memory_order_acquire) ||
            button_pressed_.load(std::memory_order_acquire) ||
            !display_available_.load(std::memory_order_acquire)) {
            return;
        }
        if (with_display([this]() {
                ui::hide_fullscreen_visual();
                (void)ui::set_chat_font_scale(
                    settings_.chat_font_scale_percent);
                if (quick_controls_enabled_) {
                    ui::sync_quick_controls(
                        device_control_.brightness_percent(),
                        device_control_.volume_percent());
                }
                ui::show_state(interaction_.state());
            })) {
            settings_display_pending_ = false;
        }
    }

    void refresh_settings(bool apply_display = true) {
        // Keep the current radio state until the phone confirms the settings
        // indication. A Wi-Fi reconnect must not interrupt that confirmation.
        if (ble_provisioning::settings_confirmation_pending()) {
            return;
        }
        const provisioning::StoredVersion version =
            settings_store_.stored_version();
        if (!version.present || version.revision == applied_revision_) {
            return;
        }
        provisioning::SettingsRecord record;
        if (!settings_store_.read(&record) || !settings_.load(record)) {
            return;
        }
        crash_diagnostics::mark(runtime::CrashEvent::settings_apply_begin);
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
        clear_all_image_state();
        python_tool_.clear_plot();
        pending_plot_.clear();
        settings_display_pending_ = true;
        if (apply_display) {
            apply_pending_settings_display();
        }
        crash_diagnostics::mark(runtime::CrashEvent::settings_apply_complete);
    }

    void enter_sleep() {
        device_control_.cancel_pending_action();
        display_available_.store(false, std::memory_order_release);
        app_mode_.store(AppMode::chat, std::memory_order_release);
        mode_button_pressed_.store(false, std::memory_order_release);
        clock_network_attempted_ = false;
        clock_ble_time_sync_requested_ = false;
        clock_network_stop_pending_ = false;
        clock_unpowered_since_ms_ = 0;
        clock_unpowered_timer_running_ = false;
        mode_display_pending_ = false;
        display_wake_pending_ = false;
        footer_shown_ = false;
        // Turn the AMOLED off before network and peripheral cleanup.
        display_sleep_keep_panel_ready_ = true;
        display_sleep_generation_ = request_display(
            false, display_sleep_keep_panel_ready_, monotonic_ms());
        const bool display_slept =
            wait_for_display(display_sleep_generation_);
        display_sleep_pending_ = !display_slept;
        display_sleep_attempted_at_ms_ = monotonic_ms();
        if (!display_slept) {
            ESP_LOGE(kTag, "Display sleep failed");
        }
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
        recording_prepared_for_press_ = false;
        pcm_sink_.cancel_and_stop();

        // The panel is black before this wait. Do not cut power while the
        // startup worker can still own NVS. A new PWR press cancels the wait.
        const std::uint32_t storage_wait_started_ms = monotonic_ms();
        EventBits_t startup_bits = xEventGroupGetBits(speech_events_);
        while ((startup_bits & kStartupServicesDoneBit) == 0 &&
               monotonic_ms() - storage_wait_started_ms <
                   kStartupServicesSleepWaitMs) {
            if (button_pressed_.load(std::memory_order_acquire)) {
                return;
            }
            startup_bits = xEventGroupWaitBits(
                speech_events_, kStartupServicesDoneBit, pdFALSE, pdTRUE,
                pdMS_TO_TICKS(10));
        }
        if ((startup_bits & kStartupServicesDoneBit) == 0) {
            ESP_LOGE(kTag, "Storage startup blocked sleep");
            recover_poweroff(monotonic_ms());
            return;
        }
        // Settings success is required for a cloud request, not for sleep.
        // The completed worker no longer owns NVS, so system-off is safe.
        (void)finish_startup_services(false);
        if (startup_memory_result_.load(std::memory_order_acquire) == ESP_OK) {
            memory_store_.clear_turn_state();
        }
        agent_loop_->clear_thread();
        clear_all_image_state();
        python_tool_.clear_plot();
        pending_plot_.clear();
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

        if (!stop_ble()) {
            // System-off removes radio power. With USB attached, the posted
            // OFF request remains active and the controller can finish it.
            ESP_LOGW(kTag, "BLE stop did not finish before system-off");
        }

        if (kDevelopmentMode) {
            if (!low_battery_poweroff_pending_) {
                crash_diagnostics::mark(
                    runtime::CrashEvent::soft_sleep_begin);
                if (!poweroff_gate_.mark_soft_sleep()) {
                    display_available_.store(true, std::memory_order_release);
                }
                return;
            }
        }
        // The cancel window has ended. Zero brightness only when system-off
        // is now the next device state. An in-session wake must not depend on
        // a CO5300 zero-to-nonzero brightness transition.
        display_sleep_keep_panel_ready_ = false;
        display_sleep_generation_ = request_display(
            false, display_sleep_keep_panel_ready_, monotonic_ms());
        display_sleep_attempted_at_ms_ = monotonic_ms();
        if (!wait_for_display(display_sleep_generation_)) {
            ESP_LOGE(kTag, "Final display shutdown failed");
            display_sleep_pending_ = true;
        }
        crash_diagnostics::mark(runtime::CrashEvent::poweroff_begin);
        if (!poweroff_gate_.mark_poweroff_ready()) {
            display_available_.store(true, std::memory_order_release);
        }
    }

    bool stop_ble_for_request() {
        return stop_ble_for_request(
            BleRestartFailurePolicy::restart_device);
    }

    bool stop_ble_for_request(
        BleRestartFailurePolicy failure_policy) {
        if (!stop_ble()) {
            return false;
        }
        return reserve_ble_restart_memory(failure_policy);
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
        ensure_ble_started();
    }

    bool stop_ble() {
        const std::uint32_t generation = request_ble(false);
        const bool stopped = wait_for_ble(generation) &&
            !ble_actual_running_.load(std::memory_order_acquire);
        ble_started_ = ble_actual_running_.load(std::memory_order_acquire);
        ble_start_attempted_ = false;
        return stopped;
    }

    bool ensure_ble_started() {
        if (!startup_services_ready()) {
            return false;
        }
        if (image_task_ != nullptr || network_warm_task_ != nullptr ||
            deferred_ui_task_ != nullptr) {
            return false;
        }
        if (ble_actual_running_.load(std::memory_order_acquire) &&
            ble_provisioning::running()) {
            ble_active_request_generation_ =
                ble_requested_generation_.load(std::memory_order_acquire);
            return true;
        }
        // The ESP controller can assert instead of returning an allocation
        // error when its 32 KiB internal block is unavailable. Release all
        // completed TLS and Wi-Fi allocations before the controller starts,
        // as at cold boot, and never call it without the required headroom.
        if (network_initialized_) {
            cancel_network_context_worker();
            stop_network_time_sync();
            cancel_transports();
            network_.shutdown();
            network_initialized_ = false;
            reset_transports();
            early_connect_attempted_at_ms_ = 0;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        ble_start_attempted_ = true;
        ble_active_request_generation_ = request_ble(true);
        return true;
    }

    bool reserve_ble_restart_memory() {
        return reserve_ble_restart_memory(
            BleRestartFailurePolicy::restart_device);
    }

    bool reserve_ble_restart_memory(
        BleRestartFailurePolicy failure_policy) {
        if (ble_restart_reservation_ != nullptr) {
            return true;
        }
        ble_restart_reservation_ = heap_caps_malloc(
            kBleControllerMinimumLargestBlockBytes,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (ble_restart_reservation_ == nullptr && network_initialized_) {
            // The active Wi-Fi driver can split the block that BLE released.
            // Recreate Wi-Fi after the reservation is in place.
            cancel_network_context_worker();
            stop_network_time_sync();
            cancel_transports();
            network_.shutdown();
            network_initialized_ = false;
            reset_transports();
            early_connect_attempted_at_ms_ = 0;
            vTaskDelay(pdMS_TO_TICKS(10));
            ble_restart_reservation_ = heap_caps_malloc(
                kBleControllerMinimumLargestBlockBytes,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        }
        if (ble_restart_reservation_ == nullptr) {
            ESP_LOGE(kTag, "BLE restart memory could not be reserved");
            if (failure_policy ==
                BleRestartFailurePolicy::return_error) {
                return false;
            }
            crash_diagnostics::mark(
                runtime::CrashEvent::ble_memory_recovery_restart);
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_restart();
            return false;
        }
        ESP_LOGI(kTag, "BLE restart memory is reserved");
        return true;
    }

    void release_ble_restart_memory() {
        if (ble_restart_reservation_ == nullptr) {
            return;
        }
        heap_caps_free(ble_restart_reservation_);
        ble_restart_reservation_ = nullptr;
    }

    void recover_poweroff(std::uint32_t now_ms) {
        low_battery_poweroff_pending_ = false;
        device_control_.cancel_pending_action();
        poweroff_gate_.recover();
        display_available_.store(true, std::memory_order_release);
        display_sleep_pending_ = false;
        (void)ensure_ble_started();
        request_display_wake(now_ms);
        show_error("CHATESP COULD NOT TURN OFF");
        interaction_.fail(monotonic_ms());
        previous_state_ = interaction_.state();
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
    agent::RestartDeviceTool restart_device_tool_;
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
    agent::ImageResults image_candidates_;
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
    TaskHandle_t display_controller_task_ = nullptr;
    TaskHandle_t ble_controller_task_ = nullptr;
    TaskHandle_t startup_services_task_ = nullptr;
    TaskHandle_t deferred_ui_task_ = nullptr;
    EventGroupHandle_t speech_events_ = nullptr;
    DeadlineCancellation *speech_cancellation_ = nullptr;
    DeadlineCancellation *image_cancellation_ = nullptr;
    std::atomic<agent::Error> speech_result_{agent::Error::model_failed};
    std::atomic<bool> network_warm_initialized_{false};
    std::atomic<agent::Error> image_error_{agent::Error::none};
    std::atomic<bool> image_requested_{false};
    std::atomic<bool> image_unavailable_{false};
    std::atomic<bool> speech_started_for_turn_{false};
    std::atomic<agent::Error> network_context_result_{
        agent::Error::model_failed};
    std::atomic<bool> queue_overflow_{false};
    std::atomic<bool> voice_priority_{false};
    std::atomic<bool> display_available_{true};
    std::atomic<bool> button_pressed_{false};
    std::atomic<bool> mode_button_pressed_{false};
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
    std::atomic<esp_err_t> startup_settings_result_{
        ESP_ERR_INVALID_STATE};
    std::atomic<esp_err_t> startup_memory_result_{
        ESP_ERR_INVALID_STATE};
    std::atomic<bool> deferred_ui_result_{false};
    std::atomic<std::uint32_t> display_requested_generation_{0};
    std::atomic<std::uint32_t> display_completed_generation_{0};
    std::atomic<bool> display_requested_on_{true};
    std::atomic<bool> display_requested_keep_ready_{true};
    std::atomic<InteractionState> display_requested_state_{
        InteractionState::booting};
    std::atomic<AppMode> display_requested_mode_{AppMode::chat};
    std::atomic<std::uint8_t> display_requested_brightness_{
        runtime::DevicePreferences::default_brightness_percent};
    std::atomic<std::uint32_t> display_request_at_ms_{0};
    std::atomic<std::uint32_t> display_completed_at_ms_{0};
    std::atomic<esp_err_t> display_result_{ESP_OK};
    std::atomic<std::uint32_t> ble_requested_generation_{0};
    std::atomic<std::uint32_t> ble_completed_generation_{0};
    std::atomic<bool> ble_requested_running_{false};
    std::atomic<bool> ble_actual_running_{false};
    std::atomic<esp_err_t> ble_result_{ESP_OK};
    runtime::PoweroffGate poweroff_gate_;
    runtime::BleControllerPlanner ble_controller_planner_{};
    std::uint32_t level_refreshed_at_ms_ = 0;
    std::uint32_t recording_started_at_ms_ = 0;
    std::uint32_t settings_checked_at_ms_ = 0;
    std::uint32_t early_connect_attempted_at_ms_ = 0;
    std::uint32_t display_wake_attempted_at_ms_ = 0;
    std::uint32_t display_sleep_attempted_at_ms_ = 0;
    std::uint32_t footer_checked_at_ms_ = 0;
    std::uint32_t battery_checked_at_ms_ = 0;
    std::uint32_t battery_power_source_revision_ = 0;
    std::uint32_t signal_checked_at_ms_ = 0;
    std::uint32_t stream_text_refreshed_at_ms_ = 0;
    std::uint32_t applied_revision_ = 0;
    std::uint32_t mode_display_attempted_at_ms_ = 0;
    std::uint32_t network_context_finished_at_ms_ = 0;
    std::uint32_t display_wake_generation_ = 0;
    std::uint32_t display_sleep_generation_ = 0;
    std::uint32_t display_completion_seen_ = 0;
    std::uint32_t ble_active_request_generation_ = 0;
    std::uint32_t ble_completion_seen_ = 0;
    std::uint32_t recording_ble_stop_generation_ = 0;
    std::uint8_t clock_refreshed_second_ = 0xff;
    bool tools_ready_ = false;
    bool startup_services_applied_ = false;
    bool startup_services_failed_reported_ = false;
    bool ble_started_ = false;
    void *ble_restart_reservation_ = nullptr;
    bool ble_start_attempted_ = false;
    bool network_initialized_ = false;
    bool context_lookup_attempted_ = false;
    bool sntp_started_ = false;
    bool sntp_complete_ = false;
    bool sntp_start_attempted_ = false;
    bool display_wake_pending_ = false;
    bool display_sleep_pending_ = false;
    bool display_sleep_keep_panel_ready_ = true;
    bool display_recovery_requested_for_press_ = true;
    bool display_refresh_pending_ = false;
    std::atomic<bool> deferred_views_ready_{false};
    bool audio_capture_read_marked_ = false;
    bool audio_first_chunk_marked_ = false;
    bool recording_prepared_for_press_ = false;
    bool recording_ble_stop_pending_ = false;
    bool battery_checked_ = false;
    bool battery_status_sample_valid_ = false;
    bool battery_wake_refresh_pending_ = false;
    bool low_battery_poweroff_pending_ = false;
    bool footer_shown_ = false;
    bool stream_text_shown_ = false;
    bool request_active_ = false;
    bool quick_controls_enabled_ = false;
    bool quick_controls_attempted_ = false;
    bool deferred_ui_task_create_failed_ = false;
    bool clock_time_available_ = false;
    bool clock_network_attempted_ = false;
    bool clock_ble_time_sync_requested_ = false;
    bool clock_network_stop_pending_ = false;
    std::uint32_t clock_network_stop_started_at_ms_ = 0;
    std::uint32_t clock_unpowered_since_ms_ = 0;
    bool clock_unpowered_timer_running_ = false;
    bool mode_display_pending_ = false;
    bool settings_display_pending_ = false;
    esp_err_t capture_prepare_result_ = ESP_ERR_INVALID_STATE;
    ui::RadioIndicator radio_indicator_ = ui::RadioIndicator::off;
    ui::RadioIndicator shown_radio_ = ui::RadioIndicator::off;
    std::uint8_t signal_band_ = 0;
    std::uint8_t shown_signal_band_ = 0;
    std::optional<power::BatteryStatus> battery_status_;
    std::optional<power::BatteryStatus> shown_battery_status_;
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

void VoiceRuntime::action_button_edge(
    bool pressed, std::uint32_t at_ms,
    bool short_press_confirmed) {
    if (impl_ != nullptr) {
        impl_->action_button_edge(
            pressed, at_ms, short_press_confirmed);
    }
}

bool VoiceRuntime::mode_button_available() const {
    return impl_ != nullptr && impl_->mode_button_available();
}

void VoiceRuntime::mode_button_edge(bool pressed) {
    if (impl_ != nullptr) {
        impl_->mode_button_edge(pressed);
    }
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
