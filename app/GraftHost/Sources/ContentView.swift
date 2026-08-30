import SwiftUI

struct ContentView: View {
    @Bindable var store: ProbeStore
    @Environment(\.scenePhase) private var scenePhase
    @AppStorage("automaticLifecycleProbe") private var automaticLifecycleProbe = true
    @State private var showingCopied = false
    var body: some View {
        NavigationStack {
            List {
                Section("Environment") {
                    LabeledContent("Device", value: UIDevice.current.model)
                    LabeledContent("OS", value: "\(UIDevice.current.systemName) \(UIDevice.current.systemVersion)")
                    LabeledContent("Page size", value: "\(getpagesize()) bytes")
                    JITStatusView(status: store.jitStatus)
                    Button("Refresh JIT", systemImage: "arrow.clockwise", action: store.refreshJITStatus)
                        .disabled(store.isRunning)
                    Text("JIT is expected to be enabled externally (for example, StikDebug). This app never embeds or invokes private JIT services.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section("Probes") {
                    if store.results.isEmpty { ContentUnavailableView("No results", systemImage: "waveform.path.ecg", description: Text("Run a probe to collect device evidence.")) }
                    ForEach(store.results) { result in ProbeRow(result: result) { store.run(name: result.name) } }
                }
                Section("Diagnostics") {
                    Text(store.logText).font(.system(.footnote, design: .monospaced)).textSelection(.enabled)
                }
            }
            .navigationTitle("GraftHost Probe")
            .toolbar {
                ToolbarItem(placement: .topBarLeading) { Button("Run All", systemImage: "play.fill", action: store.runAll).disabled(store.isRunning) }
                ToolbarItem(placement: .topBarTrailing) { Button("Export JSON", systemImage: "square.and.arrow.up", action: store.exportReport).disabled(store.results.isEmpty) }
                ToolbarItem(placement: .topBarTrailing) { Button("Copy Summary", systemImage: "doc.on.doc") { UIPasteboard.general.string = store.copySummary(); showingCopied = true } .disabled(store.results.isEmpty) }
                ToolbarItem(placement: .topBarTrailing) {
                    NavigationLink(value: SettingsRoute.settings) { Label("Settings", systemImage: "gearshape") }
                }
            }
            .navigationDestination(for: SettingsRoute.self) { _ in AppSettingsView() }
            .alert("Copied", isPresented: $showingCopied) { }
            .task { store.refreshJITStatus() }
            .onChange(of: scenePhase) { _, phase in
                switch phase {
                case .background:
                    store.noteBackground()
                case .active:
                    store.refreshJITStatus()
                    guard automaticLifecycleProbe else { break }
                    store.noteForeground()
                default:
                    break
                }
            }
        }
    }
}

private enum SettingsRoute: Hashable { case settings }
