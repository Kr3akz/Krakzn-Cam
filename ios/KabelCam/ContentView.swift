import AVFoundation
import SwiftUI
import UIKit

struct ContentView: View {
    @ObservedObject var model: AppModel
    @Environment(\.scenePhase) private var scenePhase
    @State private var dimmed = false

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            switch model.cameraPermission {
            case .denied, .restricted:
                PermissionHint()
            default:
                HStack(spacing: 0) {
                    CameraPreview(
                        session: model.camera.session,
                        isEnabled: !dimmed,
                        configurationID: model.configurationID
                    ) { orientation in
                        model.camera.setOrientation(orientation)
                    }
                    .overlay(alignment: .topLeading) {
                        ConnectionBadge(status: model.connection)
                            .padding(12)
                    }

                    SettingsPanel(model: model, dimmed: $dimmed)
                        .frame(width: 330)
                }
            }

            if dimmed {
                DimmedOverlay { dimmed = false }
            }
        }
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
        .onAppear {
            // Der Bildschirm darf nicht von selbst ausgehen, sonst stoppt iOS die Kamera.
            UIApplication.shared.isIdleTimerDisabled = true
            model.start()
        }
        .onChange(of: scenePhase) { phase in
            if phase == .active {
                UIApplication.shared.isIdleTimerDisabled = true
                model.resume()
            } else {
                dimmed = false
            }
        }
    }
}

// MARK: - Verbindungsstatus

struct ConnectionBadge: View {
    let status: StreamServer.Status

    var body: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(color)
                .frame(width: 10, height: 10)
            Text(text)
                .font(.subheadline.weight(.semibold))
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(.ultraThinMaterial, in: Capsule())
    }

    private var color: Color {
        switch status {
        case .connected: return .green
        case .waitingForPC: return .orange
        case .starting: return .gray
        case .failed: return .red
        }
    }

    private var text: String {
        switch status {
        case .connected: return "Verbunden · überträgt"
        case .waitingForPC: return "Wartet auf den PC"
        case .starting: return "Startet …"
        case .failed(let message): return message
        }
    }
}

// MARK: - Einstellungen

struct SettingsPanel: View {
    @ObservedObject var model: AppModel
    @Binding var dimmed: Bool

    var body: some View {
        Form {
            Section {
                if model.availableLenses.count > 1 {
                    Picker("Objektiv", selection: $model.settings.lens) {
                        ForEach(model.availableLenses) { lens in
                            Text(lens.label).tag(lens)
                        }
                    }
                }

                LabeledSegment(title: "Auflösung") {
                    Picker("Auflösung", selection: $model.settings.resolution) {
                        ForEach(VideoResolution.allCases) { resolution in
                            Text(resolution.label).tag(resolution)
                        }
                    }
                }

                LabeledSegment(title: "Bilder pro Sekunde") {
                    Picker("Bilder pro Sekunde", selection: $model.settings.fps) {
                        ForEach(StreamSettings.fpsOptions, id: \.self) { fps in
                            Text("\(fps)").tag(fps)
                        }
                    }
                }

                LabeledSegment(title: "Qualität") {
                    Picker("Qualität", selection: $model.settings.quality) {
                        ForEach(StreamQuality.allCases) { quality in
                            Text(quality.label).tag(quality)
                        }
                    }
                }
            } header: {
                Text("Bild")
            } footer: {
                Text(formatSummary)
            }

            if let error = model.cameraError {
                Section {
                    Label(error, systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                }
            }

            if model.thermalState == .serious || model.thermalState == .critical {
                Section {
                    Label("Das iPhone wird warm. Weniger Auflösung oder FPS entlasten es.", systemImage: "thermometer.high")
                        .foregroundStyle(.orange)
                }
            }

            Section {
                Button {
                    dimmed = true
                } label: {
                    Label("Bildschirm abdunkeln", systemImage: "moon.fill")
                }
            } footer: {
                Text("Das Bild wird weiter übertragen. Antippen weckt den Bildschirm wieder auf.")
            }

            if let expiration = ProvisioningInfo.expirationDate {
                Section {
                    Label("Signatur gültig bis \(expiration.formatted(date: .abbreviated, time: .omitted))",
                          systemImage: "signature")
                        .foregroundStyle(expiration.timeIntervalSinceNow < 2 * 86400 ? Color.orange : Color.secondary)
                } footer: {
                    Text("Danach die App mit Sideloadly neu aufspielen.")
                }
            }

            Section("So geht's") {
                Text("1. iPhone per Kabel mit dem PC verbinden\n2. KabelCam auf dem PC starten\n3. In Zoom, Teams usw. die Kamera „KabelCam“ wählen\n\nDie App muss dafür geöffnet bleiben.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private var formatSummary: String {
        let size = model.settings.resolution.size
        let megabits = Double(model.settings.bitrate) / 1_000_000
        return "\(size.width) × \(size.height) · \(model.settings.fps) FPS · \(String(format: "%.0f", megabits)) Mbit/s · H.264"
    }
}

/// Segmentierte Auswahl mit Überschrift (der segmentierte Stil blendet das Label sonst aus).
struct LabeledSegment<Content: View>: View {
    let title: String
    @ViewBuilder let content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title)
                .font(.caption)
                .foregroundStyle(.secondary)
            content
                .pickerStyle(.segmented)
                .labelsHidden()
        }
        .padding(.vertical, 2)
    }
}

// MARK: - Abdunkeln

struct DimmedOverlay: View {
    let onWake: () -> Void
    @State private var showHint = true

    var body: some View {
        Color.black
            .ignoresSafeArea()
            .overlay {
                if showHint {
                    Text("Überträgt weiter – antippen zum Aufwecken")
                        .font(.footnote)
                        .foregroundStyle(Color.white.opacity(0.35))
                }
            }
            .contentShape(Rectangle())
            .onTapGesture(perform: onWake)
            .onAppear { ScreenBrightness.dim() }
            .onDisappear { ScreenBrightness.restore() }
            .task {
                try? await Task.sleep(nanoseconds: 4_000_000_000)
                showHint = false
            }
    }
}

/// Senkt die Helligkeit beim Abdunkeln auf das Minimum und stellt sie danach wieder her.
@MainActor
enum ScreenBrightness {
    private static var saved: CGFloat?

    static func dim() {
        guard let screen = currentScreen else { return }
        if saved == nil {
            saved = screen.brightness
        }
        screen.brightness = 0
    }

    static func restore() {
        guard let screen = currentScreen, let value = saved else { return }
        screen.brightness = value
        saved = nil
    }

    private static var currentScreen: UIScreen? {
        (UIApplication.shared.connectedScenes.first as? UIWindowScene)?.screen
    }
}

// MARK: - Keine Kameraberechtigung

struct PermissionHint: View {
    var body: some View {
        VStack(spacing: 16) {
            Image(systemName: "video.slash.fill")
                .font(.system(size: 44))
            Text("Kein Zugriff auf die Kamera")
                .font(.title3.bold())
            Text("Erlaube KabelCam den Kamerazugriff in den Einstellungen.")
                .foregroundStyle(.secondary)
            Button("Einstellungen öffnen") {
                if let url = URL(string: UIApplication.openSettingsURLString) {
                    UIApplication.shared.open(url)
                }
            }
            .buttonStyle(.borderedProminent)
        }
        .padding()
    }
}
