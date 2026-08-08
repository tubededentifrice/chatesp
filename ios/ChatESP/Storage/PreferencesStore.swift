import Foundation

struct PreferencesStore {
    private let defaults: UserDefaults
    private let recordKey = "org.chatesp.preferences.record"

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    func load() -> AppPreferences {
        guard let data = defaults.data(forKey: recordKey) else {
            return AppPreferences()
        }
        if let record = try? JSONDecoder().decode(AppPreferences.self, from: data),
           record.formatVersion == AppPreferences.currentFormatVersion {
            return record
        }
        guard let legacy = try? JSONDecoder().decode(
            LegacyAppPreferences.self,
            from: data
        ), legacy.formatVersion == 1 else {
            return AppPreferences()
        }
        var record = AppPreferences(
            global: ChatESPConfiguration(
                chatEndpoint: legacy.chatEndpoint,
                chatModel: legacy.chatModel,
                transcriptionModel: legacy.transcriptionModel,
                speechModel: legacy.speechModel,
                approximateLocation: legacy.approximateLocation ?? ""),
            activeDeviceIdentifier: legacy.selectedDeviceIdentifier)
        if let identifier = legacy.selectedDeviceIdentifier {
            record.devices = [
                ChatESPDeviceRecord(
                    id: identifier,
                    name: "ChatESP",
                    provisioning: ProvisioningVersionState(
                        appliedRevision: legacy.appliedRevision,
                        acknowledgedFingerprintHex: legacy.acknowledgedFingerprintHex,
                        pendingRevision: legacy.pendingRevision,
                        pendingFingerprintHex: legacy.pendingFingerprintHex))
            ]
        }
        try? save(record)
        return record
    }

    func save(_ preferences: AppPreferences) throws {
        guard preferences.formatVersion == AppPreferences.currentFormatVersion else {
            throw ProvisioningError.unsupportedPreferences
        }
        let data = try JSONEncoder().encode(preferences)
        defaults.set(data, forKey: recordKey)
    }
}

private struct LegacyAppPreferences: Decodable {
    var formatVersion: Int
    var chatEndpoint: String
    var chatModel: String
    var transcriptionModel: String
    var speechModel: String
    var approximateLocation: String?
    var appliedRevision: UInt32
    var acknowledgedFingerprintHex: String?
    var pendingRevision: UInt32?
    var pendingFingerprintHex: String?
    var selectedDeviceIdentifier: UUID?

    private enum CodingKeys: String, CodingKey {
        case formatVersion
        case chatEndpoint
        case chatModel
        case transcriptionModel
        case speechModel
        case approximateLocation
        case appliedRevision
        case acknowledgedFingerprintHex
        case pendingRevision
        case pendingFingerprintHex
        case selectedDeviceIdentifier = "selectedWatchIdentifier"
    }
}
