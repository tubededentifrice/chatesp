import SwiftUI

@main
struct ChatESPApp: App {
    @StateObject private var store: ConfigurationStore
    @StateObject private var provisioner: BLEProvisioner

    init() {
        let store = ConfigurationStore()
        _store = StateObject(wrappedValue: store)
        _provisioner = StateObject(
            wrappedValue: BLEProvisioner(
                selectedWatchIdentifier: store.selectedWatchIdentifier))
    }

    var body: some Scene {
        WindowGroup {
            ConfigurationView()
                .environmentObject(store)
                .environmentObject(provisioner)
        }
    }
}
