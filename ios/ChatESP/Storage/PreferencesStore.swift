import Foundation

struct PreferencesStore {
    private let defaults: UserDefaults
    private let recordKey = "org.chatesp.preferences.record"

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    func load() -> AppPreferences {
        guard let data = defaults.data(forKey: recordKey),
              let record = try? JSONDecoder().decode(AppPreferences.self, from: data),
              record.formatVersion == AppPreferences.currentFormatVersion else {
            return AppPreferences()
        }
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
