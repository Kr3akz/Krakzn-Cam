import Foundation
import CoreMedia
import VideoToolbox

/// Hardware-H.264-Encoder über VideoToolbox. Ausgelegt auf wenig Verzögerung:
/// Echtzeitmodus, keine B-Frames, alle 2 Sekunden ein Keyframe.
final class H264Encoder {
    /// Fertiger Frame im Annex-B-Format. Keyframes enthalten SPS und PPS.
    /// Aufruf auf einem VideoToolbox-Thread.
    var onFrame: ((_ annexB: Data, _ keyframe: Bool, _ ptsMicros: Int64) -> Void)?

    private let lock = NSLock()
    private var session: VTCompressionSession?
    private var width: Int32 = 0
    private var height: Int32 = 0
    private var fps: Int32 = 30
    private var bitrate = 12_000_000
    private var needsRebuild = true
    private var forceKeyframe = true

    func update(fps: Int, bitrate: Int) {
        lock.lock()
        defer { lock.unlock() }
        if self.fps != Int32(fps) || self.bitrate != bitrate {
            self.fps = Int32(fps)
            self.bitrate = bitrate
            needsRebuild = true
        }
    }

    func requestKeyframe() {
        lock.lock()
        forceKeyframe = true
        lock.unlock()
    }

    /// Kodiert einen Kameraframe. Immer von derselben seriellen Queue aufrufen.
    func encode(_ sampleBuffer: CMSampleBuffer) {
        guard let imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        let frameWidth = Int32(CVPixelBufferGetWidth(imageBuffer))
        let frameHeight = Int32(CVPixelBufferGetHeight(imageBuffer))

        lock.lock()
        if session == nil || needsRebuild || frameWidth != width || frameHeight != height {
            if let old = session {
                VTCompressionSessionInvalidate(old)
            }
            session = Self.makeSession(width: frameWidth, height: frameHeight, fps: fps, bitrate: bitrate)
            width = frameWidth
            height = frameHeight
            needsRebuild = false
            forceKeyframe = true
        }
        guard let session else {
            lock.unlock()
            return
        }
        let keyframe = forceKeyframe
        forceKeyframe = false
        lock.unlock()

        var properties: CFDictionary?
        if keyframe {
            properties = [kVTEncodeFrameOptionKey_ForceKeyFrame as String: true] as CFDictionary
        }

        let status = VTCompressionSessionEncodeFrame(
            session,
            imageBuffer: imageBuffer,
            presentationTimeStamp: CMSampleBufferGetPresentationTimeStamp(sampleBuffer),
            duration: CMSampleBufferGetDuration(sampleBuffer),
            frameProperties: properties,
            infoFlagsOut: nil
        ) { [weak self] status, _, encoded in
            guard status == noErr, let encoded, let self else { return }
            self.deliver(encoded)
        }

        if status != noErr {
            // Z. B. nach einer Unterbrechung durch das System: beim nächsten Frame neu aufbauen.
            lock.lock()
            needsRebuild = true
            lock.unlock()
        }
    }

    private static func makeSession(width: Int32, height: Int32, fps: Int32, bitrate: Int) -> VTCompressionSession? {
        var created: VTCompressionSession?
        let status = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            width: width,
            height: height,
            codecType: kCMVideoCodecType_H264,
            encoderSpecification: nil,
            imageBufferAttributes: nil,
            compressedDataAllocator: nil,
            outputCallback: nil,
            refcon: nil,
            compressionSessionOut: &created
        )
        guard status == noErr, let session = created else { return nil }

        func set(_ key: CFString, _ value: CFTypeRef) {
            VTSessionSetProperty(session, key: key, value: value)
        }
        set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue)
        set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel)
        set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse) // keine B-Frames
        set(kVTCompressionPropertyKey_ExpectedFrameRate, NSNumber(value: fps))
        set(kVTCompressionPropertyKey_AverageBitRate, NSNumber(value: bitrate))
        // Spitzen auf das 1,5-Fache der Durchschnittsrate begrenzen (Bytes pro 1 Sekunde).
        let peakBytesPerSecond = Double(bitrate) * 1.5 / 8
        set(kVTCompressionPropertyKey_DataRateLimits, [NSNumber(value: peakBytesPerSecond), NSNumber(value: 1)] as CFArray)
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval, NSNumber(value: fps * 2))
        set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, NSNumber(value: 2))
        VTCompressionSessionPrepareToEncodeFrames(session)
        return session
    }

    /// Wandelt die Ausgabe von VideoToolbox (AVCC: Länge + NAL) in Annex B (Startcode + NAL) um.
    private func deliver(_ sample: CMSampleBuffer) {
        guard let blockBuffer = CMSampleBufferGetDataBuffer(sample),
              let format = CMSampleBufferGetFormatDescription(sample) else { return }
        let keyframe = Self.isKeyframe(sample)
        let startCode: [UInt8] = [0, 0, 0, 1]
        var annexB = Data()

        var nalLengthSize: Int32 = 4
        var parameterSetCount = 0
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            format,
            parameterSetIndex: 0,
            parameterSetPointerOut: nil,
            parameterSetSizeOut: nil,
            parameterSetCountOut: &parameterSetCount,
            nalUnitHeaderLengthOut: &nalLengthSize
        )

        if keyframe {
            for index in 0..<parameterSetCount {
                var pointer: UnsafePointer<UInt8>?
                var size = 0
                let status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    format,
                    parameterSetIndex: index,
                    parameterSetPointerOut: &pointer,
                    parameterSetSizeOut: &size,
                    parameterSetCountOut: nil,
                    nalUnitHeaderLengthOut: nil
                )
                if status == noErr, let pointer {
                    annexB.append(contentsOf: startCode)
                    annexB.append(pointer, count: size)
                }
            }
        }

        let totalLength = CMBlockBufferGetDataLength(blockBuffer)
        var avcc = [UInt8](repeating: 0, count: totalLength)
        guard CMBlockBufferCopyDataBytes(blockBuffer, atOffset: 0, dataLength: totalLength, destination: &avcc) == kCMBlockBufferNoErr else { return }

        let lengthSize = Int(nalLengthSize)
        var offset = 0
        while offset + lengthSize <= totalLength {
            var nalLength = 0
            for i in 0..<lengthSize {
                nalLength = (nalLength << 8) | Int(avcc[offset + i])
            }
            offset += lengthSize
            guard nalLength > 0, offset + nalLength <= totalLength else { break }
            annexB.append(contentsOf: startCode)
            annexB.append(contentsOf: avcc[offset..<(offset + nalLength)])
            offset += nalLength
        }

        let pts = CMSampleBufferGetPresentationTimeStamp(sample)
        let micros = CMTimeConvertScale(pts, timescale: 1_000_000, method: .default).value
        onFrame?(annexB, keyframe, micros)
    }

    private static func isKeyframe(_ sample: CMSampleBuffer) -> Bool {
        guard let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) else { return true }
        let array = attachments as NSArray
        guard let first = array.firstObject as? NSDictionary else { return true }
        // Fehlt "NotSync", ist der Frame ein Keyframe.
        if let notSync = first[kCMSampleAttachmentKey_NotSync] as? Bool {
            return !notSync
        }
        return true
    }
}
