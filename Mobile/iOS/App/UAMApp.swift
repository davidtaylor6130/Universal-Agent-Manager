import SwiftUI

@main
struct UAMApp: App
{
    @State private var model = MobileAppModel()
    @AppStorage("uam.appearance") private var appearance = "dark"

    var body: some Scene
    {
        WindowGroup
        {
            HomeView().environment(model).tint(UAMTheme.accent)
                .preferredColorScheme(appearance == "system" ? nil : appearance == "light" ? .light : .dark)
                .onOpenURL { model.pendingPairingURL = $0 }
        }
    }
}
