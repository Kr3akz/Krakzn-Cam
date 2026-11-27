import CoreGraphics
import Foundation

/// Bild-Einstellungen, die der PC fernsteuern kann (Belichtung, Weißabgleich, Fokus, Zoom …).
/// 0 bei ISO, Verschlusszeit und Farbtemperatur bzw. -1 bei der Fokusposition heißt: beim
/// Umschalten auf manuell den aktuellen Wert der Kamera übernehmen.
struct CameraControls: Codable, Equatable {
    var zoom: CGFloat = 1
    var exposureAuto = true
    var exposureBias: Float = 0
    var iso: Float = 0
    var shutter: Double = 0
    var whiteBalanceAuto = true
    var temperature: Float = 5500
    var tint: Float = 0
    var focusAuto = true
    var lensPosition: Float = 0.5
    var torch = false
    var hdr = false
    var stabilization = false
    var mirror = false

    private static let storageKey = "CameraControls.v1"

    static func load() -> CameraControls {
        guard let data = UserDefaults.standard.data(forKey: storageKey),
              let controls = try? JSONDecoder().decode(CameraControls.self, from: data) else {
            return CameraControls()
        }
        return controls
    }

    func save() {
        if let data = try? JSONEncoder().encode(self) {
            UserDefaults.standard.set(data, forKey: Self.storageKey)
        }
    }

    /// Übernimmt die Schlüssel eines "set"-Befehls vom PC (siehe docs/protokoll.md).
    /// Gibt false zurück, wenn kein bekannter Schlüssel dabei war.
    @discardableResult
    mutating func apply(remote command: [String: Any]) -> Bool {
        var changed = false
        func number(_ key: String) -> Double? { (command[key] as? NSNumber)?.doubleValue }
        func flag(_ key: String) -> Bool? { (command[key] as? NSNumber)?.boolValue }

        if let value = number("zoom") { zoom = CGFloat(value); changed = true }
        if let value = flag("exposure.auto") {
            exposureAuto = value
            if !value { iso = 0; shutter = 0 }  // aktuellen Wert übernehmen
            changed = true
        }
        if let value = number("ev") { exposureBias = Float(value); changed = true }
        if let value = number("iso") { iso = Float(value); exposureAuto = false; changed = true }
        if let value = number("shutter") { shutter = value; exposureAuto = false; changed = true }
        if let value = flag("wb.auto") {
            whiteBalanceAuto = value
            if !value { temperature = 0 }  // aktuellen Wert übernehmen
            changed = true
        }
        if let value = number("wb.temp") { temperature = Float(value); whiteBalanceAuto = false; changed = true }
        if let value = number("wb.tint") { tint = Float(value); whiteBalanceAuto = false; changed = true }
        if let value = flag("focus.auto") {
            focusAuto = value
            if !value { lensPosition = -1 }  // aktuellen Wert übernehmen
            changed = true
        }
        if let value = number("focus.pos") { lensPosition = Float(value); focusAuto = false; changed = true }
        if let value = flag("torch") { torch = value; changed = true }
        if let value = flag("hdr") { hdr = value; changed = true }
        if let value = flag("stabilization") { stabilization = value; changed = true }
        if let value = flag("mirror") { mirror = value; changed = true }
        return changed
    }
}
