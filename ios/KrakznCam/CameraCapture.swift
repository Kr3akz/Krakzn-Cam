import AVFoundation

/// Kamera-Aufnahme mit AVFoundation. Liefert unkomprimierte Frames (NV12) an `onFrame`.
final class CameraCapture: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    let session = AVCaptureSession()

    /// Aufruf auf der Video-Queue (seriell).
    var onFrame: ((CMSampleBuffer) -> Void)?
    /// Die Bild-Einstellungen haben sich geändert (zum Speichern). Aufruf auf der Session-Queue.
    var onControlsChanged: ((CameraControls) -> Void)?

    private let sessionQueue = DispatchQueue(label: "KrakznCam.session")
    private let videoQueue = DispatchQueue(label: "KrakznCam.video", qos: .userInteractive)
    private let output = AVCaptureVideoDataOutput()
    private var input: AVCaptureDeviceInput?
    private var orientation: AVCaptureVideoOrientation = .landscapeRight
    private var controls = CameraControls()  // nur auf sessionQueue

    static func device(for lens: CameraLens) -> AVCaptureDevice? {
        switch lens {
        case .wide: return AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back)
        case .ultraWide: return AVCaptureDevice.default(.builtInUltraWideCamera, for: .video, position: .back)
        case .telephoto: return AVCaptureDevice.default(.builtInTelephotoCamera, for: .video, position: .back)
        case .front: return AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .front)
        }
    }

    /// Stellt Objektiv, Auflösung und FPS ein. `completion` kommt auf dem Main-Thread,
    /// mit einer Fehlermeldung oder `nil`.
    func configure(lens: CameraLens, resolution: VideoResolution, fps: Int, completion: @escaping (String?) -> Void) {
        sessionQueue.async {
            let error = self.applyConfiguration(lens: lens, resolution: resolution, fps: fps)
            DispatchQueue.main.async { completion(error) }
        }
    }

    func start() {
        sessionQueue.async {
            if !self.session.isRunning {
                self.session.startRunning()
            }
        }
    }

    /// Ausrichtung des übertragenen Bildes. Folgt der Ausrichtung der Oberfläche.
    func setOrientation(_ newValue: AVCaptureVideoOrientation) {
        sessionQueue.async {
            guard newValue != self.orientation else { return }
            self.orientation = newValue
            self.applyOrientation()
        }
    }

    /// Gespeicherte Bild-Einstellungen beim Start setzen.
    func setControls(_ newValue: CameraControls) {
        sessionQueue.async {
            self.controls = newValue
            self.applyControlsNow()
        }
    }

    /// Einstell-Befehl vom PC übernehmen (Schlüssel siehe docs/protokoll.md).
    func updateControls(remote command: [String: Any]) {
        sessionQueue.async {
            var updated = self.controls
            guard updated.apply(remote: command) else { return }
            self.controls = updated
            self.applyControlsNow()
        }
    }

    /// Fähigkeiten und aktuelle Werte der Kamera für den PC. `completion` kommt auf der Session-Queue.
    func snapshot(completion: @escaping ([String: Any]) -> Void) {
        sessionQueue.async {
            guard let device = self.input?.device else { return }
            let format = device.activeFormat
            let maxDuration = Self.longestExposure(device)
            let whiteBalance = device.temperatureAndTintValues(for: device.deviceWhiteBalanceGains)
            let state: [String: Any] = [
                "zoom": Self.rounded(Double(device.videoZoomFactor), 100),
                "zoom.max": Double(min(format.videoMaxZoomFactor, 10)),
                "exposure.auto": device.exposureMode != .custom,
                "exposure.manual": device.isExposureModeSupported(.custom),
                "iso": Self.rounded(Double(device.iso), 1),
                "iso.min": Double(format.minISO),
                "iso.max": Double(format.maxISO),
                "shutter": Self.rounded(device.exposureDuration.seconds, 1_000_000),
                "shutter.min": format.minExposureDuration.seconds,
                "shutter.max": maxDuration,
                "ev": Self.rounded(Double(device.exposureTargetBias), 100),
                "ev.min": Double(max(device.minExposureTargetBias, -3)),
                "ev.max": Double(min(device.maxExposureTargetBias, 3)),
                "wb.auto": device.whiteBalanceMode != .locked,
                "wb.manual": device.isWhiteBalanceModeSupported(.locked),
                "wb.temp": Self.rounded(Double(whiteBalance.temperature), 0.1),
                "wb.tint": Self.rounded(Double(whiteBalance.tint), 1),
                "focus.auto": device.focusMode != .locked,
                "focus.manual": device.isLockingFocusWithCustomLensPositionSupported,
                "focus.pos": Self.rounded(Double(device.lensPosition), 1000),
                "torch": device.torchMode == .on,
                "torch.available": device.hasTorch,
                "hdr": format.isVideoHDRSupported && device.isVideoHDREnabled,
                "hdr.available": format.isVideoHDRSupported,
                "stabilization": self.controls.stabilization,
                "stabilization.available": format.isVideoStabilizationModeSupported(.standard),
                "mirror": self.controls.mirror,
            ]
            completion(state)
        }
    }

    private static func rounded(_ value: Double, _ scale: Double) -> Double {
        guard value.isFinite else { return 0 }
        return (value * scale).rounded() / scale
    }

    /// Längste Verschlusszeit, die die eingestellten FPS noch zulässt.
    private static func longestExposure(_ device: AVCaptureDevice) -> Double {
        let formatMax = device.activeFormat.maxExposureDuration.seconds
        let frame = device.activeVideoMaxFrameDuration.seconds
        return frame.isFinite && frame > 0 ? min(formatMax, frame) : formatMax
    }

    /// Wendet `controls` auf die Kamera an (nur auf sessionQueue). Werte, die „aktuell“
    /// bedeuten (0 bzw. -1), werden dabei durch die tatsächlichen Werte ersetzt und gespeichert.
    private func applyControlsNow() {
        guard let device = input?.device, (try? device.lockForConfiguration()) != nil else { return }
        let format = device.activeFormat
        var c = controls

        // Zoom (höchstens 10-fach, darüber wird es nur unscharf).
        let maxZoom = min(format.videoMaxZoomFactor, 10)
        device.videoZoomFactor = max(1, min(c.zoom, maxZoom))

        // Belichtung
        if c.exposureAuto || !device.isExposureModeSupported(.custom) {
            if device.isExposureModeSupported(.continuousAutoExposure) { device.exposureMode = .continuousAutoExposure }
            let bias = max(device.minExposureTargetBias, min(c.exposureBias, device.maxExposureTargetBias))
            device.setExposureTargetBias(bias, completionHandler: nil)
        } else {
            let wantedISO = c.iso > 0 ? c.iso : device.iso
            let iso = max(format.minISO, min(wantedISO, format.maxISO))
            let wantedShutter = c.shutter > 0 ? c.shutter : device.exposureDuration.seconds
            let shutter = max(format.minExposureDuration.seconds, min(wantedShutter, Self.longestExposure(device)))
            device.setExposureModeCustom(duration: CMTime(seconds: shutter, preferredTimescale: 1_000_000), iso: iso,
                                         completionHandler: nil)
            c.iso = iso
            c.shutter = shutter
        }

        // Weißabgleich
        if c.whiteBalanceAuto || !device.isWhiteBalanceModeSupported(.locked) {
            if device.isWhiteBalanceModeSupported(.continuousAutoWhiteBalance) {
                device.whiteBalanceMode = .continuousAutoWhiteBalance
            }
        } else {
            var gains: AVCaptureDevice.WhiteBalanceGains
            if c.temperature > 0 {
                let values = AVCaptureDevice.WhiteBalanceTemperatureAndTintValues(temperature: c.temperature, tint: c.tint)
                gains = device.deviceWhiteBalanceGains(for: values)
            } else {
                gains = device.deviceWhiteBalanceGains
                let current = device.temperatureAndTintValues(for: gains)
                c.temperature = current.temperature
                c.tint = current.tint
            }
            let limit = device.maxWhiteBalanceGain
            gains.redGain = max(1, min(gains.redGain, limit))
            gains.greenGain = max(1, min(gains.greenGain, limit))
            gains.blueGain = max(1, min(gains.blueGain, limit))
            device.setWhiteBalanceModeLocked(with: gains, completionHandler: nil)
        }

        // Fokus
        if c.focusAuto || !device.isLockingFocusWithCustomLensPositionSupported {
            if device.isFocusModeSupported(.continuousAutoFocus) { device.focusMode = .continuousAutoFocus }
        } else {
            let position = c.lensPosition >= 0 ? c.lensPosition : device.lensPosition
            device.setFocusModeLocked(lensPosition: max(0, min(position, 1)), completionHandler: nil)
            c.lensPosition = position
        }

        // Licht (nur Rückkameras mit Blitz)
        if device.hasTorch {
            if c.torch, device.isTorchModeSupported(.on) {
                try? device.setTorchModeOn(level: AVCaptureDevice.maxAvailableTorchLevel)
            } else if device.isTorchModeSupported(.off) {
                device.torchMode = .off
            }
        }

        // HDR-Video (sofern das Format es kann)
        if format.isVideoHDRSupported {
            device.automaticallyAdjustsVideoHDREnabled = false
            device.isVideoHDREnabled = c.hdr
        }
        device.unlockForConfiguration()

        applyOrientation()  // Spiegeln und Stabilisierung hängen an der Verbindung
        if c != controls {
            controls = c
        }
        onControlsChanged?(controls)
    }

    // MARK: - AVCaptureVideoDataOutputSampleBufferDelegate

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        onFrame?(sampleBuffer)
    }

    // MARK: - Konfiguration (nur auf sessionQueue)

    private func applyConfiguration(lens: CameraLens, resolution: VideoResolution, fps: Int) -> String? {
        guard let device = Self.device(for: lens) else {
            return "Dieses Objektiv gibt es auf deinem iPhone nicht."
        }
        guard let match = Self.bestFormat(for: device, size: resolution.size, fps: fps) else {
            return "\(resolution.label) mit \(fps) FPS kann dieses Objektiv nicht."
        }
        let (format, range) = match

        session.beginConfiguration()
        defer { session.commitConfiguration() }

        // Format selbst wählen statt über ein Preset.
        if session.sessionPreset != .inputPriority, session.canSetSessionPreset(.inputPriority) {
            session.sessionPreset = .inputPriority
        }

        if input?.device != device {
            if let input {
                session.removeInput(input)
            }
            input = nil
            do {
                let newInput = try AVCaptureDeviceInput(device: device)
                guard session.canAddInput(newInput) else { return "Die Kamera kann gerade nicht verwendet werden." }
                session.addInput(newInput)
                input = newInput
            } catch {
                return "Kamera-Fehler: \(error.localizedDescription)"
            }
        }

        if !session.outputs.contains(output) {
            // NV12 mit Videobereich: genau das Format, das der H.264-Encoder ohne Umrechnung nimmt.
            output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange]
            output.alwaysDiscardsLateVideoFrames = true
            output.setSampleBufferDelegate(self, queue: videoQueue)
            guard session.canAddOutput(output) else { return "Die Videoausgabe kann nicht eingerichtet werden." }
            session.addOutput(output)
        }

        do {
            try device.lockForConfiguration()
            device.activeFormat = format
            // Exakt die gewünschten FPS, aber innerhalb dessen, was das Format kann.
            var frameDuration = CMTime(value: 1, timescale: CMTimeScale(fps))
            frameDuration = CMTimeMaximum(frameDuration, range.minFrameDuration)
            frameDuration = CMTimeMinimum(frameDuration, range.maxFrameDuration)
            device.activeVideoMinFrameDuration = frameDuration
            device.activeVideoMaxFrameDuration = frameDuration
            if device.isFocusModeSupported(.continuousAutoFocus) {
                device.focusMode = .continuousAutoFocus
            }
            if device.isExposureModeSupported(.continuousAutoExposure) {
                device.exposureMode = .continuousAutoExposure
            }
            if device.isWhiteBalanceModeSupported(.continuousAutoWhiteBalance) {
                device.whiteBalanceMode = .continuousAutoWhiteBalance
            }
            device.unlockForConfiguration()
        } catch {
            return "Die Kamera ist gesperrt: \(error.localizedDescription)"
        }

        applyControlsNow()  // Belichtung, Weißabgleich, Fokus, Zoom … auch für das neue Objektiv
        return nil
    }

    private func applyOrientation() {
        guard let connection = output.connection(with: .video) else { return }
        if connection.isVideoOrientationSupported {
            connection.videoOrientation = orientation
        }
        // Standardmäßig nicht spiegeln: Zoom, Teams usw. spiegeln die eigene Vorschau selbst.
        if connection.isVideoMirroringSupported {
            connection.automaticallyAdjustsVideoMirroring = false
            connection.isVideoMirrored = controls.mirror
        }
        // Stabilisierung kostet etwas Verzögerung und Energie, deshalb standardmäßig aus.
        if connection.isVideoStabilizationSupported {
            connection.preferredVideoStabilizationMode = controls.stabilization ? .standard : .off
        }
    }

    /// Sucht ein 8-Bit-Format mit genau der gewünschten Größe, das die FPS schafft.
    /// Bevorzugt Formate mit niedriger Maximal-FPS (meist volle Sensorqualität und
    /// sparsamer) und den Videobereich, damit nichts umgerechnet werden muss.
    private static func bestFormat(for device: AVCaptureDevice, size: CMVideoDimensions, fps: Int) -> (AVCaptureDevice.Format, AVFrameRateRange)? {
        let target = Double(fps)
        var best: (format: AVCaptureDevice.Format, range: AVFrameRateRange, score: Double)?
        for format in device.formats {
            let description = format.formatDescription
            let dimensions = CMVideoFormatDescriptionGetDimensions(description)
            guard dimensions.width == size.width, dimensions.height == size.height else { continue }
            let subtype = CMFormatDescriptionGetMediaSubType(description)
            let isVideoRange = subtype == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
            guard isVideoRange || subtype == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange else { continue }
            guard let range = format.videoSupportedFrameRateRanges.first(where: {
                $0.minFrameRate <= target && $0.maxFrameRate >= target - 0.5
            }) else { continue }
            let score = range.maxFrameRate * 2 + (isVideoRange ? 0 : 1)
            if best == nil || score < best!.score {
                best = (format, range, score)
            }
        }
        return best.map { ($0.format, $0.range) }
    }
}
