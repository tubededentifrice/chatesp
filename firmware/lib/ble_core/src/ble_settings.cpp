#include "chatesp/ble_settings.hpp"

namespace chatesp {
namespace provisioning {

namespace {

std::uint16_t chat_font_scale(std::string_view value) {
    if (value.empty()) {
        return kDefaultChatFontScalePercent;
    }
    return static_cast<std::uint16_t>(
        (value[0] - '0') * 100 + (value[1] - '0') * 10 +
        (value[2] - '0'));
}

}  // namespace

bool SettingsRecord::assign(const ValidationResult &validation) {
    if (validation.error != ValidationError::none ||
        validation.decision == ApplyDecision::reject) {
        return false;
    }

    SettingsRecord next;
    next.revision = validation.revision;
    next.fingerprint = validation.fingerprint;
    const std::string_view english_voice =
        validation.settings.english_speech_voice.empty()
            ? std::string_view{"af_heart"}
            : validation.settings.english_speech_voice;
    const std::string_view french_voice =
        validation.settings.french_speech_voice.empty()
            ? std::string_view{"ff_siwis"}
            : validation.settings.french_speech_voice;
    const bool valid =
        next.chat_endpoint.assign(validation.settings.chat_endpoint) &&
        next.openrouter_key.assign(validation.settings.openrouter_key) &&
        next.brave_key.assign(validation.settings.brave_key) &&
        next.wifi_ssid.assign(validation.settings.wifi_ssid) &&
        next.wifi_password.assign(validation.settings.wifi_password) &&
        next.chat_model.assign(validation.settings.chat_model) &&
        next.transcription_model.assign(validation.settings.transcription_model) &&
        next.speech_model.assign(validation.settings.speech_model) &&
        next.approximate_location.assign(
            validation.settings.approximate_location) &&
        next.english_speech_voice.assign(english_voice) &&
        next.french_speech_voice.assign(french_voice);
    if (!valid) {
        next.clear();
        return false;
    }
    next.chat_font_scale_percent = chat_font_scale(
        validation.settings.chat_font_scale_percent);

    clear();
    *this = next;
    next.clear();
    return true;
}

void SettingsRecord::clear() {
    revision = 0;
    volatile std::uint8_t *cursor = fingerprint.data();
    for (std::size_t index = 0; index < fingerprint.size(); ++index) {
        cursor[index] = 0;
    }
    chat_endpoint.clear();
    openrouter_key.clear();
    brave_key.clear();
    wifi_ssid.clear();
    wifi_password.clear();
    chat_model.clear();
    transcription_model.clear();
    speech_model.clear();
    approximate_location.clear();
    english_speech_voice.clear();
    french_speech_voice.clear();
    chat_font_scale_percent = kDefaultChatFontScalePercent;
}

}  // namespace provisioning
}  // namespace chatesp
