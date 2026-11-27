import AVFoundation

/// Kamera-Aufnahme mit AVFoundation. Liefert unkomprimierte Frames (NV12) an `onFrame`.
final class CameraCapture: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    let session = AVCaptureSession()

    /// Aufruf auf der Video-Queue (seriell).
    var onFrame: ((CMSampleBuffer) -> Void)?

    private let sessionQueue = DispatchQueue(label: "KabelCam.session")
    private let videoQueue = DispatchQueue(label: "KabelCam.video", qos: .userInteractive)
    private let output = AVCaptureVideoDataOutput()
    private var input: AVCaptureDeviceInput?
    private var orientation: AVCaptureVideoOrientation = .landscapeRight

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

        applyOrientation()
        return nil
    }

    private func applyOrientation() {
        guard let connection = output.connection(with: .video) else { return }
        if connection.isVideoOrientationSupported {
            connection.videoOrientation = orientation
        }
        // Nie spiegeln: Zoom, Teams usw. spiegeln die eigene Vorschau selbst.
        if connection.isVideoMirroringSupported {
            connection.automaticallyAdjustsVideoMirroring = false
            connection.isVideoMirrored = false
        }
        // Stabilisierung kostet Verzögerung und Energie, für eine Webcam unnötig.
        if connection.isVideoStabilizationSupported {
            connection.preferredVideoStabilizationMode = .off
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
