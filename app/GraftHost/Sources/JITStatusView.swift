import SwiftUI

enum GraftJITStatus: Equatable {
    case unknown
    case enabled
    case disabled
    case unavailable

    var title: String {
        switch self {
        case .unknown: "Unknown"
        case .enabled: "Available"
        case .disabled: "Not enabled"
        case .unavailable: "Unavailable"
        }
    }

    var icon: String {
        switch self {
        case .unknown: "questionmark.circle"
        case .enabled: "checkmark.circle.fill"
        case .disabled: "xmark.circle.fill"
        case .unavailable: "minus.circle.fill"
        }
    }

    var color: Color {
        switch self {
        case .unknown: .secondary
        case .enabled: .green
        case .disabled: .orange
        case .unavailable: .secondary
        }
    }
}

struct JITStatusView: View {
    let status: GraftJITStatus

    var body: some View {
        LabeledContent {
            Label(status.title, systemImage: status.icon)
                .foregroundStyle(status.color)
        } label: {
            Label("JIT", systemImage: "bolt.fill")
        }
        .accessibilityElement(children: .contain)
        .accessibilityLabel("JIT execution")
        .accessibilityValue(status.title)
    }
}
