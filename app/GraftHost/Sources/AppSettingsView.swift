import SwiftUI

struct AppSettingsView: View {
    @AppStorage("automaticLifecycleProbe") private var automaticLifecycleProbe = true

    var body: some View {
        Form {
            Section("Probes") {
                Toggle("Automatic lifecycle JIT probe", isOn: $automaticLifecycleProbe)
                Text("When enabled, GraftHost checks JIT again after the app returns from the background. The complete executable probe remains available under Run All.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            Section("JIT provider") {
                Text("JIT is supplied externally by the LiveContainer host (for example, StikDebug). GraftHost tests executable memory transitions with public APIs; it never invokes a private JIT service.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            Section("Application") {
                LabeledContent("Version", value: AppInfo.version)
                LabeledContent("Build", value: AppInfo.build)
                LabeledContent("Commit", value: AppInfo.commit)
            }
        }
        .navigationTitle("Settings")
        .navigationBarTitleDisplayMode(.inline)
    }
}
