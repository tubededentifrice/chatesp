import SwiftUI

@main
struct ChatESPApp: App {
    @StateObject private var store = ConfigurationStore()
    @StateObject private var provisioner = BLEProvisioner()

    var body: some Scene {
        WindowGroup {
            ConfigurationView()
                .environmentObject(store)
                .environmentObject(provisioner)
        }
    }
}
