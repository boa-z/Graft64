import SwiftUI

@main
@MainActor
struct GraftHostApp: App {
    @State private var store = ProbeStore()
    var body: some Scene { WindowGroup { ContentView(store: store) } }
}
