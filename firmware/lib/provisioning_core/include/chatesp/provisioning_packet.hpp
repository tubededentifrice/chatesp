#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace chatesp {
namespace provisioning {

constexpr std::uint8_t kProtocolVersion = 4;
constexpr std::uint8_t kMinimumProtocolVersion = 1;
constexpr std::uint8_t kSettingsPacketType = 1;
constexpr std::size_t kHeaderSize = 48;
constexpr std::size_t kMaximumPacketSize = 1024;
constexpr std::size_t kFingerprintSize = 32;
constexpr std::uint8_t kDeviceContextVersion = 1;
constexpr std::size_t kDeviceContextHeaderSize = 49;
constexpr std::size_t kMaximumApproximateLocationSize = 96;
constexpr std::size_t kMaximumDeviceContextSize =
    kDeviceContextHeaderSize + kMaximumApproximateLocationSize;
constexpr std::size_t kDeviceContextAcknowledgementSize = 48;
constexpr std::uint8_t kVersion1FieldCount = 8;
constexpr std::uint8_t kVersion2FieldCount = 9;
constexpr std::uint8_t kVersion3FieldCount = 11;
constexpr std::uint8_t kRequiredFieldCount = 12;
constexpr std::uint16_t kDefaultChatFontScalePercent = 100;
constexpr std::uint16_t kMaximumChatFontScalePercent = 200;

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
    invalid_approximate_location,
    invalid_voice,
    invalid_chat_font_scale,
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
    std::string_view approximate_location;
    std::string_view english_speech_voice;
    std::string_view french_speech_voice;
    std::string_view chat_font_scale_percent;
};

[[nodiscard]] constexpr bool supported_protocol_version(std::uint8_t version) {
    return version >= kMinimumProtocolVersion && version <= kProtocolVersion;
}

struct ValidationResult {
    ValidationError error = ValidationError::none;
    ApplyDecision decision = ApplyDecision::reject;
    std::uint32_t revision = 0;
    std::array<std::uint8_t, kFingerprintSize> fingerprint{};
    SettingsView settings{};
};

enum class DeviceContextStatus : std::uint8_t {
    applied = 0x00,
    authentication_required = 0x10,
    unsupported_version = 0x11,
    malformed_packet = 0x12,
    invalid_location = 0x14,
    busy = 0x18,
};

struct DeviceContextResult {
    DeviceContextStatus status = DeviceContextStatus::malformed_packet;
    std::uint64_t epoch_seconds = 0;
    std::int16_t utc_offset_minutes = 0;
    std::array<std::uint8_t, kFingerprintSize> fingerprint{};
    std::string_view approximate_location;

    [[nodiscard]] bool valid() const {
        return status == DeviceContextStatus::applied;
    }
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

[[nodiscard]] DeviceContextResult validate_device_context_packet(
    const std::uint8_t *packet,
    std::size_t packet_size,
    const LinkSecurity &security);

[[nodiscard]] std::array<std::uint8_t, kDeviceContextAcknowledgementSize>
make_device_context_acknowledgement(
    DeviceContextStatus status,
    std::uint64_t epoch_seconds = 0,
    const std::array<std::uint8_t, kFingerprintSize> &fingerprint = {});

}  // namespace provisioning
}  // namespace chatesp
