import Foundation
import CoreMedia

enum CameraLens: String, Codable, CaseIterable, Identifiable {
    case wide, ultraWide, telephoto, front

    var id: String { rawValue }

    var label: String {
        switch self {
        case .wide: return "Weitwinkel"
        case .ultraWide: return "Ultraweitwinkel"
        case .telephoto: return "Tele"
        case .front: return "Frontkamera"
        }
    }
}

enum VideoResolution: String, Codable, CaseIterable, Identifiable {
    case hd720, hd1080, uhd4k

    var id: String { rawValue }

    var label: String {
        switch self {
        case .hd720: return "720p"
        case .hd1080: return "1080p"
        case .uhd4k: return "4K"
        }
    }

    var size: CMVideoDimensions {
        switch self {
        case .hd720: return CMVideoDimensions(width: 1280, height: 720)
        case .hd1080: return CMVideoDimensions(width: 1920, height: 1080)
        case .uhd4k: return CMVideoDimensions(width: 3840, height: 2160)
        }
    }
}

enum StreamQuality: String, Codable, CaseIterable, Identifiable {
    case balanced, high, maximum

    var id: String { rawValue }

    var label: String {
        switch self {
        case .balanced: return "Sparsam"
        case .high: return "Hoch"
        case .maximum: return "Maximal"
        }
    }

    /// Bitrate für 1080p mit 30 FPS. Andere Formate werden daraus hochgerechnet.
    var bitrateAt1080p30: Double {
        switch self {
        case .balanced: return 6_000_000
        case .high: return 12_000_000
        case .maximum: return 20_000_000
        }
    }
}

struct StreamSettings: Codable, Equatable {
    var lens: CameraLens = .wide
    var resolution: VideoResolution = .hd1080
    var fps: Int = 30
    var quality: StreamQuality = .high

    static let fpsOptions = [24, 30, 60]

    /// Ziel-Bitrate in Bit/s. Über USB ist reichlich Bandbreite da. Begrenzt wird nur,
    /// damit iPhone und PC nicht unnötig viel arbeiten.
    var bitrate: Int {
        let size = resolution.size
        let pixelsPerSecond = Double(size.width) * Double(size.height) * Double(fps)
        let reference = 1920.0 * 1080.0 * 30.0
        // Größere Bilder brauchen pro Pixel weniger Bits, deshalb nicht linear skalieren.
        let value = quality.bitrateAt1080p30 * pow(pixelsPerSecond / reference, 0.75)
        return Int(min(max(value, 2_000_000), 60_000_000))
    }

    private static let storageKey = "StreamSettings.v1"

    static func load() -> StreamSettings {
        guard let data = UserDefaults.standard.data(forKey: storageKey),
              let settings = try? JSONDecoder().decode(StreamSettings.self, from: data) else {
            return StreamSettings()
        }
        return settings
    }

    func save() {
        if let data = try? JSONEncoder().encode(self) {
            UserDefaults.standard.set(data, forKey: Self.storageKey)
        }
    }
}
