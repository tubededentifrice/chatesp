#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <unity.h>

#include "chatesp/ble_settings.hpp"
#include "chatesp/provisioning_packet.hpp"
#include "chatesp/provisioning_session.hpp"

namespace provisioning = chatesp::provisioning;

namespace {

constexpr provisioning::LinkSecurity kSecureLink{true, true, true, true};
using Fields = std::vector<std::pair<std::uint8_t, std::string>>;

Fields valid_fields() {
    return {
        {1, "https://openrouter.ai/api/v1"},
        {2, "OPENROUTER_TOKEN_PLACEHOLDER"},
        {3, "BRAVE_TOKEN_PLACEHOLDER"},
        {4, "Test Network"},
        {5, "PASSWORD_PLACEHOLDER"},
        {6, "deepseek/deepseek-v4-flash"},
        {7, "openai/whisper-large-v3-turbo"},
        {8, "google/gemini-3.1-flash-tts-preview"},
    };
}

void append_u16(std::vector<std::uint8_t> &output, std::size_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value));
}

void append_u32(std::vector<std::uint8_t> &output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 24U));
    output.push_back(static_cast<std::uint8_t>(value >> 16U));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value));
}

std::vector<std::uint8_t> make_packet(std::uint32_t revision = 7) {
    std::vector<std::uint8_t> payload;
    for (const auto &field : valid_fields()) {
        payload.push_back(field.first);
        append_u16(payload, field.second.size());
        payload.insert(payload.end(), field.second.begin(), field.second.end());
    }
    std::vector<std::uint8_t> packet{
        'C', 'E', 'S', 'P', 1, 1, 0, 8,
        static_cast<std::uint8_t>(revision >> 24U),
        static_cast<std::uint8_t>(revision >> 16U),
        static_cast<std::uint8_t>(revision >> 8U),
        static_cast<std::uint8_t>(revision),
    };
    append_u16(packet, payload.size());
    append_u16(packet, provisioning::kHeaderSize + payload.size());
    const auto fingerprint = provisioning::compute_content_fingerprint(
        packet[4], packet[5], packet[7], payload.data(), payload.size());
    packet.insert(packet.end(), fingerprint.begin(), fingerprint.end());
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

std::vector<std::uint8_t> make_control(
    std::size_t packet_size, std::uint32_t transfer_id = 0x01020304) {
    std::vector<std::uint8_t> frame{'C', 'E', 'S', 'B', 1, 1, 0, 0};
    append_u32(frame, transfer_id);
    append_u16(frame, packet_size);
    append_u16(frame, 180);
    return frame;
}

std::vector<std::uint8_t> make_data(
    const std::vector<std::uint8_t> &packet,
    std::size_t offset,
    std::size_t size,
    std::uint32_t transfer_id = 0x01020304) {
    std::vector<std::uint8_t> frame{'C', 'E', 'S', 'D', 1, 0};
    append_u32(frame, transfer_id);
    append_u16(frame, offset);
    append_u16(frame, size);
    frame.insert(frame.end(), packet.begin() + offset, packet.begin() + offset + size);
    return frame;
}

class FakeSettings final : public provisioning::SettingsSink {
public:
    provisioning::StoredVersion stored_version() const override {
        return stored;
    }

    bool store(
        const std::uint8_t *,
        std::size_t,
        const provisioning::ValidationResult &validation) override {
        ++store_calls;
        if (!allow_store) {
            return false;
        }
        stored = {true, validation.revision, validation.fingerprint};
        return true;
    }

    provisioning::StoredVersion stored{};
    int store_calls = 0;
    bool allow_store = true;
};

std::uint8_t acknowledgement_status(const provisioning::SessionResult &result) {
    return result.acknowledgement[5];
}

provisioning::SessionResult send_packet(
    provisioning::ProvisioningSession &session,
    FakeSettings &settings,
    const std::vector<std::uint8_t> &packet) {
    const auto control = make_control(packet.size());
    (void)session.handle_control(
        control.data(), control.size(), kSecureLink);
    std::size_t offset = 0;
    provisioning::SessionResult result;
    while (offset < packet.size()) {
        const std::size_t size = std::min<std::size_t>(180, packet.size() - offset);
        const auto data = make_data(packet, offset, size);
        result = session.handle_data(
            data.data(), data.size(), kSecureLink, settings);
        offset += size;
    }
    return result;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_complete_packet_is_stored_before_applied_ack() {
    provisioning::ProvisioningSession session;
    FakeSettings settings;
    const auto packet = make_packet();

    const auto result = send_packet(session, settings, packet);
    TEST_ASSERT_TRUE(result.has_acknowledgement);
    TEST_ASSERT_EQUAL_UINT8_ARRAY("CESA", result.acknowledgement.data(), 4);
    TEST_ASSERT_EQUAL_HEX8(
        static_cast<std::uint8_t>(provisioning::AcknowledgementStatus::applied),
        acknowledgement_status(result));
    TEST_ASSERT_EQUAL_INT(1, settings.store_calls);
    TEST_ASSERT_FALSE(session.active());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(packet.data() + 16, result.acknowledgement.data() + 12, 32);
}

void test_storage_failure_never_reports_applied() {
    provisioning::ProvisioningSession session;
    FakeSettings settings;
    settings.allow_store = false;

    const auto result = send_packet(session, settings, make_packet());
    TEST_ASSERT_TRUE(result.has_acknowledgement);
    TEST_ASSERT_EQUAL_HEX8(
        static_cast<std::uint8_t>(provisioning::AcknowledgementStatus::storage_failure),
        acknowledgement_status(result));
    TEST_ASSERT_EQUAL_INT(1, settings.store_calls);
}

void test_unchanged_packet_does_not_write_storage() {
    provisioning::ProvisioningSession session;
    FakeSettings settings;
    const auto packet = make_packet();
    const auto validation = provisioning::validate_settings_packet(
        packet.data(), packet.size(), kSecureLink, {});
    settings.stored = {true, validation.revision, validation.fingerprint};

    const auto result = send_packet(session, settings, packet);
    TEST_ASSERT_TRUE(result.has_acknowledgement);
    TEST_ASSERT_EQUAL_HEX8(
        static_cast<std::uint8_t>(provisioning::AcknowledgementStatus::unchanged),
        acknowledgement_status(result));
    TEST_ASSERT_EQUAL_INT(0, settings.store_calls);
}

void test_transfer_errors_acknowledge_and_clear_the_transfer() {
    provisioning::ProvisioningSession session;
    auto control = make_control(make_packet().size());
    control[4] = 2;

    const auto result = session.handle_control(
        control.data(), control.size(), kSecureLink);
    TEST_ASSERT_TRUE(result.has_acknowledgement);
    TEST_ASSERT_EQUAL_HEX8(
        static_cast<std::uint8_t>(provisioning::AcknowledgementStatus::unsupported_version),
        acknowledgement_status(result));
    TEST_ASSERT_FALSE(session.active());
}

void test_insecure_data_is_rejected_and_cleared() {
    provisioning::ProvisioningSession session;
    FakeSettings settings;
    const auto packet = make_packet();
    const auto control = make_control(packet.size());
    TEST_ASSERT_FALSE(session.handle_control(
        control.data(), control.size(), kSecureLink).has_acknowledgement);
    const auto first = make_data(packet, 0, 180);
    const provisioning::LinkSecurity insecure{true, false, true, true};

    const auto result = session.handle_data(
        first.data(), first.size(), insecure, settings);
    TEST_ASSERT_TRUE(result.has_acknowledgement);
    TEST_ASSERT_EQUAL_HEX8(
        static_cast<std::uint8_t>(provisioning::AcknowledgementStatus::authentication_required),
        acknowledgement_status(result));
    TEST_ASSERT_FALSE(session.active());
    TEST_ASSERT_EQUAL_INT(0, settings.store_calls);
}

void test_disconnect_clears_an_incomplete_transfer() {
    provisioning::ProvisioningSession session;
    const auto control = make_control(make_packet().size());
    (void)session.handle_control(control.data(), control.size(), kSecureLink);
    TEST_ASSERT_TRUE(session.active());
    session.disconnect();
    TEST_ASSERT_FALSE(session.active());
}

void test_settings_record_owns_and_clears_values() {
    auto packet = make_packet();
    const auto validation = provisioning::validate_settings_packet(
        packet.data(), packet.size(), kSecureLink, {});
    provisioning::SettingsRecord settings;
    TEST_ASSERT_TRUE(settings.assign(validation));
    std::fill(packet.begin(), packet.end(), 0);
    TEST_ASSERT_EQUAL_STRING(
        "Test Network", std::string(settings.wifi_ssid.view()).c_str());
    settings.clear();
    TEST_ASSERT_EQUAL_UINT32(0, settings.revision);
    TEST_ASSERT_TRUE(settings.openrouter_key.view().empty());
    TEST_ASSERT_TRUE(settings.wifi_password.view().empty());
}

void test_valid_transfer_can_follow_a_rejected_packet() {
    provisioning::ProvisioningSession session;
    FakeSettings settings;
    auto bad_packet = make_packet();
    bad_packet[0] = 'X';

    const auto rejected = send_packet(session, settings, bad_packet);
    TEST_ASSERT_EQUAL_HEX8(
        static_cast<std::uint8_t>(provisioning::AcknowledgementStatus::malformed_packet),
        acknowledgement_status(rejected));
    TEST_ASSERT_EQUAL_INT(0, settings.store_calls);

    const auto repaired = send_packet(session, settings, make_packet());
    TEST_ASSERT_EQUAL_HEX8(
        static_cast<std::uint8_t>(provisioning::AcknowledgementStatus::applied),
        acknowledgement_status(repaired));
    TEST_ASSERT_EQUAL_INT(1, settings.store_calls);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_complete_packet_is_stored_before_applied_ack);
    RUN_TEST(test_storage_failure_never_reports_applied);
    RUN_TEST(test_unchanged_packet_does_not_write_storage);
    RUN_TEST(test_transfer_errors_acknowledge_and_clear_the_transfer);
    RUN_TEST(test_insecure_data_is_rejected_and_cleared);
    RUN_TEST(test_disconnect_clears_an_incomplete_transfer);
    RUN_TEST(test_settings_record_owns_and_clears_values);
    RUN_TEST(test_valid_transfer_can_follow_a_rejected_packet);
    return UNITY_END();
}
