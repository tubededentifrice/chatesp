#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <unity.h>

#include "chatesp/provisioning_packet.hpp"
#include "chatesp/provisioning_transfer.hpp"

namespace provisioning = chatesp::provisioning;

namespace {

using Fields = std::vector<std::pair<std::uint8_t, std::string>>;

constexpr provisioning::LinkSecurity kSecureLink{true, true, true, true};

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

std::vector<std::uint8_t> make_packet(
    const Fields &fields = valid_fields(),
    std::uint32_t revision = 7) {
    std::vector<std::uint8_t> payload;
    for (const auto &field : fields) {
        payload.push_back(field.first);
        append_u16(payload, field.second.size());
        payload.insert(payload.end(), field.second.begin(), field.second.end());
    }

    std::vector<std::uint8_t> packet{
        'C', 'E', 'S', 'P', provisioning::kProtocolVersion,
        provisioning::kSettingsPacketType, 0, static_cast<std::uint8_t>(fields.size()),
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
    std::size_t packet_size,
    std::uint16_t frame_data_size = 180,
    std::uint32_t transfer_id = 0x01020304) {
    std::vector<std::uint8_t> frame{'C', 'E', 'S', 'B', 1, 1, 0, 0};
    append_u32(frame, transfer_id);
    append_u16(frame, packet_size);
    append_u16(frame, frame_data_size);
    return frame;
}

std::vector<std::uint8_t> make_data_frame(
    const std::vector<std::uint8_t> &packet,
    std::size_t offset,
    std::size_t count,
    std::uint32_t transfer_id = 0x01020304) {
    std::vector<std::uint8_t> frame{'C', 'E', 'S', 'D', 1, 0};
    append_u32(frame, transfer_id);
    append_u16(frame, offset);
    append_u16(frame, count);
    frame.insert(frame.end(), packet.begin() + offset, packet.begin() + offset + count);
    return frame;
}

provisioning::ValidationResult validate(
    const std::vector<std::uint8_t> &packet,
    const provisioning::StoredVersion &stored = {}) {
    return provisioning::validate_settings_packet(
        packet.data(), packet.size(), kSecureLink, stored);
}

void replace_field(Fields &fields, std::uint8_t id, std::string value) {
    for (auto &field : fields) {
        if (field.first == id) {
            field.second = std::move(value);
            return;
        }
    }
}

void assert_error(
    const std::vector<std::uint8_t> &packet,
    provisioning::ValidationError expected) {
    const auto result = validate(packet);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(result.error));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::ApplyDecision::reject),
        static_cast<int>(result.decision));
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_valid_golden_packet_is_accepted() {
    const auto packet = make_packet();
    const auto result = validate(packet);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::ValidationError::none),
        static_cast<int>(result.error));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::ApplyDecision::apply),
        static_cast<int>(result.decision));
    TEST_ASSERT_EQUAL_UINT32(7, result.revision);
    TEST_ASSERT_EQUAL_STRING("Test Network", std::string(result.settings.wifi_ssid).c_str());

    constexpr std::array<std::uint8_t, 32> expected{{
        0x44, 0xe8, 0x1b, 0xdf, 0x41, 0xe1, 0xc3, 0xfb,
        0x02, 0x16, 0x7d, 0x61, 0xc1, 0x6a, 0xee, 0x5d,
        0xad, 0xcd, 0x54, 0xd7, 0x0b, 0xc0, 0x88, 0x49,
        0x99, 0x65, 0xbd, 0xb4, 0xa3, 0x49, 0xc4, 0x94,
    }};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.data(), result.fingerprint.data(), expected.size());
}

void test_link_must_have_all_security_properties() {
    const auto packet = make_packet();
    const std::array<provisioning::LinkSecurity, 4> insecure{{
        {false, true, true, true},
        {true, false, true, true},
        {true, true, false, true},
        {true, true, true, false},
    }};
    for (const auto &link : insecure) {
        const auto result = provisioning::validate_settings_packet(
            packet.data(), packet.size(), link, {});
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(provisioning::ValidationError::authentication_required),
            static_cast<int>(result.error));
    }
}

void test_packet_envelope_is_strict() {
    auto packet = make_packet();
    assert_error(std::vector<std::uint8_t>(47, 0), provisioning::ValidationError::packet_too_short);
    assert_error(
        std::vector<std::uint8_t>(provisioning::kMaximumPacketSize + 1, 0),
        provisioning::ValidationError::packet_too_large);

    auto changed = packet;
    changed[0] = 'X';
    assert_error(changed, provisioning::ValidationError::bad_magic);
    changed = packet;
    changed[4] = 2;
    assert_error(changed, provisioning::ValidationError::unsupported_version);
    changed = packet;
    changed[5] = 2;
    assert_error(changed, provisioning::ValidationError::bad_type);
    changed = packet;
    changed[6] = 1;
    assert_error(changed, provisioning::ValidationError::bad_flags);
    changed = packet;
    changed[7] = 7;
    assert_error(changed, provisioning::ValidationError::bad_field_count);
    changed = packet;
    changed[15] ^= 1;
    assert_error(changed, provisioning::ValidationError::bad_length);
    changed = packet;
    changed[16] ^= 1;
    assert_error(changed, provisioning::ValidationError::bad_fingerprint);

    assert_error(make_packet(valid_fields(), 0), provisioning::ValidationError::stale_revision);
}

void test_transfer_assembles_ordered_bounded_frames() {
    const auto packet = make_packet();
    provisioning::TransferAssembler assembler;
    const auto control = make_control(packet.size());
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::TransferError::none),
        static_cast<int>(assembler.handle_control(control.data(), control.size(), kSecureLink)));
    TEST_ASSERT_TRUE(assembler.active());
    TEST_ASSERT_FALSE(assembler.complete());

    const auto first = make_data_frame(packet, 0, 180);
    const auto second = make_data_frame(packet, 180, packet.size() - 180);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::TransferError::none),
        static_cast<int>(assembler.handle_data(first.data(), first.size(), kSecureLink)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::TransferError::none),
        static_cast<int>(assembler.handle_data(second.data(), second.size(), kSecureLink)));
    TEST_ASSERT_TRUE(assembler.complete());
    TEST_ASSERT_EQUAL_UINT(packet.size(), assembler.packet_size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(packet.data(), assembler.packet_data(), packet.size());
}

void test_transfer_rejects_insecure_and_malformed_frames() {
    const auto packet = make_packet();
    provisioning::TransferAssembler assembler;
    auto control = make_control(packet.size());
    const provisioning::LinkSecurity insecure{true, false, true, true};
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::TransferError::authentication_required),
        static_cast<int>(assembler.handle_control(control.data(), control.size(), insecure)));

    auto changed = control;
    changed[0] = 'X';
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::TransferError::bad_magic),
        static_cast<int>(assembler.handle_control(changed.data(), changed.size(), kSecureLink)));
    changed = control;
    changed[4] = 2;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::TransferError::unsupported_version),
        static_cast<int>(assembler.handle_control(changed.data(), changed.size(), kSecureLink)));
    changed = control;
    changed[6] = 1;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::TransferError::bad_flags),
        static_cast<int>(assembler.handle_control(changed.data(), changed.size(), kSecureLink)));
    changed = make_control(packet.size(), 181);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::TransferError::bad_frame_size),
        static_cast<int>(assembler.handle_control(changed.data(), changed.size(), kSecureLink)));

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::TransferError::none),
        static_cast<int>(assembler.handle_control(control.data(), control.size(), kSecureLink)));
    auto data = make_data_frame(packet, 1, 100);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::TransferError::wrong_offset),
        static_cast<int>(assembler.handle_data(data.data(), data.size(), kSecureLink)));
    data = make_data_frame(packet, 0, 100, 0x05060708);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::TransferError::wrong_transfer_id),
        static_cast<int>(assembler.handle_data(data.data(), data.size(), kSecureLink)));
    data = make_data_frame(packet, 0, 180);
    data[5] = 1;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::TransferError::bad_flags),
        static_cast<int>(assembler.handle_data(data.data(), data.size(), kSecureLink)));
    data = make_data_frame(packet, 0, 180);
    data.pop_back();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::TransferError::bad_frame_size),
        static_cast<int>(assembler.handle_data(data.data(), data.size(), kSecureLink)));

    const std::vector<std::uint8_t> cancel{
        'C', 'E', 'S', 'B', 1, 2, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::TransferError::none),
        static_cast<int>(assembler.handle_control(cancel.data(), cancel.size(), kSecureLink)));
    TEST_ASSERT_FALSE(assembler.active());
    TEST_ASSERT_EQUAL_UINT8(0, assembler.packet_data()[0]);
}

void test_acknowledgement_has_exact_binary_layout() {
    const auto packet = make_packet();
    const auto validation = validate(packet);
    const auto acknowledgement = provisioning::make_acknowledgement(
        provisioning::AcknowledgementStatus::applied,
        validation.revision,
        validation.fingerprint);
    TEST_ASSERT_EQUAL_UINT8_ARRAY("CESA", acknowledgement.data(), 4);
    TEST_ASSERT_EQUAL_UINT8(1, acknowledgement[4]);
    TEST_ASSERT_EQUAL_UINT8(0, acknowledgement[5]);
    TEST_ASSERT_EQUAL_UINT8(7, acknowledgement[11]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        validation.fingerprint.data(), acknowledgement.data() + 12, validation.fingerprint.size());
}

void test_fields_are_complete_ordered_and_unique() {
    auto fields = valid_fields();
    fields.erase(fields.begin() + 3);
    auto packet = make_packet(fields);
    packet[7] = provisioning::kRequiredFieldCount;
    const auto fingerprint = provisioning::compute_content_fingerprint(
        packet[4], packet[5], packet[7],
        packet.data() + provisioning::kHeaderSize,
        packet.size() - provisioning::kHeaderSize);
    std::copy(fingerprint.begin(), fingerprint.end(), packet.begin() + 16);
    assert_error(packet, provisioning::ValidationError::bad_field_order);

    fields = valid_fields();
    fields[2].first = 2;
    packet = make_packet(fields);
    assert_error(packet, provisioning::ValidationError::duplicate_field);

    fields = valid_fields();
    fields.pop_back();
    packet = make_packet(fields);
    packet[7] = provisioning::kRequiredFieldCount;
    const auto missing_fingerprint = provisioning::compute_content_fingerprint(
        packet[4], packet[5], packet[7],
        packet.data() + provisioning::kHeaderSize,
        packet.size() - provisioning::kHeaderSize);
    std::copy(missing_fingerprint.begin(), missing_fingerprint.end(), packet.begin() + 16);
    assert_error(packet, provisioning::ValidationError::missing_field);
}

void test_each_field_has_a_fixed_limit_and_format() {
    auto fields = valid_fields();
    replace_field(fields, 1, "http://example.com/api");
    assert_error(make_packet(fields), provisioning::ValidationError::invalid_endpoint);

    fields = valid_fields();
    replace_field(fields, 2, "short");
    assert_error(make_packet(fields), provisioning::ValidationError::invalid_openrouter_key);

    fields = valid_fields();
    replace_field(fields, 3, std::string(129, 'B'));
    assert_error(make_packet(fields), provisioning::ValidationError::invalid_brave_key);

    fields = valid_fields();
    replace_field(fields, 4, std::string(33, 'S'));
    assert_error(make_packet(fields), provisioning::ValidationError::invalid_wifi_ssid);

    fields = valid_fields();
    replace_field(fields, 4, std::string("bad\xc0\x80", 5));
    assert_error(make_packet(fields), provisioning::ValidationError::invalid_wifi_ssid);

    fields = valid_fields();
    replace_field(fields, 5, "short");
    assert_error(make_packet(fields), provisioning::ValidationError::invalid_wifi_password);

    fields = valid_fields();
    replace_field(fields, 6, "model with space");
    assert_error(make_packet(fields), provisioning::ValidationError::invalid_model);
}

void test_revision_rules_reject_stale_and_conflicting_packets() {
    const auto packet = make_packet(valid_fields(), 7);
    const auto accepted = validate(packet);
    provisioning::StoredVersion stored{true, 7, accepted.fingerprint};

    const auto repeated = validate(packet, stored);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::ApplyDecision::unchanged),
        static_cast<int>(repeated.decision));

    const auto stale = validate(make_packet(valid_fields(), 6), stored);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::ValidationError::stale_revision),
        static_cast<int>(stale.error));

    auto changed_fields = valid_fields();
    replace_field(changed_fields, 6, "example/other-model");
    const auto conflict = validate(make_packet(changed_fields, 7), stored);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::ValidationError::revision_conflict),
        static_cast<int>(conflict.error));

    const auto newer = validate(make_packet(changed_fields, 8), stored);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(provisioning::ApplyDecision::apply),
        static_cast<int>(newer.decision));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_golden_packet_is_accepted);
    RUN_TEST(test_link_must_have_all_security_properties);
    RUN_TEST(test_packet_envelope_is_strict);
    RUN_TEST(test_fields_are_complete_ordered_and_unique);
    RUN_TEST(test_each_field_has_a_fixed_limit_and_format);
    RUN_TEST(test_revision_rules_reject_stale_and_conflicting_packets);
    RUN_TEST(test_transfer_assembles_ordered_bounded_frames);
    RUN_TEST(test_transfer_rejects_insecure_and_malformed_frames);
    RUN_TEST(test_acknowledgement_has_exact_binary_layout);
    return UNITY_END();
}
