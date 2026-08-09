import CoreBluetooth
import Foundation
import XCTest
@testable import ChatESP

final class BLEProvisionerPolicyTests: XCTestCase {
    func testUnavailablePhaseDoesNotClaimAnActiveConnection() {
        XCTAssertEqual(
            ProvisioningPhase.unavailable.text,
            "The device is asleep or unavailable. Retrying.")
    }

    func testBluetoothUnavailableStateIsNotReportedAsConnecting() {
        XCTAssertTrue(BLEProvisionerPolicy.bluetoothIsUnavailable(.poweredOff))
        XCTAssertTrue(BLEProvisionerPolicy.bluetoothIsUnavailable(.unauthorized))
        XCTAssertTrue(BLEProvisionerPolicy.bluetoothIsUnavailable(.unsupported))
        XCTAssertFalse(BLEProvisionerPolicy.bluetoothIsUnavailable(.unknown))
        XCTAssertFalse(BLEProvisionerPolicy.bluetoothIsUnavailable(.resetting))
        XCTAssertFalse(BLEProvisionerPolicy.bluetoothIsUnavailable(.poweredOn))
    }

    func testPhoneProxyRedirectsStayHTTPSAndBounded() throws {
        let source = try XCTUnwrap(URL(string: "https://example.com/start"))
        let target = try XCTUnwrap(URL(string: "https://example.net/next"))
        let insecure = try XCTUnwrap(URL(string: "http://example.net/next"))
        let response = try XCTUnwrap(
            HTTPURLResponse(
                url: source,
                statusCode: 302,
                httpVersion: nil,
                headerFields: nil))
        let task = URLSession.shared.dataTask(with: source)
        let delegate = PhoneProxySessionDelegate(
            allowsRedirects: true, maximumRedirects: 2)

        var accepted: URLRequest?
        delegate.urlSession(
            .shared,
            task: task,
            willPerformHTTPRedirection: response,
            newRequest: URLRequest(url: target)
        ) { accepted = $0 }
        XCTAssertEqual(accepted?.url, target)
        delegate.urlSession(
            .shared,
            task: task,
            willPerformHTTPRedirection: response,
            newRequest: URLRequest(url: target)
        ) { accepted = $0 }
        XCTAssertEqual(accepted?.url, target)
        delegate.urlSession(
            .shared,
            task: task,
            willPerformHTTPRedirection: response,
            newRequest: URLRequest(url: target)
        ) { accepted = $0 }
        XCTAssertNil(accepted)

        let insecureDelegate = PhoneProxySessionDelegate(
            allowsRedirects: true, maximumRedirects: 2)
        insecureDelegate.urlSession(
            .shared,
            task: task,
            willPerformHTTPRedirection: response,
            newRequest: URLRequest(url: insecure)
        ) { accepted = $0 }
        XCTAssertNil(accepted)
    }

    func testProvisioningRejectsASecondActiveRequest() {
        XCTAssertTrue(BLEProvisionerPolicy.canStartProvisioning(hasPendingRequest: false))
        XCTAssertFalse(BLEProvisionerPolicy.canStartProvisioning(hasPendingRequest: true))
    }

    func testSelectionChangeCancelsOnlyAnActiveTransferForAnotherDevice() {
        let selected = UUID()

        XCTAssertFalse(
            BLEProvisionerPolicy.mustCancelProvisioningForSelectionChange(
                isProvisioning: false,
                selectedID: selected,
                requestedID: UUID()))
        XCTAssertFalse(
            BLEProvisionerPolicy.mustCancelProvisioningForSelectionChange(
                isProvisioning: true,
                selectedID: selected,
                requestedID: selected))
        XCTAssertTrue(
            BLEProvisionerPolicy.mustCancelProvisioningForSelectionChange(
                isProvisioning: true,
                selectedID: selected,
                requestedID: UUID()))
        XCTAssertTrue(
            BLEProvisionerPolicy.mustCancelProvisioningForSelectionChange(
                isProvisioning: true,
                selectedID: selected,
                requestedID: nil))
    }

    func testCallbacksOnlyMatchTheSelectedPeripheral() {
        let selected = UUID()

        XCTAssertTrue(
            BLEProvisionerPolicy.acceptsCallback(
                selectedID: selected,
                callbackID: selected))
        XCTAssertFalse(
            BLEProvisionerPolicy.acceptsCallback(
                selectedID: selected,
                callbackID: UUID()))
        XCTAssertFalse(
            BLEProvisionerPolicy.acceptsCallback(
                selectedID: nil,
                callbackID: selected))
    }

    func testScanAndReconnectOperationsHaveFixedBounds() {
        XCTAssertEqual(BLEProvisionerPolicy.scanTimeout, 10)
        XCTAssertEqual(BLEProvisionerPolicy.serviceDiscoveryTimeout, 10)
        XCTAssertEqual(BLEProvisionerPolicy.reconnectScanTimeout, 30)
        XCTAssertEqual(BLEProvisionerPolicy.reconnectDelays, [0])
        XCTAssertNil(
            BLEProvisionerPolicy.reconnectDelay(
                attempt: BLEProvisionerPolicy.reconnectDelays.count))
    }

    func testReconnectAttemptResetsOnlyAfterSecureNotifications() {
        XCTAssertEqual(
            BLEProvisionerPolicy.reconnectAttempt(
                3, secureNotificationsReady: false),
            3)
        XCTAssertEqual(
            BLEProvisionerPolicy.reconnectAttempt(
                3, secureNotificationsReady: true),
            0)
        XCTAssertEqual(BLEProvisionerPolicy.reconnectCycleCooldown, 1)
    }

    func testReconnectDoesNotStopOnAnIntermediatePeripheralState() {
        XCTAssertEqual(
            BLEProvisionerPolicy.reconnectStateAction(.disconnected),
            .scan)
        XCTAssertEqual(
            BLEProvisionerPolicy.reconnectStateAction(.connected),
            .prepare)
        XCTAssertEqual(
            BLEProvisionerPolicy.reconnectStateAction(.connecting),
            .wait)
        XCTAssertEqual(
            BLEProvisionerPolicy.reconnectStateAction(.disconnecting),
            .wait)
        XCTAssertFalse(
            BLEProvisionerPolicy.mustResetCentralAfterReconnectWait(
                state: .connecting, waitCount: 2))
        XCTAssertFalse(
            BLEProvisionerPolicy.mustResetCentralAfterReconnectWait(
                state: .disconnecting, waitCount: 1))
        XCTAssertTrue(
            BLEProvisionerPolicy.mustResetCentralAfterReconnectWait(
                state: .disconnecting, waitCount: 2))
    }

    func testMemoryRefreshWaitsForSettingsTransfer() {
        XCTAssertFalse(
            BLEProvisionerPolicy.shouldRefreshMemories(
                isProvisioning: true))
        XCTAssertTrue(
            BLEProvisionerPolicy.shouldRefreshMemories(
                isProvisioning: false))
    }

    func testReconnectAcceptsOnlyTheSelectedDeviceAdvertisement() {
        let selected = UUID()

        XCTAssertTrue(
            BLEProvisionerPolicy.shouldReconnect(
                selectedID: selected,
                desiredID: selected,
                discoveredID: selected,
                connected: false))
        XCTAssertFalse(
            BLEProvisionerPolicy.shouldReconnect(
                selectedID: selected,
                desiredID: selected,
                discoveredID: UUID(),
                connected: false))
        XCTAssertFalse(
            BLEProvisionerPolicy.shouldReconnect(
                selectedID: selected,
                desiredID: nil,
                discoveredID: selected,
                connected: false))
        XCTAssertFalse(
            BLEProvisionerPolicy.shouldReconnect(
                selectedID: selected,
                desiredID: selected,
                discoveredID: selected,
                connected: true))
    }

    func testEachTransferFrameHasADeadline() {
        XCTAssertEqual(BLEProvisionerPolicy.frameTimeout, 10)
        XCTAssertGreaterThan(BLEProvisionerPolicy.frameTimeout, 0)
        XCTAssertEqual(
            BLEProvisionerPolicy.transferTimeoutAction(
                frameIndex: 1,
                frameCount: 3),
            .failConnection)
        XCTAssertEqual(
            BLEProvisionerPolicy.transferTimeoutAction(
                frameIndex: 3,
                frameCount: 3),
            .retryCompleteTransfer)
    }

    func testLiveLocationRequiresCurrentAuthorizationAndFreshData() {
        XCTAssertTrue(
            BLEProvisionerPolicy.hasFreshLocation(
                authorizationAllowed: true,
                updatedAt: 100,
                now: 1_000))
        XCTAssertFalse(
            BLEProvisionerPolicy.hasFreshLocation(
                authorizationAllowed: false,
                updatedAt: 999,
                now: 1_000))
        XCTAssertFalse(
            BLEProvisionerPolicy.hasFreshLocation(
                authorizationAllowed: true,
                updatedAt: 99,
                now: 1_000))
    }

    func testDeviceContextIntervalUsesMonotonicElapsedTime() {
        XCTAssertEqual(BLEProvisionerPolicy.deviceContextInterval, 3_600)
        XCTAssertTrue(
            BLEProvisionerPolicy.deviceContextSyncIsDue(
                lastSentAt: nil,
                now: 100))
        XCTAssertFalse(
            BLEProvisionerPolicy.deviceContextSyncIsDue(
                lastSentAt: 100,
                now: 3_699))
        XCTAssertTrue(
            BLEProvisionerPolicy.deviceContextSyncIsDue(
                lastSentAt: 100,
                now: 3_700))
        XCTAssertFalse(
            BLEProvisionerPolicy.deviceContextSyncIsDue(
                lastSentAt: 100,
                now: 99))
    }
}
