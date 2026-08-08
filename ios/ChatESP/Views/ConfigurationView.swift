import SwiftUI

struct ConfigurationView: View {
    @EnvironmentObject private var store: ConfigurationStore
    @EnvironmentObject private var provisioner: BLEProvisioner

    var body: some View {
        NavigationStack {
            Form {
                watchSection
                connectionSection
                locationSection
                modelSection
                actionSection
            }
            .navigationTitle("ChatESP")
        }
    }

    private var locationSection: some View {
        Section {
            TextField(
                "Location fallback (city, country)",
                text: Binding(
                    get: { store.preferences.approximateLocation ?? "" },
                    set: { value in
                        store.preferences.approximateLocation =
                            value.isEmpty ? nil : value
                    }))
                .textInputAutocapitalization(.words)
                .autocorrectionDisabled()
        } header: {
            Text("Location")
        } footer: {
            Text("After you select a watch, the app sends the current time and a rounded location when the watch connects and once per hour while connected. This optional city is the fallback. The app does not send a precise position.")
        }
    }

    private var watchSection: some View {
        Section("Watch") {
            Button("Find ChatESP") {
                store.saveEdits()
                provisioner.scan()
            }

            ForEach(provisioner.watches) { watch in
                Button {
                    provisioner.select(watch)
                } label: {
                    HStack {
                        Text(watch.name)
                        Spacer()
                        if provisioner.selectedID == watch.id {
                            Image(systemName: "checkmark.circle.fill")
                                .foregroundStyle(.green)
                        }
                    }
                }
            }

            Text(provisioner.phase.text)
                .foregroundStyle(.secondary)
        }
    }

    private var connectionSection: some View {
        Section("Connection") {
            TextField("Chat endpoint", text: $store.preferences.chatEndpoint)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .keyboardType(.URL)

            SecureField("OpenRouter key", text: $store.secrets.openRouterKey)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()

            SecureField("Brave key (optional)", text: $store.secrets.braveKey)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()

            TextField("Wi-Fi network", text: $store.secrets.wifiSSID)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()

            SecureField("Wi-Fi password", text: $store.secrets.wifiPassword)
        }
    }

    private var modelSection: some View {
        Section("Models") {
            TextField("Chat model", text: $store.preferences.chatModel)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()

            TextField("Transcription model", text: $store.preferences.transcriptionModel)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()

            TextField("Speech model", text: $store.preferences.speechModel)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
        }
    }

    private var actionSection: some View {
        Section {
            Button("Save on Watch") {
                send()
            }
            .disabled(provisioner.selectedID == nil)

            if let error = store.errorText {
                Text(error)
                    .foregroundStyle(.red)
            }
        } footer: {
            Text("The app keeps keys and Wi-Fi details in Keychain. The watch requires secure pairing before it accepts them.")
        }
    }

    private func send() {
        do {
            let packet = try store.makePacket()
            provisioner.provision(packet: packet) { result in
                switch result {
                case .success(let acknowledgement):
                    do {
                        try store.accept(acknowledgement)
                    } catch {
                        store.show(error)
                    }
                case .failure(let error):
                    store.show(error)
                }
            }
        } catch {
            store.show(error)
        }
    }
}
