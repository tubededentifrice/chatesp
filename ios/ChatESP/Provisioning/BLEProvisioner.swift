import CoreBluetooth
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

    private static let service = CBUUID(string: ProvisioningProtocolV1.serviceUUID)
    private static let control = CBUUID(string: ProvisioningProtocolV1.controlUUID)
    private static let data = CBUUID(string: ProvisioningProtocolV1.dataUUID)
    private static let acknowledgement = CBUUID(string: ProvisioningProtocolV1.acknowledgementUUID)

    private lazy var central = CBCentralManager(delegate: self, queue: .main)
    private var discovered: [UUID: CBPeripheral] = [:]
    private var selected: CBPeripheral?
    private var controlCharacteristic: CBCharacteristic?
    private var dataCharacteristic: CBCharacteristic?
    private var acknowledgementCharacteristic: CBCharacteristic?
    private var pendingPacket: ProvisioningPacket?
    private var transfer: ProvisioningTransfer?
    private var frames: [(CBCharacteristic, Data)] = []
    private var frameIndex = 0
    private var attempt = 0
    private var timeoutWork: DispatchWorkItem?
    private var completion: ((Result<ProvisioningAcknowledgement, Error>) -> Void)?
    private var scanWhenReady = false

    override init() {
        super.init()
        _ = central
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
        peripheral.discoverServices([Self.service])
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
            if self.pendingPacket != nil {
                self.prepareCharacteristics(on: peripheral)
            } else {
                self.phase = .idle
            }
        }
    }

    nonisolated func centralManager(
        _ central: CBCentralManager,
        didFailToConnect peripheral: CBPeripheral,
        error: Error?
    ) {
        Task { @MainActor in
            self.finish(.failure(error ?? ProvisioningError.disconnected))
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
                [Self.control, Self.data, Self.acknowledgement],
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
            self.beginAttempt()
        }
    }

    nonisolated func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateNotificationStateFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        Task { @MainActor in
            if let error {
                self.retryOrFinish(error)
                return
            }
            guard characteristic.uuid == Self.acknowledgement,
                  characteristic.isNotifying else {
                return
            }
            self.writeNextFrame()
        }
    }

    nonisolated func peripheral(
        _ peripheral: CBPeripheral,
        didWriteValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        Task { @MainActor in
            if let error {
                self.retryOrFinish(error)
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
                self.retryOrFinish(error)
                return
            }
            guard characteristic.uuid == Self.acknowledgement,
                  let data = characteristic.value,
                  let packet = self.pendingPacket else {
                return
            }
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
