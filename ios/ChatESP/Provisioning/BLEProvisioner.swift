import CoreBluetooth
import CoreLocation
import Foundation
import os
import UIKit

final class PhoneProxySessionDelegate: NSObject, URLSessionTaskDelegate,
    @unchecked Sendable {
    private let allowsRedirects: Bool
    private let maximumRedirects: UInt8
    private let lock = NSLock()
    private var redirectCount: UInt8 = 0

    init(allowsRedirects: Bool, maximumRedirects: UInt8) {
        self.allowsRedirects = allowsRedirects
        self.maximumRedirects = maximumRedirects
    }

    func urlSession(
        _ session: URLSession,
        task: URLSessionTask,
        willPerformHTTPRedirection response: HTTPURLResponse,
        newRequest request: URLRequest,
        completionHandler: @escaping (URLRequest?) -> Void
    ) {
        guard allowsRedirects,
              let redirectText = request.url?.absoluteString,
              PhoneProxyProtocolV1.validURL(Data(redirectText.utf8)) != nil else {
            completionHandler(nil)
            return
        }
        lock.lock()
        let canRedirect = redirectCount < maximumRedirects
        if canRedirect {
            redirectCount += 1
        }
        lock.unlock()
        completionHandler(canRedirect ? request : nil)
    }
}

struct DiscoveredDevice: Identifiable {
    let peripheral: CBPeripheral
    let name: String

    var id: UUID { peripheral.identifier }
}

enum ProvisioningPhase: Equatable {
    case idle
    case scanning
    case connecting
    case unavailable
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
        case .unavailable:
            return "The device is asleep or unavailable. Retrying."
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

    enum ReconnectStateAction: Equatable {
        case scan
        case prepare
        case wait
    }

    static let scanTimeout: TimeInterval = 10
    static let connectionTimeout: TimeInterval = 10
    static let serviceDiscoveryTimeout: TimeInterval = 10
    static let frameTimeout: TimeInterval = 10
    static let phoneProxyResponseTimeout: TimeInterval = 180
    static let deviceContextInterval: TimeInterval = 3_600
    static let reconnectScanTimeout: TimeInterval = 30
    static let reconnectDelays: [TimeInterval] = [0]
    static let reconnectCycleCooldown: TimeInterval = 1
    static let reconnectStuckStateLimit = 2

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

    static func acceptsPhoneProxyOperation(
        activeOperationID: UInt64?, callbackOperationID: UInt64
    ) -> Bool {
        activeOperationID == callbackOperationID
    }

    static func reconnectDelay(attempt: Int) -> TimeInterval? {
        guard reconnectDelays.indices.contains(attempt) else { return nil }
        return reconnectDelays[attempt]
    }

    static func reconnectAttempt(
        _ current: Int, secureNotificationsReady: Bool
    ) -> Int {
        secureNotificationsReady ? 0 : current
    }

    static func reconnectStateAction(
        _ state: CBPeripheralState
    ) -> ReconnectStateAction {
        switch state {
        case .disconnected:
            return .scan
        case .connected:
            return .prepare
        case .connecting, .disconnecting:
            return .wait
        @unknown default:
            return .wait
        }
    }

    static func bluetoothIsUnavailable(_ state: CBManagerState) -> Bool {
        switch state {
        case .unsupported, .unauthorized, .poweredOff:
            return true
        case .unknown, .resetting, .poweredOn:
            return false
        @unknown default:
            return true
        }
    }

    static func mustResetCentralAfterReconnectWait(
        state: CBPeripheralState, waitCount: Int
    ) -> Bool {
        state == .disconnecting && waitCount >= reconnectStuckStateLimit
    }

    static func shouldRefreshMemories(isProvisioning: Bool) -> Bool {
        !isProvisioning
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

    static func shouldRequestDeviceContextLocation(
        isDeviceConnected: Bool
    ) -> Bool {
        isDeviceConnected
    }

    static func shouldClearMemoryDraft(
        submitted: String, current: String, wasAdded: Bool
    ) -> Bool {
        wasAdded && current == submitted
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

    private static let service = CBUUID(string: ProvisioningProtocolV4.serviceUUID)
    private static let control = CBUUID(string: ProvisioningProtocolV4.controlUUID)
    private static let data = CBUUID(string: ProvisioningProtocolV4.dataUUID)
    private static let acknowledgement = CBUUID(string: ProvisioningProtocolV4.acknowledgementUUID)
    private static let deviceContext = CBUUID(string: ProvisioningProtocolV4.deviceContextUUID)
    private static let memoryCommand = CBUUID(string: MemoryProtocolV1.commandUUID)
    private static let memoryResponse = CBUUID(string: MemoryProtocolV1.responseUUID)
    private static let httpProxyService = CBUUID(
        string: PhoneProxyProtocolV1.serviceUUID)
    private static let httpProxyRequest = CBUUID(
        string: PhoneProxyProtocolV1.requestUUID)
    private static let httpProxyResponse = CBUUID(
        string: PhoneProxyProtocolV1.responseUUID)

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
    private var httpProxyRequestCharacteristic: CBCharacteristic?
    private var httpProxyResponseCharacteristic: CBCharacteristic?
    private var serviceDiscoveryInProgress = false
    private var serviceRediscoveryNeeded = false
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
    private var reconnectCooldownWork: DispatchWorkItem?
    private var reconnectAttempt = 0
    private var reconnectWaitCount = 0
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
    private var phoneProxyAssembler = PhoneProxyRequestAssembler()
    private var nextPhoneProxyOperationID: UInt64 = 1
    private var phoneProxyOperationID: UInt64?
    private var phoneProxyTask: Task<Void, Never>?
    private var phoneProxySession: URLSession?
    private var phoneProxyResponseTimeoutWork: DispatchWorkItem?
    private var phoneProxyRequestID: UInt32?
    private var phoneProxyResponseData = Data()
    private var phoneProxyResponseOffset = 0
    private var phoneProxyResponseExpectedSize: Int?
    private var phoneProxyDownloadComplete = false
    private var phoneProxyPendingErrorCode: UInt8?
    private var phoneProxyResponseEndSent = false
    private var phoneProxyErrorOnly = false
    private var phoneProxyPendingFrame: Data?
    private var phoneProxyWaitingForWriteResponse = false
    private var phoneProxyBackgroundTask = UIBackgroundTaskIdentifier.invalid
    private let bleLogger = Logger(
        subsystem: "org.chatesp.companion", category: "BLE")

    init(selectedDeviceIdentifier: UUID? = nil) {
        desiredSelectedID = selectedDeviceIdentifier
        super.init()
        locationManager.delegate = self
        locationManager.desiredAccuracy = kCLLocationAccuracyThreeKilometers
        locationManager.distanceFilter = 5_000
        _ = central
        trace(
            "provisioner_ready_selection_\(selectedDeviceIdentifier == nil ? 0 : 1)")
    }

    private func trace(_ event: String, error: Error? = nil) {
        if let error {
            let value = error as NSError
            bleLogger.error(
                "event=\(event, privacy: .public) error_domain=\(value.domain, privacy: .public) error_code=\(value.code)")
#if DEBUG
            print(
                "BLE_TRACE event=\(event) error_domain=\(value.domain) " +
                "error_code=\(value.code)")
#endif
        } else {
            bleLogger.notice("event=\(event, privacy: .public)")
#if DEBUG
            print("BLE_TRACE event=\(event)")
#endif
        }
    }

    private func cancelConnection(
        _ peripheral: CBPeripheral, reason: String
    ) {
        trace("cancel_\(reason)")
        central.cancelPeripheralConnection(peripheral)
    }

    deinit {
        contextTimer?.invalidate()
        scanTimeoutWork?.cancel()
        connectionTimeoutWork?.cancel()
        deviceContextTimeoutWork?.cancel()
        freshLocationWaitWork?.cancel()
        reconnectWork?.cancel()
        reconnectScanTimeoutWork?.cancel()
        reconnectCooldownWork?.cancel()
        memoryTimeoutWork?.cancel()
        phoneProxyResponseTimeoutWork?.cancel()
        phoneProxyTask?.cancel()
        phoneProxySession?.invalidateAndCancel()
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
        reconnectCooldownWork?.cancel()
        reconnectCooldownWork = nil
        scanTimeoutWork?.cancel()
        scanWhenReady = false
        discoveredDevices = []
        discovered = [:]
        phase = .scanning
        trace("scan_start")
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
        trace("restore_selection_\(identifier == nil ? 0 : 1)")
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
                cancelConnection(peripheral, reason: "restore_removed")
            }
            restoredPeripherals = [:]
            return
        }
        trace("restore_central_state_\(central.state.rawValue)")
        if selectedID == identifier, let selected {
            trace("restore_existing_state_\(selected.state.rawValue)")
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
                    reconnectCooldownWork?.cancel()
                    reconnectCooldownWork = nil
                    reconnectAttempt = 0
                    connectSelected()
                }
            }
            return
        }
        if let peripheral = restoredPeripherals[identifier] ??
            central.retrievePeripherals(withIdentifiers: [identifier]).first {
            trace("restore_retrieved_state_\(peripheral.state.rawValue)")
            select(peripheral, notifySelectionChange: false)
        } else if central.state == .poweredOn {
            trace("restore_scan_required")
            startScan()
        } else {
            trace("restore_waiting_for_central")
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
        reconnectCooldownWork?.cancel()
        reconnectCooldownWork = nil
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
        serviceDiscoveryInProgress = false
        phase = .idle
        if let oldSelection {
            oldSelection.delegate = nil
            cancelConnection(oldSelection, reason: "device_forgotten")
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
        trace(
            "select_state_\(peripheral.state.rawValue)_same_" +
            "\(selected === peripheral ? 1 : 0)")
        if selected === peripheral,
           selectedID == peripheral.identifier {
            desiredSelectedID = peripheral.identifier
            peripheral.delegate = self
            isDeviceConnected = peripheral.state == .connected
            if notifySelectionChange {
                onSelectedDeviceChanged?(peripheral.identifier)
            }
            if peripheral.state == .connected {
                if !serviceDiscoveryInProgress,
                   controlCharacteristic == nil {
                    prepareCharacteristics(on: peripheral)
                }
            } else if reconnectWork == nil,
                      reconnectScanTimeoutWork == nil {
                connectSelected()
            }
            return
        }
        reconnectWork?.cancel()
        reconnectWork = nil
        reconnectScanTimeoutWork?.cancel()
        reconnectScanTimeoutWork = nil
        reconnectCooldownWork?.cancel()
        reconnectCooldownWork = nil
        connectionTimeoutWork?.cancel()
        connectionTimeoutWork = nil
        let oldSelection = selected
        selected = peripheral
        selectedID = peripheral.identifier
        desiredSelectedID = peripheral.identifier
        if let oldSelection, oldSelection !== peripheral {
            oldSelection.delegate = nil
            cancelConnection(oldSelection, reason: "selection_changed")
        }
        reconnectAttempt = 0
        reconnectWaitCount = 0
        isDeviceConnected = peripheral.state == .connected
        serviceDiscoveryInProgress = false
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
            if controlCharacteristic != nil,
               dataCharacteristic != nil,
               acknowledgementCharacteristic != nil {
                beginAttempt()
            } else {
                prepareCharacteristics(on: selected)
            }
        } else {
            connectSelected()
        }
    }

    private func prepareCharacteristics(on peripheral: CBPeripheral) {
        guard isSelected(peripheral) else { return }
        guard !serviceDiscoveryInProgress else {
            trace("service_discovery_already_running")
            return
        }
        connectionTimeoutWork?.cancel()
        connectionTimeoutWork = nil
        phase = .pairing
        clearCharacteristics()
        clearMemoryConnectionState()
        serviceDiscoveryInProgress = true
        trace("service_discovery_start")
        peripheral.discoverServices([Self.service, Self.httpProxyService])
        startServiceDiscoveryTimeout(for: peripheral)
    }

    private func startServiceDiscoveryTimeout(for peripheral: CBPeripheral) {
        connectionTimeoutWork?.cancel()
        let work = DispatchWorkItem { [weak self, weak peripheral] in
            guard let self, let peripheral,
                  self.isSelected(peripheral),
                  self.serviceDiscoveryInProgress else { return }
            self.connectionTimeoutWork = nil
            self.serviceDiscoveryInProgress = false
            self.trace("service_discovery_timeout")
            self.cancelConnection(
                peripheral, reason: "service_discovery_timeout")
            if self.pendingPacket != nil {
                self.finish(
                    .failure(ProvisioningError.timeout),
                    sendContext: false)
            } else {
                self.phase = .failed(
                    "The ChatESP device did not complete Bluetooth setup. Retrying.")
                self.scheduleReconnect()
            }
        }
        connectionTimeoutWork = work
        DispatchQueue.main.asyncAfter(
            deadline: .now() + BLEProvisionerPolicy.serviceDiscoveryTimeout,
            execute: work)
    }

    private func clearCharacteristics() {
        controlCharacteristic = nil
        dataCharacteristic = nil
        acknowledgementCharacteristic = nil
        deviceContextCharacteristic = nil
        memoryCommandCharacteristic = nil
        memoryResponseCharacteristic = nil
        httpProxyRequestCharacteristic = nil
        httpProxyResponseCharacteristic = nil
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

    func addMemory(
        _ fact: String, completion: @escaping (Bool) -> Void = { _ in }
    ) {
        guard MemoryProtocolV1.validFact(fact) else {
            memoryMessage = "Enter one fact of at most 128 UTF-8 bytes."
            completion(false)
            return
        }
        runMemoryMutation(
            operation: .add, fact: fact, completion: completion)
    }

    func deleteMemory(id: UInt32) {
        runMemoryMutation(operation: .delete, memoryID: id)
    }

    func clearAllMemories() {
        runMemoryMutation(operation: .clear)
    }

    private func clearMemoryConnectionState() {
        clearPhoneProxyConnectionState()
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

    private func clearPhoneProxyConnectionState() {
        phoneProxyOperationID = nil
        phoneProxyResponseTimeoutWork?.cancel()
        phoneProxyResponseTimeoutWork = nil
        let task = phoneProxyTask
        phoneProxyTask = nil
        let session = phoneProxySession
        phoneProxySession = nil
        phoneProxyAssembler.reset()
        phoneProxyRequestID = nil
        phoneProxyResponseData.removeAll(keepingCapacity: false)
        phoneProxyResponseOffset = 0
        phoneProxyResponseExpectedSize = nil
        phoneProxyDownloadComplete = false
        phoneProxyPendingErrorCode = nil
        phoneProxyResponseEndSent = false
        phoneProxyErrorOnly = false
        phoneProxyPendingFrame = nil
        phoneProxyWaitingForWriteResponse = false
        endPhoneProxyBackgroundTask()
        task?.cancel()
        session?.invalidateAndCancel()
    }

    private func endPhoneProxyBackgroundTask() {
        guard phoneProxyBackgroundTask != .invalid else { return }
        UIApplication.shared.endBackgroundTask(phoneProxyBackgroundTask)
        phoneProxyBackgroundTask = .invalid
    }

    private func handlePhoneProxyRequestFrame(_ frame: Data) {
        do {
            if let request = try phoneProxyAssembler.consume(frame) {
                startPhoneProxyRequest(request)
            }
        } catch {
            trace("phone_proxy_request_rejected", error: error)
            phoneProxyAssembler.reset()
            if phoneProxyOperationID == nil,
               frame.count >= PhoneProxyProtocolV1.commonHeaderSize {
                startPhoneProxyError(
                    requestID: frame.readBigEndianUInt32(at: 6), code: 3)
            }
        }
    }

    private func allocatePhoneProxyOperationID() -> UInt64 {
        let operationID = nextPhoneProxyOperationID
        nextPhoneProxyOperationID = nextPhoneProxyOperationID == UInt64.max
            ? 1
            : nextPhoneProxyOperationID + 1
        return operationID
    }

    private func isActivePhoneProxyOperation(_ operationID: UInt64) -> Bool {
        BLEProvisionerPolicy.acceptsPhoneProxyOperation(
            activeOperationID: phoneProxyOperationID,
            callbackOperationID: operationID)
    }

    private func beginPhoneProxyOperation(requestID: UInt32) -> UInt64? {
        guard phoneProxyOperationID == nil,
              phoneProxyRequestID == nil else { return nil }
        let operationID = allocatePhoneProxyOperationID()
        phoneProxyOperationID = operationID
        phoneProxyRequestID = requestID
        return operationID
    }

    private func startPhoneProxyError(requestID: UInt32, code: UInt8) {
        guard requestID != 0,
              let operationID = beginPhoneProxyOperation(
                requestID: requestID) else { return }
        sendPhoneProxyError(
            operationID: operationID, requestID: requestID, code: code)
    }

    private func startPhoneProxyRequest(_ proxyRequest: PhoneProxyRequest) {
        guard phoneProxyTask == nil,
              let operationID = beginPhoneProxyOperation(
                requestID: proxyRequest.requestID) else { return }
        trace("phone_proxy_request_begin")
        phoneProxyBackgroundTask = UIApplication.shared.beginBackgroundTask(
            withName: "ChatESP phone proxy"
        ) { [weak self] in
            Task { @MainActor in
                guard let self,
                      self.isActivePhoneProxyOperation(operationID) else {
                    return
                }
                self.phoneProxyTask?.cancel()
                self.phoneProxySession?.invalidateAndCancel()
            }
        }
        let configuration = URLSessionConfiguration.ephemeral
        configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
        configuration.timeoutIntervalForRequest =
            proxyRequest.request.timeoutInterval
        configuration.timeoutIntervalForResource =
            proxyRequest.request.timeoutInterval
        configuration.waitsForConnectivity = true
        let session = URLSession(
            configuration: configuration,
            delegate: PhoneProxySessionDelegate(
                allowsRedirects: proxyRequest.allowsRedirects,
                maximumRedirects: proxyRequest.maximumRedirects),
            delegateQueue: nil)
        phoneProxySession = session
        phoneProxyTask = Task.detached(priority: .userInitiated) { [weak self] in
            guard let self else { return }
            do {
                let (bytes, response) = try await session.bytes(
                    for: proxyRequest.request)
                try Task.checkCancellation()
                guard let response = response as? HTTPURLResponse else {
                    await self.sendPhoneProxyError(
                        operationID: operationID,
                        requestID: proxyRequest.requestID,
                        code: 3)
                    return
                }
                let expectedLength = response.expectedContentLength
                let contentType = response.value(
                    forHTTPHeaderField: "Content-Type") ?? ""
                if expectedLength > Int64(proxyRequest.maximumResponseSize) {
                    await self.sendPhoneProxyError(
                        operationID: operationID,
                        requestID: proxyRequest.requestID,
                        code: 2)
                    return
                }
                if let contentLength =
                    PhoneProxyProtocolV1.streamablePCMResponseLength(
                        contentType: contentType,
                        expectedLength,
                        maximumResponseSize:
                            proxyRequest.maximumResponseSize) {
                    try await self.beginPhoneProxyResponse(
                        operationID: operationID,
                        requestID: proxyRequest.requestID,
                        response: response,
                        contentLength: contentLength)
                    var chunk = Data()
                    chunk.reserveCapacity(
                        PhoneProxyProtocolV1.streamingChunkSize)
                    for try await byte in bytes {
                        chunk.append(byte)
                        if chunk.count ==
                            PhoneProxyProtocolV1.streamingChunkSize {
                            try await self.appendPhoneProxyResponse(
                                operationID: operationID, chunk)
                            chunk.removeAll(keepingCapacity: true)
                            await Task.yield()
                        }
                    }
                    if !chunk.isEmpty {
                        try await self.appendPhoneProxyResponse(
                            operationID: operationID, chunk)
                    }
                    try await self.completePhoneProxyResponseDownload(
                        operationID: operationID)
                } else {
                    var data = Data()
                    for try await byte in bytes {
                        guard data.count <
                                proxyRequest.maximumResponseSize else {
                            session.invalidateAndCancel()
                            await self.sendPhoneProxyError(
                                operationID: operationID,
                                requestID: proxyRequest.requestID,
                                code: 2)
                            return
                        }
                        data.append(byte)
                    }
                    try await self.sendPhoneProxyResponse(
                        operationID: operationID,
                        requestID: proxyRequest.requestID,
                        response: response,
                        data: data)
                }
            } catch is CancellationError {
                if await self.isActivePhoneProxyOperation(operationID),
                   await self.isDeviceConnected {
                    await self.sendPhoneProxyError(
                        operationID: operationID,
                        requestID: proxyRequest.requestID,
                        code: 3)
                } else {
                    await self.finishPhoneProxyResponse(
                        operationID: operationID)
                }
            } catch {
                let code: UInt8 = (error as? URLError)?.code == .timedOut
                    ? 1
                    : 3
                await self.sendPhoneProxyError(
                    operationID: operationID,
                    requestID: proxyRequest.requestID,
                    code: code)
            }
        }
    }

    private func sendPhoneProxyResponse(
        operationID: UInt64,
        requestID: UInt32,
        response: HTTPURLResponse,
        data: Data
    ) throws {
        try beginPhoneProxyResponse(
            operationID: operationID,
            requestID: requestID,
            response: response,
            contentLength: data.count)
        try appendPhoneProxyResponse(operationID: operationID, data)
        try completePhoneProxyResponseDownload(operationID: operationID)
    }

    private func beginPhoneProxyResponse(
        operationID: UInt64,
        requestID: UInt32,
        response: HTTPURLResponse,
        contentLength: Int
    ) throws {
        guard isActivePhoneProxyOperation(operationID),
              phoneProxyRequestID == requestID,
              phoneProxyResponseExpectedSize == nil,
              contentLength >= 0 else {
            throw PhoneProxyError.invalidResponse
        }
        let contentType = response.value(
            forHTTPHeaderField: "Content-Type") ?? ""
        let date = response.value(forHTTPHeaderField: "Date") ?? ""
        let head = try PhoneProxyProtocolV1.responseHead(
            requestID: requestID,
            status: response.statusCode,
            contentLength: contentLength,
            contentType: contentType,
            date: date)
        phoneProxyResponseData.removeAll(keepingCapacity: false)
        if contentLength > 0 {
            phoneProxyResponseData.reserveCapacity(contentLength)
        }
        phoneProxyResponseOffset = 0
        phoneProxyResponseExpectedSize = contentLength
        phoneProxyDownloadComplete = false
        phoneProxyPendingErrorCode = nil
        phoneProxyResponseEndSent = false
        phoneProxyErrorOnly = false
        queuePhoneProxyFrame(operationID: operationID, head)
    }

    private func appendPhoneProxyResponse(
        operationID: UInt64, _ data: Data
    ) throws {
        guard isActivePhoneProxyOperation(operationID),
              let expectedSize = phoneProxyResponseExpectedSize,
              phoneProxyResponseData.count <= expectedSize,
              data.count <= expectedSize - phoneProxyResponseData.count else {
            throw PhoneProxyError.invalidResponse
        }
        phoneProxyResponseData.append(data)
        if phoneProxyPendingFrame == nil {
            sendNextPhoneProxyResponseFrame(operationID: operationID)
        }
    }

    private func completePhoneProxyResponseDownload(
        operationID: UInt64
    ) throws {
        guard isActivePhoneProxyOperation(operationID),
              let expectedSize = phoneProxyResponseExpectedSize,
              phoneProxyResponseData.count == expectedSize else {
            throw PhoneProxyError.invalidResponse
        }
        phoneProxyDownloadComplete = true
        if phoneProxyPendingFrame == nil {
            sendNextPhoneProxyResponseFrame(operationID: operationID)
        }
    }

    private func sendPhoneProxyError(
        operationID: UInt64, requestID: UInt32, code: UInt8
    ) {
        guard isActivePhoneProxyOperation(operationID),
              phoneProxyRequestID == requestID else { return }
        phoneProxyPendingErrorCode = code
        phoneProxyDownloadComplete = true
        if phoneProxyPendingFrame == nil {
            sendNextPhoneProxyResponseFrame(operationID: operationID)
        }
    }

    private func queuePhoneProxyFrame(
        operationID: UInt64, _ frame: Data
    ) {
        guard isActivePhoneProxyOperation(operationID) else { return }
        startPhoneProxyResponseTimeout(operationID: operationID)
        guard phoneProxyPendingFrame == nil else {
            finishPhoneProxyResponse(
                operationID: operationID, cancelNetwork: true)
            return
        }
        phoneProxyPendingFrame = frame
        drainPhoneProxyFrame(operationID: operationID)
    }

    private func drainPhoneProxyFrame(operationID: UInt64) {
        guard isActivePhoneProxyOperation(operationID),
              let selected,
              let characteristic = httpProxyResponseCharacteristic,
              isActive(selected),
              let frame = phoneProxyPendingFrame else {
            finishPhoneProxyResponse(
                operationID: operationID, cancelNetwork: true)
            return
        }
        let type = phoneProxyWriteType(for: frame)
        guard frame.count <= selected.maximumWriteValueLength(for: type) else {
            finishPhoneProxyResponse(
                operationID: operationID, cancelNetwork: true)
            return
        }
        if type == .withResponse {
            guard !phoneProxyWaitingForWriteResponse else { return }
            phoneProxyWaitingForWriteResponse = true
            selected.writeValue(frame, for: characteristic, type: type)
        } else {
            guard selected.canSendWriteWithoutResponse else { return }
            selected.writeValue(frame, for: characteristic, type: type)
            phoneProxyPendingFrame = nil
            DispatchQueue.main.async { [weak self] in
                self?.sendNextPhoneProxyResponseFrame(
                    operationID: operationID)
            }
        }
    }

    private func phoneProxyWriteType(
        for frame: Data
    ) -> CBCharacteristicWriteType {
        guard frame.count > 5,
              frame[5] == PhoneProxyProtocolV1.FrameType.responseData.rawValue else {
            return .withResponse
        }
        return .withoutResponse
    }

    private func sendNextPhoneProxyResponseFrame(operationID: UInt64) {
        guard isActivePhoneProxyOperation(operationID),
              let requestID = phoneProxyRequestID,
              let selected else {
            finishPhoneProxyResponse(operationID: operationID)
            return
        }
        if phoneProxyErrorOnly {
            finishPhoneProxyResponse(operationID: operationID)
            return
        }
        if let errorCode = phoneProxyPendingErrorCode {
            phoneProxyPendingErrorCode = nil
            phoneProxyErrorOnly = true
            queuePhoneProxyFrame(
                operationID: operationID,
                PhoneProxyProtocolV1.responseError(
                    requestID: requestID, code: errorCode))
            return
        }
        if phoneProxyResponseOffset < phoneProxyResponseData.count {
            let capacity = selected.maximumWriteValueLength(
                for: .withoutResponse) - PhoneProxyProtocolV1.dataHeaderSize
            guard capacity > 0 else {
                finishPhoneProxyResponse(
                    operationID: operationID, cancelNetwork: true)
                return
            }
            let end = min(
                phoneProxyResponseData.count,
                phoneProxyResponseOffset + capacity)
            let bytes = phoneProxyResponseData.subdata(
                in: phoneProxyResponseOffset..<end)
            let frame = PhoneProxyProtocolV1.responseData(
                requestID: requestID,
                offset: phoneProxyResponseOffset,
                bytes: bytes)
            phoneProxyResponseOffset = end
            queuePhoneProxyFrame(operationID: operationID, frame)
            return
        }
        if !phoneProxyDownloadComplete {
            return
        }
        if !phoneProxyResponseEndSent {
            phoneProxyResponseEndSent = true
            queuePhoneProxyFrame(
                operationID: operationID,
                PhoneProxyProtocolV1.responseEnd(
                    requestID: requestID,
                    size: phoneProxyResponseExpectedSize ??
                        phoneProxyResponseData.count))
            return
        }
        trace("phone_proxy_request_complete")
        finishPhoneProxyResponse(operationID: operationID)
    }

    private func startPhoneProxyResponseTimeout(operationID: UInt64) {
        guard isActivePhoneProxyOperation(operationID),
              phoneProxyResponseTimeoutWork == nil else { return }
        let work = DispatchWorkItem { [weak self] in
            guard let self,
                  self.isActivePhoneProxyOperation(operationID) else { return }
            self.trace("phone_proxy_response_timeout")
            let peripheral = self.selected
            self.finishPhoneProxyResponse(
                operationID: operationID, cancelNetwork: true)
            if let peripheral, self.isSelected(peripheral) {
                self.cancelConnection(
                    peripheral, reason: "phone_proxy_response_timeout")
            }
        }
        phoneProxyResponseTimeoutWork = work
        DispatchQueue.main.asyncAfter(
            deadline: .now() + BLEProvisionerPolicy.phoneProxyResponseTimeout,
            execute: work)
    }

    private func finishPhoneProxyResponse(
        operationID: UInt64, cancelNetwork: Bool = false
    ) {
        guard isActivePhoneProxyOperation(operationID) else { return }
        phoneProxyOperationID = nil
        phoneProxyResponseTimeoutWork?.cancel()
        phoneProxyResponseTimeoutWork = nil
        let task = phoneProxyTask
        let session = phoneProxySession
        phoneProxySession = nil
        phoneProxyTask = nil
        phoneProxyRequestID = nil
        phoneProxyResponseData.removeAll(keepingCapacity: false)
        phoneProxyResponseOffset = 0
        phoneProxyResponseExpectedSize = nil
        phoneProxyDownloadComplete = false
        phoneProxyPendingErrorCode = nil
        phoneProxyResponseEndSent = false
        phoneProxyErrorOnly = false
        phoneProxyPendingFrame = nil
        phoneProxyWaitingForWriteResponse = false
        endPhoneProxyBackgroundTask()
        if cancelNetwork {
            task?.cancel()
            session?.invalidateAndCancel()
        } else {
            session?.finishTasksAndInvalidate()
        }
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
        fact: String = "",
        completion: ((Bool) -> Void)? = nil
    ) {
        guard memoryAvailable else {
            memoryMessage = isDeviceConnected
                ? "Update the ChatESP device firmware to manage memories."
                : "Connect a ChatESP device to manage memories."
            completion?(false)
            return
        }
        guard pendingMemoryCommand == nil else {
            memoryMessage = "A memory request is already in progress."
            completion?(false)
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
                    completion?(false)
                case .success(let response):
                    switch response.status {
                    case .applied, .unchanged:
                        self.memoryRevision = response.revision
                        self.memoryFingerprint = response.fingerprint
                        completion?(true)
                        self.refreshMemories()
                    case .full:
                        self.memoryMessage =
                            "Ask ChatESP to compact memories, or remove a fact."
                        completion?(false)
                    case .revisionConflict:
                        self.memoryMessage =
                            "Memories changed on the ChatESP device. The list was reloaded."
                        completion?(false)
                        self.refreshMemories()
                    default:
                        self.memoryMessage = self.memoryStatusText(response.status)
                        completion?(false)
                    }
                }
            }
        } catch {
            memoryMessage = memoryErrorText(error)
            completion?(false)
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
        guard BLEProvisionerPolicy.shouldRequestDeviceContextLocation(
            isDeviceConnected: isDeviceConnected) else { return }
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
        if reconnectWork != nil {
            trace("reconnect_skip_work")
            return
        }
        if reconnectScanTimeoutWork != nil {
            trace("reconnect_skip_scan")
            return
        }
        if reconnectCooldownWork != nil {
            trace("reconnect_skip_cooldown")
            return
        }
        if pendingPacket != nil {
            trace("reconnect_skip_transfer")
            return
        }
        guard let selected else {
            trace("reconnect_skip_selection")
            return
        }
        guard let delay = BLEProvisionerPolicy.reconnectDelay(
                attempt: reconnectAttempt) else {
            phase = .unavailable
            scheduleReconnectCooldown()
            return
        }
        reconnectAttempt += 1
        trace(
            "reconnect_scheduled_\(reconnectAttempt)_state_" +
            "\(selected.state.rawValue)")
        let work = DispatchWorkItem { [weak self, weak selected] in
            guard let self else { return }
            self.reconnectWork = nil
            guard let selected,
                  self.isSelected(selected),
                  self.central.state == .poweredOn else { return }
            self.trace("reconnect_run_state_\(selected.state.rawValue)")
            switch BLEProvisionerPolicy.reconnectStateAction(selected.state) {
            case .scan:
                self.reconnectWaitCount = 0
                self.startReconnectScan()
            case .prepare:
                self.reconnectWaitCount = 0
                self.connectSelected()
            case .wait:
                if selected.state == .connecting {
                    self.cancelConnection(
                        selected, reason: "reconnect_waiting_for_cancel")
                }
                self.reconnectWaitCount += 1
                if BLEProvisionerPolicy.mustResetCentralAfterReconnectWait(
                    state: selected.state,
                    waitCount: self.reconnectWaitCount
                ) {
                    self.resetCentralForReconnect()
                } else {
                    self.scheduleReconnect()
                }
            }
        }
        reconnectWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: work)
    }

    private func scheduleReconnectCooldown() {
        guard reconnectCooldownWork == nil,
              pendingPacket == nil,
              selectedID != nil else { return }
        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.reconnectCooldownWork = nil
            guard self.selected != nil,
                  !self.isDeviceConnected,
                  self.central.state == .poweredOn else { return }
            self.reconnectAttempt = 0
            self.scheduleReconnect()
        }
        reconnectCooldownWork = work
        DispatchQueue.main.asyncAfter(
            deadline: .now() + BLEProvisionerPolicy.reconnectCycleCooldown,
            execute: work)
    }

    private func connectSelected() {
        guard let selected, central.state == .poweredOn else {
            phase = .failed(ProvisioningError.bluetoothUnavailable.localizedDescription)
            return
        }
        phase = .connecting
        selected.delegate = self
        trace("connect_state_\(selected.state.rawValue)")
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

    private func resetCentralForReconnect() {
        guard desiredSelectedID != nil else { return }
        trace("central_reset_stuck_disconnect")
        reconnectWork?.cancel()
        reconnectWork = nil
        reconnectScanTimeoutWork?.cancel()
        reconnectScanTimeoutWork = nil
        reconnectCooldownWork?.cancel()
        reconnectCooldownWork = nil
        connectionTimeoutWork?.cancel()
        connectionTimeoutWork = nil
        central.stopScan()
        central.delegate = nil
        selected?.delegate = nil
        selected = nil
        restoredPeripherals = [:]
        discovered = [:]
        discoveredDevices = []
        isDeviceConnected = false
        serviceDiscoveryInProgress = false
        clearMemoryConnectionState()
        clearCharacteristics()
        resetDeviceContextTransfer()
        reconnectAttempt = 0
        reconnectWaitCount = 0
        scanWhenReady = true
        phase = .connecting
        central = CBCentralManager(
            delegate: self,
            queue: .main,
            options: [
                CBCentralManagerOptionRestoreIdentifierKey:
                    "com.chatesp.provisioning.central"
            ])
    }

    private func startConnectionTimeout(for peripheral: CBPeripheral) {
        connectionTimeoutWork?.cancel()
        let work = DispatchWorkItem { [weak self, weak peripheral] in
            guard let self, let peripheral, self.isSelected(peripheral),
                  peripheral.state != .connected else { return }
            self.connectionTimeoutWork = nil
            self.cancelConnection(peripheral, reason: "connection_timeout")
            if self.pendingPacket != nil {
                self.finish(.failure(ProvisioningError.timeout), sendContext: false)
            } else {
                self.phase = .unavailable
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
        phase = .unavailable
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
                    self.cancelConnection(selected, reason: "transfer_timeout")
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
        if memoryAvailable, memoryRefreshNeeded,
           pendingMemoryCommand == nil {
            memoryRefreshNeeded = false
            refreshMemories()
        }
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
            guard central === self.central else { return }
            self.trace("central_state_\(central.state.rawValue)")
            if central.state == .poweredOn, self.scanWhenReady {
                self.trace("central_resume_pending_selection")
                if let identifier = self.desiredSelectedID,
                   let peripheral = self.restoredPeripherals[identifier] ??
                    self.central.retrievePeripherals(
                        withIdentifiers: [identifier]).first {
                    self.select(peripheral, notifySelectionChange: false)
                } else {
                    self.trace("central_resume_scan")
                    self.startScan()
                }
                return
            }
            if central.state == .poweredOn,
               self.selected == nil,
               let identifier = self.desiredSelectedID {
                self.trace("central_restore_saved_selection")
                if let peripheral = self.restoredPeripherals[identifier] ??
                    self.central.retrievePeripherals(
                        withIdentifiers: [identifier]).first {
                    self.select(peripheral, notifySelectionChange: false)
                } else {
                    self.trace("central_restore_scan")
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
            if BLEProvisionerPolicy.bluetoothIsUnavailable(central.state) {
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
                self.reconnectCooldownWork?.cancel()
                self.reconnectCooldownWork = nil
                self.isDeviceConnected = false
                self.serviceDiscoveryInProgress = false
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
            guard central === self.central else { return }
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
            guard central === self.central else { return }
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
            guard central === self.central else { return }
            guard self.isSelected(peripheral) else {
                if self.restoredPeripherals[peripheral.identifier] !== peripheral {
                    self.cancelConnection(
                        peripheral, reason: "unexpected_connection")
                }
                return
            }
            self.trace("link_connected")
            self.reconnectWork?.cancel()
            self.reconnectWork = nil
            self.reconnectScanTimeoutWork?.cancel()
            self.reconnectScanTimeoutWork = nil
            self.reconnectCooldownWork?.cancel()
            self.reconnectCooldownWork = nil
            self.connectionTimeoutWork?.cancel()
            self.connectionTimeoutWork = nil
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
            guard central === self.central else { return }
            guard self.isSelected(peripheral) else { return }
            self.trace("link_connect_failed", error: error)
            self.connectionTimeoutWork?.cancel()
            self.connectionTimeoutWork = nil
            self.isDeviceConnected = false
            self.serviceDiscoveryInProgress = false
            self.clearMemoryConnectionState()
            if self.pendingPacket != nil {
                self.finish(.failure(error ?? ProvisioningError.disconnected))
            } else {
                self.phase = .unavailable
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
            guard central === self.central else { return }
            guard self.isSelected(peripheral) else { return }
            self.trace("link_disconnected", error: error)
            self.connectionTimeoutWork?.cancel()
            self.connectionTimeoutWork = nil
            self.isDeviceConnected = false
            self.serviceDiscoveryInProgress = false
            self.clearMemoryConnectionState()
            if self.pendingPacket != nil {
                self.finish(.failure(error ?? ProvisioningError.disconnected))
            } else if self.selectedID == peripheral.identifier {
                self.phase = .unavailable
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
                self.connectionTimeoutWork?.cancel()
                self.connectionTimeoutWork = nil
                self.serviceDiscoveryInProgress = false
                self.trace("service_discovery_failed", error: error)
                self.finish(.failure(error))
                return
            }
            guard let service = peripheral.services?.first(where: { $0.uuid == Self.service }) else {
                self.connectionTimeoutWork?.cancel()
                self.connectionTimeoutWork = nil
                self.serviceDiscoveryInProgress = false
                self.trace("service_missing")
                self.finish(.failure(ProvisioningError.missingService))
                return
            }
            self.trace("service_discovery_complete")
            if let proxyService = peripheral.services?.first(
                where: { $0.uuid == Self.httpProxyService }) {
                peripheral.discoverCharacteristics(
                    [Self.httpProxyRequest, Self.httpProxyResponse],
                    for: proxyService)
            } else {
                self.trace("phone_proxy_unavailable")
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
            if service.uuid == Self.httpProxyService {
                if let error {
                    self.trace("phone_proxy_discovery_failed", error: error)
                    return
                }
                self.httpProxyRequestCharacteristic =
                    service.characteristics?.first {
                        $0.uuid == Self.httpProxyRequest
                    }
                self.httpProxyResponseCharacteristic =
                    service.characteristics?.first {
                        $0.uuid == Self.httpProxyResponse
                    }
                if let proxyRequest = self.httpProxyRequestCharacteristic,
                   self.httpProxyResponseCharacteristic != nil {
                    self.trace("phone_proxy_notify_request")
                    peripheral.setNotifyValue(true, for: proxyRequest)
                } else {
                    self.trace("phone_proxy_unavailable")
                }
                return
            }
            self.connectionTimeoutWork?.cancel()
            self.connectionTimeoutWork = nil
            self.serviceDiscoveryInProgress = false
            if let error {
                self.trace("characteristic_discovery_failed", error: error)
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
            let requiredReady = self.controlCharacteristic != nil &&
                self.dataCharacteristic != nil &&
                self.acknowledgementCharacteristic != nil &&
                self.deviceContextCharacteristic != nil
            let memoryReady = self.memoryCommandCharacteristic != nil &&
                self.memoryResponseCharacteristic != nil
            self.trace(
                "characteristics_required_\(requiredReady ? 1 : 0)_memory_\(memoryReady ? 1 : 0)")
            if let response = self.memoryResponseCharacteristic,
               self.memoryCommandCharacteristic != nil {
                self.trace("memory_notify_request")
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
            if self.serviceRediscoveryNeeded {
                self.serviceRediscoveryNeeded = false
                DispatchQueue.main.async { [weak self, weak peripheral] in
                    guard let self, let peripheral,
                          self.isActive(peripheral) else { return }
                    self.prepareCharacteristics(on: peripheral)
                }
            }
        }
    }

    nonisolated func peripheral(
        _ peripheral: CBPeripheral,
        didModifyServices invalidatedServices: [CBService]
    ) {
        Task { @MainActor in
            guard self.isActive(peripheral),
                  invalidatedServices.contains(where: {
                    $0.uuid == Self.service ||
                        $0.uuid == Self.httpProxyService
                  }) else { return }
            self.trace("service_change_received")
            if self.serviceDiscoveryInProgress {
                self.serviceRediscoveryNeeded = true
            } else {
                self.prepareCharacteristics(on: peripheral)
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
                    self.trace("memory_notify_failed", error: error)
                    self.memoryAvailable = false
                    self.memories = []
                    self.memoryMessage = self.memoryErrorText(error)
                } else if characteristic.isNotifying,
                          self.memoryCommandCharacteristic != nil {
                    self.trace("memory_notify_ready")
                    self.memoryAvailable = true
                    if BLEProvisionerPolicy.shouldRefreshMemories(
                        isProvisioning: self.isProvisioning
                    ) {
                        self.memoryMessage = "Loading memories."
                        self.refreshMemories()
                    } else {
                        self.memoryRefreshNeeded = true
                    }
                }
                return
            }
            if characteristic.uuid == Self.httpProxyRequest {
                if let error {
                    self.trace("phone_proxy_notify_failed", error: error)
                    self.clearPhoneProxyConnectionState()
                } else if characteristic.isNotifying,
                          self.httpProxyResponseCharacteristic != nil {
                    self.trace("phone_proxy_ready")
                }
                return
            }
            if let error {
                self.trace("ack_notify_failed", error: error)
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
                self.trace("ack_notify_disabled")
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
            self.trace("ack_notify_ready")
            self.reconnectAttempt = BLEProvisionerPolicy.reconnectAttempt(
                self.reconnectAttempt, secureNotificationsReady: true)
            self.reconnectWaitCount = 0
            self.reconnectCooldownWork?.cancel()
            self.reconnectCooldownWork = nil
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
            if characteristic.uuid == Self.httpProxyResponse {
                guard let operationID = self.phoneProxyOperationID,
                      self.phoneProxyWaitingForWriteResponse else { return }
                self.phoneProxyWaitingForWriteResponse = false
                self.phoneProxyPendingFrame = nil
                if let error {
                    self.trace("phone_proxy_response_write_failed", error: error)
                    self.finishPhoneProxyResponse(
                        operationID: operationID, cancelNetwork: true)
                } else {
                    self.sendNextPhoneProxyResponseFrame(
                        operationID: operationID)
                }
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
                self.trace("write_failed", error: error)
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

    nonisolated func peripheralIsReady(
        toSendWriteWithoutResponse peripheral: CBPeripheral
    ) {
        Task { @MainActor in
            guard self.isActive(peripheral),
                  let operationID = self.phoneProxyOperationID else { return }
            self.drainPhoneProxyFrame(operationID: operationID)
        }
    }

    nonisolated func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        // The central manager dispatches peripheral events on its configured
        // main queue. Process each value before Core Bluetooth replaces the
        // characteristic property with the next notification.
        MainActor.assumeIsolated {
            guard self.isActive(peripheral),
                  characteristic.service?.peripheral === peripheral else {
                return
            }
            if characteristic.uuid == Self.httpProxyRequest {
                if let error {
                    self.trace("phone_proxy_receive_failed", error: error)
                    self.clearPhoneProxyConnectionState()
                    return
                }
                guard let data = characteristic.value else { return }
                self.handlePhoneProxyRequestFrame(data)
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
                self.trace("device_context_acknowledged")
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
                self.trace("settings_acknowledged")
                self.finish(.success(acknowledgement))
            } catch {
                self.trace("settings_ack_failed", error: error)
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
