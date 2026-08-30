import Foundation
import Observation
import UIKit

@MainActor @Observable
final class ProbeStore {
    private(set) var results: [ProbeResultModel] = []
    private(set) var isRunning = false
    private(set) var lastExportURL: URL?
    private(set) var jitStatus: GraftJITStatus = .unknown
    var logText = "Ready. JIT probes require a device with JIT enabled by the host."
    private var backgroundObserved = false
    private let liveContainerEvidence: [String]

    init() {
        let environment = ProcessInfo.processInfo.environment
        liveContainerEvidence = ["LC_HOME_PATH", "LP_HOME_PATH"]
            .filter { environment[$0] != nil }
            .map { "environment:\($0)" }
        let bundleRoot = Bundle.main.bundleURL.path
        let bundleExecutable = Bundle.main.executableURL?.path ?? "unavailable"
        let runtimeRoot = bundleRoot
        let dataRoot = URL.documentsDirectory.appending(path: "Graft64", directoryHint: .isDirectory).path
        let cacheRoot = URL.cachesDirectory.appending(path: "Graft64", directoryHint: .isDirectory).path
        do {
            try FileManager.default.createDirectory(atPath: dataRoot, withIntermediateDirectories: true)
            try FileManager.default.createDirectory(atPath: cacheRoot, withIntermediateDirectories: true)
        } catch {
            logText += "\nRuntime directory setup failed: \(error.localizedDescription)"
        }
        let pathResult = Self.withCStringPointers([bundleRoot, runtimeRoot, dataRoot, cacheRoot][...]) { pointers in
            var context = graft_path_context(guest_bundle_root: pointers[0],
                                              runtime_root: pointers[1],
                                              data_root: pointers[2],
                                              cache_root: pointers[3])
            return graft_configure_path_context(&context)
        }
        if pathResult != 0 {
            logText += "\nRuntime root configuration failed."
        }
        let evidence = liveContainerEvidence.isEmpty ? "none" : liveContainerEvidence.joined(separator: ",")
        let observationValues = [
            bundleRoot,
            bundleExecutable,
            CommandLine.arguments.first ?? bundleExecutable,
            FileManager.default.currentDirectoryPath,
            NSHomeDirectory(),
            URL.documentsDirectory.path,
            URL.libraryDirectory.path,
            FileManager.default.temporaryDirectory.path,
            AppInfo.version,
            evidence,
        ]
        let observationResult = Self.withCStringPointers(observationValues[...]) { pointers in
            var observation = graft_runtime_observation(
                bundle_url: pointers[0],
                bundle_executable_url: pointers[1],
                argv0: pointers[2],
                current_working_directory: pointers[3],
                home_directory: pointers[4],
                documents_directory: pointers[5],
                library_directory: pointers[6],
                temporary_directory: pointers[7],
                app_version: pointers[8],
                livecontainer_evidence: pointers[9]
            )
            return graft_configure_runtime_observation(&observation)
        }
        if observationResult != 0 {
            logText += "\nRuntime observation configuration failed."
        }
        if let helperURL = Bundle.main.url(forResource: "GraftProbeHelper", withExtension: nil) {
            _ = graft_configure_helper(helperURL.path)
        }
        if let dylibURL = Bundle.main.url(forResource: "GraftProbeTest", withExtension: "dylib") {
            _ = graft_configure_dylib(dylibURL.path)
        }
    }

    var passCount: Int { results.filter { $0.status == .pass }.count }

    func refreshJITStatus() {
        let status = graft_jit_check()
        jitStatus = switch status {
        case GRAFT_JIT_STATUS_ENABLED: .enabled
        case GRAFT_JIT_STATUS_DISABLED: .disabled
        case GRAFT_JIT_STATUS_UNAVAILABLE: .unavailable
        default: .unknown
        }
        logText += "\nJIT status: \(jitStatus.title)"
    }

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

    func noteBackground() {
        backgroundObserved = true
        _ = graft_lifecycle_note_background()
    }

    func noteForeground() {
        guard backgroundObserved, !isRunning else { return }
        guard graft_lifecycle_note_foreground() == 0 else { return }
        isRunning = true
        let context = Unmanaged.passUnretained(self).toOpaque()
        _ = graft_run_probe("lifecycle_jit", Self.callback, context)
        isRunning = false
    }

    func exportReport() {
        let report = DeviceReport(
            schemaVersion: 2,
            appVersion: AppInfo.version,
            buildCommit: AppInfo.commit,
            timestampUTC: .now,
            device: .init(model: UIDevice.current.model, systemName: UIDevice.current.systemName, systemVersion: UIDevice.current.systemVersion, machine: Self.machineIdentifier, pageSize: Int(getpagesize())),
            environment: .init(livecontainerDetected: !liveContainerEvidence.isEmpty, livecontainerEvidence: liveContainerEvidence, jitExpected: true),
            probes: results
        )
        do {
            let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]; encoder.dateEncodingStrategy = .iso8601
            let data = try encoder.encode(report)
            let reportsURL = URL.documentsDirectory.appending(path: "Graft64/Reports", directoryHint: .isDirectory)
            try FileManager.default.createDirectory(at: reportsURL, withIntermediateDirectories: true)
            let stamp = ISO8601DateFormatter().string(from: report.timestampUTC).replacing("/", with: "-").replacing(":", with: "")
            let url = reportsURL.appending(path: "\(stamp)-\(UIDevice.current.model).json")
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
        let updated = ProbeResultModel(name: name, status: status, reasonCode: Int32(result.pointee.reason_code.rawValue), graftError: Int32(result.pointee.graft_error.rawValue), osError: result.pointee.os_error, durationNanoseconds: result.pointee.duration_ns, summary: summary, detailsJSON: details)
        if let index = results.firstIndex(where: { $0.id == updated.id }) {
            results[index] = updated
        } else {
            results.append(updated)
        }
        logText += "\n\(status.rawValue) \(name): \(summary)"
    }

    private static var machineIdentifier: String {
        if let simulator = ProcessInfo.processInfo.environment["SIMULATOR_MODEL_IDENTIFIER"] {
            return simulator
        }
        var systemInfo = utsname()
        guard uname(&systemInfo) == 0 else { return "unknown" }
        return withUnsafeBytes(of: &systemInfo.machine) { bytes in
            String(decoding: bytes.prefix { $0 != 0 }, as: UTF8.self)
        }
    }

    private static func withCStringPointers<Result>(
        _ values: ArraySlice<String>,
        pointers: [UnsafePointer<CChar>] = [],
        body: ([UnsafePointer<CChar>]) -> Result
    ) -> Result {
        guard let first = values.first else { return body(pointers) }
        return first.withCString { pointer in
            withCStringPointers(values.dropFirst(), pointers: pointers + [pointer], body: body)
        }
    }
}
