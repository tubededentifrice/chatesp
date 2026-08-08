import SwiftUI

struct ConfigurationView: View {
    @EnvironmentObject private var store: ConfigurationStore
    @EnvironmentObject private var provisioner: BLEProvisioner
    @StateObject private var modelCatalog = ModelCatalog()
    @State private var showingDiscovery = false
    @State private var lastAutomaticSync: AutomaticSettingsSyncTarget?

    var body: some View {
        NavigationStack {
            List {
                Section("Global Settings") {
                    NavigationLink {
                        GlobalSettingsView(modelCatalog: modelCatalog)
                    } label: {
                        Label("Edit global settings", systemImage: "slider.horizontal.3")
                    }
                }

                Section("ChatESP Devices") {
                    if store.devices.isEmpty {
                        Text("No ChatESP devices are added.")
                            .foregroundStyle(.secondary)
                    }
                    ForEach(store.devices) { device in
                        NavigationLink {
                            DeviceSettingsView(
                                deviceID: device.id,
                                modelCatalog: modelCatalog)
                        } label: {
                            HStack(alignment: .firstTextBaseline) {
                                Text(device.name)
                                    .lineLimit(1)
                                Spacer()
                                Text(deviceStatus(device.id))
                                    .font(.caption)
                                    .multilineTextAlignment(.trailing)
                                    .foregroundStyle(deviceStatusColor(device.id))
                            }
                        }
                    }

                    Button("Add a new device", systemImage: "plus.circle") {
                        showingDiscovery = true
                    }
                    .disabled(provisioner.isProvisioning)
                }

                if let error = store.errorText {
                    Section("Needs Attention") {
                        Text(error).foregroundStyle(.red)
                    }
                }
            }
            .navigationTitle("ChatESP")
            .sheet(isPresented: $showingDiscovery) {
                DeviceDiscoveryView()
            }
            .onAppear {
                provisioner.onSelectedDeviceChanged = { identifier in
                    store.setActiveDevice(identifier)
                }
                provisioner.restoreSelectedDevice(
                    identifier: store.activeDeviceIdentifier)
            }
            .task(id: automaticSyncTrigger) {
                await synchronizeSettingsIfNeeded(automaticSyncTrigger)
            }
        }
    }

    private var automaticSyncTrigger: AutomaticSettingsSyncTrigger {
        AutomaticSettingsSyncTrigger(
            target: store.activeDeviceIdentifier.flatMap { deviceID in
                store.settings(for: deviceID).flatMap {
                    AutomaticSettingsSyncPolicy.target(
                        deviceID: deviceID,
                        settings: $0)
                }
            },
            selectedDeviceID: provisioner.selectedID,
            connected: provisioner.isDeviceConnected,
            provisioning: provisioner.isProvisioning)
    }

    private func synchronizeSettingsIfNeeded(
        _ trigger: AutomaticSettingsSyncTrigger
    ) async {
        guard AutomaticSettingsSyncPolicy.shouldStart(
            trigger: trigger,
            lastAttempt: lastAutomaticSync
        ) else {
            if !trigger.connected {
                lastAutomaticSync = nil
            }
            return
        }
        try? await Task.sleep(for: .milliseconds(350))
        guard !Task.isCancelled,
              trigger == automaticSyncTrigger,
              let target = trigger.target else {
            return
        }
        lastAutomaticSync = target
        synchronizeSettings(for: target.deviceID)
    }

    private func synchronizeSettings(for deviceID: UUID) {
        do {
            send(
                try store.makePacket(for: deviceID),
                deviceID: deviceID,
                canRecoverRevision: true)
        } catch {
            store.show(error)
        }
    }

    private func send(
        _ packet: ProvisioningPacket,
        deviceID: UUID,
        canRecoverRevision: Bool
    ) {
        provisioner.provision(packet: packet) { result in
            switch result {
            case .success(let acknowledgement):
                do {
                    if acknowledgement.isRevisionRecovery {
                        guard canRecoverRevision else {
                            throw ProvisioningError.acknowledgementMismatch
                        }
                        let recoveryPacket = try store.recoverRevision(
                            from: acknowledgement,
                            for: deviceID)
                        send(
                            recoveryPacket,
                            deviceID: deviceID,
                            canRecoverRevision: false)
                    } else {
                        try store.accept(acknowledgement, for: deviceID)
                    }
                } catch {
                    store.show(error)
                }
            case .failure(let error):
                store.show(error)
            }
        }
    }

    private func deviceStatus(_ id: UUID) -> String {
        guard provisioner.selectedID == id else { return "Not connected" }
        switch provisioner.phase {
        case .failed(let message):
            return message
        case .pairing, .transferring, .waitingForConfirmation:
            return provisioner.phase.text
        case .scanning, .connecting:
            return provisioner.phase.text
        default:
            return provisioner.isDeviceConnected
                ? "Connected"
                : "Connecting"
        }
    }

    private func deviceStatusColor(_ id: UUID) -> Color {
        guard provisioner.selectedID == id else { return .secondary }
        if case .failed = provisioner.phase { return .red }
        return provisioner.isDeviceConnected ? .green : .secondary
    }
}

private struct DeviceDiscoveryView: View {
    @EnvironmentObject private var store: ConfigurationStore
    @EnvironmentObject private var provisioner: BLEProvisioner
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List {
                if provisioner.discoveredDevices.isEmpty {
                    HStack {
                        if provisioner.phase == .scanning {
                            ProgressView()
                        }
                        Text(
                            provisioner.phase == .scanning
                                ? "Looking for ChatESP devices"
                                : "No ChatESP devices found")
                            .foregroundStyle(.secondary)
                    }
                }
                ForEach(provisioner.discoveredDevices) { device in
                    Button {
                        store.addDevice(id: device.id)
                        provisioner.select(device)
                        dismiss()
                    } label: {
                        HStack {
                            Text(device.name)
                            Spacer()
                            if store.devices.contains(where: { $0.id == device.id }) {
                                Text("Added")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }
                }
            }
            .navigationTitle("Add a new device")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { dismiss() }
                }
            }
            .onAppear { provisioner.scan() }
        }
    }
}

private struct GlobalSettingsView: View {
    @EnvironmentObject private var store: ConfigurationStore
    @ObservedObject var modelCatalog: ModelCatalog

    var body: some View {
        Form {
            ConnectionSettings(
                configuration: globalConfigurationBinding,
                secrets: globalSecretsBinding)
            LocationSettings(configuration: globalConfigurationBinding)
            ModelSettings(
                configuration: globalConfigurationBinding,
                modelCatalog: modelCatalog)
            if let error = store.errorText {
                Section("Needs Attention") {
                    Text(error).foregroundStyle(.red)
                }
            }
        }
        .navigationTitle("Global Settings")
        .navigationBarTitleDisplayMode(.inline)
    }

    private var globalConfigurationBinding: Binding<ChatESPConfiguration> {
        Binding(
            get: { store.preferences.global },
            set: { newValue in
                store.updateGlobalConfiguration { $0 = newValue }
            })
    }

    private var globalSecretsBinding: Binding<ProvisioningSecretValues> {
        Binding(
            get: { store.secrets.global },
            set: { newValue in
                store.updateGlobalSecrets { $0 = newValue }
            })
    }
}

private struct DeviceSettingsView: View {
    @EnvironmentObject private var store: ConfigurationStore
    @EnvironmentObject private var provisioner: BLEProvisioner
    @Environment(\.dismiss) private var dismiss
    let deviceID: UUID
    @ObservedObject var modelCatalog: ModelCatalog
    @State private var newMemory = ""
    @State private var confirmClearMemories = false
    @State private var confirmRemove = false

    var body: some View {
        Form {
            deviceSection
            DeviceConnectionOverrides(deviceID: deviceID)
            DeviceLocationOverride(deviceID: deviceID)
            DeviceModelOverrides(
                deviceID: deviceID,
                modelCatalog: modelCatalog)
            memorySection
            statusSection
            removeSection
        }
        .navigationTitle(device?.name ?? "ChatESP Device")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear {
            store.setActiveDevice(deviceID)
            provisioner.restoreSelectedDevice(identifier: deviceID)
        }
        .alert("Clear all memories?", isPresented: $confirmClearMemories) {
            Button("Cancel", role: .cancel) {}
            Button("Clear All", role: .destructive) {
                provisioner.clearAllMemories()
            }
        } message: {
            Text("This deletes all saved facts from this ChatESP device.")
        }
    }

    private var device: ChatESPDeviceRecord? {
        store.preferences.device(id: deviceID)
    }

    private var deviceSection: some View {
        Section("Device") {
            TextField(
                "Name",
                text: Binding(
                    get: { device?.name ?? "ChatESP" },
                    set: { store.setDeviceName(id: deviceID, name: $0) }))
            LabeledContent(
                "Connection",
                value: provisioner.selectedID == deviceID &&
                    provisioner.isDeviceConnected ? "Connected" : "Not connected")
        }
    }

    private var statusSection: some View {
        let issues = store.validationIssues(for: deviceID)
        let secrets = store.secretValues(for: deviceID)
        let configuration = store.configuration(for: deviceID)
        return Section("Status") {
            Text(deviceStatusText)
                .foregroundStyle(.secondary)
            if !issues.isEmpty {
                ForEach(issues) { issue in
                    Label(issue.message, systemImage: "exclamationmark.circle")
                        .foregroundStyle(.orange)
                }
            }
            if secrets.wifiSSID.isEmpty {
                Text("Wi-Fi is not configured. ChatESP will show an error when a voice request needs it.")
                    .foregroundStyle(.secondary)
            }
            if secrets.openRouterKey.isEmpty {
                Text("The OpenRouter key is not configured. ChatESP will show an error when a cloud request needs it.")
                    .foregroundStyle(.secondary)
            }
            if secrets.braveKey.isEmpty {
                Text("Web and image search are off because the optional Brave key is empty.")
                    .foregroundStyle(.secondary)
            }
            if configuration.map({
                $0.chatEndpoint.isEmpty || $0.chatModel.isEmpty ||
                    $0.transcriptionModel.isEmpty || $0.speechModel.isEmpty ||
                    $0.englishSpeechVoice.isEmpty || $0.frenchSpeechVoice.isEmpty
            }) == true {
                Text("Empty endpoint, model, or voice values use the built-in defaults.")
                    .foregroundStyle(.secondary)
            }
            if let error = store.errorText {
                Text(error).foregroundStyle(.red)
            }
        }
    }

    private var deviceStatusText: String {
        guard provisioner.selectedID == deviceID else {
            return "This ChatESP device is not connected. Changes are saved and will sync after connection."
        }
        guard provisioner.isDeviceConnected else {
            return provisioner.phase.text
        }
        switch provisioner.phase {
        case .idle:
            return "Connected. Settings sync automatically."
        default:
            return provisioner.phase.text
        }
    }

    private var memorySection: some View {
        Section {
            if provisioner.selectedID != deviceID || !provisioner.isDeviceConnected {
                Text(provisioner.memoryMessage)
                    .foregroundStyle(.secondary)
            } else if !provisioner.memoryAvailable {
                Text(provisioner.memoryMessage)
                    .foregroundStyle(.secondary)
            } else {
                HStack {
                    TextField("One concise fact", text: $newMemory)
                        .textInputAutocapitalization(.sentences)
                    Button("Add") {
                        provisioner.addMemory(newMemory)
                        newMemory = ""
                    }
                    .disabled(!MemoryProtocolV1.validFact(newMemory))
                }
                ForEach(provisioner.memories) { memory in
                    HStack(alignment: .top) {
                        Text(memory.fact)
                        Spacer()
                        Button(role: .destructive) {
                            provisioner.deleteMemory(id: memory.id)
                        } label: {
                            Image(systemName: "trash")
                        }
                        .accessibilityLabel("Delete memory \(memory.id)")
                    }
                }
                HStack {
                    Button("Refresh") { provisioner.refreshMemories() }
                    Spacer()
                    Button("Clear All", role: .destructive) {
                        confirmClearMemories = true
                    }
                    .disabled(provisioner.memories.isEmpty)
                }
                Text(provisioner.memoryMessage)
                    .foregroundStyle(.secondary)
            }
        } header: {
            Text("Memories")
        } footer: {
            Text("Facts are stored in plaintext on the ChatESP device. It sends them to the configured chat model with each request.")
        }
    }

    private var removeSection: some View {
        Section {
            Button("Remove ChatESP Device", role: .destructive) {
                confirmRemove = true
            }
            .confirmationDialog(
                "Remove this ChatESP device?",
                isPresented: $confirmRemove,
                titleVisibility: .visible
            ) {
                Button("Remove ChatESP Device", role: .destructive) {
                    if provisioner.selectedID == deviceID {
                        provisioner.forgetSelectedDevice()
                    }
                    store.removeDevice(id: deviceID)
                    dismiss()
                }
            } message: {
                Text("The global settings stay available. You can add this ChatESP device again later.")
            }
        }
    }

}

private struct ConnectionSettings: View {
    @Binding var configuration: ChatESPConfiguration
    @Binding var secrets: ProvisioningSecretValues

    var body: some View {
        Section("Connection") {
            TextField("Chat endpoint", text: $configuration.chatEndpoint)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .keyboardType(.URL)
            SecureField("OpenRouter key", text: $secrets.openRouterKey)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            SecureField("Brave key (optional)", text: $secrets.braveKey)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            TextField("Wi-Fi network", text: $secrets.wifiSSID)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            SecureField("Wi-Fi password", text: $secrets.wifiPassword)
        }
    }
}

private struct LocationSettings: View {
    @Binding var configuration: ChatESPConfiguration

    var body: some View {
        Section {
            TextField(
                "Fallback city and country (optional)",
                text: $configuration.approximateLocation)
                .textInputAutocapitalization(.words)
                .autocorrectionDisabled()
        } header: {
            Text("Location")
        } footer: {
            Text("The app sends the current time and a rounded location after connection and at most once per hour. This city-level value is the fallback. The app does not send a precise position.")
        }
    }
}

private struct ModelSettings: View {
    @Binding var configuration: ChatESPConfiguration
    @ObservedObject var modelCatalog: ModelCatalog

    var body: some View {
        Section {
            modelRow(.chat, selection: $configuration.chatModel)
            modelRow(.transcription, selection: $configuration.transcriptionModel)
            modelRow(.speech, selection: $configuration.speechModel)
            voiceRow(.english, selection: $configuration.englishSpeechVoice)
            voiceRow(.french, selection: $configuration.frenchSpeechVoice)
        } header: {
            Text("Models")
        } footer: {
            Text("The browser shows only models that declare the required input and output types. The chat list also requires tool calling.")
        }
    }

    private func modelRow(
        _ purpose: ModelPurpose,
        selection: Binding<String>
    ) -> some View {
        NavigationLink {
            ModelBrowserView(
                purpose: purpose,
                selection: selection,
                catalog: modelCatalog,
                onSelect: purpose == .speech ? { model in
                    configuration.englishSpeechVoice = model.preferredVoice(
                        for: .english,
                        preserving: configuration.englishSpeechVoice) ?? ""
                    configuration.frenchSpeechVoice = model.preferredVoice(
                        for: .french,
                        preserving: configuration.frenchSpeechVoice) ?? ""
                } : nil)
        } label: {
            VStack(alignment: .leading, spacing: 3) {
                Text(purpose.title)
                Text(purpose.explanation)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text(selection.wrappedValue)
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
            }
        }
    }

    private func voiceRow(
        _ language: SpeechVoiceLanguage,
        selection: Binding<String>
    ) -> some View {
        NavigationLink {
            VoiceBrowserView(
                language: language,
                modelID: configuration.speechModel,
                selection: selection,
                catalog: modelCatalog)
        } label: {
            LabeledContent(language.title) {
                Text(selection.wrappedValue)
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
            }
        }
    }
}

private struct DeviceConnectionOverrides: View {
    @EnvironmentObject private var store: ConfigurationStore
    let deviceID: UUID

    var body: some View {
        OverrideTextField(
            title: "Chat endpoint",
            globalValue: store.preferences.global.chatEndpoint,
            value: configuration(\.chatEndpoint),
            keyboard: .URL)
        OverrideTextField(
            title: "OpenRouter key",
            globalValue: store.secrets.global.openRouterKey,
            value: secret(\.openRouterKey),
            secure: true)
        OverrideTextField(
            title: "Brave key",
            globalValue: store.secrets.global.braveKey,
            value: secret(\.braveKey),
            secure: true)
        OverrideTextField(
            title: "Wi-Fi network",
            globalValue: store.secrets.global.wifiSSID,
            value: secret(\.wifiSSID))
        OverrideTextField(
            title: "Wi-Fi password",
            globalValue: store.secrets.global.wifiPassword,
            value: secret(\.wifiPassword),
            secure: true)
    }

    private func configuration(
        _ keyPath: WritableKeyPath<ChatESPConfigurationOverrides, String?>
    ) -> Binding<String?> {
        Binding(
            get: { store.preferences.device(id: deviceID)?.overrides[keyPath: keyPath] },
            set: { value in
                store.updateDeviceOverrides(id: deviceID) {
                    $0[keyPath: keyPath] = value
                }
            })
    }

    private func secret(
        _ keyPath: WritableKeyPath<ProvisioningSecretOverrides, String?>
    ) -> Binding<String?> {
        Binding(
            get: { store.secrets.deviceOverrides[deviceID]?[keyPath: keyPath] },
            set: { value in
                store.updateDeviceSecretOverrides(id: deviceID) {
                    $0[keyPath: keyPath] = value
                }
            })
    }
}

private struct DeviceLocationOverride: View {
    @EnvironmentObject private var store: ConfigurationStore
    let deviceID: UUID

    var body: some View {
        OverrideTextField(
            title: "Fallback city and country",
            globalValue: store.preferences.global.approximateLocation,
            value: Binding(
                get: {
                    store.preferences.device(id: deviceID)?.overrides
                        .approximateLocation
                },
                set: { value in
                    store.updateDeviceOverrides(id: deviceID) {
                        $0.approximateLocation = value
                    }
                }))
    }
}

private struct DeviceModelOverrides: View {
    @EnvironmentObject private var store: ConfigurationStore
    let deviceID: UUID
    @ObservedObject var modelCatalog: ModelCatalog

    var body: some View {
        Section {
            modelOverride(.chat, keyPath: \.chatModel)
            modelOverride(.transcription, keyPath: \.transcriptionModel)
            modelOverride(.speech, keyPath: \.speechModel)
            voiceOverride(.english, keyPath: \.englishSpeechVoice)
            voiceOverride(.french, keyPath: \.frenchSpeechVoice)
        } header: {
            Text("Model Overrides")
        } footer: {
            Text("Turn off an override to use the related global model.")
        }
    }

    private func modelOverride(
        _ purpose: ModelPurpose,
        keyPath: WritableKeyPath<ChatESPConfigurationOverrides, String?>
    ) -> some View {
        let global = globalModel(purpose)
        let override = Binding<String?>(
            get: {
                store.preferences.device(id: deviceID)?.overrides[keyPath: keyPath]
            },
            set: { value in
                store.updateDeviceOverrides(id: deviceID) {
                    $0[keyPath: keyPath] = value
                }
            })
        return VStack(alignment: .leading, spacing: 8) {
            Toggle(
                "Override \(purpose.title)",
                isOn: Binding(
                    get: { override.wrappedValue != nil },
                    set: { enabled in
                        override.wrappedValue = enabled ? global : nil
                    }))
            if override.wrappedValue != nil {
                NavigationLink {
                    ModelBrowserView(
                        purpose: purpose,
                        selection: Binding(
                            get: { override.wrappedValue ?? global },
                            set: { override.wrappedValue = $0 }),
                        catalog: modelCatalog,
                        onSelect: purpose == .speech ? updateSpeechVoices : nil)
                } label: {
                    Text(override.wrappedValue ?? global)
                        .font(.caption.monospaced())
                }
            } else {
                Text("Global: \(global)")
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
            }
            Text(purpose.explanation)
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    private func voiceOverride(
        _ language: SpeechVoiceLanguage,
        keyPath: WritableKeyPath<ChatESPConfigurationOverrides, String?>
    ) -> some View {
        let global = globalVoice(language)
        let override = Binding<String?>(
            get: {
                store.preferences.device(id: deviceID)?.overrides[keyPath: keyPath]
            },
            set: { value in
                store.updateDeviceOverrides(id: deviceID) {
                    $0[keyPath: keyPath] = value
                }
            })
        return VStack(alignment: .leading, spacing: 8) {
            Toggle(
                "Override \(language.title)",
                isOn: Binding(
                    get: { override.wrappedValue != nil },
                    set: { enabled in
                        override.wrappedValue = enabled ? global : nil
                    }))
            if override.wrappedValue != nil {
                NavigationLink {
                    VoiceBrowserView(
                        language: language,
                        modelID: effectiveSpeechModel,
                        selection: Binding(
                            get: { override.wrappedValue ?? global },
                            set: { override.wrappedValue = $0 }),
                        catalog: modelCatalog)
                } label: {
                    Text(override.wrappedValue ?? global)
                        .font(.caption.monospaced())
                }
            } else {
                Text("Global: \(global)")
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
            }
        }
    }

    private var effectiveSpeechModel: String {
        store.preferences.device(id: deviceID)?.overrides.speechModel ??
            store.preferences.global.speechModel
    }

    private func globalVoice(_ language: SpeechVoiceLanguage) -> String {
        switch language {
        case .english: return store.preferences.global.englishSpeechVoice
        case .french: return store.preferences.global.frenchSpeechVoice
        }
    }

    private func updateSpeechVoices(_ model: OpenRouterModel) {
        let global = store.preferences.global
        store.updateDeviceOverrides(id: deviceID) { overrides in
            let english = overrides.englishSpeechVoice ?? global.englishSpeechVoice
            let french = overrides.frenchSpeechVoice ?? global.frenchSpeechVoice
            if let selected = model.preferredVoice(
                for: .english, preserving: english), selected != english {
                overrides.englishSpeechVoice = selected
            }
            if let selected = model.preferredVoice(
                for: .french, preserving: french), selected != french {
                overrides.frenchSpeechVoice = selected
            }
        }
    }

    private func globalModel(_ purpose: ModelPurpose) -> String {
        switch purpose {
        case .chat: return store.preferences.global.chatModel
        case .transcription: return store.preferences.global.transcriptionModel
        case .speech: return store.preferences.global.speechModel
        }
    }
}

private struct OverrideTextField: View {
    let title: String
    let globalValue: String
    @Binding var value: String?
    var secure = false
    var keyboard: UIKeyboardType = .default

    var body: some View {
        Section {
            Toggle(
                "Override \(title)",
                isOn: Binding(
                    get: { value != nil },
                    set: { value = $0 ? globalValue : nil }))
            if value != nil {
                if secure {
                    SecureField(title, text: effectiveBinding)
                } else {
                    TextField(title, text: effectiveBinding)
                        .keyboardType(keyboard)
                }
            } else {
                LabeledContent("Global value", value: redactedGlobalValue)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private var effectiveBinding: Binding<String> {
        Binding(
            get: { value ?? globalValue },
            set: { value = $0 })
    }

    private var redactedGlobalValue: String {
        guard secure, !globalValue.isEmpty else {
            return globalValue.isEmpty ? "Empty" : globalValue
        }
        return "Saved in Keychain"
    }
}

private struct ModelBrowserView: View {
    let purpose: ModelPurpose
    @Binding var selection: String
    @ObservedObject var catalog: ModelCatalog
    let onSelect: ((OpenRouterModel) -> Void)?
    @Environment(\.dismiss) private var dismiss
    @State private var searchText = ""

    init(
        purpose: ModelPurpose,
        selection: Binding<String>,
        catalog: ModelCatalog,
        onSelect: ((OpenRouterModel) -> Void)? = nil
    ) {
        self.purpose = purpose
        _selection = selection
        self.catalog = catalog
        self.onSelect = onSelect
    }

    var body: some View {
        List {
            Section {
                Text(purpose.explanation)
                    .font(.footnote)
            }
            if catalog.isLoading {
                Section {
                    HStack {
                        ProgressView()
                        Text("Loading compatible models…")
                    }
                }
            } else if let error = catalog.errorText {
                Section("Could Not Load Models") {
                    Text(error).foregroundStyle(.red)
                    Button("Try Again") {
                        Task { await catalog.load(force: true) }
                    }
                }
            } else {
                if !currentSelectionIsListed {
                    Section("Current Selection") {
                        Text(selection).font(.body.monospaced())
                        Text("OpenRouter did not list this ID as compatible. Select a compatible model below.")
                            .font(.caption)
                            .foregroundStyle(.orange)
                    }
                }
                Section("Compatible Models") {
                    if filteredModels.isEmpty {
                        Text("No compatible model matches this search.")
                            .foregroundStyle(.secondary)
                    }
                    ForEach(filteredModels) { model in
                        Button {
                            selection = model.id
                            onSelect?(model)
                            dismiss()
                        } label: {
                            VStack(alignment: .leading, spacing: 4) {
                                HStack {
                                    Text(model.name)
                                    Spacer()
                                    if model.id == selection {
                                        Image(systemName: "checkmark.circle.fill")
                                            .foregroundStyle(.green)
                                    }
                                }
                                Text(model.id)
                                    .font(.caption.monospaced())
                                    .foregroundStyle(.secondary)
                                Text(model.pricingSummary)
                                    .font(.caption)
                                    .foregroundStyle(.blue)
                                if purpose == .speech {
                                    Text("\(model.supportedVoices.count) voices")
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                }
                                if !model.description.isEmpty {
                                    Text(model.description)
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                        .lineLimit(3)
                                }
                            }
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
        }
        .navigationTitle(purpose.title)
        .navigationBarTitleDisplayMode(.inline)
        .searchable(text: $searchText, prompt: "Search compatible models")
        .task { await catalog.load(force: true) }
    }

    private var compatibleModels: [OpenRouterModel] {
        catalog.models.filter { $0.supports(purpose) }
            .sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }

    private var filteredModels: [OpenRouterModel] {
        guard !searchText.isEmpty else { return compatibleModels }
        return compatibleModels.filter {
            $0.name.localizedCaseInsensitiveContains(searchText) ||
                $0.id.localizedCaseInsensitiveContains(searchText)
        }
    }

    private var currentSelectionIsListed: Bool {
        compatibleModels.contains { $0.id == selection }
    }
}

private struct VoiceBrowserView: View {
    let language: SpeechVoiceLanguage
    let modelID: String
    @Binding var selection: String
    @ObservedObject var catalog: ModelCatalog
    @Environment(\.dismiss) private var dismiss
    @State private var searchText = ""

    var body: some View {
        List {
            Section("Text-to-Speech Model") {
                Text(modelID).font(.caption.monospaced())
                if let model {
                    Text(model.pricingSummary)
                        .font(.caption)
                        .foregroundStyle(.blue)
                }
            }
            if catalog.isLoading && model == nil {
                Section {
                    HStack {
                        ProgressView()
                        Text("Loading voices…")
                    }
                }
            } else if let error = catalog.errorText, model == nil {
                Section("Could Not Load Voices") {
                    Text(error).foregroundStyle(.red)
                    Button("Try Again") {
                        Task { await catalog.load(force: true) }
                    }
                }
            } else if let model {
                if !model.supportedVoices.contains(selection) {
                    Section("Current Selection") {
                        Text(selection).font(.body.monospaced())
                        Text("This model does not list this voice. Select a voice below.")
                            .font(.caption)
                            .foregroundStyle(.orange)
                    }
                }
                Section("Compatible Voices") {
                    if filteredVoices.isEmpty {
                        Text("No compatible voice matches this search.")
                            .foregroundStyle(.secondary)
                    }
                    ForEach(filteredVoices, id: \.self) { voice in
                        Button {
                            selection = voice
                            dismiss()
                        } label: {
                            HStack {
                                Text(voice).font(.body.monospaced())
                                Spacer()
                                if voice == selection {
                                    Image(systemName: "checkmark.circle.fill")
                                        .foregroundStyle(.green)
                                }
                            }
                        }
                        .buttonStyle(.plain)
                    }
                }
            } else {
                Section("Voices Not Available") {
                    Text("OpenRouter does not list this model or its voices. Select a text-to-speech model first.")
                        .foregroundStyle(.secondary)
                }
            }
        }
        .navigationTitle(language.title)
        .navigationBarTitleDisplayMode(.inline)
        .searchable(text: $searchText, prompt: "Search voices")
        .task { await catalog.load() }
    }

    private var model: OpenRouterModel? {
        catalog.models.first { $0.id == modelID }
    }

    private var filteredVoices: [String] {
        guard let voices = model?.supportedVoices else { return [] }
        let sorted = voices.sorted {
            $0.localizedStandardCompare($1) == .orderedAscending
        }
        guard !searchText.isEmpty else { return sorted }
        return sorted.filter { $0.localizedCaseInsensitiveContains(searchText) }
    }
}
