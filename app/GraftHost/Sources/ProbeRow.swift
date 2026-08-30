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
            if result.reasonCode != 0 || result.graftError != 0 {
                Text("reason: \(result.reasonCode) · graft: \(result.graftError)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            if result.osError != 0 {
                Text("OS error: \(result.osError)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 6)
    }
}
