import SwiftUI

struct ProbeRow: View {
    let result: ProbeResultModel
    let runAction: () -> Void
    @State private var isExpanded = false

    var body: some View {
        VStack(alignment: .leading) {
            HStack(alignment: .firstTextBaseline) {
                Text(result.name)
                    .font(.headline.monospaced())
                Spacer()
                StatusBadge(status: result.status)
            }
            Text(result.summary)
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            Button("Run Probe", systemImage: "play.fill", action: runAction)
                .buttonStyle(.bordered)
                .frame(minHeight: 44)
                .disabled(result.status == .pass && result.name == "runtime_paths")
                .accessibilityLabel("Run \(result.name) probe")
            DisclosureGroup(isExpanded: $isExpanded) {
                VStack(alignment: .leading) {
                    LabeledContent("Reason code") {
                        Text(result.reasonCode, format: .number)
                    }
                    LabeledContent("Graft error") {
                        Text(result.graftError, format: .number)
                    }
                    LabeledContent("OS error") {
                        Text(result.osError, format: .number)
                    }
                    LabeledContent("Duration") {
                        Text(result.durationNanoseconds, format: .number)
                        Text("ns")
                    }
                    Divider()
                    Text("Evidence JSON")
                        .font(.subheadline)
                        .bold()
                    Text(result.detailsJSON)
                        .font(.footnote.monospaced())
                        .foregroundStyle(.secondary)
                        .textSelection(.enabled)
                        .fixedSize(horizontal: false, vertical: true)
                        .accessibilityLabel("Probe evidence JSON")
                        .accessibilityValue(result.detailsJSON)
                }
                .padding(.top)
            } label: {
                Label("Probe Details", systemImage: "info.circle")
            }
            .accessibilityHint(isExpanded ? "Collapses probe evidence and error codes" : "Shows probe evidence and error codes")
        }
        .padding(.vertical)
    }
}
