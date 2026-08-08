import Foundation

@MainActor
final class ConfigurationStore: ObservableObject {
    @Published var preferences: AppPreferences
    @Published var secrets: ProvisioningSecrets
    @Published private(set) var errorText: String?

    private let preferencesStore: PreferencesStore
    private let keychainStore: KeychainStore

    var selectedWatchIdentifier: UUID? {
        preferences.selectedWatchIdentifier
    }

    init(
        preferencesStore: PreferencesStore = PreferencesStore(),
        keychainStore: KeychainStore = KeychainStore()
    ) {
        self.preferencesStore = preferencesStore
        self.keychainStore = keychainStore
        preferences = preferencesStore.load()
        do {
            secrets = try keychainStore.load()
        } catch {
            secrets = ProvisioningSecrets()
            errorText = "The app could not read the saved secrets."
        }
    }

    func makePacket() throws -> ProvisioningPacket {
        let settings = ProvisioningSettings(preferences: preferences, secrets: secrets)
        let fingerprint = try settings.contentFingerprint()
        let revision = try preferences.revision(for: fingerprint)
        try persist()
        return try settings.packet(revision: revision)
    }

    func accept(_ acknowledgement: ProvisioningAcknowledgement) throws {
        guard acknowledgement.isSuccess else {
            throw ProvisioningError.deviceRejected(acknowledgement.status)
        }
        try preferences.acknowledge(
            revision: acknowledgement.revision,
            fingerprint: acknowledgement.fingerprint)
        try persist()
    }

    func recoverRevision(
        from acknowledgement: ProvisioningAcknowledgement
    ) throws -> ProvisioningPacket {
        guard acknowledgement.isRevisionRecovery else {
            throw ProvisioningError.acknowledgementMismatch
        }
        try preferences.recoverActiveVersion(
            revision: acknowledgement.revision,
            fingerprint: acknowledgement.fingerprint)
        try preferencesStore.save(preferences)
        return try makePacket()
    }

    func setSelectedWatchIdentifier(_ identifier: UUID?) throws {
        let previous = preferences.selectedWatchIdentifier
        preferences.selectedWatchIdentifier = identifier
        do {
            try preferencesStore.save(preferences)
        } catch {
            preferences.selectedWatchIdentifier = previous
            throw error
        }
    }

    func saveEdits() {
        do {
            try persist()
            errorText = nil
        } catch {
            errorText = "The app could not save the configuration."
        }
    }

    func show(_ error: Error) {
        if let error = error as? LocalizedError, let description = error.errorDescription {
            errorText = description
        } else {
            errorText = "Provisioning did not complete."
        }
    }

    private func persist() throws {
        try preferencesStore.save(preferences)
        try keychainStore.save(secrets)
    }
}
