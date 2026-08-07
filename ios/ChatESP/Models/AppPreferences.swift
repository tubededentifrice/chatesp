import Foundation

struct AppPreferences: Codable, Equatable {
    static let currentFormatVersion = 1

    var formatVersion = currentFormatVersion
    var chatEndpoint = "https://openrouter.ai/api/v1"
    var chatModel = "deepseek/deepseek-v4-flash"
    var transcriptionModel = "openai/whisper-large-v3-turbo"
    var speechModel = "hexgrad/kokoro-82m"
    var appliedRevision: UInt32 = 0
    var acknowledgedFingerprintHex: String?
    var pendingRevision: UInt32?
    var pendingFingerprintHex: String?

    mutating func revision(for fingerprint: Data) throws -> UInt32 {
        let hex = fingerprint.hexString
        if pendingFingerprintHex == hex, let pendingRevision {
            return pendingRevision
        }
        if pendingRevision == nil,
           acknowledgedFingerprintHex == hex,
           appliedRevision > 0 {
            pendingRevision = appliedRevision
            pendingFingerprintHex = hex
            return appliedRevision
        }
        let baseRevision = max(appliedRevision, pendingRevision ?? 0)
        guard baseRevision < UInt32.max else {
            throw ProvisioningError.revisionExhausted
        }
        let next = baseRevision + 1
        pendingRevision = next
        pendingFingerprintHex = hex
        return next
    }

    mutating func acknowledge(revision: UInt32, fingerprint: Data) throws {
        let hex = fingerprint.hexString
        guard pendingRevision == revision, pendingFingerprintHex == hex else {
            throw ProvisioningError.acknowledgementMismatch
        }
        appliedRevision = revision
        acknowledgedFingerprintHex = hex
        pendingRevision = nil
        pendingFingerprintHex = nil
    }
}

struct ProvisioningSecrets: Codable, Equatable {
    var openRouterKey = ""
    var braveKey = ""
    var wifiSSID = ""
    var wifiPassword = ""
}

extension Data {
    var hexString: String {
        map { String(format: "%02x", $0) }.joined()
    }
}
