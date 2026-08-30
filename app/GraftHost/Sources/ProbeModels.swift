import Foundation

enum ProbeStatus: String, Codable, CaseIterable {
    case pass = "PASS"
    case fail = "FAIL"
    case skip = "SKIP"
    case blocked = "BLOCKED"
}

struct ProbeResultModel: Identifiable, Codable, Hashable {
    let id: String
    let name: String
    let status: ProbeStatus
    let systemError: Int32
    let durationNanoseconds: UInt64
    let summary: String
    let detailsJSON: String

    enum CodingKeys: String, CodingKey {
        case id, name, status
        case systemError = "system_error"
        case durationNanoseconds = "duration_nanoseconds"
        case summary
        case detailsJSON = "details_json"
    }

    init(name: String, status: ProbeStatus, systemError: Int32, durationNanoseconds: UInt64, summary: String, detailsJSON: String) {
        self.id = name
        self.name = name
        self.status = status
        self.systemError = systemError
        self.durationNanoseconds = durationNanoseconds
        self.summary = summary
        self.detailsJSON = detailsJSON
    }
}

struct DeviceReport: Codable {
    struct Device: Codable {
        let model: String
        let systemName: String
        let systemVersion: String
        let machine: String
        let pageSize: Int
        enum CodingKeys: String, CodingKey {
            case model
            case systemName = "system_name"
            case systemVersion = "system_version"
            case machine
            case pageSize = "page_size"
        }
    }
    struct Environment: Codable {
        let livecontainerDetected: Bool
        let jitExpected: Bool
        enum CodingKeys: String, CodingKey {
            case livecontainerDetected = "livecontainer_detected"
            case jitExpected = "jit_expected"
        }
    }
    let schemaVersion: Int
    let appVersion: String
    let buildCommit: String
    let timestampUTC: Date
    let device: Device
    let environment: Environment
    let probes: [ProbeResultModel]
    enum CodingKeys: String, CodingKey {
        case schemaVersion = "schema_version"
        case appVersion = "app_version"
        case buildCommit = "build_commit"
        case timestampUTC = "timestamp_utc"
        case device, environment, probes
    }
}
