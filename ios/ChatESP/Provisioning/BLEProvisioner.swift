import CoreBluetooth
import CoreLocation
import Foundation

struct DiscoveredWatch: Identifiable {
    let peripheral: CBPeripheral
    let name: String

    var id: UUID { peripheral.identifier }
}

enum ProvisioningPhase: Equatable {
    case idle
    case scanning
    case connecting
    case pairing
    case transferring(Int)
    case waitingForConfirmation
    case complete
    case failed(String)

    var text: String {
        switch self {
        case .idle: return "Ready"
        case .scanning: return "Searching for ChatESP"
        case .connecting: return "Connecting"
        case .pairing: return "Preparing the secure connection"
        case .transferring(let percent): return "Sending settings: \(percent)%"
        case .waitingForConfirmation: return "Waiting for the watch"
        case .complete: return "Settings saved on the watch"
        case .failed(let message): return message
        }
    }
}

@MainActor
final class BLEProvisioner: NSObject, ObservableObject {
    @Published private(set) var watches: [DiscoveredWatch] = []
    @Published private(set) var selectedID: UUID?
    @Published private(set) var phase: ProvisioningPhase = .idle

    private static let service = CBUUID(string: ProvisioningProtocolV2.serviceUUID)
    private static let control = CBUUID(string: ProvisioningProtocolV2.controlUUID)
    private static let data = CBUUID(string: ProvisioningProtocolV2.dataUUID)
    private static let acknowledgement = CBUUID(string: ProvisioningProtocolV2.acknowledgementUUID)
    private static let deviceContext = CBUUID(string: ProvisioningProtocolV2.deviceContextUUID)

    private lazy var central = CBCentralManager(delegate: self, queue: .main)
    private var discovered: [UUID: CBPeripheral] = [:]
    private var selected: CBPeripheral?
    private var controlCharacteristic: CBCharacteristic?
    private var dataCharacteristic: CBCharacteristic?
    private var acknowledgementCharacteristic: CBCharacteristic?
    private var deviceContextCharacteristic: CBCharacteristic?
    private var pendingPacket: ProvisioningPacket?
    private var transfer: ProvisioningTransfer?
    private var frames: [(CBCharacteristic, Data)] = []
    private var frameIndex = 0
    private var attempt = 0
    private var timeoutWork: DispatchWorkItem?
    private var completion: ((Result<ProvisioningAcknowledgement, Error>) -> Void)?
    private var scanWhenReady = false
    private let locationManager = CLLocationManager()
    private var approximateLocation = ""
    private var contextTimer: Timer?
    private var pendingDeviceContext: DeviceContextPacket?
    private var lastDeviceContextSentAt: Date?
    private var deviceContextAttempt = 0
    private var deviceContextTimeoutWork: DispatchWorkItem?
    private var freshLocationWaitWork: DispatchWorkItem?
    private var waitingForFreshLocation = false
    private var reconnectWork: DispatchWorkItem?

    override init() {
        super.init()
        locationManager.delegate = self
        locationManager.desiredAccuracy = kCLLocationAccuracyThreeKilometers
        locationManager.distanceFilter = 5_000
        _ = central
    }

    deinit {
        contextTimer?.invalidate()
        deviceContextTimeoutWork?.cancel()
        freshLocationWaitWork?.cancel()
        reconnectWork?.cancel()
    }

    func scan() {
        if central.state == .unknown || central.state == .resetting {
            scanWhenReady = true
            phase = .scanning
            return
        }
        guard central.state == .poweredOn else {
            phase = .failed(ProvisioningError.bluetoothUnavailable.localizedDescription)
            return
        }
        startScan()
    }

    private func startScan() {
        scanWhenReady = false
        watches = []
        discovered = [:]
        phase = .scanning
        central.scanForPeripherals(
            withServices: [Self.service],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
    }

    func select(_ watch: DiscoveredWatch) {
        central.stopScan()
        if let selected, selected != watch.peripheral {
            central.cancelPeripheralConnection(selected)
        }
        selected = watch.peripheral
        selectedID = watch.id
        deviceContextTimeoutWork?.cancel()
        cancelFreshLocationWait()
        pendingDeviceContext = nil
        deviceContextAttempt = 0
        startDeviceContextSync()
        phase = .connecting
        watch.peripheral.delegate = self
        central.connect(watch.peripheral)
    }

    func provision(
        packet: ProvisioningPacket,
        completion: @escaping (Result<ProvisioningAcknowledgement, Error>) -> Void
    ) {
        guard let selected else {
            completion(.failure(ProvisioningError.noDevice))
            return
        }
        self.completion = completion
        deviceContextTimeoutWork?.cancel()
        cancelFreshLocationWait()
        pendingDeviceContext = nil
        deviceContextAttempt = 0
        pendingPacket = packet
        attempt = 0
        if selected.state == .connected {
            prepareCharacteristics(on: selected)
        } else {
            phase = .connecting
            central.connect(selected)
        }
    }

    private func prepareCharacteristics(on peripheral: CBPeripheral) {
        phase = .pairing
        controlCharacteristic = nil
        dataCharacteristic = nil
        acknowledgementCharacteristic = nil
        deviceContextCharacteristic = nil
        peripheral.discoverServices([Self.service])
    }

    private func startDeviceContextSync() {
        locationManager.requestWhenInUseAuthorization()
        locationManager.startMonitoringSignificantLocationChanges()
        requestFreshLocation()
        if contextTimer == nil {
            contextTimer = Timer.scheduledTimer(withTimeInterval: 3_600, repeats: true) {
                [weak self] _ in
                Task { @MainActor in
                    self?.sendDeviceContext()
                }
            }
        }
    }

    private func prepareDeviceContextSync() {
        guard let selected,
              let acknowledgementCharacteristic,
              deviceContextCharacteristic != nil else {
            phase = .failed(ProvisioningError.missingCharacteristic.localizedDescription)
            return
        }
        phase = .idle
        if acknowledgementCharacteristic.isNotifying {
            sendDeviceContextAfterFreshLocation()
        } else {
            selected.setNotifyValue(true, for: acknowledgementCharacteristic)
        }
    }

    private func requestFreshLocation() {
        switch locationManager.authorizationStatus {
        case .authorizedAlways, .authorizedWhenInUse:
            locationManager.requestLocation()
        default:
            break
        }
    }

    private func sendDeviceContextAfterFreshLocation() {
        cancelFreshLocationWait()
        waitingForFreshLocation = true
        requestFreshLocation()
        let work = DispatchWorkItem { [weak self] in
            guard let self, self.waitingForFreshLocation else { return }
            self.waitingForFreshLocation = false
            self.freshLocationWaitWork = nil
            self.sendDeviceContext(force: true)
        }
        freshLocationWaitWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 1, execute: work)
    }

    private func cancelFreshLocationWait() {
        freshLocationWaitWork?.cancel()
        freshLocationWaitWork = nil
        waitingForFreshLocation = false
    }

    private func sendDeviceContext(force: Bool = false, retry: Bool = false) {
        guard pendingPacket == nil,
              pendingDeviceContext == nil,
              let selected,
              selected.state == .connected,
              let deviceContextCharacteristic,
              acknowledgementCharacteristic?.isNotifying == true else {
            return
        }
        if !force, let lastDeviceContextSentAt,
           Date().timeIntervalSince(lastDeviceContextSentAt) < 3_600 {
            return
        }
        do {
            let packet = try DeviceContextPacket(
                date: Date(), approximateLocation: approximateLocation)
            pendingDeviceContext = packet
            if !retry {
                deviceContextAttempt = 0
            }
            deviceContextAttempt += 1
            lastDeviceContextSentAt = Date()
            startDeviceContextTimeout()
            selected.writeValue(
                packet.data, for: deviceContextCharacteristic, type: .withResponse)
        } catch {
            pendingDeviceContext = nil
            lastDeviceContextSentAt = nil
            deviceContextAttempt = 0
        }
    }

    private func startDeviceContextTimeout() {
        deviceContextTimeoutWork?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.pendingDeviceContext = nil
            self.lastDeviceContextSentAt = nil
            if self.deviceContextAttempt < 2 {
                self.sendDeviceContext(force: true, retry: true)
            } else {
                self.deviceContextAttempt = 0
            }
        }
        deviceContextTimeoutWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 10, execute: work)
    }

    private func scheduleReconnect() {
        reconnectWork?.cancel()
        guard pendingPacket == nil, let selected else { return }
        let work = DispatchWorkItem { [weak self, weak selected] in
            guard let self, let selected,
                  self.central.state == .poweredOn,
                  selected.state == .disconnected else { return }
            self.phase = .connecting
            self.central.connect(selected)
        }
        reconnectWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 2, execute: work)
    }

    private func beginAttempt() {
        guard let selected,
              let packet = pendingPacket,
              let controlCharacteristic,
              let dataCharacteristic,
              let acknowledgementCharacteristic else {
            finish(.failure(ProvisioningError.missingCharacteristic))
            return
        }
        guard selected.maximumWriteValueLength(for: .withResponse) >= 194 else {
            finish(.failure(ProvisioningError.writeFailed))
            return
        }
        do {
            attempt += 1
            let transfer = try ProvisioningTransfer(packet: packet)
            self.transfer = transfer
            frames = [(controlCharacteristic, transfer.beginFrame)]
            frames.append(contentsOf: transfer.dataFrames.map { (dataCharacteristic, $0) })
            frameIndex = 0
            if acknowledgementCharacteristic.isNotifying {
                writeNextFrame()
            } else {
                selected.setNotifyValue(true, for: acknowledgementCharacteristic)
            }
        } catch {
            finish(.failure(error))
        }
    }

    private func writeNextFrame() {
        guard let selected else {
            finish(.failure(ProvisioningError.disconnected))
            return
        }
        guard frameIndex < frames.count else {
            phase = .waitingForConfirmation
            startTimeout()
            return
        }
        let percent = frames.count > 1
            ? max(0, min(100, (frameIndex * 100) / frames.count))
            : 0
        phase = .transferring(percent)
        let frame = frames[frameIndex]
        selected.writeValue(frame.1, for: frame.0, type: .withResponse)
    }

    private func startTimeout() {
        timeoutWork?.cancel()
        let work = DispatchWorkItem { [weak self] in
            self?.retryOrFinish(ProvisioningError.timeout)
        }
        timeoutWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 10, execute: work)
    }

    private func retryOrFinish(_ error: Error) {
        timeoutWork?.cancel()
        if attempt < 2 {
            beginAttempt()
        } else {
            finish(.failure(error))
        }
    }

    private func finish(_ result: Result<ProvisioningAcknowledgement, Error>) {
        timeoutWork?.cancel()
        timeoutWork = nil
        transfer = nil
        frames = []
        frameIndex = 0
        pendingPacket = nil
        switch result {
        case .success:
            phase = .complete
        case .failure(let error):
            phase = .failed((error as? LocalizedError)?.errorDescription ?? "Provisioning failed.")
        }
        let callback = completion
        completion = nil
        callback?(result)
        sendDeviceContext()
    }
}

extension BLEProvisioner: CBCentralManagerDelegate {
    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        Task { @MainActor in
            if central.state == .poweredOn, self.scanWhenReady {
                self.startScan()
                return
            }
            if central.state != .poweredOn, self.phase == .scanning {
                self.scanWhenReady = false
                self.phase = .failed(ProvisioningError.bluetoothUnavailable.localizedDescription)
            }
        }
    }

    nonisolated func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        Task { @MainActor in
            self.discovered[peripheral.identifier] = peripheral
            self.watches = self.discovered.values
                .map { DiscoveredWatch(peripheral: $0, name: $0.name ?? "ChatESP") }
                .sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        Task { @MainActor in
            self.reconnectWork?.cancel()
            self.lastDeviceContextSentAt = nil
            self.prepareCharacteristics(on: peripheral)
        }
    }

    nonisolated func centralManager(
        _ central: CBCentralManager,
        didFailToConnect peripheral: CBPeripheral,
        error: Error?
    ) {
        Task { @MainActor in
            if self.pendingPacket != nil {
                self.finish(.failure(error ?? ProvisioningError.disconnected))
            } else {
                self.phase = .idle
                self.scheduleReconnect()
            }
        }
    }

    nonisolated func centralManager(
        _ central: CBCentralManager,
        didDisconnectPeripheral peripheral: CBPeripheral,
        error: Error?
    ) {
        Task { @MainActor in
            if self.pendingPacket != nil {
                self.finish(.failure(error ?? ProvisioningError.disconnected))
            } else if self.selectedID == peripheral.identifier {
                self.phase = .idle
                self.pendingDeviceContext = nil
                self.deviceContextTimeoutWork?.cancel()
                self.cancelFreshLocationWait()
                self.deviceContextAttempt = 0
                self.scheduleReconnect()
            }
        }
    }
}

extension BLEProvisioner: CBPeripheralDelegate {
    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        Task { @MainActor in
            if let error {
                self.finish(.failure(error))
                return
            }
            guard let service = peripheral.services?.first(where: { $0.uuid == Self.service }) else {
                self.finish(.failure(ProvisioningError.missingService))
                return
            }
            peripheral.discoverCharacteristics(
                [Self.control, Self.data, Self.acknowledgement, Self.deviceContext],
                for: service)
        }
    }

    nonisolated func peripheral(
        _ peripheral: CBPeripheral,
        didDiscoverCharacteristicsFor service: CBService,
        error: Error?
    ) {
        Task { @MainActor in
            if let error {
                self.finish(.failure(error))
                return
            }
            self.controlCharacteristic = service.characteristics?.first { $0.uuid == Self.control }
            self.dataCharacteristic = service.characteristics?.first { $0.uuid == Self.data }
            self.acknowledgementCharacteristic = service.characteristics?.first {
                $0.uuid == Self.acknowledgement
            }
            self.deviceContextCharacteristic = service.characteristics?.first {
                $0.uuid == Self.deviceContext
            }
            if self.pendingPacket != nil {
                self.beginAttempt()
            } else {
                self.prepareDeviceContextSync()
            }
        }
    }

    nonisolated func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateNotificationStateFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        Task { @MainActor in
            if let error {
                if self.pendingPacket != nil {
                    self.retryOrFinish(error)
                } else {
                    self.deviceContextTimeoutWork?.cancel()
                    self.pendingDeviceContext = nil
                    self.lastDeviceContextSentAt = nil
                    self.deviceContextAttempt = 0
                    self.phase = .idle
                }
                return
            }
            guard characteristic.uuid == Self.acknowledgement,
                  characteristic.isNotifying else {
                return
            }
            if self.pendingPacket != nil {
                self.writeNextFrame()
            } else {
                self.sendDeviceContextAfterFreshLocation()
            }
        }
    }

    nonisolated func peripheral(
        _ peripheral: CBPeripheral,
        didWriteValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        Task { @MainActor in
            if let error {
                if characteristic.uuid == Self.deviceContext {
                    self.deviceContextTimeoutWork?.cancel()
                    self.pendingDeviceContext = nil
                    self.lastDeviceContextSentAt = nil
                    if self.deviceContextAttempt < 2 {
                        self.sendDeviceContext(force: true, retry: true)
                    } else {
                        self.deviceContextAttempt = 0
                    }
                    return
                }
                self.retryOrFinish(error)
                return
            }
            if characteristic.uuid == Self.deviceContext {
                return
            }
            self.frameIndex += 1
            self.writeNextFrame()
        }
    }

    nonisolated func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        Task { @MainActor in
            if let error {
                if self.pendingDeviceContext != nil {
                    self.deviceContextTimeoutWork?.cancel()
                    self.pendingDeviceContext = nil
                    self.lastDeviceContextSentAt = nil
                    self.deviceContextAttempt = 0
                } else if self.pendingPacket != nil {
                    self.retryOrFinish(error)
                }
                return
            }
            guard characteristic.uuid == Self.acknowledgement,
                  let data = characteristic.value else {
                return
            }
            if data.prefix(4) == Data("CESR".utf8) {
                guard let packet = self.pendingDeviceContext else { return }
                self.deviceContextTimeoutWork?.cancel()
                defer {
                    self.pendingDeviceContext = nil
                    self.deviceContextAttempt = 0
                }
                do {
                    let acknowledgement = try DeviceContextAcknowledgement(data: data)
                    guard acknowledgement.status == .applied,
                          acknowledgement.epochSeconds == packet.epochSeconds,
                          acknowledgement.fingerprint == packet.fingerprint else {
                        self.lastDeviceContextSentAt = nil
                        return
                    }
                } catch {
                    self.lastDeviceContextSentAt = nil
                    return
                }
                return
            }
            guard let packet = self.pendingPacket else { return }
            do {
                let acknowledgement = try ProvisioningAcknowledgement(data: data)
                guard acknowledgement.isSuccess else {
                    throw ProvisioningError.deviceRejected(acknowledgement.status)
                }
                guard acknowledgement.revision == packet.revision,
                      acknowledgement.fingerprint == packet.fingerprint else {
                    throw ProvisioningError.acknowledgementMismatch
                }
                self.finish(.success(acknowledgement))
            } catch {
                self.finish(.failure(error))
            }
        }
    }
}

extension BLEProvisioner: CLLocationManagerDelegate {
    nonisolated func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        guard manager.authorizationStatus == .authorizedAlways ||
                manager.authorizationStatus == .authorizedWhenInUse else {
            return
        }
        manager.startMonitoringSignificantLocationChanges()
        manager.requestLocation()
    }

    nonisolated func locationManager(
        _ manager: CLLocationManager,
        didUpdateLocations locations: [CLLocation]
    ) {
        guard let location = locations.last,
              location.horizontalAccuracy >= 0,
              location.horizontalAccuracy <= 20_000,
              abs(location.timestamp.timeIntervalSinceNow) <= 900 else {
            return
        }
        let latitude = (location.coordinate.latitude * 10).rounded() / 10
        let longitude = (location.coordinate.longitude * 10).rounded() / 10
        let text = String(
            format: "latitude %.1f, longitude %.1f",
            locale: Locale(identifier: "en_US_POSIX"),
            latitude, longitude)
        Task { @MainActor in
            let changed = self.approximateLocation != text
            self.approximateLocation = text
            if self.waitingForFreshLocation {
                self.cancelFreshLocationWait()
                self.sendDeviceContext(force: true)
            } else if changed {
                self.sendDeviceContext()
            }
        }
    }

    nonisolated func locationManager(
        _ manager: CLLocationManager,
        didFailWithError error: Error
    ) {
        // Time synchronization continues when location is not available.
    }
}
