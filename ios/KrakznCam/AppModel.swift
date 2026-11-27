import AVFoundation
import Combine
import Foundation

/// Verbindet Kamera → Encoder → Server und hält den Zustand für die Oberfläche.
/// Alle `@Published`-Werte werden nur auf dem Main-Thread geändert.
final class AppModel: ObservableObject {
    @Published private(set) var connection: StreamServer.Status = .starting
    @Published private(set) var cameraPermission = AVCaptureDevice.authorizationStatus(for: .video)
    @Published private(set) var cameraError: String?
    @Published private(set) var thermalState = ProcessInfo.processInfo.thermalState
    /// Erhöht sich nach jeder Kamera-Umstellung, damit die Vorschau ihre Ausrichtung neu setzt.
    @Published private(set) var configurationID = 0
    @Published var settings: StreamSettings {
        didSet {
            guard settings != oldValue else { return }
            settings.save()
            applySettings()
        }
    }

    let availableLenses: [CameraLens]
    let camera = CameraCapture()
    private let encoder = H264Encoder()
    private let server = StreamServer(port: KrakznCamProtocol.port)
    private var started = false
    private var observers: [NSObjectProtocol] = []
    private var stateTimer: Timer?
    private var lastStateJSON: Data?

    init() {
        let lenses = CameraLens.allCases.filter { CameraCapture.device(for: $0) != nil }
        var loaded = StreamSettings.load()
        if !lenses.contains(loaded.lens) {
            loaded.lens = lenses.first ?? .wide
        }
        availableLenses = lenses
        settings = loaded

        let encoder = self.encoder
        let server = self.server

        camera.onFrame = { sample in
            // Nur kodieren, wenn wirklich ein PC zuschaut. Das spart viel Energie.
            guard server.hasClient else { return }
            encoder.encode(sample)
        }
        encoder.onFrame = { annexB, keyframe, ptsMicros in
            server.sendVideoFrame(ptsMicros: ptsMicros, keyframe: keyframe, annexB: annexB)
        }
        server.onClientConnected = { [weak self] in
            encoder.requestKeyframe()
            DispatchQueue.main.async { self?.lastStateJSON = nil }  // neuer PC bekommt den vollen Zustand
        }
        server.onKeyframeRequest = {
            encoder.requestKeyframe()
        }
        server.onCommand = { [weak self] command in
            DispatchQueue.main.async { self?.handleRemote(command) }
        }
        camera.onControlsChanged = { controls in
            controls.save()
        }
        server.onStatus = { [weak self] status in
            DispatchQueue.main.async { self?.connection = status }
        }

        observers.append(NotificationCenter.default.addObserver(
            forName: ProcessInfo.thermalStateDidChangeNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            self?.thermalState = ProcessInfo.processInfo.thermalState
        })
    }

    /// Beim ersten Erscheinen: Kamerazugriff erfragen und alles starten.
    func start() {
        guard !started else { return }
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            startPipeline()
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .video) { granted in
                DispatchQueue.main.async {
                    self.cameraPermission = granted ? .authorized : .denied
                    if granted {
                        self.startPipeline()
                    }
                }
            }
        default:
            cameraPermission = AVCaptureDevice.authorizationStatus(for: .video)
        }
    }

    /// App kommt wieder in den Vordergrund.
    func resume() {
        if started {
            camera.start()
        } else {
            start()
        }
    }

    private func startPipeline() {
        guard !started else { return }
        started = true
        cameraPermission = .authorized
        server.start()
        camera.setControls(CameraControls.load())
        applySettings()
        camera.start()
        // Viermal pro Sekunde den Kamera-Zustand an den PC melden, aber nur Änderungen.
        stateTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            self?.publishState()
        }
    }

    /// Einstell-Befehl vom PC: Format-Schlüssel ändern die Einstellungen, der Rest die Kamera.
    private func handleRemote(_ command: [String: Any]) {
        var updated = settings
        if let value = command["lens"] as? String, let lens = CameraLens(rawValue: value), availableLenses.contains(lens) {
            updated.lens = lens
        }
        if let value = command["resolution"] as? String, let resolution = VideoResolution(rawValue: value) {
            updated.resolution = resolution
        }
        if let value = (command["fps"] as? NSNumber)?.intValue, StreamSettings.fpsOptions.contains(value) {
            updated.fps = value
        }
        if let value = command["quality"] as? String, let quality = StreamQuality(rawValue: value) {
            updated.quality = quality
        }
        if updated != settings {
            settings = updated  // didSet stellt die Kamera um
        }
        camera.updateControls(remote: command)
        lastStateJSON = nil
    }

    private func publishState() {
        guard server.hasClient else {
            lastStateJSON = nil
            return
        }
        let current = settings
        let lenses = availableLenses.map(\.rawValue).joined(separator: ",")
        camera.snapshot { [weak self] cameraState in
            DispatchQueue.main.async {
                guard let self else { return }
                var state = cameraState
                state["lens"] = current.lens.rawValue
                state["lenses"] = lenses
                state["resolution"] = current.resolution.rawValue
                state["fps"] = current.fps
                state["quality"] = current.quality.rawValue
                guard let json = try? JSONSerialization.data(withJSONObject: state, options: [.sortedKeys]),
                      json != self.lastStateJSON else { return }
                self.lastStateJSON = json
                self.server.sendState(state)
            }
        }
    }

    private func applySettings() {
        guard started else { return }
        let current = settings
        encoder.update(fps: current.fps, bitrate: current.bitrate)
        camera.configure(lens: current.lens, resolution: current.resolution, fps: current.fps) { [weak self] error in
            guard let self else { return }
            self.cameraError = error
            self.configurationID += 1
            if error == nil {
                self.server.setHello(Self.helloInfo(for: current))
                self.encoder.requestKeyframe()
            }
        }
    }

    private static func helloInfo(for settings: StreamSettings) -> [String: Any] {
        let size = settings.resolution.size
        var info: [String: Any] = [
            "version": KrakznCamProtocol.version,
            "app": "KrakznCam",
            "codec": "h264",
            "width": Int(size.width),
            "height": Int(size.height),
            "fps": settings.fps,
            "bitrate": settings.bitrate,
            "lens": settings.lens.rawValue,
        ]
        // Damit das Windows-Programm rechtzeitig ans Erneuern mit Sideloadly erinnern kann.
        if let expiration = ProvisioningInfo.expirationDate {
            info["signatureExpires"] = Int(expiration.timeIntervalSince1970)
        }
        return info
    }
}
