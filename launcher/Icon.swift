import AppKit

// The app icon, as in the other native ports: the game's own art on a rounded tile. Here, the skull-faced Gordon from
// the mod's main-menu background (Resources/art/backdrop.png); without that art, a λ on the dark tile.
func writeIconset(_ directory: URL) throws {
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let art = launcherArt("backdrop")
    for size in [16, 32, 128, 256, 512] {
        for scale in [1, 2] {
            let pixels = size * scale, width = CGFloat(pixels)
            let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels,
                                          bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                                          isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
            NSGraphicsContext.saveGraphicsState(); NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
            let r = NSRect(x: width * 0.06, y: width * 0.06, width: width * 0.88, height: width * 0.88)
            let outline = NSBezierPath(roundedRect: r, xRadius: width * 0.18, yRadius: width * 0.18)
            NSGradient(starting: NSColor(srgbRed: 0.35, green: 0.05, blue: 0.04, alpha: 1), ending: NSColor(white: 0.045, alpha: 1))!.draw(in: outline, angle: 90)
            if let art = art {
                // the skull and glasses: a square from the top of the 16:9 image, centred on 79% of its width
                let s = art.size, side = s.height * 0.5, cx = s.width * 0.79
                let from = NSRect(x: min(cx - side / 2, s.width - side), y: s.height - side, width: side, height: side)
                NSGraphicsContext.saveGraphicsState(); outline.addClip()
                art.draw(in: r, from: from, operation: .sourceOver, fraction: 1, respectFlipped: false, hints: [.interpolation: NSImageInterpolation.high])
                NSGraphicsContext.restoreGraphicsState()
            } else {
                let font = NSFont.systemFont(ofSize: width * 0.77, weight: .bold)
                let text = "λ" as NSString, attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: accent]
                let dimensions = text.size(withAttributes: attrs)
                text.draw(at: NSPoint(x: (width - dimensions.width) / 2, y: (width - dimensions.height) / 2 + width * 0.015), withAttributes: attrs)
            }
            accent.setStroke(); outline.lineWidth = max(1, width * 0.02); outline.stroke()
            NSGraphicsContext.restoreGraphicsState()
            let name = "icon_\(size)x\(size)" + (scale == 2 ? "@2x" : "") + ".png"
            try bitmap.representation(using: .png, properties: [:])!.write(to: directory.appendingPathComponent(name))
        }
    }
}
