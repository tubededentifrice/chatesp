#include "voice_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string_view>

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
#include "network_manager.hpp"
#include "pcm_playback_sink.hpp"
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
constexpr std::uint32_t kTranscriptVisibleMs = 450;
constexpr std::uint32_t kPoweroffGraceMs = 250;
constexpr std::uint32_t kInteractionTimeoutMs = 90'000;
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
            speech_model.assign("google/gemini-3.1-flash-tts-preview");
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
                "Achird",
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

class VoiceRuntime::Impl final : public agent::AgentProgressObserver {
public:
    Impl()
        : openrouter_connection_(settings_.openrouter()),
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
          pcm_sink_(playback_) {
        tools_ready_ = tools_.add(web_tool_) == agent::Error::none &&
            tools_.add(image_tool_) == agent::Error::none;
        void *agent_memory = heap_caps_malloc(
            sizeof(agent::AgentLoop), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (agent_memory != nullptr) {
            agent_loop_ = new (agent_memory)
                agent::AgentLoop(chat_provider_, tools_, this);
        }
        tools_ready_ = tools_ready_ && agent_loop_ != nullptr;
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
    }

    [[nodiscard]] bool started() const {
        return started_.load(std::memory_order_acquire);
    }

    esp_err_t start(bool startup_button_down, std::uint32_t startup_at_ms) {
        if (!tools_ready_) {
            return ESP_FAIL;
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
        if (startup_button_down) {
            button_pressed_.store(true, std::memory_order_release);
            voice_priority_.store(true, std::memory_order_release);
            cancellation_.cancel();
            const Command command{
                CommandKind::wake_button_down, startup_at_ms};
            queue_command(command);
        }
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
    void with_display(Callback callback) {
        if (bsp_display_lock(100)) {
            callback();
            bsp_display_unlock();
        }
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
        interaction_.ready(monotonic_ms());
        previous_state_ = interaction_.state();
        show_state(previous_state_);

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
            if (interaction_.state() == InteractionState::recording) {
                capture_audio(now_ms);
            } else if (interaction_.state() == InteractionState::idle &&
                       now_ms - settings_checked_at_ms_ >=
                           kSettingsRefreshMs) {
                settings_checked_at_ms_ = now_ms;
                if (!interaction_.button_is_down() && !ble_started_ &&
                    !ble_start_attempted_) {
                    ensure_ble_started();
                }
                refresh_settings();
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
            wake_for_button(command.at_ms);
            return;
        }
        if (command.kind == CommandKind::button_down) {
            interaction_.button_down(command.at_ms);
        } else if (command.kind == CommandKind::button_up) {
            interaction_.button_up(command.at_ms);
            if (interaction_.state() == InteractionState::idle) {
                voice_priority_.store(false, std::memory_order_release);
            }
        }
    }

    void wake_for_button(std::uint32_t now_ms) {
        display_available_.store(true, std::memory_order_release);
        poweroff_gate_.recover();
        ui::wake(InteractionState::idle);
        interaction_.ready(now_ms);
        interaction_.wake_button_down(now_ms);
        previous_state_ = interaction_.state();
    }

    void process_state_change(std::uint32_t now_ms) {
        if (interaction_.state() == previous_state_) {
            return;
        }
        const InteractionState prior = previous_state_;
        previous_state_ = interaction_.state();
        if (previous_state_ == InteractionState::sleep_pending) {
            poweroff_gate_.begin_sleep();
        }
        show_state(previous_state_);

        if (previous_state_ == InteractionState::recording) {
            begin_recording(now_ms);
        } else if (prior == InteractionState::recording &&
                   previous_state_ == InteractionState::transcribing) {
            finish_recording_and_request();
        } else if (previous_state_ == InteractionState::sleep_pending) {
            enter_sleep();
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

        with_display([]() { ui::show_wifi_progress("CONNECTING"); });
        if (!network_initialized_) {
            const esp_err_t network_result = network_.initialize();
            if (network_result != ESP_OK) {
                capture_.discard();
                fail("WI-FI COULD NOT START");
                return;
            }
            network_initialized_ = true;
        }
        agent::Error error = network_.connect(
            settings_.wifi(), request_cancellation);
        error = request_cancellation.normalize(error);
        if (error != agent::Error::none) {
            capture_.discard();
            finish_with_error(error);
            return;
        }
        show_state(InteractionState::transcribing);

        agent::FixedText<agent::Limits::max_transcript_bytes> transcript;
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

        agent::FixedText<agent::Limits::max_answer_bytes> answer;
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
        if (error != agent::Error::none) {
            pcm_sink_.cancel_and_stop();
            finish_with_error(error);
            return;
        }

        interaction_.interaction_finished(monotonic_ms());
        previous_state_ = interaction_.state();
        show_state(previous_state_);
        voice_priority_.store(false, std::memory_order_release);
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

    void finish_with_error(agent::Error error) {
        if (error == agent::Error::cancelled || cancellation_.cancelled()) {
            cancel_current();
        } else {
            fail(error_message(error));
        }
    }

    void cancel_current() {
        capture_.discard();
        pcm_sink_.cancel_and_stop();
        image_tool_.clear_results();
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
        show_state(interaction_.state());
    }

    void enter_sleep() {
        cancellation_.cancel();
        transport_.cancel_active();
        capture_.discard();
        pcm_sink_.cancel_and_stop();
        agent_loop_->clear_thread();
        image_tool_.clear_results();
        if (network_initialized_) {
            network_.shutdown();
            network_initialized_ = false;
        }
        display_available_.store(false, std::memory_order_release);

        const std::uint32_t grace_started_ms = monotonic_ms();
        while (monotonic_ms() - grace_started_ms < kPoweroffGraceMs) {
            if (button_pressed_.load(std::memory_order_acquire)) {
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        stop_ble_for_sleep();

        if (kDevelopmentMode) {
            ui::sleep();
            if (!poweroff_gate_.mark_soft_sleep()) {
                display_available_.store(true, std::memory_order_release);
            }
            return;
        }
        ui::sleep();
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
        ensure_ble_started();
        interaction_.fail(now_ms);
        previous_state_ = interaction_.state();
        ui::wake(previous_state_);
        show_error("THE WATCH COULD NOT TURN OFF");
    }

    RuntimeSettings settings_;
    SettingsStore settings_store_;
    network::NetworkManager network_;
    transport::HttpTransport transport_;
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
    std::uint32_t applied_revision_ = 0;
    bool tools_ready_ = false;
    bool ble_started_ = false;
    bool ble_start_attempted_ = false;
    bool network_initialized_ = false;
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
