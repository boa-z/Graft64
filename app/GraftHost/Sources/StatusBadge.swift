import SwiftUI

struct StatusBadge: View {
    let status: ProbeStatus
    var body: some View {
        Label(status.rawValue, systemImage: icon)
            .font(.caption.weight(.semibold))
            .foregroundStyle(color)
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(color.opacity(0.14), in: Capsule())
            .accessibilityLabel("Status \(status.rawValue)")
    }
    private var icon: String { switch status { case .pass: "checkmark.circle.fill"; case .fail: "xmark.circle.fill"; case .skip: "minus.circle.fill"; case .blocked: "nosign" } }
    private var color: Color { switch status { case .pass: .green; case .fail: .red; case .skip: .secondary; case .blocked: .orange } }
}
