#include "chatesp/provisioning_session.hpp"

namespace chatesp {
namespace provisioning {
namespace {

SessionResult acknowledgement(
    AcknowledgementStatus status,
    std::uint32_t revision = 0,
    const std::array<std::uint8_t, kFingerprintSize> &fingerprint = {}) {
    return {true, make_acknowledgement(status, revision, fingerprint)};
}

AcknowledgementStatus validation_status(ValidationError error) {
    switch (error) {
        case ValidationError::none:
            return AcknowledgementStatus::applied;
        case ValidationError::authentication_required:
            return AcknowledgementStatus::authentication_required;
        case ValidationError::unsupported_version:
            return AcknowledgementStatus::unsupported_version;
        case ValidationError::invalid_endpoint:
        case ValidationError::invalid_openrouter_key:
        case ValidationError::invalid_brave_key:
        case ValidationError::invalid_wifi_ssid:
        case ValidationError::invalid_wifi_password:
        case ValidationError::invalid_model:
        case ValidationError::invalid_utf8:
            return AcknowledgementStatus::invalid_field;
        case ValidationError::stale_revision:
            return AcknowledgementStatus::stale_revision;
        case ValidationError::revision_conflict:
            return AcknowledgementStatus::revision_conflict;
        default:
            return AcknowledgementStatus::malformed_packet;
    }
}

}  // namespace

SessionResult ProvisioningSession::handle_control(
    const std::uint8_t *frame,
    std::size_t frame_size,
    const LinkSecurity &security) {
    assembler_.reset();
    const TransferError error =
        assembler_.handle_control(frame, frame_size, security);
    return error == TransferError::none ? SessionResult{} : transfer_error(error);
}

SessionResult ProvisioningSession::handle_data(
    const std::uint8_t *frame,
    std::size_t frame_size,
    const LinkSecurity &security,
    SettingsSink &settings) {
    const TransferError error = assembler_.handle_data(frame, frame_size, security);
    if (error != TransferError::none) {
        return transfer_error(error);
    }
    if (!assembler_.complete()) {
        return {};
    }

    const ValidationResult validation = validate_settings_packet(
        assembler_.packet_data(), assembler_.packet_size(), security,
        settings.stored_version());

    AcknowledgementStatus status = validation_status(validation.error);
    if (validation.error == ValidationError::none) {
        if (validation.decision == ApplyDecision::unchanged) {
            status = AcknowledgementStatus::unchanged;
        } else if (validation.decision == ApplyDecision::apply &&
                   settings.store(
                       assembler_.packet_data(), assembler_.packet_size(), validation)) {
            status = AcknowledgementStatus::applied;
        } else {
            status = AcknowledgementStatus::storage_failure;
        }
    }

    const SessionResult result = acknowledgement(
        status, validation.revision, validation.fingerprint);
    assembler_.reset();
    return result;
}

SessionResult ProvisioningSession::busy_acknowledgement() const {
    return acknowledgement(AcknowledgementStatus::busy);
}

void ProvisioningSession::disconnect() { assembler_.reset(); }

bool ProvisioningSession::active() const { return assembler_.active(); }

SessionResult ProvisioningSession::transfer_error(TransferError error) {
    const AcknowledgementStatus status =
        error == TransferError::authentication_required
            ? AcknowledgementStatus::authentication_required
            : error == TransferError::unsupported_version
                  ? AcknowledgementStatus::unsupported_version
                  : AcknowledgementStatus::malformed_transfer;
    assembler_.reset();
    return acknowledgement(status);
}

}  // namespace provisioning
}  // namespace chatesp
