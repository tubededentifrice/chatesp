#include "voice_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <string_view>
#include <utility>

#include "audio_capture.hpp"
#include "audio_playback.hpp"
#include "ble_provisioning.hpp"
#include "bsp/esp-bsp.h"
#include "chatesp/agent_loop.hpp"
#include "chatesp/audio_level.hpp"
#include "chatesp/ble_settings.hpp"
#include "chatesp/interaction_state.hpp"
#include "chatesp/runtime_control.hpp"
#include "cloud_providers.hpp"
#include "device_settings.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "http_transport.hpp"
#include "image_fetch_provider.hpp"
#include "jpeg_image_sink.hpp"
#include "network_manager.hpp"
#include "pcm_playback_sink.hpp"
#include "power_control.hpp"
#include "settings_store.hpp"
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
constexpr std::uint32_t kTranscriptVisibleMs = 450;
constexpr std::uint32_t kPoweroffGraceMs = 250;
constexpr std::uint32_t kInteractionTimeoutMs = 180'000;
constexpr std::size_t kMinimumRecordingSamples =
    AudioCapture::kSampleRateHz / 10;
constexpr UBaseType_t kRuntimePriority = 5;
constexpr std::uint32_t kRuntimeStackBytes = 32 * 1024;
constexpr UBaseType_t kPasskeyPriority = 6;
constexpr std::uint32_t kPasskeyStackBytes = 4 * 1024;

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

    void clear() {
        transcript.clear();
        answer.clear();
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
            chat_model.assign("deepseek/deepseek-v4-flash") &&
            transcription_model.assign("openai/whisper-large-v3-turbo") &&
            speech_model.assign("hexgrad/kokoro-82m");
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
            next.speech_model.assign(record.speech_model.view());
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

    provisioning::BoundedSetting<192> endpoint;
    provisioning::BoundedSetting<256> openrouter_key;
    provisioning::BoundedSetting<128> brave_key;
    provisioning::BoundedSetting<32> wifi_ssid;
    provisioning::BoundedSetting<63> wifi_password;
    provisioning::BoundedSetting<96> chat_model;
    provisioning::BoundedSetting<96> transcription_model;
    provisioning::BoundedSetting<96> speech_model;
    bool configured = false;
};

enum class CommandKind : std::uint8_t {
    button_down,
    wake_button_down,
    button_up,
    wake_from_poweroff,
    poweroff_failed,
    provisioning_activity,
};

struct Command {
    CommandKind kind = CommandKind::button_down;
    std::uint32_t at_ms = 0;
};

struct PasskeyEvent {
    std::uint32_t passkey = 0;
    bool visible = false;
};

const char *error_message(agent::Error error) {
    switch (error) {
        case agent::Error::authentication:
            return "CHECK THE SERVICE KEY";
        case agent::Error::payment_required:
            return "THE SERVICE NEEDS CREDIT";
        case agent::Error::rate_limited:
            return "THE SERVICE IS BUSY";
        case agent::Error::connect_timeout:
        case agent::Error::disconnected:
            return "CHECK THE WI-FI CONNECTION";
        case agent::Error::first_byte_timeout:
        case agent::Error::idle_timeout:
        case agent::Error::total_timeout:
            return "THE REQUEST TOOK TOO LONG";
        case agent::Error::unsupported_media:
            return "THE AUDIO FORMAT WAS NOT VALID";
        case agent::Error::limit_exceeded:
            return "THE REQUEST WAS TOO LARGE";
        default:
            return "PLEASE TRY THE REQUEST AGAIN";
    }
}

std::size_t bounded_speech_size(const char *text, std::size_t size) {
    if (text == nullptr) {
        return 0;
    }
    std::size_t result =
        std::min(size, agent::Limits::max_tts_input_bytes);
    while (result > 0 &&
           (static_cast<unsigned char>(text[result]) & 0xc0U) == 0x80U) {
        --result;
    }
    return result;
}

}  // namespace

class VoiceRuntime::Impl final : public agent::AgentProgressObserver,
                                 public agent::ChatTextSink {
public:
    Impl()
        : image_fetch_provider_(transport_),
          openrouter_connection_(settings_.openrouter()),
          brave_key_(settings_.brave()),
          web_provider_(transport_, network_, brave_key_),
          image_provider_(transport_, network_, brave_key_),
          web_tool_(web_provider_),
          image_tool_(image_provider_),
          chat_provider_(
              transport_, network_, openrouter_connection_, tools_),
          transcription_provider_(
              transport_, network_, openrouter_connection_),
          speech_provider_(transport_, network_, openrouter_connection_),
          pcm_sink_(playback_),
          interaction_(interaction_config_for_mode(kDevelopmentMode)) {
        tools_ready_ = tools_.add(web_tool_) == agent::Error::none &&
            tools_.add(image_tool_) == agent::Error::none;
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
        transport_.cancel_active();
        capture_.cancel();
        pcm_sink_.cancel();
        if (ble_started_) {
            ble_provisioning::stop();
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
        // Start NVS through the settings layer before Wi-Fi or NimBLE can use
        // it. In production, this is the controlled HMAC security start.
        const esp_err_t settings_result = settings_store_.initialize();
        if (settings_result != ESP_OK) {
            return settings_result;
        }
        command_queue_ = xQueueCreate(16, sizeof(Command));
        passkey_queue_ = xQueueCreate(1, sizeof(PasskeyEvent));
        if (command_queue_ == nullptr || passkey_queue_ == nullptr) {
            if (command_queue_ != nullptr) {
                vQueueDelete(command_queue_);
                command_queue_ = nullptr;
            }
            if (passkey_queue_ != nullptr) {
                vQueueDelete(passkey_queue_);
                passkey_queue_ = nullptr;
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
            command_queue_ = nullptr;
            passkey_queue_ = nullptr;
            return ESP_ERR_NO_MEM;
        }
        if (startup_button_down) {
            button_pressed_.store(true, std::memory_order_release);
            voice_priority_.store(true, std::memory_order_release);
            cancellation_.cancel();
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
            command_queue_ = nullptr;
            passkey_queue_ = nullptr;
            return ESP_ERR_NO_MEM;
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
            voice_priority_.store(true, std::memory_order_release);
            cancellation_.cancel();
            capture_.cancel();
            pcm_sink_.cancel();
            transport_.cancel_active();
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
                show_model_progress("ASKING THE MODEL");
                break;
            case agent::AgentProgressEvent::tool_start:
                interaction_.tool_started(now_ms);
                show_state(interaction_.state());
                break;
            case agent::AgentProgressEvent::answer_ready:
                show_model_progress("ANSWER READY");
                break;
            case agent::AgentProgressEvent::speech_start:
                interaction_.speech_started(now_ms);
                show_state(interaction_.state());
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

private:
    static void task_entry(void *context) {
        static_cast<Impl *>(context)->run();
    }

    static void passkey_task_entry(void *context) {
        static_cast<Impl *>(context)->run_passkey_ui();
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
        with_display([state]() { ui::show_state(state); });
    }

    void show_error(const char *message) {
        with_display([message]() { ui::show_error(message); });
    }

    void show_model_progress(const char *message) {
        with_display(
            [message]() { ui::show_model_progress(message); });
    }

    void hide_image() {
        with_display([]() { ui::hide_fullscreen_image(); });
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
            network_.state(), settings_.configured);
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
                with_display([desired]() {
                    ui::show_ble_passkey(
                        desired.passkey, desired.visible);
                });
                shown = desired;
                has_shown = true;
            }
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

            const std::uint32_t now_ms = monotonic_ms();
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
            if (!voice_priority_.load(std::memory_order_acquire) &&
                now_ms - footer_checked_at_ms_ >= kFooterRefreshMs) {
                footer_checked_at_ms_ = now_ms;
                refresh_footer(now_ms, false);
            }
            if (!recording &&
                interaction_.state() == InteractionState::idle &&
                now_ms - settings_checked_at_ms_ >=
                    kSettingsRefreshMs) {
                settings_checked_at_ms_ = now_ms;
                if (!interaction_.button_is_down() && !ble_started_ &&
                    !ble_start_attempted_) {
                    ensure_ble_started();
                }
                refresh_settings();
                if (!interaction_.button_is_down()) {
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
            interaction_.note_idle_activity(command.at_ms);
            return;
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
            const esp_err_t panel_error = ui::reassert_panel();
            if (panel_error != ESP_OK) {
                display_wake_pending_ = true;
                display_wake_attempted_at_ms_ = command.at_ms;
            }
        } else if (command.kind == CommandKind::button_up) {
            ESP_LOGI(kTag, "Action button was released");
            interaction_.button_up(command.at_ms);
            if (interaction_.state() == InteractionState::idle) {
                voice_priority_.store(false, std::memory_order_release);
            }
        }
    }

    void start_network_early(bool force) {
        if (!settings_.configured || network_.connected() ||
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
        const esp_err_t result = ui::wake(interaction_.state());
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
        pcm_sink_.cancel_and_stop();
        capture_.discard();
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
                interaction_.button_up(now_ms);
                process_state_change(now_ms);
            } else {
                fail("MICROPHONE READ FAILED");
            }
            return;
        }
        if (now_ms - level_refreshed_at_ms_ >= kLevelRefreshMs) {
            level_refreshed_at_ms_ = now_ms;
            const std::uint8_t level = pcm_peak_percent(
                capture_.samples(),
                capture_.sample_count(),
                AudioCapture::kChunkSamples);
            with_display(
                [level]() { ui::show_recording_level(level); });
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
        if (!settings_.configured) {
            capture_.discard();
            fail("SET UP THE WATCH WITH THE IPHONE APP");
            return;
        }
        DeadlineCancellation request_cancellation(
            cancellation_, monotonic_ms(), kInteractionTimeoutMs);

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
        error = transcription_provider_.transcribe(
            audio, transcript, request_cancellation);
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
        if (!cancellable_delay(
                kTranscriptVisibleMs, request_cancellation)) {
            finish_with_error(request_cancellation.normalize(
                agent::Error::cancelled));
            return;
        }

        auto &answer = request_scratch_->answer;
        answer.clear();
        stream_text_shown_ = false;
        stream_text_refreshed_at_ms_ = 0;
        SecureTextGuard answer_guard(answer);
        error = agent_loop_->run(
            transcript.data(), transcript.size(), answer,
            request_cancellation);
        error = request_cancellation.normalize(error);
        if (error != agent::Error::none) {
            finish_with_error(error);
            return;
        }

        with_display([&answer]() {
            ui::show_answer({answer.data(), answer.size()});
        });
        const std::size_t speech_size =
            bounded_speech_size(answer.data(), answer.size());
        if (speech_size == 0) {
            fail("THE ANSWER COULD NOT BE SPOKEN");
            return;
        }
        SpeechStartSink speech_sink(pcm_sink_, *agent_loop_);
        error = speech_provider_.speak(
            answer.data(), speech_size, speech_sink,
            request_cancellation);
        error = request_cancellation.normalize(error);
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
            with_display([&answer]() {
                ui::show_answer_notice(
                    {answer.data(), answer.size()},
                    "SPEECH TEMPORARILY UNAVAILABLE");
            });
        }

        if (!show_selected_image(request_cancellation) &&
            cancellation_.cancelled()) {
            cancel_current();
            return;
        }

        interaction_.interaction_finished(monotonic_ms());
        previous_state_ = interaction_.state();
        if (!speech_failed) {
            show_state(previous_state_);
        }
        voice_priority_.store(false, std::memory_order_release);
        ESP_LOGI(kTag, "Interaction complete; idle timer started");
        ESP_LOGI(
            kTag,
            "Runtime stack minimum free bytes: %u",
            static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    }

    bool cancellable_delay(
        std::uint32_t duration_ms,
        agent::CancellationToken &cancellation) {
        const std::uint32_t started = monotonic_ms();
        while (monotonic_ms() - started < duration_ms) {
            if (cancellation.cancelled()) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        return true;
    }

    bool show_selected_image(DeadlineCancellation &cancellation) {
        agent::ImageResult selected;
        if (!image_tool_.take_selected_or_first(selected)) {
            return true;
        }

        ESP_LOGI(kTag, "A bounded image result was selected");

        show_model_progress("GETTING THE IMAGE");
        image::JpegImageSink sink(cancellation);
        const agent::ImageFetchRequest request{
            selected.thumbnail_url.c_str(),
            agent::Limits::max_image_download_bytes,
            agent::Limits::max_image_dimension,
            0,
            agent::image_fetch_policy(),
        };
        agent::Error error = image_fetch_provider_.fetch(
            request, sink, cancellation);
        error = cancellation.normalize(error);
        if (cancellation_.cancelled()) {
            return false;
        }
        if (error != agent::Error::none || !sink.ready()) {
            ESP_LOGW(
                kTag,
                "Optional image was not available (category %u)",
                static_cast<unsigned>(error));
            return true;
        }

        image::Rgb565Frame frame = sink.take_frame();
        bool shown = false;
        with_display([&frame, &shown]() {
            shown = ui::show_fullscreen_image(std::move(frame));
        });
        if (!shown) {
            ESP_LOGW(kTag, "Optional image could not be shown");
        } else {
            ESP_LOGI(kTag, "The bounded image is on the display");
        }
        return true;
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
            fail(error_message(error));
        }
    }

    void cancel_current() {
        capture_.discard();
        pcm_sink_.cancel_and_stop();
        image_tool_.clear_results();
        hide_image();
        interaction_.ready(monotonic_ms());
        previous_state_ = interaction_.state();
        show_state(previous_state_);
        voice_priority_.store(false, std::memory_order_release);
    }

    void fail(const char *message) {
        capture_.discard();
        pcm_sink_.cancel_and_stop();
        image_tool_.clear_results();
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
        transcription_provider_.set_connection(openrouter_connection_);
        speech_provider_.set_connection(openrouter_connection_);
        web_provider_.set_api_key(brave_key_);
        image_provider_.set_api_key(brave_key_);
        if (network_initialized_) {
            network_.disconnect();
        }
        agent_loop_->clear_thread();
        image_tool_.clear_results();
        hide_image();
        show_state(interaction_.state());
        if (!interaction_.button_is_down()) {
            start_network_early(true);
        }
    }

    void enter_sleep() {
        display_available_.store(false, std::memory_order_release);
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
        cancellation_.cancel();
        transport_.cancel_active();
        capture_.discard();
        pcm_sink_.cancel_and_stop();
        agent_loop_->clear_thread();
        image_tool_.clear_results();
        hide_image();
        if (network_initialized_) {
            network_.shutdown();
            network_initialized_ = false;
        }
        early_connect_attempted_at_ms_ = 0;
        const std::uint32_t grace_started_ms = monotonic_ms();
        while (monotonic_ms() - grace_started_ms < kPoweroffGraceMs) {
            if (button_pressed_.load(std::memory_order_acquire)) {
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        stop_ble_for_sleep();

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

    void stop_ble_for_sleep() {
        if (!ble_started_) {
            return;
        }
        const esp_err_t result = ble_provisioning::stop();
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "BLE provisioning stop failed");
        }
        ble_started_ = ble_provisioning::running();
        ble_start_attempted_ = ble_started_;
    }

    bool ensure_ble_started() {
        if (ble_started_ && ble_provisioning::running()) {
            return true;
        }
        ble_start_attempted_ = true;
        const esp_err_t result = ble_provisioning::start(
            &settings_store_, passkey_callback, this);
        ble_started_ = result == ESP_OK;
        if (!ble_started_) {
            ESP_LOGE(kTag, "BLE provisioning restart failed");
        }
        return ble_started_;
    }

    void recover_poweroff(std::uint32_t now_ms) {
        display_available_.store(true, std::memory_order_release);
        display_sleep_pending_ = false;
        ensure_ble_started();
        interaction_.fail(now_ms);
        previous_state_ = interaction_.state();
        request_display_wake(now_ms);
        show_error("THE WATCH COULD NOT TURN OFF");
    }

    RuntimeSettings settings_;
    SettingsStore settings_store_;
    network::NetworkManager network_;
    transport::HttpTransport transport_;
    image::HttpImageFetchProvider image_fetch_provider_;
    AudioCapture capture_;
    AudioPlayback playback_;
    AtomicCancellation cancellation_;
    agent::ToolRegistry tools_;
    cloud::OpenRouterConnectionView openrouter_connection_;
    provider::SecretView brave_key_;
    cloud::BraveWebSearchProvider web_provider_;
    cloud::BraveImageSearchProvider image_provider_;
    agent::SearchWebTool web_tool_;
    agent::SearchImagesTool image_tool_;
    cloud::OpenRouterChatProvider chat_provider_;
    cloud::OpenRouterTranscriptionProvider transcription_provider_;
    cloud::OpenRouterSpeechProvider speech_provider_;
    agent::AgentLoop *agent_loop_ = nullptr;
    RequestScratch *request_scratch_ = nullptr;
    PcmPlaybackSink pcm_sink_;
    InteractionStateMachine interaction_;
    InteractionState previous_state_ = InteractionState::booting;
    QueueHandle_t command_queue_ = nullptr;
    QueueHandle_t passkey_queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    TaskHandle_t passkey_task_ = nullptr;
    std::atomic<bool> queue_overflow_{false};
    std::atomic<bool> voice_priority_{false};
    std::atomic<bool> display_available_{true};
    std::atomic<bool> button_pressed_{false};
    std::atomic<bool> started_{false};
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
    bool tools_ready_ = false;
    bool ble_started_ = false;
    bool ble_start_attempted_ = false;
    bool network_initialized_ = false;
    bool display_wake_pending_ = false;
    bool display_sleep_pending_ = false;
    bool battery_checked_ = false;
    bool footer_shown_ = false;
    bool stream_text_shown_ = false;
    bool request_active_ = false;
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
    bool startup_button_down, std::uint32_t startup_at_ms) {
    if (impl_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    impl_ = new (std::nothrow) Impl();
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

bool VoiceRuntime::poweroff_ready() const {
    return impl_ != nullptr && impl_->poweroff_ready();
}

void VoiceRuntime::poweroff_failed() {
    if (impl_ != nullptr) {
        impl_->poweroff_failed();
    }
}

}  // namespace chatesp
