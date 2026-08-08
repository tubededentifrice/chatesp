import CryptoKit
import Foundation
import Security

enum ProvisioningProtocolV2 {
    static let version: UInt8 = 2
    static let maximumPacketSize = 1_024
    static let packetHeaderSize = 48
    static let fieldCount: UInt8 = 9
    static let dataBytesPerFrame = 180

    static let serviceUUID = "7B2E1000-6F3C-4B8A-9D71-4C4553500001"
    static let controlUUID = "7B2E1001-6F3C-4B8A-9D71-4C4553500001"
    static let dataUUID = "7B2E1002-6F3C-4B8A-9D71-4C4553500001"
    static let acknowledgementUUID = "7B2E1003-6F3C-4B8A-9D71-4C4553500001"
    static let deviceContextUUID = "7B2E1004-6F3C-4B8A-9D71-4C4553500001"
}

struct DeviceContextPacket: Equatable {
    static let version: UInt8 = 1
    static let maximumLocationBytes = 96

    let data: Data
    let epochSeconds: UInt64
    let utcOffsetMinutes: Int16
    let fingerprint: Data

    init(
        date: Date,
        timeZone: TimeZone = .current,
        approximateLocation: String
    ) throws {
        let seconds = date.timeIntervalSince1970.rounded(.down)
        guard seconds >= 1_577_836_800,
              seconds <= 253_402_300_799 else {
            throw ProvisioningError.invalidDeviceContext
        }
        epochSeconds = UInt64(seconds)
        let offsetSeconds = timeZone.secondsFromGMT(for: date)
        guard offsetSeconds.isMultiple(of: 60),
              (-50_400...50_400).contains(offsetSeconds) else {
            throw ProvisioningError.invalidDeviceContext
        }
        utcOffsetMinutes = Int16(offsetSeconds / 60)
        guard let location = approximateLocation.data(using: .utf8),
              location.count <= Self.maximumLocationBytes,
              approximateLocation.unicodeScalars.allSatisfy({
                  !CharacterSet.controlCharacters.contains($0)
              }) else {
            throw ProvisioningError.invalidDeviceContext
        }

        var fingerprintInput = Data("CESP-CONTEXT-V1".utf8)
        fingerprintInput.append(Self.version)
        fingerprintInput.appendBigEndian(epochSeconds)
        fingerprintInput.appendBigEndian(utcOffsetMinutes)
        fingerprintInput.append(UInt8(location.count))
        fingerprintInput.append(location)
        fingerprint = Data(SHA256.hash(data: fingerprintInput))

        var packet = Data("CESC".utf8)
        packet.append(Self.version)
        packet.append(0)
        packet.appendBigEndian(epochSeconds)
        packet.appendBigEndian(utcOffsetMinutes)
        packet.append(fingerprint)
        packet.append(UInt8(location.count))
        packet.append(location)
        data = packet
    }
}

struct DeviceContextAcknowledgement: Equatable {
    let status: ProvisioningStatus
    let epochSeconds: UInt64
    let fingerprint: Data

    init(data: Data) throws {
        guard data.count == 48,
              data.prefix(4) == Data("CESR".utf8),
              data[4] == DeviceContextPacket.version,
              data[6] == 0,
              data[7] == 0,
              let status = ProvisioningStatus(rawValue: data[5]) else {
            throw ProvisioningError.malformedAcknowledgement
        }
        self.status = status
        epochSeconds = data.readBigEndianUInt64(at: 8)
        fingerprint = data.subdata(in: 16..<48)
    }
}

struct ProvisioningSettings: Equatable {
    let chatEndpoint: String
    let openRouterKey: String
    let braveKey: String
    let wifiSSID: String
    let wifiPassword: String
    let chatModel: String
    let transcriptionModel: String
    let speechModel: String
    let approximateLocation: String

    init(preferences: AppPreferences, secrets: ProvisioningSecrets) {
        chatEndpoint = preferences.chatEndpoint
        openRouterKey = secrets.openRouterKey
        braveKey = secrets.braveKey
        wifiSSID = secrets.wifiSSID
        wifiPassword = secrets.wifiPassword
        chatModel = preferences.chatModel
        transcriptionModel = preferences.transcriptionModel
        speechModel = preferences.speechModel
        approximateLocation = preferences.approximateLocation ?? ""
    }

    func contentFingerprint() throws -> Data {
        let payload = try encodedPayload()
        var input = Data("CESP-CONTENT-V2".utf8)
        input.append(ProvisioningProtocolV2.version)
        input.append(1)
        input.append(ProvisioningProtocolV2.fieldCount)
        input.append(payload)
        return Data(SHA256.hash(data: input))
    }

    func packet(revision: UInt32) throws -> ProvisioningPacket {
        guard revision > 0 else {
            throw ProvisioningError.invalidRevision
        }
        let payload = try encodedPayload()
        let fingerprint = try contentFingerprint()
        let totalLength = ProvisioningProtocolV2.packetHeaderSize + payload.count
        guard totalLength <= ProvisioningProtocolV2.maximumPacketSize else {
            throw ProvisioningError.packetTooLarge
        }

        var data = Data("CESP".utf8)
        data.append(ProvisioningProtocolV2.version)
        data.append(1)
        data.append(0)
        data.append(ProvisioningProtocolV2.fieldCount)
        data.appendBigEndian(revision)
        data.appendBigEndian(UInt16(payload.count))
        data.appendBigEndian(UInt16(totalLength))
        data.append(fingerprint)
        data.append(payload)
        return ProvisioningPacket(data: data, revision: revision, fingerprint: fingerprint)
    }

    private func encodedPayload() throws -> Data {
        let values = [
            try field(id: 1, text: chatEndpoint, rule: .endpoint),
            try field(id: 2, text: openRouterKey, rule: .openRouterKey),
            try field(id: 3, text: braveKey, rule: .braveKey),
            try field(id: 4, text: wifiSSID, rule: .wifiSSID),
            try field(id: 5, text: wifiPassword, rule: .wifiPassword),
            try field(id: 6, text: chatModel, rule: .model),
            try field(id: 7, text: transcriptionModel, rule: .model),
            try field(id: 8, text: speechModel, rule: .model),
            try field(id: 9, text: approximateLocation, rule: .approximateLocation),
        ]
        return values.reduce(into: Data()) { $0.append($1) }
    }

    private enum FieldRule {
        case endpoint
        case openRouterKey
        case braveKey
        case wifiSSID
        case wifiPassword
        case model
        case approximateLocation
    }

    private func field(id: UInt8, text: String, rule: FieldRule) throws -> Data {
        guard let bytes = text.data(using: .utf8) else {
            throw ProvisioningError.invalidField(id)
        }
        let isValid: Bool
        switch rule {
        case .endpoint:
            isValid = Self.validEndpoint(text, byteCount: bytes.count)
        case .openRouterKey:
            isValid = (8...256).contains(bytes.count) && Self.visibleASCII(bytes)
        case .braveKey:
            isValid = bytes.count <= 128 && (bytes.isEmpty || Self.visibleASCII(bytes))
        case .wifiSSID:
            isValid = (1...32).contains(bytes.count) && !bytes.contains(0)
        case .wifiPassword:
            isValid = (8...63).contains(bytes.count) && !bytes.contains(0)
        case .model:
            isValid = (1...96).contains(bytes.count) && bytes.allSatisfy { byte in
                (0x30...0x39).contains(byte) || (0x41...0x5a).contains(byte) ||
                    (0x61...0x7a).contains(byte) || [0x2d, 0x2e, 0x2f, 0x3a, 0x5f].contains(byte)
            }
        case .approximateLocation:
            isValid = bytes.count <= 96 && text.unicodeScalars.allSatisfy {
                !CharacterSet.controlCharacters.contains($0)
            }
        }
        guard isValid else {
            throw ProvisioningError.invalidField(id)
        }
        var result = Data([id])
        result.appendBigEndian(UInt16(bytes.count))
        result.append(bytes)
        return result
    }

    private static func visibleASCII(_ data: Data) -> Bool {
        data.allSatisfy { (0x21...0x7e).contains($0) }
    }

    private static func validEndpoint(_ value: String, byteCount: Int) -> Bool {
        guard (12...192).contains(byteCount),
              let bytes = value.data(using: .utf8),
              visibleASCII(bytes),
              let parts = URLComponents(string: value),
              parts.scheme == "https",
              parts.user == nil,
              parts.password == nil,
              parts.query == nil,
              parts.fragment == nil,
              let host = parts.host,
              host.contains("."),
              !host.hasPrefix("."),
              !host.hasSuffix(".") else {
            return false
        }
        return true
    }
}

struct ProvisioningPacket: Equatable {
    let data: Data
    let revision: UInt32
    let fingerprint: Data
}

enum ProvisioningStatus: UInt8, Equatable {
    case applied = 0x00
    case unchanged = 0x01
    case authenticationRequired = 0x10
    case unsupportedVersion = 0x11
    case malformedTransfer = 0x12
    case malformedPacket = 0x13
    case invalidField = 0x14
    case staleRevision = 0x15
    case revisionConflict = 0x16
    case storageFailure = 0x17
    case busy = 0x18
}

struct ProvisioningAcknowledgement: Equatable {
    let status: ProvisioningStatus
    let revision: UInt32
    let fingerprint: Data

    var isSuccess: Bool {
        status == .applied || status == .unchanged
    }

    init(data: Data) throws {
        guard data.count == 44,
              data.prefix(4) == Data("CESA".utf8),
              data[4] == ProvisioningProtocolV2.version,
              data[6] == 0,
              data[7] == 0,
              let status = ProvisioningStatus(rawValue: data[5]) else {
            throw ProvisioningError.malformedAcknowledgement
        }
        self.status = status
        revision = data.readBigEndianUInt32(at: 8)
        fingerprint = data.subdata(in: 12..<44)
    }
}

struct ProvisioningTransfer {
    let transferID: UInt32
    let packet: ProvisioningPacket

    init(packet: ProvisioningPacket, transferID: UInt32? = nil) throws {
        self.packet = packet
        if let transferID {
            self.transferID = transferID
        } else {
            var random: UInt32 = 0
            guard SecRandomCopyBytes(kSecRandomDefault, MemoryLayout.size(ofValue: random), &random)
                    == errSecSuccess else {
                throw ProvisioningError.randomFailure
            }
            self.transferID = random
        }
    }

    var beginFrame: Data {
        var data = Data("CESB".utf8)
        data.append(ProvisioningProtocolV2.version)
        data.append(1)
        data.appendBigEndian(UInt16(0))
        data.appendBigEndian(transferID)
        data.appendBigEndian(UInt16(packet.data.count))
        data.appendBigEndian(UInt16(ProvisioningProtocolV2.dataBytesPerFrame))
        return data
    }

    var dataFrames: [Data] {
        stride(
            from: 0,
            to: packet.data.count,
            by: ProvisioningProtocolV2.dataBytesPerFrame
        ).map { offset in
            let end = min(offset + ProvisioningProtocolV2.dataBytesPerFrame, packet.data.count)
            let bytes = packet.data.subdata(in: offset..<end)
            var frame = Data("CESD".utf8)
            frame.append(ProvisioningProtocolV2.version)
            frame.append(0)
            frame.appendBigEndian(transferID)
            frame.appendBigEndian(UInt16(offset))
            frame.appendBigEndian(UInt16(bytes.count))
            frame.append(bytes)
            return frame
        }
    }
}

enum MemoryProtocolV1 {
    static let version: UInt8 = 1
    static let maximumFacts = 10
    static let maximumFactBytes = 128
    static let commandHeaderSize = 54
    static let responseHeaderSize = 56
    static let commandUUID = "7B2E1005-6F3C-4B8A-9D71-4C4553500001"
    static let responseUUID = "7B2E1006-6F3C-4B8A-9D71-4C4553500001"

    static func validFact(_ fact: String) -> Bool {
        guard let bytes = fact.data(using: .utf8),
              !bytes.isEmpty,
              bytes.count <= maximumFactBytes else {
            return false
        }
        return fact.unicodeScalars.allSatisfy {
            !CharacterSet.controlCharacters.contains($0)
        }
    }

    static func fingerprint(for facts: [MemoryFact]) throws -> Data {
        guard facts.count <= maximumFacts else {
            throw MemoryProtocolError.invalidField
        }
        var input = Data("CHATESP-MEMORY-V1".utf8)
        input.append(version)
        input.append(UInt8(facts.count))
        var priorID: UInt32 = 0
        for fact in facts {
            guard fact.id > priorID,
                  validFact(fact.fact),
                  let bytes = fact.fact.data(using: .utf8) else {
                throw MemoryProtocolError.invalidField
            }
            input.appendBigEndian(fact.id)
            input.appendBigEndian(UInt16(bytes.count))
            input.append(bytes)
            priorID = fact.id
        }
        return Data(SHA256.hash(data: input))
    }
}

struct MemoryFact: Identifiable, Equatable {
    let id: UInt32
    let fact: String
}

enum MemoryOperation: UInt8, Equatable {
    case changed = 0
    case listPage = 1
    case add = 2
    case delete = 3
    case clear = 4
}

enum MemoryStatus: UInt8, Equatable {
    case applied = 0x00
    case unchanged = 0x01
    case full = 0x02
    case notFound = 0x03
    case revisionConflict = 0x04
    case invalidField = 0x10
    case storageFailure = 0x11
    case authenticationRequired = 0x12
    case busy = 0x13
    case unsupportedVersion = 0x14
}

struct MemoryCommand: Equatable {
    let operation: MemoryOperation
    let requestID: UInt32
    let expectedRevision: UInt32
    let expectedFingerprint: Data
    let memoryID: UInt32
    let fact: String

    var data: Data {
        var result = Data("CEMC".utf8)
        result.append(MemoryProtocolV1.version)
        result.append(operation.rawValue)
        result.appendBigEndian(UInt16(0))
        result.appendBigEndian(requestID)
        result.appendBigEndian(expectedRevision)
        result.append(expectedFingerprint)
        result.appendBigEndian(memoryID)
        let factData = Data(fact.utf8)
        result.appendBigEndian(UInt16(factData.count))
        result.append(factData)
        return result
    }

    init(
        operation: MemoryOperation,
        requestID: UInt32,
        expectedRevision: UInt32,
        expectedFingerprint: Data,
        memoryID: UInt32 = 0,
        fact: String = ""
    ) throws {
        guard operation != .changed,
              requestID != 0,
              expectedFingerprint.count == 32 else {
            throw MemoryProtocolError.invalidField
        }
        switch operation {
        case .listPage:
            guard fact.isEmpty else { throw MemoryProtocolError.invalidField }
        case .add:
            guard memoryID == 0, MemoryProtocolV1.validFact(fact) else {
                throw MemoryProtocolError.invalidField
            }
        case .delete:
            guard memoryID != 0, fact.isEmpty else {
                throw MemoryProtocolError.invalidField
            }
        case .clear:
            guard memoryID == 0, fact.isEmpty else {
                throw MemoryProtocolError.invalidField
            }
        case .changed:
            throw MemoryProtocolError.invalidField
        }
        self.operation = operation
        self.requestID = requestID
        self.expectedRevision = expectedRevision
        self.expectedFingerprint = expectedFingerprint
        self.memoryID = memoryID
        self.fact = fact
    }
}

struct MemoryResponse: Equatable {
    let status: MemoryStatus
    let operation: MemoryOperation
    let requestID: UInt32
    let revision: UInt32
    let fingerprint: Data
    let memoryID: UInt32
    let totalCount: Int
    let fact: String?
    let hasMore: Bool
    let isChangeEvent: Bool

    init(data: Data) throws {
        guard data.count >= MemoryProtocolV1.responseHeaderSize,
              data.count <= MemoryProtocolV1.responseHeaderSize +
                MemoryProtocolV1.maximumFactBytes,
              data.prefix(4) == Data("CEMR".utf8),
              data[4] == MemoryProtocolV1.version,
              let status = MemoryStatus(rawValue: data[5]),
              let operation = MemoryOperation(rawValue: data[6]),
              data[7] & ~UInt8(0x07) == 0,
              data[55] == 0 else {
            throw MemoryProtocolError.malformedResponse
        }
        let flags = data[7]
        let hasItem = flags & 0x01 != 0
        hasMore = flags & 0x02 != 0
        isChangeEvent = flags & 0x04 != 0
        requestID = data.readBigEndianUInt32(at: 8)
        revision = data.readBigEndianUInt32(at: 12)
        fingerprint = data.subdata(in: 16..<48)
        memoryID = data.readBigEndianUInt32(at: 48)
        let factLength = Int(data.readBigEndianUInt16(at: 52))
        totalCount = Int(data[54])
        guard totalCount <= MemoryProtocolV1.maximumFacts,
              data.count == MemoryProtocolV1.responseHeaderSize + factLength else {
            throw MemoryProtocolError.malformedResponse
        }
        if hasItem {
            let bytes = data.subdata(
                in: MemoryProtocolV1.responseHeaderSize..<data.count)
            guard memoryID != 0,
                  let value = String(data: bytes, encoding: .utf8),
                  MemoryProtocolV1.validFact(value) else {
                throw MemoryProtocolError.malformedResponse
            }
            fact = value
        } else {
            guard factLength == 0 else {
                throw MemoryProtocolError.malformedResponse
            }
            fact = nil
        }
        guard !hasMore || hasItem,
              !hasItem || operation == .listPage else {
            throw MemoryProtocolError.malformedResponse
        }
        if isChangeEvent {
            guard operation == .changed,
                  requestID == 0,
                  status == .applied,
                  !hasItem,
                  !hasMore else {
                throw MemoryProtocolError.malformedResponse
            }
        } else {
            guard operation != .changed, requestID != 0 else {
                throw MemoryProtocolError.malformedResponse
            }
        }
        self.status = status
        self.operation = operation
    }
}

enum MemoryProtocolError: Error, Equatable {
    case invalidField
    case malformedResponse
    case responseMismatch
    case unavailable
    case disconnected
    case timeout
    case rejected(MemoryStatus)
}

extension MemoryProtocolError: LocalizedError {
    var errorDescription: String? {
        switch self {
        case .invalidField:
            return "The memory fact is not valid."
        case .malformedResponse, .responseMismatch:
            return "The watch returned an invalid memory response."
        case .unavailable:
            return "Update the watch firmware to manage memories."
        case .disconnected:
            return "Connect the selected watch to manage memories."
        case .timeout:
            return "The watch did not confirm the memory request in time."
        case .rejected(let status):
            return "The watch rejected the memory request (code \(status.rawValue))."
        }
    }
}

enum ProvisioningError: Error, Equatable {
    case invalidField(UInt8)
    case invalidRevision
    case invalidDeviceContext
    case packetTooLarge
    case revisionExhausted
    case acknowledgementMismatch
    case malformedAcknowledgement
    case unsupportedPreferences
    case keychain(OSStatus)
    case randomFailure
    case bluetoothUnavailable
    case noDevice
    case disconnected
    case missingService
    case missingCharacteristic
    case writeFailed
    case timeout
    case deviceRejected(ProvisioningStatus)
}

extension ProvisioningError: LocalizedError {
    var errorDescription: String? {
        switch self {
        case .invalidField:
            return "One setting is not valid. Check each value and try again."
        case .invalidRevision, .revisionExhausted:
            return "The settings revision is not valid."
        case .invalidDeviceContext:
            return "The time or approximate location is not valid."
        case .packetTooLarge:
            return "The settings are too large."
        case .acknowledgementMismatch, .malformedAcknowledgement:
            return "The watch returned an invalid confirmation."
        case .unsupportedPreferences:
            return "The saved settings use an unsupported version."
        case .keychain:
            return "The app could not use Keychain."
        case .randomFailure:
            return "The app could not start a secure transfer."
        case .bluetoothUnavailable:
            return "Bluetooth is not available."
        case .noDevice:
            return "Select a watch first."
        case .disconnected:
            return "The watch disconnected."
        case .missingService, .missingCharacteristic:
            return "The watch does not support this provisioning version."
        case .writeFailed:
            return "The watch did not accept a transfer frame."
        case .timeout:
            return "The watch did not confirm the settings in time."
        case .deviceRejected(let status):
            return "The watch rejected the settings (code \(status.rawValue))."
        }
    }
}

extension Data {
    mutating func appendBigEndian(_ value: UInt16) {
        append(UInt8(value >> 8))
        append(UInt8(value & 0xff))
    }

    mutating func appendBigEndian(_ value: Int16) {
        appendBigEndian(UInt16(bitPattern: value))
    }

    mutating func appendBigEndian(_ value: UInt32) {
        append(UInt8(value >> 24))
        append(UInt8((value >> 16) & 0xff))
        append(UInt8((value >> 8) & 0xff))
        append(UInt8(value & 0xff))
    }

    mutating func appendBigEndian(_ value: UInt64) {
        for offset in stride(from: 56, through: 0, by: -8) {
            append(UInt8((value >> UInt64(offset)) & 0xff))
        }
    }

    func readBigEndianUInt32(at offset: Int) -> UInt32 {
        (UInt32(self[offset]) << 24) |
            (UInt32(self[offset + 1]) << 16) |
            (UInt32(self[offset + 2]) << 8) |
            UInt32(self[offset + 3])
    }

    func readBigEndianUInt16(at offset: Int) -> UInt16 {
        (UInt16(self[offset]) << 8) | UInt16(self[offset + 1])
    }

    func readBigEndianUInt64(at offset: Int) -> UInt64 {
        (0..<8).reduce(UInt64(0)) { value, index in
            (value << 8) | UInt64(self[offset + index])
        }
    }
}
