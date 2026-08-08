import XCTest
@testable import ChatESP

private enum TestSecretsError: Error {
    case unavailable
}

private final class TestSecretsStore: SecretsStore {
    var loadResult: Result<ProvisioningSecrets, Error>
    private(set) var saved: [ProvisioningSecrets] = []

    init(loadResult: Result<ProvisioningSecrets, Error>) {
        self.loadResult = loadResult
    }

    func load() throws -> ProvisioningSecrets {
        try loadResult.get()
    }

    func save(_ secrets: ProvisioningSecrets) throws {
        saved.append(secrets)
    }
}

final class ProvisioningProtocolTests: XCTestCase {
    private let fixturePassword = ["PASS", "WORD", "_PLACEHOLDER"].joined()

    private func goldenSettings() -> ProvisioningSettings {
        let configuration = ChatESPConfiguration(
            chatEndpoint: "https://openrouter.ai/api/v1",
            chatModel: "~deepseek/deepseek-v4-flash-latest",
            transcriptionModel: "openai/whisper-large-v3-turbo",
            speechModel: "google/gemini-3.1-flash-tts-preview",
            approximateLocation: "Dubai, United Arab Emirates")
        let secrets = ProvisioningSecretValues(
            openRouterKey: "OPENROUTER_TOKEN_PLACEHOLDER",
            braveKey: "BRAVE_TOKEN_PLACEHOLDER",
            wifiSSID: "Test Network",
            wifiPassword: fixturePassword)
        return ProvisioningSettings(
            configuration: configuration,
            secrets: secrets)
    }

    private func acknowledgement(
        status: ProvisioningStatus,
        revision: UInt32,
        fingerprint: Data,
        flags: UInt16 = 0
    ) throws -> ProvisioningAcknowledgement {
        var bytes = Data("CESA".utf8)
        bytes.append(ProvisioningProtocolV2.version)
        bytes.append(status.rawValue)
        bytes.appendBigEndian(flags)
        bytes.appendBigEndian(revision)
        bytes.append(fingerprint)
        return try ProvisioningAcknowledgement(data: bytes)
    }

    @MainActor
    func testUnavailableKeychainCannotCreateAnEmptySettingsPacket() throws {
        let suite = "org.chatesp.tests.\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let preferencesStore = PreferencesStore(defaults: defaults)
        let deviceID = UUID()
        var preferences = AppPreferences()
        preferences.devices = [
            ChatESPDeviceRecord(id: deviceID, name: "Desk")
        ]
        preferences.activeDeviceIdentifier = deviceID
        try preferencesStore.save(preferences)
        let secretsStore = TestSecretsStore(
            loadResult: .failure(TestSecretsError.unavailable))
        let store = ConfigurationStore(
            preferencesStore: preferencesStore,
            keychainStore: secretsStore)

        XCTAssertFalse(store.secretsAvailable)
        XCTAssertNil(store.settings(for: deviceID))
        XCTAssertThrowsError(try store.makePacket(for: deviceID)) { error in
            XCTAssertEqual(error as? ProvisioningError, .secretsUnavailable)
        }
        XCTAssertNil(
            store.preferences.device(id: deviceID)?.provisioning.pendingRevision)
        XCTAssertTrue(secretsStore.saved.isEmpty)

        secretsStore.loadResult = .success(ProvisioningSecrets())
        store.reloadSecretsIfNeeded()

        XCTAssertTrue(store.secretsAvailable)
        XCTAssertNotNil(store.settings(for: deviceID))
        XCTAssertNoThrow(try store.makePacket(for: deviceID))
    }

    @MainActor
    func testOversizedPacketDoesNotSaveAPendingRevision() throws {
        let suite = "org.chatesp.tests.\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let preferencesStore = PreferencesStore(defaults: defaults)
        let deviceID = UUID()
        var preferences = AppPreferences()
        preferences.global = ChatESPConfiguration(
            chatEndpoint: "https://a." + String(repeating: "b", count: 182),
            chatModel: String(repeating: "a", count: 96),
            transcriptionModel: String(repeating: "b", count: 96),
            speechModel: String(repeating: "c", count: 96),
            approximateLocation: String(repeating: "d", count: 96))
        preferences.devices = [
            ChatESPDeviceRecord(id: deviceID, name: "Desk")
        ]
        preferences.activeDeviceIdentifier = deviceID
        try preferencesStore.save(preferences)
        let secretsStore = TestSecretsStore(
            loadResult: .success(
                ProvisioningSecrets(
                    global: ProvisioningSecretValues(
                        openRouterKey: String(repeating: "e", count: 256),
                        braveKey: String(repeating: "f", count: 128),
                        wifiSSID: String(repeating: "g", count: 32),
                        wifiPassword: String(repeating: "h", count: 63)))))
        let store = ConfigurationStore(
            preferencesStore: preferencesStore,
            keychainStore: secretsStore)

        XCTAssertThrowsError(try store.makePacket(for: deviceID)) { error in
            XCTAssertEqual(error as? ProvisioningError, .packetTooLarge)
        }
        XCTAssertNil(
            store.preferences.device(id: deviceID)?.provisioning.pendingRevision)
        XCTAssertNil(
            preferencesStore.load().device(id: deviceID)?
                .provisioning.pendingRevision)
    }

    func testGoldenPacketMatchesFirmwareVector() throws {
        let packet = try goldenSettings().packet(revision: 7)
        XCTAssertEqual(packet.data.count, 311)
        XCTAssertEqual(
            packet.fingerprint.hexString,
            "3dd4441effe5f36c424310634670e91ea4dd936e958b38c21db1449a5fda98ce")
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
        XCTAssertEqual(transfer.dataFrames[1].count, 145)
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

    func testRevisionRecoveryRequiresFlaggedActiveVersion() throws {
        let activeFingerprint = Data(repeating: 0x5a, count: 32)
        let recovery = try acknowledgement(
            status: .staleRevision,
            revision: 7,
            fingerprint: activeFingerprint,
            flags: ProvisioningProtocolV2.activeVersionAcknowledgementFlag)
        XCTAssertTrue(recovery.isRevisionRecovery)

        let legacyError = try acknowledgement(
            status: .staleRevision,
            revision: 1,
            fingerprint: activeFingerprint)
        XCTAssertFalse(legacyError.isRevisionRecovery)

        XCTAssertThrowsError(try acknowledgement(
            status: .staleRevision,
            revision: 0,
            fingerprint: activeFingerprint,
            flags: ProvisioningProtocolV2.activeVersionAcknowledgementFlag))
        XCTAssertThrowsError(try acknowledgement(
            status: .storageFailure,
            revision: 7,
            fingerprint: activeFingerprint,
            flags: ProvisioningProtocolV2.activeVersionAcknowledgementFlag))
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
        var configuration = ChatESPConfiguration()
        let validSecrets = ProvisioningSecretValues(
            openRouterKey: "OPENROUTER_TOKEN_PLACEHOLDER",
            braveKey: "",
            wifiSSID: "Test Network",
            wifiPassword: fixturePassword)

        configuration.chatEndpoint = "http://example.com"
        XCTAssertThrowsError(
            try ProvisioningSettings(configuration: configuration, secrets: validSecrets).packet(revision: 1))

        configuration = ChatESPConfiguration()
        configuration.chatModel = "model with spaces"
        XCTAssertThrowsError(
            try ProvisioningSettings(configuration: configuration, secrets: validSecrets).packet(revision: 1))

        configuration = ChatESPConfiguration()
        configuration.approximateLocation = String(repeating: "L", count: 97)
        XCTAssertThrowsError(
            try ProvisioningSettings(configuration: configuration, secrets: validSecrets).packet(revision: 1))

        configuration = ChatESPConfiguration()
        configuration.approximateLocation = "Dubai\nUnited Arab Emirates"
        XCTAssertThrowsError(
            try ProvisioningSettings(configuration: configuration, secrets: validSecrets).packet(revision: 1))

        var invalidSecrets = validSecrets
        invalidSecrets.openRouterKey = "short"
        XCTAssertThrowsError(
            try ProvisioningSettings(configuration: configuration, secrets: invalidSecrets).packet(revision: 1))

        invalidSecrets = validSecrets
        invalidSecrets.wifiSSID = String(repeating: "S", count: 33)
        XCTAssertThrowsError(
            try ProvisioningSettings(configuration: configuration, secrets: invalidSecrets).packet(revision: 1))

        invalidSecrets = validSecrets
        invalidSecrets.wifiPassword = "short"
        XCTAssertThrowsError(
            try ProvisioningSettings(configuration: configuration, secrets: invalidSecrets).packet(revision: 1))
    }

    func testRevisionStaysStableUntilMatchingAcknowledgement() throws {
        let fingerprint = try goldenSettings().contentFingerprint()
        var state = ProvisioningVersionState()
        XCTAssertEqual(try state.revision(for: fingerprint), 1)
        XCTAssertEqual(try state.revision(for: fingerprint), 1)

        var other = fingerprint
        other[0] ^= 1
        XCTAssertEqual(try state.revision(for: other), 2)
        XCTAssertThrowsError(try state.acknowledge(revision: 1, fingerprint: fingerprint))
        try state.acknowledge(revision: 2, fingerprint: other)
        XCTAssertEqual(state.appliedRevision, 2)
        XCTAssertEqual(try state.revision(for: other), 2)
        try state.acknowledge(revision: 2, fingerprint: other)
        XCTAssertEqual(try state.revision(for: fingerprint), 3)
    }

    func testRevisionRecoveryHandlesMatchingAndChangedContent() throws {
        let current = try goldenSettings().contentFingerprint()
        let different = Data(repeating: 0x5a, count: 32)

        var matching = ProvisioningVersionState()
        _ = try matching.revision(for: current)
        try matching.recoverActiveVersion(revision: 7, fingerprint: current)
        XCTAssertEqual(try matching.revision(for: current), 7)

        var changed = ProvisioningVersionState()
        _ = try changed.revision(for: current)
        try changed.recoverActiveVersion(revision: 7, fingerprint: different)
        XCTAssertNil(changed.pendingRevision)
        XCTAssertEqual(try changed.revision(for: current), 8)
    }

    func testPreferencesUseOneVersionedRecord() throws {
        let suite = "org.chatesp.tests.\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let store = PreferencesStore(defaults: defaults)
        let deviceID = UUID()
        var preferences = AppPreferences()
        preferences.global.chatModel = "example/model"
        preferences.devices = [ChatESPDeviceRecord(id: deviceID, name: "Office")]
        preferences.activeDeviceIdentifier = deviceID
        try store.save(preferences)
        let record = defaults.persistentDomain(forName: suite)
        XCTAssertEqual(record?.keys.count, 1)
        XCTAssertEqual(store.load(), preferences)
    }

    func testPreferencesSaveAnIncompleteEditWithoutPacketValidation() throws {
        let suite = "org.chatesp.tests.\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let store = PreferencesStore(defaults: defaults)
        var preferences = AppPreferences()
        preferences.global.chatEndpoint = "h"
        try store.save(preferences)
        XCTAssertEqual(store.load().global.chatEndpoint, "h")
    }

    @MainActor
    func testNewDeviceUsesTheChatESPDefaultName() throws {
        let suite = "org.chatesp.tests.\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let store = ConfigurationStore(
            preferencesStore: PreferencesStore(defaults: defaults),
            keychainStore: TestSecretsStore(
                loadResult: .success(ProvisioningSecrets())))
        let deviceID = UUID()

        store.addDevice(id: deviceID)

        XCTAssertEqual(store.preferences.device(id: deviceID)?.name, "ChatESP")
        XCTAssertEqual(store.activeDeviceIdentifier, deviceID)
    }

    func testPreferencesMigrateTheLegacySelectedDevice() throws {
        let suite = "org.chatesp.tests.\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let store = PreferencesStore(defaults: defaults)
        let recordKey = "org.chatesp.preferences.record"
        let deviceID = UUID()
        let legacy: [String: Any] = [
            "formatVersion": 1,
            "chatEndpoint": "https://openrouter.ai/api/v1",
            "chatModel": "example/chat",
            "transcriptionModel": "example/transcription",
            "speechModel": "example/speech",
            "appliedRevision": 4,
            "selectedWatchIdentifier": deviceID.uuidString,
        ]
        defaults.set(try JSONSerialization.data(withJSONObject: legacy), forKey: recordKey)

        let migrated = store.load()
        XCTAssertEqual(migrated.formatVersion, 2)
        XCTAssertEqual(migrated.activeDeviceIdentifier, deviceID)
        XCTAssertEqual(migrated.devices.first?.provisioning.appliedRevision, 4)
        XCTAssertEqual(migrated.global.chatModel, "example/chat")
    }

    func testDeviceOverridesInheritGlobalValues() throws {
        let deviceID = UUID()
        var preferences = AppPreferences()
        preferences.global.chatModel = "global/chat"
        preferences.global.approximateLocation = "Dubai, UAE"
        preferences.devices = [
            ChatESPDeviceRecord(
                id: deviceID,
                name: "Desk",
                overrides: ChatESPConfigurationOverrides(
                    speechModel: "device/speech",
                    approximateLocation: ""))
        ]
        let effective = try XCTUnwrap(
            preferences.effectiveConfiguration(for: deviceID))
        XCTAssertEqual(effective.chatModel, "global/chat")
        XCTAssertEqual(effective.speechModel, "device/speech")
        XCTAssertEqual(effective.approximateLocation, "")
    }

    func testSecretOverridesInheritAndCanDisableSearch() {
        let deviceID = UUID()
        let secrets = ProvisioningSecrets(
            global: ProvisioningSecretValues(
                openRouterKey: "global-key",
                braveKey: "global-search",
                wifiSSID: "Global Wi-Fi",
                wifiPassword: fixturePassword),
            deviceOverrides: [
                deviceID: ProvisioningSecretOverrides(braveKey: "")
            ])
        let effective = secrets.effectiveValues(for: deviceID)
        XCTAssertEqual(effective.openRouterKey, "global-key")
        XCTAssertEqual(effective.braveKey, "")
        XCTAssertEqual(effective.wifiSSID, "Global Wi-Fi")
    }

    func testLegacySecretsBecomeGlobalSecrets() throws {
        let legacy: [String: String] = [
            "openRouterKey": "old-router-key",
            "braveKey": "old-search-key",
            "wifiSSID": "Old Wi-Fi",
            "wifiPassword": fixturePassword,
        ]
        let data = try JSONSerialization.data(withJSONObject: legacy)
        let decoded = try JSONDecoder().decode(ProvisioningSecrets.self, from: data)
        XCTAssertEqual(decoded.global.openRouterKey, "old-router-key")
        XCTAssertEqual(decoded.global.wifiSSID, "Old Wi-Fi")
        XCTAssertTrue(decoded.deviceOverrides.isEmpty)
    }

    func testEmptyCredentialsCanBeSent() throws {
        let settings = ProvisioningSettings(
            configuration: ChatESPConfiguration(),
            secrets: ProvisioningSecretValues())
        XCTAssertTrue(settings.validationIssues.isEmpty)
        XCTAssertNoThrow(try settings.packet(revision: 1))
    }

    func testEmptyEndpointAndModelsUseSafeDefaults() throws {
        var configuration = ChatESPConfiguration()
        configuration.chatEndpoint = ""
        configuration.chatModel = ""
        configuration.transcriptionModel = ""
        configuration.speechModel = ""

        let settings = ProvisioningSettings(
            configuration: configuration,
            secrets: ProvisioningSecretValues())
        let defaults = ChatESPConfiguration()
        XCTAssertEqual(settings.chatEndpoint, defaults.chatEndpoint)
        XCTAssertEqual(settings.chatModel, defaults.chatModel)
        XCTAssertEqual(settings.transcriptionModel, defaults.transcriptionModel)
        XCTAssertEqual(settings.speechModel, defaults.speechModel)
        XCTAssertNoThrow(try settings.packet(revision: 1))
    }

    func testAutomaticSyncRequiresAReadyMatchingConnection() {
        let deviceID = UUID()
        let target = AutomaticSettingsSyncPolicy.target(
            deviceID: deviceID,
            settings: ProvisioningSettings(
                configuration: ChatESPConfiguration(),
                secrets: ProvisioningSecretValues()))
        XCTAssertNotNil(target)

        let ready = AutomaticSettingsSyncTrigger(
            target: target,
            selectedDeviceID: deviceID,
            connected: true,
            provisioning: false)
        XCTAssertTrue(AutomaticSettingsSyncPolicy.shouldStart(
            trigger: ready,
            lastAttempt: nil))
        XCTAssertFalse(AutomaticSettingsSyncPolicy.shouldStart(
            trigger: ready,
            lastAttempt: target))
        XCTAssertFalse(AutomaticSettingsSyncPolicy.shouldStart(
            trigger: AutomaticSettingsSyncTrigger(
                target: target,
                selectedDeviceID: UUID(),
                connected: true,
                provisioning: false),
            lastAttempt: nil))
        XCTAssertFalse(AutomaticSettingsSyncPolicy.shouldStart(
            trigger: AutomaticSettingsSyncTrigger(
                target: target,
                selectedDeviceID: deviceID,
                connected: false,
                provisioning: false),
            lastAttempt: nil))
        XCTAssertFalse(AutomaticSettingsSyncPolicy.shouldStart(
            trigger: AutomaticSettingsSyncTrigger(
                target: target,
                selectedDeviceID: deviceID,
                connected: true,
                provisioning: true),
            lastAttempt: nil))

        var invalidConfiguration = ChatESPConfiguration()
        invalidConfiguration.chatEndpoint = "h"
        XCTAssertNil(AutomaticSettingsSyncPolicy.target(
            deviceID: deviceID,
            settings: ProvisioningSettings(
                configuration: invalidConfiguration,
                secrets: ProvisioningSecretValues())))
    }

    func testModelCatalogFiltersRequiredCapabilities() throws {
        let chatJSON = """
        {
          "id": "example/chat",
          "name": "Chat",
          "description": "",
          "architecture": {
            "input_modalities": ["text"],
            "output_modalities": ["text"]
          },
          "supported_parameters": ["tools"]
        }
        """
        let transcriptionJSON = """
        {
          "id": "example/transcription",
          "name": "Transcription",
          "description": "",
          "architecture": {
            "input_modalities": ["audio"],
            "output_modalities": ["transcription"]
          },
          "supported_parameters": []
        }
        """
        let speechJSON = """
        {
          "id": "example/speech",
          "name": "Speech",
          "description": "",
          "architecture": {
            "input_modalities": ["text"],
            "output_modalities": ["speech"]
          },
          "supported_parameters": [],
          "supported_voices": ["af_heart", "ff_siwis"]
        }
        """
        let chat = try JSONDecoder().decode(
            OpenRouterModel.self,
            from: Data(chatJSON.utf8))
        let transcription = try JSONDecoder().decode(
            OpenRouterModel.self,
            from: Data(transcriptionJSON.utf8))
        let speech = try JSONDecoder().decode(
            OpenRouterModel.self,
            from: Data(speechJSON.utf8))
        XCTAssertTrue(chat.supports(.chat))
        XCTAssertTrue(transcription.supports(.transcription))
        XCTAssertTrue(speech.supports(.speech))
        XCTAssertFalse(chat.supports(.transcription))
        XCTAssertFalse(transcription.supports(.speech))
    }

    func testModelCatalogRequestsEveryOutputModalityWithoutAKey() throws {
        let request = try ModelCatalogClient.catalogRequest()
        XCTAssertEqual(
            request.url?.absoluteString,
            "https://openrouter.ai/api/v1/models?output_modalities=all")
        XCTAssertNil(request.value(forHTTPHeaderField: "Authorization"))
        XCTAssertEqual(
            request.value(forHTTPHeaderField: "Accept"),
            "application/json")
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
