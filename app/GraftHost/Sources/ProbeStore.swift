import Foundation
import Observation
import UIKit

@MainActor @Observable
final class ProbeStore {
    private(set) var results: [ProbeResultModel] = []
    private(set) var isRunning = false
    private(set) var lastExportURL: URL?
    var logText = "Ready. JIT probes require a device with JIT enabled by the host."

    var passCount: Int { results.count(where: { $0.status == .pass }) }

    func runAll() {
        guard !isRunning else { return }
        isRunning = true
        results = []
        logText = "Running all host probes…"
        let context = Unmanaged.passUnretained(self).toOpaque()
        _ = graft_run_all_probes(Self.callback, context)
        isRunning = false
        logText += "\nCompleted \(results.count) probes."
    }

    func run(name: String) {
        guard !isRunning else { return }
        isRunning = true
        let context = Unmanaged.passUnretained(self).toOpaque()
        _ = graft_run_probe(name, Self.callback, context)
        isRunning = false
    }

    func exportReport() {
        let report = DeviceReport(
            schemaVersion: 1,
            appVersion: Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0.1",
            buildCommit: Bundle.main.object(forInfoDictionaryKey: "GRAFT_BUILD_COMMIT") as? String ?? "unavailable",
            timestampUTC: .now,
            device: .init(model: UIDevice.current.model, systemName: UIDevice.current.systemName, systemVersion: UIDevice.current.systemVersion, machine: ProcessInfo.processInfo.environment["SIMULATOR_MODEL_IDENTIFIER"] ?? "arm64-device", pageSize: Int(getpagesize())),
            environment: .init(livecontainerDetected: results.contains(where: { $0.detailsJSON.localizedStandardContains("LiveContainer") }), jitExpected: true),
            probes: results
        )
        do {
            let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]; encoder.dateEncodingStrategy = .iso8601
            let data = try encoder.encode(report)
            let reportsURL = URL.documentsDirectory.appending(path: "Graft64/Reports", directoryHint: .isDirectory)
            try FileManager.default.createDirectory(at: reportsURL, withIntermediateDirectories: true)
            let stamp = ISO8601DateFormatter().string(from: report.timestampUTC).replacing("/", with: "-").replacing(":", with: "")
            let url = reportsURL.appending(path: "(stamp)-(UIDevice.current.model).json")
            try data.write(to: url, options: .atomic)
            lastExportURL = url
            logText += "\nExported report: \(url.path)"
        } catch {
            logText += "\nExport failed: \(error.localizedDescription)"
        }
    }

    func copySummary() -> String {
        results.map { "\($0.status.rawValue) \($0.name): \($0.summary)" }.joined(separator: "\n")
    }

    private static let callback: graft_probe_callback = { result, context in
        guard let result, let context else { return }
        let store = Unmanaged<ProbeStore>.fromOpaque(context).takeUnretainedValue()
        store.append(result)
    }

    private func append(_ result: UnsafePointer<graft_probe_result>) {
        let name = String(cString: result.pointee.name)
        let summary = String(cString: result.pointee.summary)
        let details = String(cString: result.pointee.details_json)
        let status: ProbeStatus = switch result.pointee.status { case GRAFT_PROBE_PASS: .pass; case GRAFT_PROBE_FAIL: .fail; case GRAFT_PROBE_BLOCKED: .blocked; default: .skip }
        results.append(.init(name: name, status: status, systemError: result.pointee.system_error, durationNanoseconds: result.pointee.duration_ns, summary: summary, detailsJSON: details))
        logText += "\n\(status.rawValue) \(name): \(summary)"
    }
}
