import Foundation
import Darwin

/// Kleiner TCP-Server auf 127.0.0.1. Der PC erreicht ihn über den USB-Tunnel (usbmux)
/// des Apple Mobile Device Service. Es wird immer nur ein PC gleichzeitig bedient.
///
/// Absichtlich mit BSD-Sockets geschrieben: So lässt sich genau steuern, auf welcher
/// Adresse gelauscht wird und wann Frames verworfen werden.
final class StreamServer {
    enum Status: Equatable {
        case starting
        case waitingForPC
        case connected
        case failed(String)
    }

    /// Alle Callbacks kommen auf Hintergrund-Threads.
    var onStatus: ((Status) -> Void)?
    var onClientConnected: (() -> Void)?
    var onKeyframeRequest: (() -> Void)?
    /// Einstell-Befehl vom PC ({"cmd": "set", ...}), siehe docs/protokoll.md.
    var onCommand: (([String: Any]) -> Void)?

    private let port: UInt16
    private let lock = NSLock()
    private var started = false
    private var clientFD: Int32 = -1
    private var generation = 0
    private var queuedFrames = 0
    private var queuedBytes = 0
    private var waitingForKeyframe = true
    private var helloInfo: [String: Any] = [:]
    /// Wie viele Frames höchstens auf das Senden warten dürfen, bevor verworfen wird: etwa eine
    /// Viertelsekunde. Früher waren es 3 Frames (50 ms bei 60 FPS); dann führte jeder kurze Hänger
    /// am PC (Spiel, OBS) zu einem Sprung im Bild und einem großen Keyframe hinterher.
    private var maxQueuedFrames = 15
    /// Obergrenze in Bytes, damit 4K mit hoher Bitrate nicht beliebig viel puffert.
    private let maxQueuedBytes = 8 * 1024 * 1024
    private let sendQueue = DispatchQueue(label: "KrakznCam.send", qos: .userInteractive)

    init(port: UInt16) {
        self.port = port
    }

    var hasClient: Bool {
        lock.lock()
        defer { lock.unlock() }
        return clientFD >= 0
    }

    func start() {
        lock.lock()
        defer { lock.unlock() }
        guard !started else { return }
        started = true
        let thread = Thread { [weak self] in self?.serverLoop() }
        thread.name = "KrakznCam.Server"
        thread.qualityOfService = .userInitiated
        thread.start()
    }

    /// Passt den Puffer an die Bildrate an (eine Viertelsekunde, mindestens 4 Frames).
    func setFrameRate(_ fps: Int) {
        lock.lock()
        defer { lock.unlock() }
        maxQueuedFrames = max(4, fps / 4)
    }

    /// Speichert die Formatinfos. Sie gehen bei jeder neuen Verbindung als erste
    /// Nachricht raus und sofort, falls gerade ein PC verbunden ist.
    func setHello(_ info: [String: Any]) {
        lock.lock()
        defer { lock.unlock() }
        helloInfo = info
        if clientFD >= 0 {
            enqueueHelloLocked()
        }
    }

    /// Reiht einen kodierten Frame zum Senden ein. Kommt der PC nicht hinterher,
    /// werden Frames bis zum nächsten Keyframe verworfen, damit die Verzögerung klein bleibt.
    func sendVideoFrame(ptsMicros: Int64, keyframe: Bool, annexB: Data) {
        lock.lock()
        guard clientFD >= 0 else {
            lock.unlock()
            return
        }
        if queuedFrames >= maxQueuedFrames || queuedBytes >= maxQueuedBytes {
            let alreadyWaiting = waitingForKeyframe
            waitingForKeyframe = true
            lock.unlock()
            if !alreadyWaiting || keyframe {
                onKeyframeRequest?()
            }
            return
        }
        if waitingForKeyframe && !keyframe {
            lock.unlock()
            return
        }
        waitingForKeyframe = false
        let message = MessageCodec.videoFrame(ptsMicros: ptsMicros, keyframe: keyframe, annexB: annexB)
        queuedFrames += 1
        queuedBytes += message.count
        let fd = clientFD
        let gen = generation
        lock.unlock()

        sendQueue.async { [weak self] in
            self?.write(message, fd: fd, generation: gen, isFrame: true)
        }
    }

    // MARK: - Verbindungen annehmen

    private func serverLoop() {
        while true {
            report(.starting)
            let listenFD = makeListenSocket()
            guard listenFD >= 0 else {
                report(.failed("Port \(port) ist nicht verfügbar."))
                Thread.sleep(forTimeInterval: 2)
                continue
            }
            report(.waitingForPC)
            acceptLoop(listenFD)
            close(listenFD)
            Thread.sleep(forTimeInterval: 0.5)
        }
    }

    /// Nimmt Verbindungen an, bis der Lausch-Socket kaputtgeht (iOS schließt ihn
    /// z. B., wenn die App länger im Hintergrund war). Dann wird er neu erstellt.
    private func acceptLoop(_ listenFD: Int32) {
        while true {
            let fd = accept(listenFD, nil, nil)
            if fd < 0 {
                if errno == EINTR { continue }
                return
            }
            configureClient(fd)
            serveClient(fd)
            report(.waitingForPC)
        }
    }

    private func serveClient(_ fd: Int32) {
        lock.lock()
        generation += 1
        clientFD = fd
        queuedFrames = 0
        queuedBytes = 0
        waitingForKeyframe = true
        enqueueHelloLocked()
        lock.unlock()

        report(.connected)
        onClientConnected?()

        // Befehle vom PC lesen, bis die Verbindung endet.
        let chunkSize = 4096
        var chunk = [UInt8](repeating: 0, count: chunkSize)
        var buffer = Data()
        while true {
            let count = recv(fd, &chunk, chunkSize, 0)
            if count < 0 && errno == EINTR { continue }
            if count <= 0 { break }
            buffer.append(chunk, count: count)
            if !processIncoming(&buffer) { break }
        }

        lock.lock()
        clientFD = -1
        lock.unlock()
        // Erst schließen, wenn alle bereits eingereihten Sendeaufträge durch sind.
        sendQueue.async { close(fd) }
    }

    // MARK: - Empfangen

    /// Zerlegt den Puffer in Nachrichten. Gibt `false` bei einem Protokollfehler zurück.
    private func processIncoming(_ buffer: inout Data) -> Bool {
        while buffer.count >= 4 {
            let header = [UInt8](buffer.prefix(4))
            let length = Int(header[0]) << 24 | Int(header[1]) << 16 | Int(header[2]) << 8 | Int(header[3])
            guard length >= 1, length <= KrakznCamProtocol.maxIncomingMessage else { return false }
            guard buffer.count >= 4 + length else { return true }
            let message = [UInt8](buffer.prefix(4 + length))
            buffer.removeFirst(4 + length)
            if message[4] == MessageType.command.rawValue {
                handleCommand(Data(message[5...]))
            }
        }
        return true
    }

    private func handleCommand(_ payload: Data) {
        guard let object = try? JSONSerialization.jsonObject(with: payload),
              let dictionary = object as? [String: Any],
              let command = dictionary["cmd"] as? String else { return }
        switch command {
        case "keyframe":
            onKeyframeRequest?()
        case "set":
            onCommand?(dictionary)
        default:
            break // Unbekannte Befehle ignorieren, damit neuere PC-Programme nichts kaputt machen.
        }
    }

    // MARK: - Senden

    /// Schickt den Kamera-Zustand (Fähigkeiten und aktuelle Werte) an den PC. Wird nie verworfen.
    func sendState(_ state: [String: Any]) {
        lock.lock()
        defer { lock.unlock() }
        guard clientFD >= 0,
              let json = try? JSONSerialization.data(withJSONObject: state, options: [.sortedKeys]) else { return }
        enqueueLocked(MessageCodec.message(.state, payload: json))
    }

    /// Muss mit gehaltenem `lock` aufgerufen werden.
    private func enqueueHelloLocked() {
        guard let json = try? JSONSerialization.data(withJSONObject: helloInfo) else { return }
        enqueueLocked(MessageCodec.message(.hello, payload: json))
    }

    /// Muss mit gehaltenem `lock` aufgerufen werden.
    private func enqueueLocked(_ message: Data) {
        let fd = clientFD
        let gen = generation
        sendQueue.async { [weak self] in
            self?.write(message, fd: fd, generation: gen, isFrame: false)
        }
    }

    private func write(_ message: Data, fd: Int32, generation gen: Int, isFrame: Bool) {
        lock.lock()
        let isCurrent = gen == generation && clientFD == fd
        lock.unlock()

        let ok = isCurrent && Self.sendAll(fd, message)

        lock.lock()
        if isFrame && gen == generation {
            queuedFrames -= 1
            queuedBytes -= message.count
        }
        lock.unlock()

        if isCurrent && !ok {
            // Weckt recv() in serveClient auf, dadurch wird die Verbindung sauber beendet.
            shutdown(fd, SHUT_RDWR)
        }
    }

    private static func sendAll(_ fd: Int32, _ data: Data) -> Bool {
        data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) -> Bool in
            guard var pointer = raw.baseAddress else { return true }
            var remaining = raw.count
            while remaining > 0 {
                let sent = Darwin.send(fd, pointer, remaining, 0)
                if sent < 0 && errno == EINTR { continue }
                if sent <= 0 { return false }
                pointer = pointer.advanced(by: sent)
                remaining -= sent
            }
            return true
        }
    }

    // MARK: - Sockets

    private func makeListenSocket() -> Int32 {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { return -1 }
        var on: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, socklen_t(MemoryLayout<Int32>.size))

        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = port.bigEndian
        // 127.0.0.1: erreichbar über usbmux, aber nicht aus dem WLAN.
        address.sin_addr = in_addr(s_addr: UInt32(0x7F00_0001).bigEndian)

        let bound = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bound == 0, listen(fd, 2) == 0 else {
            close(fd)
            return -1
        }
        return fd
    }

    private func configureClient(_ fd: Int32) {
        var on: Int32 = 1
        let intSize = socklen_t(MemoryLayout<Int32>.size)
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, intSize)
        setsockopt(fd, Int32(IPPROTO_TCP), TCP_NODELAY, &on, intSize)
        // Großer Sendepuffer: Ein 4K-Keyframe passt komplett hinein, ohne dass send() blockiert.
        var sendBuffer: Int32 = 2 * 1024 * 1024
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sendBuffer, intSize)
        // Hängt der PC komplett, bricht send() nach 2 s ab und die Verbindung wird beendet.
        var timeout = timeval(tv_sec: 2, tv_usec: 0)
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
    }

    private func report(_ status: Status) {
        onStatus?(status)
    }
}
