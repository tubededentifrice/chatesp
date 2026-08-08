import XCTest
@testable import ChatESP

final class BLEProvisionerPolicyTests: XCTestCase {
    func testProvisioningRejectsASecondActiveRequest() {
        XCTAssertTrue(BLEProvisionerPolicy.canStartProvisioning(hasPendingRequest: false))
        XCTAssertFalse(BLEProvisionerPolicy.canStartProvisioning(hasPendingRequest: true))
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
        XCTAssertEqual(BLEProvisionerPolicy.reconnectDelays, [2, 4, 8, 16])
        XCTAssertNil(
            BLEProvisionerPolicy.reconnectDelay(
                attempt: BLEProvisionerPolicy.reconnectDelays.count))
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
}
