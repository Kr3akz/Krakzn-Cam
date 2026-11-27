import Foundation

/// Gemeinsame Konstanten des KabelCam-Protokolls. Beschreibung: docs/protokoll.md
enum KabelCamProtocol {
    /// TCP-Port der App. Sie lauscht nur auf 127.0.0.1 und ist dadurch nur über USB (usbmux) erreichbar.
    static let port: UInt16 = 7878
    static let version = 1
    /// Obergrenze für Nachrichten vom PC an das iPhone.
    static let maxIncomingMessage = 64 * 1024
}

enum MessageType: UInt8 {
    case hello = 0x01       // iPhone -> PC: JSON mit Formatinfos
    case videoFrame = 0x02  // iPhone -> PC: H.264-Frame (Annex B)
    case command = 0x10     // PC -> iPhone: JSON-Befehl
}

enum MessageCodec {
    /// [UInt32 Länge (Typ + Nutzdaten), Big Endian][UInt8 Typ][Nutzdaten]
    static func message(_ type: MessageType, payload: Data) -> Data {
        var data = Data(capacity: 5 + payload.count)
        data.appendBigEndian(UInt32(1 + payload.count))
        data.append(type.rawValue)
        data.append(payload)
        return data
    }

    /// Videoframe: [Int64 PTS in µs][UInt8 Flags][Annex-B-Daten]
    static func videoFrame(ptsMicros: Int64, keyframe: Bool, annexB: Data) -> Data {
        var payload = Data(capacity: 9 + annexB.count)
        payload.appendBigEndian(UInt64(bitPattern: ptsMicros))
        payload.append(UInt8(keyframe ? 1 : 0))
        payload.append(annexB)
        return message(.videoFrame, payload: payload)
    }
}

extension Data {
    mutating func appendBigEndian<T: FixedWidthInteger>(_ value: T) {
        var bigEndian = value.bigEndian
        Swift.withUnsafeBytes(of: &bigEndian) { append(contentsOf: $0) }
    }
}
