import AppKit

func writeIconset(_ directory: URL) throws {
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    for size in [16, 32, 128, 256, 512] {
        for scale in [1, 2] {
            let pixels = size * scale, width = CGFloat(pixels)
            let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels,
                                          bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                                          isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
            NSGraphicsContext.saveGraphicsState(); NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
            let r = NSRect(x: width * 0.06, y: width * 0.06, width: width * 0.88, height: width * 0.88)
            let outline = NSBezierPath(roundedRect: r, xRadius: width * 0.18, yRadius: width * 0.18)
            NSGradient(starting: NSColor(srgbRed: 0.26, green: 0.17, blue: 0.08, alpha: 1),
                       ending: NSColor(white: 0.045, alpha: 1))!.draw(in: outline, angle: 90)
            gold.setStroke(); outline.lineWidth = max(1, width * 0.018); outline.stroke()
            let font = NSFont.systemFont(ofSize: width * 0.77, weight: .bold)
            let text = "λ" as NSString, attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: gold]
            let dimensions = text.size(withAttributes: attrs)
            text.draw(at: NSPoint(x: (width - dimensions.width) / 2, y: (width - dimensions.height) / 2 + width * 0.015), withAttributes: attrs)
            NSGraphicsContext.restoreGraphicsState()
            let name = "icon_\(size)x\(size)" + (scale == 2 ? "@2x" : "") + ".png"
            try bitmap.representation(using: .png, properties: [:])!.write(to: directory.appendingPathComponent(name))
        }
    }
}
