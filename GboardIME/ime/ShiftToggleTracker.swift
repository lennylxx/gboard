import Cocoa

/// Tracks bare-Shift key presses to determine when to toggle Chinese/English mode.
/// A "bare Shift" is when Shift is pressed and released without any other key or modifier in between.
class ShiftToggleTracker {

    enum Result {
        case none
        case shouldToggle
    }

    private var trackedShiftKeyCode: UInt16?
    private var shiftKeyUsedWithOtherKey = false

    func handleFlagsChanged(keyCode: UInt16, modifierFlags: NSEvent.ModifierFlags) -> Result {
        let otherModifiers = modifierFlags.intersection([.command, .control, .option])

        // 1. If another key/modifier changed (not Shift)
        if !isShiftKey(keyCode) {
            if trackedShiftKeyCode != nil {
                // Any other modifier (Cmd, Ctrl, Opt) was pressed or released while tracking Shift
                cancelTracking()
            }
            return .none
        }

        // 2. Shift key itself changed:
        if modifierFlags.contains(.shift) {
            // Shift was pressed down: only track if NO other modifiers are held
            if otherModifiers.isEmpty {
                startTrackingShift(keyCode)
            } else {
                cancelTracking()
            }
            return .none
        }

        // 3. Shift was released:
        // If other modifiers are still held down, it's not a bare Shift
        if !otherModifiers.isEmpty {
            cancelTracking()
            return .none
        }

        return finishTrackingShift()
    }

    func handleKeyDown(keyCode: UInt16, modifierFlags: NSEvent.ModifierFlags) {
        if isShiftKey(keyCode) {
            let otherModifiers = modifierFlags.intersection([.command, .control, .option])
            if otherModifiers.isEmpty {
                startTrackingShift(keyCode)
            } else {
                cancelTracking()
            }
            return
        }

        if trackedShiftKeyCode != nil {
            shiftKeyUsedWithOtherKey = true
        }
    }

    func handleKeyUp(keyCode: UInt16) -> Result {
        guard isShiftKey(keyCode) else {
            if trackedShiftKeyCode != nil {
                shiftKeyUsedWithOtherKey = true
            }
            return .none
        }
        return finishTrackingShift()
    }

    func cancelTracking() {
        trackedShiftKeyCode = nil
        shiftKeyUsedWithOtherKey = false
    }

    private func isShiftKey(_ keyCode: UInt16) -> Bool {
        return keyCode == 56 || keyCode == 60  // kVK_Shift, kVK_RightShift
    }

    private func startTrackingShift(_ keyCode: UInt16) {
        trackedShiftKeyCode = keyCode
        shiftKeyUsedWithOtherKey = false
    }

    private func finishTrackingShift() -> Result {
        guard trackedShiftKeyCode != nil else { return .none }
        trackedShiftKeyCode = nil
        defer { shiftKeyUsedWithOtherKey = false }
        return shiftKeyUsedWithOtherKey ? .none : .shouldToggle
    }
}
