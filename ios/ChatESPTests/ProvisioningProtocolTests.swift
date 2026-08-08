import XCTest
@testable import ChatESP

final class ProvisioningProtocolTests: XCTestCase {
    private let fixturePassword = ["PASS", "WORD", "_PLACEHOLDER"].joined()

    private func goldenSettings() -> ProvisioningSettings {
        let preferences = AppPreferences(
            chatEndpoint: "https://openrouter.ai/api/v1",
            chatModel: "deepseek/deepseek-v4-flash",
            transcriptionModel: "openai/whisper-large-v3-turbo",
            speechModel: "google/gemini-3.1-flash-tts-preview",
            approximateLocation: "Dubai, United Arab Emirates")
        let secrets = ProvisioningSecrets(
            openRouterKey: "OPENROUTER_TOKEN_PLACEHOLDER",
            braveKey: "BRAVE_TOKEN_PLACEHOLDER",
            wifiSSID: "Test Network",
            wifiPassword: fixturePassword)
        return ProvisioningSettings(preferences: preferences, secrets: secrets)
    }

    func testGoldenPacketMatchesFirmwareVector() throws {
        let packet = try goldenSettings().packet(revision: 7)
        XCTAssertEqual(packet.data.count, 303)
        XCTAssertEqual(
            packet.fingerprint.hexString,
            "09fe4fdf6757295ba4960dccbf729df3a2efe5a3acfe2eca61a5335594d27ba0")
        XCTAssertEqual(packet.data.prefix(4), Data("CESP".utf8))
        XCTAssertEqual(packet.data[4], 2)
        XCTAssertEqual(packet.data[7], 9)
        XCTAssertEqual(packet.data.readBigEndianUInt32(at: 8), 7)
    }

    func testTransferFramesAreBoundedAndOrdered() throws {
        let packet = try goldenSettings().packet(revision: 7)
        let transfer = try ProvisioningTransfer(packet: packet, transferID: 0x01020304)
        XCTAssertEqual(transfer.beginFrame.count, 16)
        XCTAssertEqual(transfer.beginFrame.prefix(4), Data("CESB".utf8))
        XCTAssertEqual(transfer.beginFrame.readBigEndianUInt32(at: 8), 0x01020304)
        XCTAssertEqual(transfer.dataFrames.count, 2)
        XCTAssertEqual(transfer.dataFrames[0].count, 194)
        XCTAssertEqual(transfer.dataFrames[1].count, 137)
        XCTAssertEqual(transfer.dataFrames[0].readBigEndianUInt32(at: 6), 0x01020304)
        XCTAssertEqual(UInt16(transfer.dataFrames[0][10]) << 8 | UInt16(transfer.dataFrames[0][11]), 0)
        XCTAssertEqual(UInt16(transfer.dataFrames[1][10]) << 8 | UInt16(transfer.dataFrames[1][11]), 180)
    }

    func testAcknowledgementRequiresExactEnvelope() throws {
        let packet = try goldenSettings().packet(revision: 7)
        var bytes = Data("CESA".utf8)
        bytes.append(ProvisioningProtocolV2.version)
        bytes.append(0)
        bytes.appendBigEndian(UInt16(0))
        bytes.appendBigEndian(UInt32(7))
        bytes.append(packet.fingerprint)
        let acknowledgement = try ProvisioningAcknowledgement(data: bytes)
        XCTAssertTrue(acknowledgement.isSuccess)
        XCTAssertEqual(acknowledgement.revision, 7)
        XCTAssertEqual(acknowledgement.fingerprint, packet.fingerprint)

        XCTAssertThrowsError(try ProvisioningAcknowledgement(data: bytes.dropLast()))
        var badFlags = bytes
        badFlags[7] = 1
        XCTAssertThrowsError(try ProvisioningAcknowledgement(data: badFlags))
        var unknownStatus = bytes
        unknownStatus[5] = 0xff
        XCTAssertThrowsError(try ProvisioningAcknowledgement(data: unknownStatus))
    }

    func testDeviceContextHasAuthenticatedBoundedLayout() throws {
        let date = Date(timeIntervalSince1970: 1_786_147_200)
        let packet = try DeviceContextPacket(
            date: date,
            timeZone: TimeZone(secondsFromGMT: 14_400)!,
            approximateLocation: "latitude 25.2, longitude 55.3")
        XCTAssertEqual(packet.data.count, 78)
        XCTAssertEqual(packet.data.prefix(4), Data("CESC".utf8))
        XCTAssertEqual(packet.data[4], 1)
        XCTAssertEqual(packet.data.readBigEndianUInt64(at: 6), 1_786_147_200)
        XCTAssertEqual(packet.utcOffsetMinutes, 240)
        XCTAssertEqual(
            packet.fingerprint.hexString,
            "d3a73c211aa2d932725513317cabdccd55494cd56614c9232c175acce58ccdb8")

        var bytes = Data("CESR".utf8)
        bytes.append(DeviceContextPacket.version)
        bytes.append(ProvisioningStatus.applied.rawValue)
        bytes.appendBigEndian(UInt16(0))
        bytes.appendBigEndian(packet.epochSeconds)
        bytes.append(packet.fingerprint)
        let acknowledgement = try DeviceContextAcknowledgement(data: bytes)
        XCTAssertEqual(acknowledgement.status, .applied)
        XCTAssertEqual(acknowledgement.epochSeconds, packet.epochSeconds)
        XCTAssertEqual(acknowledgement.fingerprint, packet.fingerprint)

        XCTAssertThrowsError(try DeviceContextPacket(
            date: date, timeZone: TimeZone(secondsFromGMT: 14_400)!,
            approximateLocation: String(repeating: "L", count: 97)))
        XCTAssertThrowsError(try DeviceContextPacket(
            date: date, timeZone: TimeZone(secondsFromGMT: 14_400)!,
            approximateLocation: "latitude 25.2\nlongitude 55.3"))
    }

    func testInvalidFieldsAreRejectedBeforeTransfer() throws {
        var preferences = AppPreferences()
        let validSecrets = ProvisioningSecrets(
            openRouterKey: "OPENROUTER_TOKEN_PLACEHOLDER",
            braveKey: "",
            wifiSSID: "Test Network",
            wifiPassword: fixturePassword)

        preferences.chatEndpoint = "http://example.com"
        XCTAssertThrowsError(
            try ProvisioningSettings(preferences: preferences, secrets: validSecrets).packet(revision: 1))

        preferences = AppPreferences()
        preferences.chatModel = "model with spaces"
        XCTAssertThrowsError(
            try ProvisioningSettings(preferences: preferences, secrets: validSecrets).packet(revision: 1))

        preferences = AppPreferences()
        preferences.approximateLocation = String(repeating: "L", count: 97)
        XCTAssertThrowsError(
            try ProvisioningSettings(preferences: preferences, secrets: validSecrets).packet(revision: 1))

        preferences = AppPreferences()
        preferences.approximateLocation = "Dubai\nUnited Arab Emirates"
        XCTAssertThrowsError(
            try ProvisioningSettings(preferences: preferences, secrets: validSecrets).packet(revision: 1))

        var invalidSecrets = validSecrets
        invalidSecrets.openRouterKey = "short"
        XCTAssertThrowsError(
            try ProvisioningSettings(preferences: preferences, secrets: invalidSecrets).packet(revision: 1))

        invalidSecrets = validSecrets
        invalidSecrets.wifiSSID = String(repeating: "S", count: 33)
        XCTAssertThrowsError(
            try ProvisioningSettings(preferences: preferences, secrets: invalidSecrets).packet(revision: 1))

        invalidSecrets = validSecrets
        invalidSecrets.wifiPassword = "short"
        XCTAssertThrowsError(
            try ProvisioningSettings(preferences: preferences, secrets: invalidSecrets).packet(revision: 1))
    }

    func testRevisionStaysStableUntilMatchingAcknowledgement() throws {
        let fingerprint = try goldenSettings().contentFingerprint()
        var preferences = AppPreferences()
        XCTAssertEqual(try preferences.revision(for: fingerprint), 1)
        XCTAssertEqual(try preferences.revision(for: fingerprint), 1)

        var other = fingerprint
        other[0] ^= 1
        XCTAssertEqual(try preferences.revision(for: other), 2)
        XCTAssertThrowsError(try preferences.acknowledge(revision: 1, fingerprint: fingerprint))
        try preferences.acknowledge(revision: 2, fingerprint: other)
        XCTAssertEqual(preferences.appliedRevision, 2)
        XCTAssertEqual(try preferences.revision(for: other), 2)
        try preferences.acknowledge(revision: 2, fingerprint: other)
        XCTAssertEqual(try preferences.revision(for: fingerprint), 3)
    }

    func testPreferencesUseOneVersionedRecord() throws {
        let suite = "org.chatesp.tests.\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let store = PreferencesStore(defaults: defaults)
        var preferences = AppPreferences()
        preferences.chatModel = "example/model"
        try store.save(preferences)
        let record = defaults.persistentDomain(forName: suite)
        XCTAssertEqual(record?.keys.count, 1)
        XCTAssertEqual(store.load(), preferences)
    }

    func testMemoryFingerprintMatchesFirmwareVector() throws {
        let facts = [
            MemoryFact(id: 2, fact: "User likes tea."),
            MemoryFact(id: 7, fact: "Use short answers."),
        ]
        XCTAssertEqual(
            try MemoryProtocolV1.fingerprint(for: facts).hexString,
            "9b9a7dc2d6f8b263694fd1b4b02d675daa47cb56bb9d84b7219ee93309440d1d")
        XCTAssertThrowsError(try MemoryProtocolV1.fingerprint(for: [
            MemoryFact(id: 2, fact: "One"),
            MemoryFact(id: 2, fact: "Two"),
        ]))
    }

    func testMemoryCommandUsesExactNetworkOrderLayout() throws {
        let fingerprint = Data((0..<32).map(UInt8.init))
        let command = try MemoryCommand(
            operation: .add,
            requestID: 0x01020304,
            expectedRevision: 0x05060708,
            expectedFingerprint: fingerprint,
            fact: "Tea")
        XCTAssertEqual(command.data.count, 57)
        XCTAssertEqual(command.data.prefix(4), Data("CEMC".utf8))
        XCTAssertEqual(command.data[4], 1)
        XCTAssertEqual(command.data[5], MemoryOperation.add.rawValue)
        XCTAssertEqual(command.data.readBigEndianUInt32(at: 8), 0x01020304)
        XCTAssertEqual(command.data.readBigEndianUInt32(at: 12), 0x05060708)
        XCTAssertEqual(command.data.subdata(in: 16..<48), fingerprint)
        XCTAssertEqual(command.data.readBigEndianUInt16(at: 52), 3)
        XCTAssertEqual(command.data.suffix(3), Data("Tea".utf8))
        XCTAssertThrowsError(try MemoryCommand(
            operation: .add,
            requestID: 1,
            expectedRevision: 0,
            expectedFingerprint: fingerprint,
            fact: "Line\nbreak"))
    }

    func testMemoryResponseRequiresExactFlagsAndLength() throws {
        let fingerprint = Data(repeating: 0x5a, count: 32)
        var bytes = Data("CEMR".utf8)
        bytes.append(MemoryProtocolV1.version)
        bytes.append(MemoryStatus.applied.rawValue)
        bytes.append(MemoryOperation.listPage.rawValue)
        bytes.append(0x03)
        bytes.appendBigEndian(UInt32(9))
        bytes.appendBigEndian(UInt32(4))
        bytes.append(fingerprint)
        bytes.appendBigEndian(UInt32(7))
        bytes.appendBigEndian(UInt16(3))
        bytes.append(2)
        bytes.append(0)
        bytes.append(Data("Tea".utf8))
        let response = try MemoryResponse(data: bytes)
        XCTAssertEqual(response.requestID, 9)
        XCTAssertEqual(response.revision, 4)
        XCTAssertEqual(response.memoryID, 7)
        XCTAssertEqual(response.fact, "Tea")
        XCTAssertTrue(response.hasMore)
        XCTAssertEqual(response.totalCount, 2)

        var badFlags = bytes
        badFlags[7] = 0x80
        XCTAssertThrowsError(try MemoryResponse(data: badFlags))
        XCTAssertThrowsError(try MemoryResponse(data: bytes.dropLast()))
    }

    func testMemoryChangeEventHasNoRequestOrFact() throws {
        var bytes = Data("CEMR".utf8)
        bytes.append(MemoryProtocolV1.version)
        bytes.append(MemoryStatus.applied.rawValue)
        bytes.append(MemoryOperation.changed.rawValue)
        bytes.append(0x04)
        bytes.appendBigEndian(UInt32(0))
        bytes.appendBigEndian(UInt32(8))
        bytes.append(Data(repeating: 0x31, count: 32))
        bytes.appendBigEndian(UInt32(0))
        bytes.appendBigEndian(UInt16(0))
        bytes.append(3)
        bytes.append(0)
        let response = try MemoryResponse(data: bytes)
        XCTAssertTrue(response.isChangeEvent)
        XCTAssertEqual(response.operation, .changed)
    }
}
