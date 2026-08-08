import CoreBluetooth
import CoreLocation
import Foundation

struct DiscoveredDevice: Identifiable {
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
        case .waitingForConfirmation: return "Waiting for the ChatESP device"
        case .complete: return "Settings saved on the ChatESP device"
        case .failed(let message): return message
        }
    }
}

enum BLEProvisionerError: Error, Equatable, LocalizedError {
    case requestInProgress
    case stopped

    var errorDescription: String? {
        switch self {
        case .requestInProgress:
            return "A settings transfer is already in progress."
        case .stopped:
            return "The ChatESP device connection was stopped."
        }
    }
}

struct BLEProvisionerPolicy {
    enum TransferTimeoutAction: Equatable {
        case failConnection
        case retryCompleteTransfer
    }

    static let scanTimeout: TimeInterval = 10
    static let connectionTimeout: TimeInterval = 10
    static let frameTimeout: TimeInterval = 10
    static let deviceContextInterval: TimeInterval = 3_600
    static let reconnectScanTimeout: TimeInterval = 10
    static let reconnectDelays: [TimeInterval] = [2, 4, 8, 16]

    static func canStartProvisioning(hasPendingRequest: Bool) -> Bool {
        !hasPendingRequest
    }

    static func mustCancelProvisioningForSelectionChange(
        isProvisioning: Bool,
        selectedID: UUID?,
        requestedID: UUID?
    ) -> Bool {
        isProvisioning && selectedID != requestedID
    }

    static func acceptsCallback(selectedID: UUID?, callbackID: UUID) -> Bool {
        selectedID == callbackID
    }

    static func reconnectDelay(attempt: Int) -> TimeInterval? {
        guard reconnectDelays.indices.contains(attempt) else { return nil }
        return reconnectDelays[attempt]
    }

    static func shouldReconnect(
        selectedID: UUID?,
        desiredID: UUID?,
        discoveredID: UUID,
        connected: Bool
    ) -> Bool {
        !connected && selectedID == discoveredID && desiredID == discoveredID
    }

    static func transferTimeoutAction(
        frameIndex: Int,
        frameCount: Int
    ) -> TransferTimeoutAction {
        frameIndex < frameCount ? .failConnection : .retryCompleteTransfer
    }

    static func hasFreshLocation(
        authorizationAllowed: Bool,
        updatedAt: TimeInterval?,
        now: TimeInterval
    ) -> Bool {
        guard authorizationAllowed, let updatedAt else { return false }
        let elapsed = now - updatedAt
        return elapsed >= 0 && elapsed <= 900
    }

    static func deviceContextSyncIsDue(
        lastSentAt: TimeInterval?,
        now: TimeInterval
    ) -> Bool {
        guard let lastSentAt else { return true }
        return now - lastSentAt >= deviceContextInterval
    }
}

@MainActor
final class BLEProvisioner: NSObject, ObservableObject {
    @Published private(set) var discoveredDevices: [DiscoveredDevice] = []
    @Published private(set) var selectedID: UUID?
    @Published private(set) var phase: ProvisioningPhase = .idle
    @Published private(set) var memories: [MemoryFact] = []
    @Published private(set) var memoryAvailable = false
    @Published private(set) var memoryMessage =
        "Connect a ChatESP device to manage memories."
    @Published private(set) var isDeviceConnected = false
    @Published private(set) var isProvisioning = false

    var onSelectedDeviceChanged: ((UUID?) -> Void)?

    private static let service = CBUUID(string: ProvisioningProtocolV3.serviceUUID)
    private static let control = CBUUID(string: ProvisioningProtocolV3.controlUUID)
    private static let data = CBUUID(string: ProvisioningProtocolV3.dataUUID)
    private static let acknowledgement = CBUUID(string: ProvisioningProtocolV3.acknowledgementUUID)
    private static let deviceContext = CBUUID(string: ProvisioningProtocolV3.deviceContextUUID)
    private static let memoryCommand = CBUUID(string: MemoryProtocolV1.commandUUID)
    private static let memoryResponse = CBUUID(string: MemoryProtocolV1.responseUUID)

    private lazy var central = CBCentralManager(
        delegate: self,
        queue: .main,
        options: [
            CBCentralManagerOptionRestoreIdentifierKey:
                "com.chatesp.provisioning.central"
        ])
    private var discovered: [UUID: CBPeripheral] = [:]
    private var restoredPeripherals: [UUID: CBPeripheral] = [:]
    private var desiredSelectedID: UUID?
    private var selected: CBPeripheral?
    private var controlCharacteristic: CBCharacteristic?
    private var dataCharacteristic: CBCharacteristic?
    private var acknowledgementCharacteristic: CBCharacteristic?
    private var deviceContextCharacteristic: CBCharacteristic?
    private var memoryCommandCharacteristic: CBCharacteristic?
    private var memoryResponseCharacteristic: CBCharacteristic?
    private var pendingPacket: ProvisioningPacket?
    private var transfer: ProvisioningTransfer?
    private var frames: [(CBCharacteristic, Data)] = []
    private var frameIndex = 0
    private var attempt = 0
    private var timeoutWork: DispatchWorkItem?
    private var completion: ((Result<ProvisioningAcknowledgement, Error>) -> Void)?
    private var scanWhenReady = false
    private var scanTimeoutWork: DispatchWorkItem?
    private var connectionTimeoutWork: DispatchWorkItem?
    private let locationManager = CLLocationManager()
    private var approximateLocation = ""
    private var approximateLocationUpdatedAt: TimeInterval?
    private var contextTimer: Timer?
    private var pendingDeviceContext: DeviceContextPacket?
    private var lastDeviceContextSentAtUptime: TimeInterval?
    private var deviceContextAttempt = 0
    private var deviceContextTimeoutWork: DispatchWorkItem?
    private var freshLocationWaitWork: DispatchWorkItem?
    private var waitingForFreshLocation = false
    private var reconnectWork: DispatchWorkItem?
    private var reconnectScanTimeoutWork: DispatchWorkItem?
    private var reconnectAttempt = 0
    private var memoryRevision: UInt32 = 0
    private var memoryFingerprint = Data(repeating: 0, count: 32)
    private var nextMemoryRequestID: UInt32 = 1
    private var pendingMemoryCommand: MemoryCommand?
    private var pendingMemoryAttempt = 0
    private var memoryTimeoutWork: DispatchWorkItem?
    private var memoryCompletion: ((Result<MemoryResponse, Error>) -> Void)?
    private var memoryList: [MemoryFact] = []
    private var memoryListRevision: UInt32 = 0
    private var memoryListFingerprint = Data(repeating: 0, count: 32)
    private var memoryListRestartCount = 0
    private var memoryRefreshNeeded = false

    init(selectedDeviceIdentifier: UUID? = nil) {
        desiredSelectedID = selectedDeviceIdentifier
        super.init()
        locationManager.delegate = self
        locationManager.desiredAccuracy = kCLLocationAccuracyThreeKilometers
        locationManager.distanceFilter = 5_000
        _ = central
    }

    deinit {
        contextTimer?.invalidate()
        scanTimeoutWork?.cancel()
        connectionTimeoutWork?.cancel()
        deviceContextTimeoutWork?.cancel()
        freshLocationWaitWork?.cancel()
        reconnectWork?.cancel()
        reconnectScanTimeoutWork?.cancel()
        memoryTimeoutWork?.cancel()
        locationManager.stopMonitoringSignificantLocationChanges()
    }

    func scan() {
        guard !isProvisioning else { return }
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
        reconnectWork?.cancel()
        reconnectWork = nil
        reconnectScanTimeoutWork?.cancel()
        reconnectScanTimeoutWork = nil
        scanTimeoutWork?.cancel()
        scanWhenReady = false
        discoveredDevices = []
        discovered = [:]
        phase = .scanning
        central.scanForPeripherals(
            withServices: [Self.service],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.scanTimeoutWork = nil
            self.central.stopScan()
            if self.phase == .scanning {
                self.phase = .idle
            }
            if self.selected != nil, !self.isDeviceConnected {
                self.scheduleReconnect()
            }
        }
        scanTimeoutWork = work
        DispatchQueue.main.asyncAfter(
            deadline: .now() + BLEProvisionerPolicy.scanTimeout,
            execute: work)
    }

    func select(_ device: DiscoveredDevice) {
        guard !isProvisioning else { return }
        select(device.peripheral, notifySelectionChange: true)
    }

    var selectedDeviceIdentifier: UUID? {
        selectedID
    }

    func restoreSelectedDevice(identifier: UUID?) {
        if BLEProvisionerPolicy.mustCancelProvisioningForSelectionChange(
            isProvisioning: isProvisioning,
            selectedID: selectedID,
            requestedID: identifier
        ) {
            finish(.failure(BLEProvisionerError.stopped), sendContext: false)
        }
        desiredSelectedID = identifier
        guard let identifier else {
            for peripheral in restoredPeripherals.values where
                !isSelected(peripheral) {
                central.cancelPeripheralConnection(peripheral)
            }
            restoredPeripherals = [:]
            return
        }
        if selectedID == identifier, let selected {
            selected.delegate = self
            if selected.state == .connected {
                isDeviceConnected = true
                if controlCharacteristic == nil ||
                    dataCharacteristic == nil ||
                    acknowledgementCharacteristic == nil {
                    prepareCharacteristics(on: selected)
                } else if phase == .connecting {
                    phase = .idle
                }
            } else {
                isDeviceConnected = false
                if reconnectWork == nil,
                   reconnectScanTimeoutWork == nil {
                    reconnectAttempt = 0
                    connectSelected()
                }
            }
            return
        }
        if let peripheral = restoredPeripherals[identifier] ??
            central.retrievePeripherals(withIdentifiers: [identifier]).first {
            select(peripheral, notifySelectionChange: false)
        } else if central.state == .poweredOn {
            startScan()
        } else {
            scanWhenReady = true
        }
    }

    func forgetSelectedDevice() {
        scanWhenReady = false
        central.stopScan()
        scanTimeoutWork?.cancel()
        scanTimeoutWork = nil
        reconnectWork?.cancel()
        reconnectWork = nil
        reconnectScanTimeoutWork?.cancel()
        reconnectScanTimeoutWork = nil
        connectionTimeoutWork?.cancel()
        connectionTimeoutWork = nil
        contextTimer?.invalidate()
        contextTimer = nil
        deviceContextTimeoutWork?.cancel()
        deviceContextTimeoutWork = nil
        cancelFreshLocationWait()
        locationManager.stopMonitoringSignificantLocationChanges()
        approximateLocation = ""
        approximateLocationUpdatedAt = nil
        pendingDeviceContext = nil
        deviceContextAttempt = 0
        lastDeviceContextSentAtUptime = nil
        let oldSelection = selected
        selected = nil
        selectedID = nil
        desiredSelectedID = nil
        restoredPeripherals = [:]
        isDeviceConnected = false
        if pendingPacket != nil || completion != nil {
            finish(.failure(BLEProvisionerError.stopped), sendContext: false)
        }
        if pendingMemoryCommand != nil {
            finishMemoryCommand(.failure(MemoryProtocolError.disconnected))
        }
        clearMemoryConnectionState()
        clearCharacteristics()
        phase = .idle
        if let oldSelection {
            oldSelection.delegate = nil
            central.cancelPeripheralConnection(oldSelection)
        }
        onSelectedDeviceChanged?(nil)
    }

    private func select(
        _ peripheral: CBPeripheral,
        notifySelectionChange: Bool
    ) {
        central.stopScan()
        scanTimeoutWork?.cancel()
        scanTimeoutWork = nil
        reconnectWork?.cancel()
        reconnectWork = nil
        reconnectScanTimeoutWork?.cancel()
        reconnectScanTimeoutWork = nil
        connectionTimeoutWork?.cancel()
        connectionTimeoutWork = nil
        let oldSelection = selected
        selected = peripheral
        selectedID = peripheral.identifier
        desiredSelectedID = peripheral.identifier
        if let oldSelection, oldSelection !== peripheral {
            oldSelection.delegate = nil
            central.cancelPeripheralConnection(oldSelection)
        }
        reconnectAttempt = 0
        isDeviceConnected = peripheral.state == .connected
        clearMemoryConnectionState()
        clearCharacteristics()
        resetDeviceContextTransfer()
        startDeviceContextSync()
        peripheral.delegate = self
        if notifySelectionChange {
            onSelectedDeviceChanged?(peripheral.identifier)
        }
        if peripheral.state == .connected {
            lastDeviceContextSentAtUptime = nil
            prepareCharacteristics(on: peripheral)
        } else {
            connectSelected()
        }
    }

    func provision(
        packet: ProvisioningPacket,
        completion: @escaping (Result<ProvisioningAcknowledgement, Error>) -> Void
    ) {
        guard let selected else {
            completion(.failure(ProvisioningError.noDevice))
            return
        }
        guard BLEProvisionerPolicy.canStartProvisioning(
            hasPendingRequest: pendingPacket != nil || self.completion != nil
        ) else {
            completion(.failure(BLEProvisionerError.requestInProgress))
            return
        }
        self.completion = completion
        isProvisioning = true
        deviceContextTimeoutWork?.cancel()
        cancelFreshLocationWait()
        pendingDeviceContext = nil
        deviceContextAttempt = 0
        pendingPacket = packet
        attempt = 0
        if selected.state == .connected {
            prepareCharacteristics(on: selected)
        } else {
            connectSelected()
        }
    }

    private func prepareCharacteristics(on peripheral: CBPeripheral) {
        guard isSelected(peripheral) else { return }
        connectionTimeoutWork?.cancel()
        connectionTimeoutWork = nil
        phase = .pairing
        clearCharacteristics()
        clearMemoryConnectionState()
        peripheral.discoverServices([Self.service])
    }

    private func clearCharacteristics() {
        controlCharacteristic = nil
        dataCharacteristic = nil
        acknowledgementCharacteristic = nil
        deviceContextCharacteristic = nil
        memoryCommandCharacteristic = nil
        memoryResponseCharacteristic = nil
    }

    private func resetDeviceContextTransfer() {
        deviceContextTimeoutWork?.cancel()
        deviceContextTimeoutWork = nil
        cancelFreshLocationWait()
        pendingDeviceContext = nil
        deviceContextAttempt = 0
    }

    private func isSelected(_ peripheral: CBPeripheral) -> Bool {
        selected === peripheral && BLEProvisionerPolicy.acceptsCallback(
            selectedID: selectedID,
            callbackID: peripheral.identifier)
    }

    private func isActive(_ peripheral: CBPeripheral) -> Bool {
        isSelected(peripheral) && isDeviceConnected &&
            peripheral.state == .connected
    }

    func refreshMemories() {
        guard memoryAvailable else {
            memoryMessage = isDeviceConnected
                ? "Update the ChatESP device firmware to manage memories."
                : "Connect a ChatESP device to manage memories."
            return
        }
        guard pendingMemoryCommand == nil else {
            memoryRefreshNeeded = true
            return
        }
        memoryListRestartCount = 0
        startMemoryList()
    }

    func addMemory(_ fact: String) {
        guard MemoryProtocolV1.validFact(fact) else {
            memoryMessage = "Enter one fact of at most 128 UTF-8 bytes."
            return
        }
        runMemoryMutation(operation: .add, fact: fact)
    }

    func deleteMemory(id: UInt32) {
        runMemoryMutation(operation: .delete, memoryID: id)
    }

    func clearAllMemories() {
        runMemoryMutation(operation: .clear)
    }

    private func clearMemoryConnectionState() {
        memoryTimeoutWork?.cancel()
        memoryTimeoutWork = nil
        pendingMemoryCommand = nil
        pendingMemoryAttempt = 0
        memoryCompletion = nil
        memoryList = []
        memoryRevision = 0
        memoryFingerprint = Data(repeating: 0, count: 32)
        memories = []
        memoryAvailable = false
        memoryRefreshNeeded = false
        memoryMessage = isDeviceConnected
            ? "Update the ChatESP device firmware to manage memories."
            : "Connect a ChatESP device to manage memories."
    }

    private func allocateMemoryRequestID() -> UInt32 {
        let value = nextMemoryRequestID
        nextMemoryRequestID = nextMemoryRequestID == UInt32.max
            ? 1
            : nextMemoryRequestID + 1
        return value
    }

    private func startMemoryList() {
        memoryList = []
        memories = []
        memoryListRevision = 0
        memoryListFingerprint = Data(repeating: 0, count: 32)
        requestMemoryPage(
            cursor: 0,
            revision: 0,
            fingerprint: Data(repeating: 0, count: 32))
    }

    private func requestMemoryPage(
        cursor: UInt32,
        revision: UInt32,
        fingerprint: Data
    ) {
        do {
            let command = try MemoryCommand(
                operation: .listPage,
                requestID: allocateMemoryRequestID(),
                expectedRevision: revision,
                expectedFingerprint: fingerprint,
                memoryID: cursor)
            sendMemoryCommand(command) { [weak self] result in
                guard let self else { return }
                switch result {
                case .failure(let error):
                    self.memoryMessage = self.memoryErrorText(error)
                case .success(let response):
                    self.acceptMemoryPage(response)
                }
            }
        } catch {
            memoryMessage = memoryErrorText(error)
        }
    }

    private func acceptMemoryPage(_ response: MemoryResponse) {
        guard response.operation == .listPage else {
            memoryMessage = MemoryProtocolError.responseMismatch.localizedDescription
            return
        }
        if response.status == .revisionConflict {
            if memoryListRestartCount < 1 {
                memoryListRestartCount += 1
                startMemoryList()
            } else {
                memoryMessage = "Memories changed during refresh. Try again."
            }
            return
        }
        guard response.status == .applied || response.status == .unchanged else {
            memoryMessage = memoryStatusText(response.status)
            return
        }
        if memoryList.isEmpty {
            memoryListRevision = response.revision
            memoryListFingerprint = response.fingerprint
        } else if response.revision != memoryListRevision ||
                    response.fingerprint != memoryListFingerprint {
            if memoryListRestartCount < 1 {
                memoryListRestartCount += 1
                startMemoryList()
            } else {
                memoryMessage = "Memories changed during refresh. Try again."
            }
            return
        }
        if let fact = response.fact {
            guard response.memoryID > (memoryList.last?.id ?? 0),
                  !memoryList.contains(where: { $0.id == response.memoryID }) else {
                memoryMessage = MemoryProtocolError.malformedResponse.localizedDescription
                return
            }
            memoryList.append(MemoryFact(id: response.memoryID, fact: fact))
        }
        if response.hasMore {
            guard let cursor = memoryList.last?.id else {
                memoryMessage = MemoryProtocolError.malformedResponse.localizedDescription
                return
            }
            requestMemoryPage(
                cursor: cursor,
                revision: memoryListRevision,
                fingerprint: memoryListFingerprint)
            return
        }
        do {
            guard memoryList.count == response.totalCount,
                  try MemoryProtocolV1.fingerprint(for: memoryList) ==
                    memoryListFingerprint else {
                throw MemoryProtocolError.malformedResponse
            }
            memories = memoryList
            memoryRevision = memoryListRevision
            memoryFingerprint = memoryListFingerprint
            memoryMessage = memories.isEmpty
                ? "The ChatESP device has no saved memories."
                : "Memories are current."
        } catch {
            memories = []
            memoryMessage = memoryErrorText(error)
        }
    }

    private func runMemoryMutation(
        operation: MemoryOperation,
        memoryID: UInt32 = 0,
        fact: String = ""
    ) {
        guard memoryAvailable else {
            memoryMessage = isDeviceConnected
                ? "Update the ChatESP device firmware to manage memories."
                : "Connect a ChatESP device to manage memories."
            return
        }
        guard pendingMemoryCommand == nil else {
            memoryMessage = "A memory request is already in progress."
            return
        }
        do {
            let command = try MemoryCommand(
                operation: operation,
                requestID: allocateMemoryRequestID(),
                expectedRevision: memoryRevision,
                expectedFingerprint: memoryFingerprint,
                memoryID: memoryID,
                fact: fact)
            sendMemoryCommand(command) { [weak self] result in
                guard let self else { return }
                switch result {
                case .failure(let error):
                    self.memoryMessage = self.memoryErrorText(error)
                case .success(let response):
                    switch response.status {
                    case .applied, .unchanged:
                        self.memoryRevision = response.revision
                        self.memoryFingerprint = response.fingerprint
                        self.refreshMemories()
                    case .full:
                        self.memoryMessage =
                            "Ask ChatESP to compact memories, or remove a fact."
                    case .revisionConflict:
                        self.memoryMessage =
                            "Memories changed on the ChatESP device. The list was reloaded."
                        self.refreshMemories()
                    default:
                        self.memoryMessage = self.memoryStatusText(response.status)
                    }
                }
            }
        } catch {
            memoryMessage = memoryErrorText(error)
        }
    }

    private func sendMemoryCommand(
        _ command: MemoryCommand,
        completion: @escaping (Result<MemoryResponse, Error>) -> Void
    ) {
        guard isDeviceConnected,
              let selected,
              selected.state == .connected,
              let memoryCommandCharacteristic,
              memoryResponseCharacteristic?.isNotifying == true else {
            completion(.failure(MemoryProtocolError.disconnected))
            return
        }
        guard selected.maximumWriteValueLength(for: .withResponse) >=
                command.data.count else {
            completion(.failure(MemoryProtocolError.unavailable))
            return
        }
        pendingMemoryCommand = command
        pendingMemoryAttempt = 1
        memoryCompletion = completion
        memoryMessage = "Waiting for the ChatESP device."
        selected.writeValue(
            command.data, for: memoryCommandCharacteristic, type: .withResponse)
        startMemoryTimeout()
    }

    private func startMemoryTimeout() {
        memoryTimeoutWork?.cancel()
        let work = DispatchWorkItem { [weak self] in
            self?.retryMemoryCommand(MemoryProtocolError.timeout)
        }
        memoryTimeoutWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 10, execute: work)
    }

    private func retryMemoryCommand(_ error: Error) {
        memoryTimeoutWork?.cancel()
        guard pendingMemoryAttempt < 2,
              let selected,
              selected.state == .connected,
              let characteristic = memoryCommandCharacteristic,
              let command = pendingMemoryCommand else {
            finishMemoryCommand(.failure(error))
            return
        }
        pendingMemoryAttempt += 1
        selected.writeValue(command.data, for: characteristic, type: .withResponse)
        startMemoryTimeout()
    }

    private func finishMemoryCommand(_ result: Result<MemoryResponse, Error>) {
        memoryTimeoutWork?.cancel()
        memoryTimeoutWork = nil
        pendingMemoryCommand = nil
        pendingMemoryAttempt = 0
        let callback = memoryCompletion
        memoryCompletion = nil
        callback?(result)
        if memoryRefreshNeeded, pendingMemoryCommand == nil {
            memoryRefreshNeeded = false
            refreshMemories()
        }
    }

    private func memoryErrorText(_ error: Error) -> String {
        (error as? LocalizedError)?.errorDescription ??
            "The memory request failed."
    }

    private func memoryStatusText(_ status: MemoryStatus) -> String {
        switch status {
        case .full:
            return "Ask ChatESP to compact memories, or remove a fact."
        case .notFound:
            return "That memory is no longer on the ChatESP device. Refresh the list."
        case .revisionConflict:
            return "Memories changed on the ChatESP device. Refresh the list."
        case .authenticationRequired:
            return "Pair with the ChatESP device before you manage memories."
        case .busy:
            return "The ChatESP device is busy. Try again."
        case .unsupportedVersion:
            return "Update the app or ChatESP device firmware to manage memories."
        case .storageFailure:
            return "The ChatESP device could not save the memory change."
        case .invalidField:
            return "The ChatESP device rejected an invalid memory field."
        case .applied, .unchanged:
            return "Memories are current."
        }
    }

    private func startDeviceContextSync() {
        locationManager.requestWhenInUseAuthorization()
        if locationManager.authorizationStatus == .authorizedAlways {
            locationManager.startMonitoringSignificantLocationChanges()
        }
        requestFreshLocation()
        if contextTimer == nil {
            contextTimer = Timer.scheduledTimer(
                withTimeInterval: BLEProvisionerPolicy.deviceContextInterval,
                repeats: true
            ) {
                [weak self] _ in
                Task { @MainActor in
                    self?.sendDeviceContextAfterFreshLocation()
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
        let now = ProcessInfo.processInfo.systemUptime
        if !force, !BLEProvisionerPolicy.deviceContextSyncIsDue(
            lastSentAt: lastDeviceContextSentAtUptime,
            now: now
        ) {
            return
        }
        do {
            let packet = try DeviceContextPacket(
                date: Date(), approximateLocation: currentApproximateLocation())
            pendingDeviceContext = packet
            if !retry {
                deviceContextAttempt = 0
            }
            deviceContextAttempt += 1
            lastDeviceContextSentAtUptime = now
            startDeviceContextTimeout()
            selected.writeValue(
                packet.data, for: deviceContextCharacteristic, type: .withResponse)
        } catch {
            pendingDeviceContext = nil
            lastDeviceContextSentAtUptime = nil
            deviceContextAttempt = 0
        }
    }

    private func currentApproximateLocation() -> String {
        let authorizationAllowed =
            locationManager.authorizationStatus == .authorizedAlways ||
            locationManager.authorizationStatus == .authorizedWhenInUse
        guard BLEProvisionerPolicy.hasFreshLocation(
            authorizationAllowed: authorizationAllowed,
            updatedAt: approximateLocationUpdatedAt,
            now: ProcessInfo.processInfo.systemUptime) else {
            return ""
        }
        return approximateLocation
    }

    private func startDeviceContextTimeout() {
        deviceContextTimeoutWork?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.pendingDeviceContext = nil
            self.lastDeviceContextSentAtUptime = nil
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
        guard reconnectWork == nil,
              reconnectScanTimeoutWork == nil,
              pendingPacket == nil,
              let selected,
              let delay = BLEProvisionerPolicy.reconnectDelay(
                attempt: reconnectAttempt) else {
            if selectedID != nil, reconnectAttempt >=
                BLEProvisionerPolicy.reconnectDelays.count {
                phase = .failed("The ChatESP device did not reconnect.")
            }
            return
        }
        reconnectAttempt += 1
        let work = DispatchWorkItem { [weak self, weak selected] in
            guard let self else { return }
            self.reconnectWork = nil
            guard let selected,
                  self.isSelected(selected),
                  self.central.state == .poweredOn,
                  selected.state == .disconnected else { return }
            self.startReconnectScan()
        }
        reconnectWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: work)
    }

    private func connectSelected() {
        guard let selected, central.state == .poweredOn else {
            phase = .failed(ProvisioningError.bluetoothUnavailable.localizedDescription)
            return
        }
        phase = .connecting
        selected.delegate = self
        switch selected.state {
        case .connected:
            lastDeviceContextSentAtUptime = nil
            isDeviceConnected = true
            prepareCharacteristics(on: selected)
        case .connecting:
            startConnectionTimeout(for: selected)
        case .disconnected:
            central.connect(selected)
            startConnectionTimeout(for: selected)
        case .disconnecting:
            scheduleReconnect()
        @unknown default:
            scheduleReconnect()
        }
    }

    private func startConnectionTimeout(for peripheral: CBPeripheral) {
        connectionTimeoutWork?.cancel()
        let work = DispatchWorkItem { [weak self, weak peripheral] in
            guard let self, let peripheral, self.isSelected(peripheral),
                  peripheral.state != .connected else { return }
            self.connectionTimeoutWork = nil
            self.central.cancelPeripheralConnection(peripheral)
            if self.pendingPacket != nil {
                self.finish(.failure(ProvisioningError.timeout), sendContext: false)
            } else {
                self.scheduleReconnect()
            }
        }
        connectionTimeoutWork = work
        DispatchQueue.main.asyncAfter(
            deadline: .now() + BLEProvisionerPolicy.connectionTimeout,
            execute: work)
    }

    private func startReconnectScan() {
        guard let selected, isSelected(selected),
              central.state == .poweredOn,
              selected.state == .disconnected else {
            scheduleReconnect()
            return
        }
        phase = .connecting
        central.scanForPeripherals(
            withServices: [Self.service],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
        let work = DispatchWorkItem { [weak self, weak selected] in
            guard let self else { return }
            self.reconnectScanTimeoutWork = nil
            guard let selected, self.isSelected(selected),
                  !self.isDeviceConnected else { return }
            self.central.stopScan()
            self.scheduleReconnect()
        }
        reconnectScanTimeoutWork = work
        DispatchQueue.main.asyncAfter(
            deadline: .now() + BLEProvisionerPolicy.reconnectScanTimeout,
            execute: work)
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
                startTimeout()
                selected.setNotifyValue(true, for: acknowledgementCharacteristic)
            }
        } catch {
            finish(.failure(error))
        }
    }

    private func writeNextFrame() {
        guard pendingPacket != nil, isProvisioning else { return }
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
        startTimeout()
        selected.writeValue(frame.1, for: frame.0, type: .withResponse)
    }

    private func startTimeout() {
        timeoutWork?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self,
                  self.pendingPacket != nil,
                  self.isProvisioning else { return }
            if BLEProvisionerPolicy.transferTimeoutAction(
                frameIndex: self.frameIndex,
                frameCount: self.frames.count) == .failConnection {
                if let selected = self.selected {
                    self.central.cancelPeripheralConnection(selected)
                }
                self.finish(
                    .failure(ProvisioningError.timeout),
                    sendContext: false)
            } else {
                self.retryOrFinish(ProvisioningError.timeout)
            }
        }
        timeoutWork = work
        DispatchQueue.main.asyncAfter(
            deadline: .now() + BLEProvisionerPolicy.frameTimeout,
            execute: work)
    }

    private func retryOrFinish(_ error: Error) {
        timeoutWork?.cancel()
        if attempt < 2 {
            beginAttempt()
        } else {
            finish(.failure(error))
        }
    }

    private func finish(
        _ result: Result<ProvisioningAcknowledgement, Error>,
        sendContext shouldSendContext: Bool = true
    ) {
        timeoutWork?.cancel()
        timeoutWork = nil
        transfer = nil
        frames = []
        frameIndex = 0
        pendingPacket = nil
        isProvisioning = false
        switch result {
        case .success:
            phase = .complete
        case .failure(let error):
            phase = .failed((error as? LocalizedError)?.errorDescription ?? "Provisioning failed.")
        }
        let callback = completion
        completion = nil
        callback?(result)
        if shouldSendContext {
            sendDeviceContext()
        }
        if !isDeviceConnected, selected != nil,
           central.state == .poweredOn {
            scheduleReconnect()
        }
    }
}

extension BLEProvisioner: CBCentralManagerDelegate {
    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        Task { @MainActor in
            if central.state == .poweredOn, self.scanWhenReady {
                if let identifier = self.desiredSelectedID,
                   let peripheral = self.restoredPeripherals[identifier] ??
                    self.central.retrievePeripherals(
                        withIdentifiers: [identifier]).first {
                    self.select(peripheral, notifySelectionChange: false)
                } else {
                    self.startScan()
                }
                return
            }
            if central.state == .poweredOn,
               self.selected != nil,
               !self.isDeviceConnected,
               self.pendingPacket == nil {
                self.scheduleReconnect()
            }
            if central.state != .poweredOn, self.phase == .scanning {
                self.scanWhenReady = false
                self.phase = .failed(ProvisioningError.bluetoothUnavailable.localizedDescription)
            }
            if central.state != .poweredOn {
                self.connectionTimeoutWork?.cancel()
                self.connectionTimeoutWork = nil
                self.reconnectWork?.cancel()
                self.reconnectWork = nil
                self.reconnectScanTimeoutWork?.cancel()
                self.reconnectScanTimeoutWork = nil
                self.isDeviceConnected = false
                self.clearMemoryConnectionState()
                self.resetDeviceContextTransfer()
                if self.pendingPacket != nil || self.completion != nil {
                    self.finish(
                        .failure(ProvisioningError.bluetoothUnavailable),
                        sendContext: false)
                }
            }
        }
    }

    nonisolated func centralManager(
        _ central: CBCentralManager,
        willRestoreState dict: [String: Any]
    ) {
        let peripherals = dict[CBCentralManagerRestoredStatePeripheralsKey]
            as? [CBPeripheral] ?? []
        Task { @MainActor in
            for peripheral in peripherals {
                peripheral.delegate = self
                self.restoredPeripherals[peripheral.identifier] = peripheral
            }
            guard let identifier = self.desiredSelectedID,
                  let peripheral = self.restoredPeripherals[identifier] else {
                self.central.stopScan()
                return
            }
            self.select(peripheral, notifySelectionChange: false)
        }
    }

    nonisolated func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        Task { @MainActor in
            guard central.isScanning else { return }
            self.discovered[peripheral.identifier] = peripheral
            self.discoveredDevices = self.discovered.values
                .map { DiscoveredDevice(peripheral: $0, name: $0.name ?? "ChatESP") }
                .sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
            if self.reconnectScanTimeoutWork != nil,
               BLEProvisionerPolicy.shouldReconnect(
                selectedID: self.selectedID,
                desiredID: self.desiredSelectedID,
                discoveredID: peripheral.identifier,
                connected: self.isDeviceConnected
            ) {
                self.reconnectScanTimeoutWork?.cancel()
                self.reconnectScanTimeoutWork = nil
                self.scanTimeoutWork?.cancel()
                self.scanTimeoutWork = nil
                central.stopScan()
                if self.selected !== peripheral {
                    self.selected?.delegate = nil
                    self.selected = peripheral
                }
                peripheral.delegate = self
                self.connectSelected()
                return
            }
            if self.selected == nil,
               self.desiredSelectedID == peripheral.identifier {
                self.select(peripheral, notifySelectionChange: false)
            }
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        Task { @MainActor in
            guard self.isSelected(peripheral) else {
                if self.restoredPeripherals[peripheral.identifier] !== peripheral {
                    central.cancelPeripheralConnection(peripheral)
                }
                return
            }
            self.reconnectWork?.cancel()
            self.reconnectWork = nil
            self.reconnectScanTimeoutWork?.cancel()
            self.reconnectScanTimeoutWork = nil
            self.connectionTimeoutWork?.cancel()
            self.connectionTimeoutWork = nil
            self.reconnectAttempt = 0
            self.lastDeviceContextSentAtUptime = nil
            self.isDeviceConnected = true
            self.prepareCharacteristics(on: peripheral)
        }
    }

    nonisolated func centralManager(
        _ central: CBCentralManager,
        didFailToConnect peripheral: CBPeripheral,
        error: Error?
    ) {
        Task { @MainActor in
            guard self.isSelected(peripheral) else { return }
            self.connectionTimeoutWork?.cancel()
            self.connectionTimeoutWork = nil
            self.isDeviceConnected = false
            self.clearMemoryConnectionState()
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
            guard self.isSelected(peripheral) else { return }
            self.connectionTimeoutWork?.cancel()
            self.connectionTimeoutWork = nil
            self.isDeviceConnected = false
            self.clearMemoryConnectionState()
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
            guard self.isActive(peripheral) else { return }
            if let error {
                self.finish(.failure(error))
                return
            }
            guard let service = peripheral.services?.first(where: { $0.uuid == Self.service }) else {
                self.finish(.failure(ProvisioningError.missingService))
                return
            }
            peripheral.discoverCharacteristics(
                [Self.control, Self.data, Self.acknowledgement, Self.deviceContext,
                 Self.memoryCommand, Self.memoryResponse],
                for: service)
        }
    }

    nonisolated func peripheral(
        _ peripheral: CBPeripheral,
        didDiscoverCharacteristicsFor service: CBService,
        error: Error?
    ) {
        Task { @MainActor in
            guard self.isActive(peripheral), service.peripheral === peripheral else {
                return
            }
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
            self.memoryCommandCharacteristic = service.characteristics?.first {
                $0.uuid == Self.memoryCommand
            }
            self.memoryResponseCharacteristic = service.characteristics?.first {
                $0.uuid == Self.memoryResponse
            }
            if let response = self.memoryResponseCharacteristic,
               self.memoryCommandCharacteristic != nil {
                peripheral.setNotifyValue(true, for: response)
            } else {
                self.memoryAvailable = false
                self.memories = []
                self.memoryMessage =
                    "Update the ChatESP device firmware to manage memories."
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
            guard self.isActive(peripheral),
                  characteristic.service?.peripheral === peripheral else {
                return
            }
            if characteristic.uuid == Self.memoryResponse {
                if let error {
                    self.memoryAvailable = false
                    self.memories = []
                    self.memoryMessage = self.memoryErrorText(error)
                } else if characteristic.isNotifying,
                          self.memoryCommandCharacteristic != nil {
                    self.memoryAvailable = true
                    self.memoryMessage = "Loading memories."
                    self.refreshMemories()
                }
                return
            }
            if let error {
                if self.pendingPacket != nil {
                    self.retryOrFinish(error)
                } else {
                    self.deviceContextTimeoutWork?.cancel()
                    self.pendingDeviceContext = nil
                    self.lastDeviceContextSentAtUptime = nil
                    self.deviceContextAttempt = 0
                    self.phase = .idle
                }
                return
            }
            guard characteristic.uuid == Self.acknowledgement else {
                return
            }
            guard characteristic.isNotifying else {
                if self.pendingPacket != nil {
                    self.retryOrFinish(ProvisioningError.writeFailed)
                } else {
                    self.deviceContextTimeoutWork?.cancel()
                    self.pendingDeviceContext = nil
                    self.lastDeviceContextSentAtUptime = nil
                    self.deviceContextAttempt = 0
                    self.phase = .idle
                }
                return
            }
            self.timeoutWork?.cancel()
            self.timeoutWork = nil
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
            guard self.isActive(peripheral),
                  characteristic.service?.peripheral === peripheral else {
                return
            }
            if characteristic.uuid == Self.memoryCommand {
                if let error {
                    self.retryMemoryCommand(error)
                }
                return
            }
            if characteristic.uuid == Self.control || characteristic.uuid == Self.data {
                guard self.pendingPacket != nil, self.isProvisioning else {
                    return
                }
                self.timeoutWork?.cancel()
                self.timeoutWork = nil
            }
            if let error {
                if characteristic.uuid == Self.deviceContext {
                    self.deviceContextTimeoutWork?.cancel()
                    self.pendingDeviceContext = nil
                    self.lastDeviceContextSentAtUptime = nil
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
            guard self.isActive(peripheral),
                  characteristic.service?.peripheral === peripheral else {
                return
            }
            if characteristic.uuid == Self.memoryResponse {
                if let error {
                    if self.pendingMemoryCommand != nil {
                        self.retryMemoryCommand(error)
                    } else {
                        self.memoryMessage = self.memoryErrorText(error)
                    }
                    return
                }
                guard let data = characteristic.value else { return }
                do {
                    let response = try MemoryResponse(data: data)
                    if response.isChangeEvent {
                        if self.pendingMemoryCommand == nil {
                            self.refreshMemories()
                        } else {
                            self.memoryRefreshNeeded = true
                        }
                        return
                    }
                    guard let command = self.pendingMemoryCommand,
                          response.requestID == command.requestID,
                          response.operation == command.operation else {
                        throw MemoryProtocolError.responseMismatch
                    }
                    self.finishMemoryCommand(.success(response))
                } catch {
                    if self.pendingMemoryCommand != nil {
                        self.finishMemoryCommand(.failure(error))
                    } else {
                        self.memoryMessage = self.memoryErrorText(error)
                    }
                }
                return
            }
            if let error {
                if self.pendingDeviceContext != nil {
                    self.deviceContextTimeoutWork?.cancel()
                    self.pendingDeviceContext = nil
                    self.lastDeviceContextSentAtUptime = nil
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
                        self.lastDeviceContextSentAtUptime = nil
                        return
                    }
                } catch {
                    self.lastDeviceContextSentAtUptime = nil
                    return
                }
                return
            }
            guard let packet = self.pendingPacket else { return }
            do {
                let acknowledgement = try ProvisioningAcknowledgement(data: data)
                if acknowledgement.isRevisionRecovery {
                    self.finish(.success(acknowledgement))
                    return
                }
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
        Task { @MainActor in
            guard self.selected != nil else { return }
            switch manager.authorizationStatus {
            case .authorizedAlways:
                manager.startMonitoringSignificantLocationChanges()
                manager.requestLocation()
            case .authorizedWhenInUse:
                manager.stopMonitoringSignificantLocationChanges()
                manager.requestLocation()
            default:
                manager.stopMonitoringSignificantLocationChanges()
                self.approximateLocation = ""
                self.approximateLocationUpdatedAt = nil
            }
        }
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
            guard self.selected != nil else { return }
            let changed = self.approximateLocation != text
            self.approximateLocation = text
            self.approximateLocationUpdatedAt = ProcessInfo.processInfo.systemUptime
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
