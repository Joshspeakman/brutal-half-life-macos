import AppKit

// The launch window's palette and type, as in the other native ports' launchers (Dungeon Siege, Delta Force), with
// Brutal Half-Life's blood red as the accent.
let accent = NSColor(srgbRed: 0.93, green: 0.25, blue: 0.18, alpha: 1)
let accentDim = NSColor(srgbRed: 0.56, green: 0.14, blue: 0.10, alpha: 1)
let textColor = NSColor(srgbRed: 0.93, green: 0.93, blue: 0.88, alpha: 1)
let dimColor = NSColor(srgbRed: 0.66, green: 0.66, blue: 0.6, alpha: 1)
func din(_ size: CGFloat) -> NSFont { NSFont(name: "DINCondensed-Bold", size: size) ?? .systemFont(ofSize: size, weight: .bold) }
func body(_ size: CGFloat, bold: Bool = false) -> NSFont {
    NSFont(name: bold ? "AvenirNextCondensed-DemiBold" : "AvenirNextCondensed-Medium", size: size)
        ?? .systemFont(ofSize: size, weight: bold ? .semibold : .regular)
}

/// The launch window's art (tools/bhl_art.py at install time): the mod's own main-menu background.
func launcherArt(_ name: String) -> NSImage? {
    guard let dir = Bundle.main.resourceURL?.appendingPathComponent("art") else { return nil }
    for ext in ["png", "jpg"] {
        if let image = NSImage(contentsOf: dir.appendingPathComponent("\(name).\(ext)")), image.size.width > 16 { return image }
    }
    return nil
}
