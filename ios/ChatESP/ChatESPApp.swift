import SwiftUI

@main
struct ChatESPApp: App {
    @Environment(\.scenePhase) private var scenePhase
    @StateObject private var store: ConfigurationStore
    @StateObject private var provisioner: BLEProvisioner

    init() {
        let store = ConfigurationStore()
        _store = StateObject(wrappedValue: store)
        _provisioner = StateObject(
            wrappedValue: BLEProvisioner(
                selectedDeviceIdentifier: store.activeDeviceIdentifier))
    }

    var body: some Scene {
        WindowGroup {
            ConfigurationView()
                .environmentObject(store)
                .environmentObject(provisioner)
                .onChange(of: scenePhase) { _, phase in
                    if phase == .active {
                        store.reloadSecretsIfNeeded()
                    }
                }
        }
    }
}
