import Foundation

struct AppInfo {
    static var version: String { value(forKey: "CFBundleShortVersionString", fallback: "0.1") }
    static var build: String { value(forKey: "CFBundleVersion", fallback: "1") }
    static var commit: String {
        let value = value(forKey: "GRAFT_BUILD_COMMIT", fallback: "unknown")
        return value.contains("$(") ? "unknown" : value
    }

    private static func value(forKey key: String, fallback: String) -> String {
        guard let value = Bundle.main.object(forInfoDictionaryKey: key) as? String,
              !value.isEmpty else { return fallback }
        return value
    }
}
