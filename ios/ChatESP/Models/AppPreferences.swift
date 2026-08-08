import Foundation

struct ChatESPConfiguration: Codable, Equatable {
    var chatEndpoint = "https://openrouter.ai/api/v1"
    var chatModel = "~deepseek/deepseek-v4-flash-latest"
    var transcriptionModel = "openai/whisper-large-v3-turbo"
    var speechModel = "hexgrad/kokoro-82m"
    var englishSpeechVoice = "af_heart"
    var frenchSpeechVoice = "ff_siwis"
    var approximateLocation = ""

    private enum CodingKeys: String, CodingKey {
        case chatEndpoint
        case chatModel
        case transcriptionModel
        case speechModel
        case englishSpeechVoice
        case frenchSpeechVoice
        case approximateLocation
    }

    init(
        chatEndpoint: String = "https://openrouter.ai/api/v1",
        chatModel: String = "~deepseek/deepseek-v4-flash-latest",
        transcriptionModel: String = "openai/whisper-large-v3-turbo",
        speechModel: String = "hexgrad/kokoro-82m",
        englishSpeechVoice: String = "af_heart",
        frenchSpeechVoice: String = "ff_siwis",
        approximateLocation: String = ""
    ) {
        self.chatEndpoint = chatEndpoint
        self.chatModel = chatModel
        self.transcriptionModel = transcriptionModel
        self.speechModel = speechModel
        self.englishSpeechVoice = englishSpeechVoice
        self.frenchSpeechVoice = frenchSpeechVoice
        self.approximateLocation = approximateLocation
    }

    init(from decoder: Decoder) throws {
        let defaults = Self()
        let values = try decoder.container(keyedBy: CodingKeys.self)
        chatEndpoint = try values.decodeIfPresent(
            String.self, forKey: .chatEndpoint) ?? defaults.chatEndpoint
        chatModel = try values.decodeIfPresent(
            String.self, forKey: .chatModel) ?? defaults.chatModel
        transcriptionModel = try values.decodeIfPresent(
            String.self, forKey: .transcriptionModel) ?? defaults.transcriptionModel
        speechModel = try values.decodeIfPresent(
            String.self, forKey: .speechModel) ?? defaults.speechModel
        englishSpeechVoice = try values.decodeIfPresent(
            String.self, forKey: .englishSpeechVoice) ?? defaults.englishSpeechVoice
        frenchSpeechVoice = try values.decodeIfPresent(
            String.self, forKey: .frenchSpeechVoice) ?? defaults.frenchSpeechVoice
        approximateLocation = try values.decodeIfPresent(
            String.self, forKey: .approximateLocation) ?? defaults.approximateLocation
    }
}

struct ChatESPConfigurationOverrides: Codable, Equatable {
    var chatEndpoint: String?
    var chatModel: String?
    var transcriptionModel: String?
    var speechModel: String?
    var englishSpeechVoice: String?
    var frenchSpeechVoice: String?
    var approximateLocation: String?

    var isEmpty: Bool {
        chatEndpoint == nil && chatModel == nil && transcriptionModel == nil &&
            speechModel == nil && englishSpeechVoice == nil &&
            frenchSpeechVoice == nil && approximateLocation == nil
    }

    func applying(to global: ChatESPConfiguration) -> ChatESPConfiguration {
        ChatESPConfiguration(
            chatEndpoint: chatEndpoint ?? global.chatEndpoint,
            chatModel: chatModel ?? global.chatModel,
            transcriptionModel: transcriptionModel ?? global.transcriptionModel,
            speechModel: speechModel ?? global.speechModel,
            englishSpeechVoice: englishSpeechVoice ?? global.englishSpeechVoice,
            frenchSpeechVoice: frenchSpeechVoice ?? global.frenchSpeechVoice,
            approximateLocation: approximateLocation ?? global.approximateLocation)
    }
}

struct ProvisioningVersionState: Codable, Equatable {
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

    mutating func recoverActiveVersion(
        revision: UInt32,
        fingerprint: Data
    ) throws {
        guard revision > 0, fingerprint.count == 32 else {
            throw ProvisioningError.malformedAcknowledgement
        }
        appliedRevision = revision
        acknowledgedFingerprintHex = fingerprint.hexString
        pendingRevision = nil
        pendingFingerprintHex = nil
    }
}

struct ChatESPDeviceRecord: Codable, Equatable, Identifiable {
    let id: UUID
    var name: String
    var overrides = ChatESPConfigurationOverrides()
    var provisioning = ProvisioningVersionState()
}

struct AppPreferences: Codable, Equatable {
    static let currentFormatVersion = 2

    var formatVersion = currentFormatVersion
    var global = ChatESPConfiguration()
    var devices: [ChatESPDeviceRecord] = []
    var activeDeviceIdentifier: UUID?

    func device(id: UUID) -> ChatESPDeviceRecord? {
        devices.first { $0.id == id }
    }

    func effectiveConfiguration(for id: UUID) -> ChatESPConfiguration? {
        device(id: id)?.overrides.applying(to: global)
    }
}

struct ProvisioningSecretValues: Codable, Equatable {
    var openRouterKey = ""
    var braveKey = ""
    var wifiSSID = ""
    var wifiPassword = ""
}

struct ProvisioningSecretOverrides: Codable, Equatable {
    var openRouterKey: String?
    var braveKey: String?
    var wifiSSID: String?
    var wifiPassword: String?

    var isEmpty: Bool {
        openRouterKey == nil && braveKey == nil && wifiSSID == nil &&
            wifiPassword == nil
    }

    func applying(to global: ProvisioningSecretValues) -> ProvisioningSecretValues {
        ProvisioningSecretValues(
            openRouterKey: openRouterKey ?? global.openRouterKey,
            braveKey: braveKey ?? global.braveKey,
            wifiSSID: wifiSSID ?? global.wifiSSID,
            wifiPassword: wifiPassword ?? global.wifiPassword)
    }
}

struct ProvisioningSecrets: Codable, Equatable {
    var global = ProvisioningSecretValues()
    var deviceOverrides: [UUID: ProvisioningSecretOverrides] = [:]

    func effectiveValues(for id: UUID) -> ProvisioningSecretValues {
        deviceOverrides[id]?.applying(to: global) ?? global
    }

    private enum CodingKeys: String, CodingKey {
        case global
        case deviceOverrides
        case openRouterKey
        case braveKey
        case wifiSSID
        case wifiPassword
    }

    init(
        global: ProvisioningSecretValues = ProvisioningSecretValues(),
        deviceOverrides: [UUID: ProvisioningSecretOverrides] = [:]
    ) {
        self.global = global
        self.deviceOverrides = deviceOverrides
    }

    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        if let global = try values.decodeIfPresent(
            ProvisioningSecretValues.self,
            forKey: .global
        ) {
            self.global = global
            deviceOverrides = try values.decodeIfPresent(
                [UUID: ProvisioningSecretOverrides].self,
                forKey: .deviceOverrides
            ) ?? [:]
            return
        }
        global = ProvisioningSecretValues(
            openRouterKey: try values.decodeIfPresent(
                String.self,
                forKey: .openRouterKey
            ) ?? "",
            braveKey: try values.decodeIfPresent(String.self, forKey: .braveKey) ?? "",
            wifiSSID: try values.decodeIfPresent(String.self, forKey: .wifiSSID) ?? "",
            wifiPassword: try values.decodeIfPresent(
                String.self,
                forKey: .wifiPassword
            ) ?? "")
        deviceOverrides = [:]
    }

    func encode(to encoder: Encoder) throws {
        var values = encoder.container(keyedBy: CodingKeys.self)
        try values.encode(global, forKey: .global)
        try values.encode(deviceOverrides, forKey: .deviceOverrides)
    }
}

extension Data {
    var hexString: String {
        map { String(format: "%02x", $0) }.joined()
    }
}
