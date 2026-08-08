import Foundation

struct AutomaticSettingsSyncTarget: Equatable {
    let deviceID: UUID
    let fingerprintHex: String
}

struct AutomaticSettingsSyncTrigger: Equatable {
    let target: AutomaticSettingsSyncTarget?
    let selectedDeviceID: UUID?
    let connected: Bool
    let provisioning: Bool
}

enum AutomaticSettingsSyncPolicy {
    static func target(
        deviceID: UUID,
        settings: ProvisioningSettings
    ) -> AutomaticSettingsSyncTarget? {
        guard settings.validationIssues.isEmpty,
              let fingerprint = try? settings.contentFingerprint() else {
            return nil
        }
        return AutomaticSettingsSyncTarget(
            deviceID: deviceID,
            fingerprintHex: fingerprint.hexString)
    }

    static func shouldStart(
        trigger: AutomaticSettingsSyncTrigger,
        lastAttempt: AutomaticSettingsSyncTarget?
    ) -> Bool {
        guard let target = trigger.target else { return false }
        return trigger.selectedDeviceID == target.deviceID &&
            trigger.connected && !trigger.provisioning &&
            lastAttempt != target
    }
}

@MainActor
final class ConfigurationStore: ObservableObject {
    @Published private(set) var preferences: AppPreferences
    @Published private(set) var secrets: ProvisioningSecrets
    @Published private(set) var secretsAvailable: Bool
    @Published private(set) var errorText: String?

    private let preferencesStore: PreferencesStore
    private let keychainStore: any SecretsStore

    var devices: [ChatESPDeviceRecord] {
        preferences.devices
    }

    var activeDeviceIdentifier: UUID? {
        preferences.activeDeviceIdentifier
    }

    init(
        preferencesStore: PreferencesStore = PreferencesStore(),
        keychainStore: any SecretsStore = KeychainStore()
    ) {
        self.preferencesStore = preferencesStore
        self.keychainStore = keychainStore
        preferences = preferencesStore.load()
        do {
            secrets = try keychainStore.load()
            secretsAvailable = true
        } catch {
            secrets = ProvisioningSecrets()
            secretsAvailable = false
            errorText = "The app could not read the saved secrets."
        }
    }

    func configuration(for deviceID: UUID) -> ChatESPConfiguration? {
        preferences.effectiveConfiguration(for: deviceID)
    }

    func secretValues(for deviceID: UUID) -> ProvisioningSecretValues {
        secrets.effectiveValues(for: deviceID)
    }

    func settings(for deviceID: UUID) -> ProvisioningSettings? {
        guard secretsAvailable,
              let configuration = configuration(for: deviceID) else {
            return nil
        }
        return ProvisioningSettings(
            configuration: configuration,
            secrets: secretValues(for: deviceID))
    }

    func validationIssues(for deviceID: UUID) -> [ConfigurationValidationIssue] {
        settings(for: deviceID)?.validationIssues ?? []
    }

    func addDevice(id: UUID) {
        var next = preferences
        if !next.devices.contains(where: { $0.id == id }) {
            next.devices.append(
                ChatESPDeviceRecord(
                    id: id,
                    name: "ChatESP"))
        }
        next.activeDeviceIdentifier = id
        savePreferences(next)
    }

    func removeDevice(id: UUID) {
        guard secretsAvailable else {
            errorText = "The app could not read the saved secrets."
            return
        }
        var nextPreferences = preferences
        nextPreferences.devices.removeAll { $0.id == id }
        if nextPreferences.activeDeviceIdentifier == id {
            nextPreferences.activeDeviceIdentifier = nil
        }
        var nextSecrets = secrets
        nextSecrets.deviceOverrides[id] = nil
        do {
            try keychainStore.save(nextSecrets)
            try preferencesStore.save(nextPreferences)
            secrets = nextSecrets
            preferences = nextPreferences
            errorText = nil
        } catch {
            try? keychainStore.save(secrets)
            showSaveError()
        }
    }

    func setActiveDevice(_ identifier: UUID?) {
        var next = preferences
        guard identifier == nil || next.devices.contains(where: { $0.id == identifier }) else {
            return
        }
        next.activeDeviceIdentifier = identifier
        savePreferences(next)
    }

    func setDeviceName(id: UUID, name: String) {
        guard let cleanName = Self.validName(name) else { return }
        var next = preferences
        guard let index = next.devices.firstIndex(where: { $0.id == id }) else {
            return
        }
        next.devices[index].name = cleanName
        savePreferences(next)
    }

    func updateGlobalConfiguration(
        _ change: (inout ChatESPConfiguration) -> Void
    ) {
        var next = preferences
        change(&next.global)
        savePreferences(next)
    }

    func updateGlobalSecrets(
        _ change: (inout ProvisioningSecretValues) -> Void
    ) {
        guard secretsAvailable else {
            errorText = "The app could not read the saved secrets."
            return
        }
        var next = secrets
        change(&next.global)
        saveSecrets(next)
    }

    func updateDeviceOverrides(
        id: UUID,
        _ change: (inout ChatESPConfigurationOverrides) -> Void
    ) {
        var next = preferences
        guard let index = next.devices.firstIndex(where: { $0.id == id }) else {
            return
        }
        change(&next.devices[index].overrides)
        savePreferences(next)
    }

    func updateDeviceSecretOverrides(
        id: UUID,
        _ change: (inout ProvisioningSecretOverrides) -> Void
    ) {
        guard secretsAvailable else {
            errorText = "The app could not read the saved secrets."
            return
        }
        var next = secrets
        var overrides = next.deviceOverrides[id] ?? ProvisioningSecretOverrides()
        change(&overrides)
        next.deviceOverrides[id] = overrides.isEmpty ? nil : overrides
        saveSecrets(next)
    }

    func makePacket(for deviceID: UUID) throws -> ProvisioningPacket {
        guard secretsAvailable else {
            throw ProvisioningError.secretsUnavailable
        }
        guard let settings = settings(for: deviceID),
              settings.validationIssues.isEmpty else {
            throw settings(for: deviceID)?.validationIssues.first.map {
                ProvisioningError.invalidField($0.fieldID)
            } ?? ProvisioningError.noDevice
        }
        let fingerprint = try settings.contentFingerprint()
        var next = preferences
        guard let index = next.devices.firstIndex(where: { $0.id == deviceID }) else {
            throw ProvisioningError.noDevice
        }
        let revision = try next.devices[index].provisioning.revision(
            for: fingerprint)
        let packet = try settings.packet(revision: revision)
        try preferencesStore.save(next)
        preferences = next
        return packet
    }

    func accept(
        _ acknowledgement: ProvisioningAcknowledgement,
        for deviceID: UUID
    ) throws {
        guard acknowledgement.isSuccess else {
            throw ProvisioningError.deviceRejected(acknowledgement.status)
        }
        var next = preferences
        guard let index = next.devices.firstIndex(where: { $0.id == deviceID }) else {
            throw ProvisioningError.noDevice
        }
        try next.devices[index].provisioning.acknowledge(
            revision: acknowledgement.revision,
            fingerprint: acknowledgement.fingerprint)
        try preferencesStore.save(next)
        preferences = next
        errorText = nil
    }

    func recoverRevision(
        from acknowledgement: ProvisioningAcknowledgement,
        for deviceID: UUID
    ) throws -> ProvisioningPacket {
        guard acknowledgement.isRevisionRecovery else {
            throw ProvisioningError.acknowledgementMismatch
        }
        var next = preferences
        guard let index = next.devices.firstIndex(where: { $0.id == deviceID }) else {
            throw ProvisioningError.noDevice
        }
        try next.devices[index].provisioning.recoverActiveVersion(
            revision: acknowledgement.revision,
            fingerprint: acknowledgement.fingerprint)
        try preferencesStore.save(next)
        preferences = next
        return try makePacket(for: deviceID)
    }

    func show(_ error: Error) {
        if let error = error as? LocalizedError,
           let description = error.errorDescription {
            errorText = description
        } else {
            errorText = "The settings transfer did not complete."
        }
    }

    func reloadSecretsIfNeeded() {
        guard !secretsAvailable else { return }
        do {
            secrets = try keychainStore.load()
            secretsAvailable = true
            errorText = nil
        } catch {
            errorText = "The app could not read the saved secrets."
        }
    }

    private func savePreferences(_ next: AppPreferences) {
        guard next != preferences else { return }
        do {
            try preferencesStore.save(next)
            preferences = next
            errorText = nil
        } catch {
            showSaveError()
        }
    }

    private func saveSecrets(_ next: ProvisioningSecrets) {
        guard next != secrets else { return }
        do {
            try keychainStore.save(next)
            secrets = next
            secretsAvailable = true
            errorText = nil
        } catch {
            showSaveError()
        }
    }

    private func showSaveError() {
        errorText = "The app could not save this change."
    }

    private static func validName(_ candidate: String) -> String? {
        let trimmed = candidate.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty,
              trimmed.utf8.count <= 48,
              !trimmed.unicodeScalars.contains(where: {
                  CharacterSet.controlCharacters.contains($0)
              }) else {
            return nil
        }
        return trimmed
    }
}
