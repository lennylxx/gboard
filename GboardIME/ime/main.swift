import Cocoa
import InputMethodKit
import os

private let logger = Logger(subsystem: Bundle.main.bundleIdentifier!, category: "ime")

func imeLog(_ msg: String) {
    logger.debug("\(msg, privacy: .public)")
}

let kConnectionName = "GboardIME_Connection"
let bundleId = Bundle.main.bundleIdentifier!

imeLog("Starting — bundle=\(bundleId) conn=\(kConnectionName)")
let server = IMKServer(name: kConnectionName, bundleIdentifier: bundleId)
imeLog("IMKServer created: \(String(describing: server))")

NSApplication.shared.run()
