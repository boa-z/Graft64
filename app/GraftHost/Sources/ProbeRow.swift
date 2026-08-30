import SwiftUI

struct ProbeRow: View {
    let result: ProbeResultModel
    let runAction: () -> Void
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(result.name).font(.headline.monospaced())
                Spacer()
                StatusBadge(status: result.status)
                Button("Run", action: runAction)
                    .buttonStyle(.bordered)
                    .disabled(result.status == .pass && result.name == "runtime_paths")
            }
            Text(result.summary).font(.subheadline).foregroundStyle(.secondary)
            if result.systemError != 0 { Text("errno: \(result.systemError)").font(.caption).foregroundStyle(.secondary) }
        }
        .padding(.vertical, 6)
    }
}
