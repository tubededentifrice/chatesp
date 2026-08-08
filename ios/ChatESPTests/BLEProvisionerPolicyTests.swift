import XCTest
@testable import ChatESP

final class BLEProvisionerPolicyTests: XCTestCase {
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
        XCTAssertEqual(BLEProvisionerPolicy.reconnectScanTimeout, 10)
        XCTAssertEqual(BLEProvisionerPolicy.reconnectDelays, [2, 4, 8, 16])
        XCTAssertNil(
            BLEProvisionerPolicy.reconnectDelay(
                attempt: BLEProvisionerPolicy.reconnectDelays.count))
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
