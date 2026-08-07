import Foundation
import Security

struct KeychainStore {
    private let service = "org.chatesp.companion.provisioning"
    private let account = "settings-secrets-v1"

    func load() throws -> ProvisioningSecrets {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var item: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &item)
        if status == errSecItemNotFound {
            return ProvisioningSecrets()
        }
        guard status == errSecSuccess, let data = item as? Data else {
            throw ProvisioningError.keychain(status)
        }
        return try JSONDecoder().decode(ProvisioningSecrets.self, from: data)
    }

    func save(_ secrets: ProvisioningSecrets) throws {
        let data = try JSONEncoder().encode(secrets)
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        let update: [String: Any] = [kSecValueData as String: data]
        let updateStatus = SecItemUpdate(query as CFDictionary, update as CFDictionary)
        if updateStatus == errSecSuccess {
            return
        }
        guard updateStatus == errSecItemNotFound else {
            throw ProvisioningError.keychain(updateStatus)
        }
        var add = query
        add[kSecValueData as String] = data
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        add[kSecAttrSynchronizable as String] = false
        let addStatus = SecItemAdd(add as CFDictionary, nil)
        guard addStatus == errSecSuccess else {
            throw ProvisioningError.keychain(addStatus)
        }
    }
}
