import Cocoa
import InputMethodKit

func imeLog(_ msg: String) {
    let path = "/tmp/gboardime_debug.log"
    let line = "\(Date()): \(msg)\n"
    if let fh = FileHandle(forWritingAtPath: path) {
        fh.seekToEndOfFile()
        fh.write(line.data(using: .utf8)!)
        fh.closeFile()
    } else {
        FileManager.default.createFile(atPath: path, contents: line.data(using: .utf8))
    }
    NSLog("[GboardIME] %@", msg)
}

let kConnectionName = "GboardIME_Connection"
let bundleId = Bundle.main.bundleIdentifier!

imeLog("Starting — bundle=\(bundleId) conn=\(kConnectionName)")
let server = IMKServer(name: kConnectionName, bundleIdentifier: bundleId)
imeLog("IMKServer created: \(String(describing: server))")

NSApplication.shared.run()
