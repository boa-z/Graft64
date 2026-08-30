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
    struct Device: Codable { let model: String; let systemName: String; let systemVersion: String; let machine: String; let pageSize: Int }
    struct Environment: Codable { let livecontainerDetected: Bool; let jitExpected: Bool }
    let schemaVersion: Int
    let appVersion: String
    let buildCommit: String
    let timestampUTC: Date
    let device: Device
    let environment: Environment
    let probes: [ProbeResultModel]
}
