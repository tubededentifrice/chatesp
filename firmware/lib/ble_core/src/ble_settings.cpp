#include "chatesp/ble_settings.hpp"

namespace chatesp {
namespace provisioning {

bool SettingsRecord::assign(const ValidationResult &validation) {
    if (validation.error != ValidationError::none ||
        validation.decision == ApplyDecision::reject) {
        return false;
    }

    SettingsRecord next;
    next.revision = validation.revision;
    next.fingerprint = validation.fingerprint;
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
            validation.settings.approximate_location);
    if (!valid) {
        next.clear();
        return false;
    }

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
}

}  // namespace provisioning
}  // namespace chatesp
