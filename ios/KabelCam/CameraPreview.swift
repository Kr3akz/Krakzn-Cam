import AVFoundation
import SwiftUI
import UIKit

/// Live-Vorschau der Kamera. Meldet die Ausrichtung der Oberfläche zurück, damit das
/// übertragene Bild genauso steht wie die Vorschau.
struct CameraPreview: UIViewRepresentable {
    let session: AVCaptureSession
    /// Beim Abdunkeln aus, das spart GPU-Arbeit.
    var isEnabled: Bool
    var configurationID: Int
    var onOrientationChange: (AVCaptureVideoOrientation) -> Void

    func makeUIView(context: Context) -> PreviewView {
        let view = PreviewView()
        view.backgroundColor = .black
        view.previewLayer.session = session
        view.previewLayer.videoGravity = .resizeAspect
        return view
    }

    func updateUIView(_ view: PreviewView, context: Context) {
        view.onOrientationChange = onOrientationChange
        view.isPreviewEnabled = isEnabled
        view.refresh()
    }
}

final class PreviewView: UIView {
    override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }

    var previewLayer: AVCaptureVideoPreviewLayer {
        // layerClass ist oben festgelegt, der Cast kann nicht fehlschlagen.
        layer as! AVCaptureVideoPreviewLayer
    }

    var onOrientationChange: ((AVCaptureVideoOrientation) -> Void)?
    var isPreviewEnabled = true
    private var reportedOrientation: AVCaptureVideoOrientation?
    private var geometryObservation: NSKeyValueObservation?

    override func layoutSubviews() {
        super.layoutSubviews()
        refresh()
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        // Eine Drehung um 180° ändert die Größe nicht, deshalb die Geometrie der Szene beobachten.
        geometryObservation = window?.windowScene?.observe(\.effectiveGeometry, options: [.new]) { [weak self] _, _ in
            DispatchQueue.main.async { self?.refresh() }
        }
        refresh()
    }

    func refresh() {
        let orientation = currentOrientation()
        if let connection = previewLayer.connection {
            connection.isEnabled = isPreviewEnabled
            if connection.isVideoOrientationSupported {
                connection.videoOrientation = orientation
            }
        }
        if orientation != reportedOrientation {
            reportedOrientation = orientation
            onOrientationChange?(orientation)
        }
    }

    private func currentOrientation() -> AVCaptureVideoOrientation {
        guard let interfaceOrientation = window?.windowScene?.effectiveGeometry.interfaceOrientation else {
            return reportedOrientation ?? .landscapeRight
        }
        switch interfaceOrientation {
        case .landscapeLeft: return .landscapeLeft
        case .portrait: return .portrait
        case .portraitUpsideDown: return .portraitUpsideDown
        default: return .landscapeRight
        }
    }
}
