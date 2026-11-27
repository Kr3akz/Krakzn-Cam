import Foundation

/// Liest das Signaturprofil, das Sideloadly beim Aufspielen in die App legt
/// (`embedded.mobileprovision`). Daraus kommt das Ablaufdatum der App
/// (bei kostenloser Apple-ID 7 Tage). Ohne Profil, etwa beim unsignierten Cloud-Build, `nil`.
enum ProvisioningInfo {
    static let expirationDate: Date? = readExpirationDate()

    private static func readExpirationDate() -> Date? {
        guard let url = Bundle.main.url(forResource: "embedded", withExtension: "mobileprovision"),
              let data = try? Data(contentsOf: url) else { return nil }
        // Die Datei ist eine signierte Hülle (CMS) um eine XML-Property-List.
        guard let start = data.range(of: Data("<?xml".utf8)),
              let end = data.range(of: Data("</plist>".utf8), in: start.lowerBound..<data.endIndex) else { return nil }
        let plistData = data.subdata(in: start.lowerBound..<end.upperBound)
        guard let plist = try? PropertyListSerialization.propertyList(from: plistData, options: [], format: nil)
                as? [String: Any] else { return nil }
        return plist["ExpirationDate"] as? Date
    }
}
