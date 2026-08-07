#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace chatesp {
namespace provisioning {

constexpr std::uint8_t kProtocolVersion = 1;
constexpr std::uint8_t kSettingsPacketType = 1;
constexpr std::size_t kHeaderSize = 48;
constexpr std::size_t kMaximumPacketSize = 1024;
constexpr std::size_t kFingerprintSize = 32;
constexpr std::uint8_t kRequiredFieldCount = 8;

enum class ValidationError : std::uint8_t {
    none,
    authentication_required,
    packet_too_short,
    packet_too_large,
    bad_magic,
    unsupported_version,
    bad_type,
    bad_flags,
    bad_length,
    bad_field_count,
    bad_fingerprint,
    bad_field_order,
    missing_field,
    duplicate_field,
    invalid_utf8,
    invalid_endpoint,
    invalid_openrouter_key,
    invalid_brave_key,
    invalid_wifi_ssid,
    invalid_wifi_password,
    invalid_model,
    stale_revision,
    revision_conflict,
};

enum class ApplyDecision : std::uint8_t {
    reject,
    apply,
    unchanged,
};

struct LinkSecurity {
    bool encrypted = false;
    bool authenticated = false;
    bool bonded = false;
    bool lesc = false;
};

struct StoredVersion {
    bool present = false;
    std::uint32_t revision = 0;
    std::array<std::uint8_t, kFingerprintSize> fingerprint{};
};

struct SettingsView {
    std::string_view chat_endpoint;
    std::string_view openrouter_key;
    std::string_view brave_key;
    std::string_view wifi_ssid;
    std::string_view wifi_password;
    std::string_view chat_model;
    std::string_view transcription_model;
    std::string_view speech_model;
};

struct ValidationResult {
    ValidationError error = ValidationError::none;
    ApplyDecision decision = ApplyDecision::reject;
    std::uint32_t revision = 0;
    std::array<std::uint8_t, kFingerprintSize> fingerprint{};
    SettingsView settings{};
};

[[nodiscard]] bool link_is_secure(const LinkSecurity &security);

[[nodiscard]] std::array<std::uint8_t, kFingerprintSize>
compute_content_fingerprint(
    std::uint8_t version,
    std::uint8_t packet_type,
    std::uint8_t field_count,
    const std::uint8_t *payload,
    std::size_t payload_size);

[[nodiscard]] ValidationResult validate_settings_packet(
    const std::uint8_t *packet,
    std::size_t packet_size,
    const LinkSecurity &security,
    const StoredVersion &stored);

}  // namespace provisioning
}  // namespace chatesp
