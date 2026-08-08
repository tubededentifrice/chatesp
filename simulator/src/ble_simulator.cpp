#include "chatesp/simulator/ble_simulator.hpp"

#include <algorithm>
#include <array>
#include <vector>

#include "chatesp/provisioning_packet.hpp"
#include "chatesp/provisioning_transfer.hpp"

namespace chatesp::simulator {
namespace {

using ByteBuffer = std::vector<std::uint8_t>;

void append_u16(ByteBuffer &output, std::size_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value));
}

void append_u32(ByteBuffer &output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 24U));
    output.push_back(static_cast<std::uint8_t>(value >> 16U));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value));
}

ByteBuffer settings_packet(std::uint32_t revision) {
    constexpr std::array<std::string_view, provisioning::kRequiredFieldCount>
        kValues{{
            "https://openrouter.ai/api/v1",
            "",
            "",
            "",
            "",
            "~deepseek/deepseek-v4-flash-latest",
            "openai/whisper-large-v3-turbo",
            "hexgrad/kokoro-82m",
            "",
            "af_heart",
            "ff_siwis",
        }};
    ByteBuffer payload;
    for (std::size_t index = 0; index < kValues.size(); ++index) {
        payload.push_back(static_cast<std::uint8_t>(index + 1));
        append_u16(payload, kValues[index].size());
        payload.insert(payload.end(), kValues[index].begin(), kValues[index].end());
    }

    ByteBuffer packet{
        'C', 'E', 'S', 'P', provisioning::kProtocolVersion,
        provisioning::kSettingsPacketType, 0,
        provisioning::kRequiredFieldCount};
    append_u32(packet, revision);
    append_u16(packet, payload.size());
    append_u16(packet, provisioning::kHeaderSize + payload.size());
    const auto fingerprint = provisioning::compute_content_fingerprint(
        packet[4], packet[5], packet[7], payload.data(), payload.size());
    packet.insert(packet.end(), fingerprint.begin(), fingerprint.end());
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

ByteBuffer control_frame(std::size_t packet_size, std::uint32_t transfer_id) {
    ByteBuffer frame{
        'C', 'E', 'S', 'B', provisioning::kProtocolVersion, 1, 0, 0};
    append_u32(frame, transfer_id);
    append_u16(frame, packet_size);
    append_u16(frame, provisioning::kMaximumFrameDataSize);
    return frame;
}

ByteBuffer data_frame(
    const ByteBuffer &packet,
    std::size_t offset,
    std::size_t count,
    std::uint32_t transfer_id) {
    ByteBuffer frame{
        'C', 'E', 'S', 'D', provisioning::kProtocolVersion, 0};
    append_u32(frame, transfer_id);
    append_u16(frame, offset);
    append_u16(frame, count);
    frame.insert(
        frame.end(), packet.begin() + static_cast<std::ptrdiff_t>(offset),
        packet.begin() + static_cast<std::ptrdiff_t>(offset + count));
    return frame;
}

std::uint32_t next_random(std::uint32_t &state) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
}

}  // namespace

provisioning::StoredVersion SimulatorSettingsSink::stored_version() const {
    if (record_.revision == 0) {
        return {};
    }
    return {true, record_.revision, record_.fingerprint};
}

bool SimulatorSettingsSink::store(
    const std::uint8_t *,
    std::size_t,
    const provisioning::ValidationResult &validation) {
    if (fail_next_store_) {
        fail_next_store_ = false;
        return false;
    }
    if (!record_.assign(validation)) {
        return false;
    }
    ++writes_;
    return true;
}

void SimulatorSettingsSink::fail_next_store() { fail_next_store_ = true; }

void SimulatorSettingsSink::clear() {
    record_.clear();
    writes_ = 0;
    fail_next_store_ = false;
}

std::uint32_t SimulatorSettingsSink::revision() const {
    return record_.revision;
}

std::size_t SimulatorSettingsSink::writes() const { return writes_; }

BleSimulator::BleSimulator(bool development_mode)
    : development_mode_(development_mode) {}

void BleSimulator::factory_reset() {
    session_.disconnect();
    settings_.clear();
    security_ = {};
    state_ = BleState::off;
    outcome_ = BleOutcome::none;
    passkey_ = 0;
    next_passkey_ = 123'456;
    attempts_ = 0;
    fuzz_cases_ = 0;
    bonded_ = false;
}

void BleSimulator::start_radio() {
    if (state_ == BleState::off) {
        state_ = BleState::advertising;
    }
}

void BleSimulator::stop_radio() {
    disconnect_link();
    state_ = BleState::off;
}

void BleSimulator::restart_radio() {
    stop_radio();
    start_radio();
}

void BleSimulator::reboot() {
    stop_radio();
    if (development_mode_) {
        bonded_ = false;
        settings_.clear();
    }
    outcome_ = BleOutcome::none;
    attempts_ = 0;
    start_radio();
}

bool BleSimulator::connect() {
    if (state_ != BleState::advertising) {
        return false;
    }
    security_ = {};
    if (bonded_) {
        security_ = {true, true, true, true};
        state_ = BleState::connected;
        passkey_ = 0;
    } else {
        state_ = BleState::pairing;
        passkey_ = next_passkey_;
        next_passkey_ = next_passkey_ == 999'999 ? 100'000
                                                 : next_passkey_ + 1;
    }
    outcome_ = BleOutcome::none;
    return true;
}

bool BleSimulator::confirm_pairing(std::uint32_t passkey) {
    if (state_ != BleState::pairing) {
        return false;
    }
    if (passkey != passkey_) {
        outcome_ = BleOutcome::pairing_failed;
        disconnect_link();
        state_ = BleState::advertising;
        return true;
    }
    bonded_ = true;
    security_ = {true, true, true, true};
    passkey_ = 0;
    state_ = BleState::connected;
    outcome_ = BleOutcome::none;
    return true;
}

bool BleSimulator::reject_pairing() {
    if (state_ != BleState::pairing) {
        return false;
    }
    outcome_ = BleOutcome::pairing_failed;
    disconnect_link();
    state_ = BleState::advertising;
    return true;
}

bool BleSimulator::disconnect() {
    if (state_ != BleState::pairing && state_ != BleState::connected) {
        return false;
    }
    disconnect_link();
    state_ = BleState::advertising;
    return true;
}

bool BleSimulator::provision(std::uint32_t revision, BleFault fault) {
    if ((state_ != BleState::pairing && state_ != BleState::connected) ||
        revision == 0) {
        return false;
    }
    outcome_ = BleOutcome::none;
    attempts_ = 0;
    const ByteBuffer packet = settings_packet(revision);
    constexpr std::size_t kMaximumAttempts = 2;
    constexpr std::uint32_t kTransferId = 0x01020304;

    for (std::size_t attempt = 0; attempt < kMaximumAttempts; ++attempt) {
        ++attempts_;
        if (state_ != BleState::pairing && state_ != BleState::connected) {
            outcome_ = BleOutcome::disconnected;
            return true;
        }
        const ByteBuffer control = control_frame(packet.size(), kTransferId);
        provisioning::SessionResult result = session_.handle_control(
            control.data(), control.size(), security_);
        if (result.has_acknowledgement) {
            set_outcome_from_acknowledgement(result);
            return true;
        }
        if (fault == BleFault::storage_failure && attempt == 0) {
            settings_.fail_next_store();
        }
        if (fault == BleFault::disconnect_after_control && attempt == 0) {
            disconnect_link();
            state_ = BleState::advertising;
            outcome_ = BleOutcome::disconnected;
            return true;
        }

        std::size_t offset = 0;
        std::size_t frame_index = 0;
        while (offset < packet.size()) {
            const std::size_t count = std::min(
                provisioning::kMaximumFrameDataSize,
                packet.size() - offset);
            ByteBuffer frame = data_frame(packet, offset, count, kTransferId);
            if (fault == BleFault::corrupt_data && attempt == 0 &&
                frame_index == 0) {
                frame[0] = 'X';
            }
            result = session_.handle_data(
                frame.data(), frame.size(), security_, settings_);
            offset += count;
            ++frame_index;
            if (result.has_acknowledgement) {
                break;
            }
            if (fault == BleFault::disconnect_after_data && attempt == 0 &&
                frame_index == 1) {
                disconnect_link();
                state_ = BleState::advertising;
                break;
            }
        }

        if (state_ == BleState::advertising) {
            outcome_ = BleOutcome::disconnected;
            return true;
        }
        if (!result.has_acknowledgement) {
            outcome_ = BleOutcome::protocol_error;
            return true;
        }
        if (fault == BleFault::drop_acknowledgement && attempt == 0) {
            continue;
        }
        set_outcome_from_acknowledgement(result);
        return true;
    }
    outcome_ = BleOutcome::disconnected;
    return true;
}

bool BleSimulator::fuzz(std::size_t cases, std::uint32_t seed) {
    if (cases == 0 || cases > kMaximumBleFuzzCases) {
        return false;
    }
    provisioning::ProvisioningSession session;
    SimulatorSettingsSink settings;
    std::uint32_t random = seed == 0 ? 0x6d2b79f5U : seed;
    for (std::size_t index = 0; index < cases; ++index) {
        const std::size_t size = next_random(random) % 1'300U;
        ByteBuffer frame(size);
        for (std::uint8_t &byte : frame) {
            byte = static_cast<std::uint8_t>(next_random(random));
        }
        const provisioning::LinkSecurity security{
            (next_random(random) & 1U) != 0,
            (next_random(random) & 1U) != 0,
            (next_random(random) & 1U) != 0,
            (next_random(random) & 1U) != 0,
        };
        const std::uint8_t *data = frame.empty() ? nullptr : frame.data();
        if ((next_random(random) & 1U) == 0) {
            (void)session.handle_control(data, frame.size(), security);
        } else {
            (void)session.handle_data(data, frame.size(), security, settings);
        }
        if ((next_random(random) & 0x0fU) == 0) {
            session.disconnect();
        }
    }
    fuzz_cases_ = cases;
    outcome_ = BleOutcome::none;
    return true;
}

BleSnapshot BleSimulator::snapshot() const {
    return {
        state_,
        outcome_,
        passkey_,
        settings_.revision(),
        attempts_,
        settings_.writes(),
        fuzz_cases_,
        provisioning::link_is_secure(security_),
        bonded_,
        state_ == BleState::pairing && passkey_ != 0,
    };
}

void BleSimulator::disconnect_link() {
    session_.disconnect();
    security_ = {};
    passkey_ = 0;
}

void BleSimulator::set_outcome_from_acknowledgement(
    const provisioning::SessionResult &result) {
    if (!result.has_acknowledgement) {
        outcome_ = BleOutcome::protocol_error;
        return;
    }
    switch (static_cast<provisioning::AcknowledgementStatus>(
        result.acknowledgement[5])) {
        case provisioning::AcknowledgementStatus::applied:
            outcome_ = BleOutcome::applied;
            break;
        case provisioning::AcknowledgementStatus::unchanged:
            outcome_ = BleOutcome::unchanged;
            break;
        case provisioning::AcknowledgementStatus::authentication_required:
            outcome_ = BleOutcome::authentication_required;
            break;
        case provisioning::AcknowledgementStatus::unsupported_version:
            outcome_ = BleOutcome::unsupported_version;
            break;
        case provisioning::AcknowledgementStatus::malformed_transfer:
            outcome_ = BleOutcome::malformed_transfer;
            break;
        case provisioning::AcknowledgementStatus::malformed_packet:
            outcome_ = BleOutcome::malformed_packet;
            break;
        case provisioning::AcknowledgementStatus::invalid_field:
            outcome_ = BleOutcome::invalid_field;
            break;
        case provisioning::AcknowledgementStatus::stale_revision:
            outcome_ = BleOutcome::stale_revision;
            break;
        case provisioning::AcknowledgementStatus::revision_conflict:
            outcome_ = BleOutcome::revision_conflict;
            break;
        case provisioning::AcknowledgementStatus::storage_failure:
            outcome_ = BleOutcome::storage_failure;
            break;
        case provisioning::AcknowledgementStatus::busy:
            outcome_ = BleOutcome::busy;
            break;
    }
}

const char *ble_state_name(BleState state) {
    switch (state) {
        case BleState::off: return "off";
        case BleState::advertising: return "advertising";
        case BleState::pairing: return "pairing";
        case BleState::connected: return "connected";
    }
    return "unknown";
}

const char *ble_outcome_name(BleOutcome outcome) {
    switch (outcome) {
        case BleOutcome::none: return "none";
        case BleOutcome::applied: return "applied";
        case BleOutcome::unchanged: return "unchanged";
        case BleOutcome::authentication_required: return "authentication_required";
        case BleOutcome::unsupported_version: return "unsupported_version";
        case BleOutcome::malformed_transfer: return "malformed_transfer";
        case BleOutcome::malformed_packet: return "malformed_packet";
        case BleOutcome::invalid_field: return "invalid_field";
        case BleOutcome::stale_revision: return "stale_revision";
        case BleOutcome::revision_conflict: return "revision_conflict";
        case BleOutcome::storage_failure: return "storage_failure";
        case BleOutcome::busy: return "busy";
        case BleOutcome::disconnected: return "disconnected";
        case BleOutcome::pairing_failed: return "pairing_failed";
        case BleOutcome::protocol_error: return "protocol_error";
    }
    return "unknown";
}

bool parse_ble_fault(std::string_view text, BleFault &fault) {
    if (text.empty() || text == "none") {
        fault = BleFault::none;
    } else if (text == "disconnect-after-control") {
        fault = BleFault::disconnect_after_control;
    } else if (text == "disconnect-after-data") {
        fault = BleFault::disconnect_after_data;
    } else if (text == "drop-ack") {
        fault = BleFault::drop_acknowledgement;
    } else if (text == "corrupt-data") {
        fault = BleFault::corrupt_data;
    } else if (text == "storage-failure") {
        fault = BleFault::storage_failure;
    } else {
        return false;
    }
    return true;
}

}  // namespace chatesp::simulator
