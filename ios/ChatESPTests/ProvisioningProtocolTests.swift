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
}
