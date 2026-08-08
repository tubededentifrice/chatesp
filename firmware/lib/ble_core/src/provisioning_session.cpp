#include "chatesp/provisioning_session.hpp"

namespace chatesp {
namespace provisioning {
namespace {

SessionResult acknowledgement(
    AcknowledgementStatus status,
    std::uint32_t revision = 0,
    const std::array<std::uint8_t, kFingerprintSize> &fingerprint = {},
    std::uint8_t version = kProtocolVersion,
    std::uint16_t flags = 0) {
    return {true, make_acknowledgement(
        status, revision, fingerprint, version, flags)};
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
        case ValidationError::invalid_approximate_location:
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

    const StoredVersion stored = settings.stored_version();
    const ValidationResult validation = validate_settings_packet(
        assembler_.packet_data(), assembler_.packet_size(), security,
        stored);

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

    const bool include_active_version =
        stored.present &&
        stored.revision > 0 &&
        validation.revision > 0 &&
        (validation.error == ValidationError::stale_revision ||
         validation.error == ValidationError::revision_conflict);
    const SessionResult result = acknowledgement(
        status,
        include_active_version ? stored.revision : validation.revision,
        include_active_version ? stored.fingerprint : validation.fingerprint,
        assembler_.version(),
        include_active_version ? kAcknowledgementActiveVersionFlag : 0);
    assembler_.reset();
    return result;
}

SessionResult ProvisioningSession::busy_acknowledgement() const {
    return acknowledgement(
        AcknowledgementStatus::busy, 0, {}, assembler_.version());
}

void ProvisioningSession::disconnect() { assembler_.reset(); }

bool ProvisioningSession::active() const { return assembler_.active(); }

SessionResult ProvisioningSession::transfer_error(TransferError error) {
    const std::uint8_t version = assembler_.version();
    const AcknowledgementStatus status =
        error == TransferError::authentication_required
            ? AcknowledgementStatus::authentication_required
            : error == TransferError::unsupported_version
                  ? AcknowledgementStatus::unsupported_version
                  : AcknowledgementStatus::malformed_transfer;
    assembler_.reset();
    return acknowledgement(status, 0, {}, version);
}

}  // namespace provisioning
}  // namespace chatesp
