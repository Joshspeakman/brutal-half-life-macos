import AppKit

let gold = NSColor(srgbRed: 0.91, green: 0.62, blue: 0.29, alpha: 1)
let parchment = NSColor(srgbRed: 0.94, green: 0.91, blue: 0.84, alpha: 1)
let muted = NSColor(srgbRed: 0.68, green: 0.64, blue: 0.55, alpha: 1)
func copper(_ size: CGFloat, bold: Bool = false) -> NSFont {
    NSFont(name: bold ? "Copperplate-Bold" : "Copperplate-Light", size: size) ?? .systemFont(ofSize: size)
}
func label(_ text: String, _ rect: NSRect, size: CGFloat = 14, color: NSColor = parchment) -> NSTextField {
    let field = NSTextField(wrappingLabelWithString: text)
    field.frame = rect; field.font = copper(size); field.textColor = color
    return field
}
final class MetalButton: NSButton {
    var primary = false
    override var acceptsFirstResponder: Bool { true }
    override func draw(_ dirtyRect: NSRect) {
        let edge = NSBezierPath(roundedRect: bounds.insetBy(dx: 2, dy: 2), xRadius: 3, yRadius: 3)
        NSGradient(starting: NSColor(white: isHighlighted ? 0.15 : 0.29, alpha: 1),
                   ending: NSColor(white: 0.08, alpha: 1))!.draw(in: edge, angle: 90)
        gold.withAlphaComponent(isEnabled ? 0.75 : 0.3).setStroke(); edge.lineWidth = 1.5; edge.stroke()
        NSColor.black.setStroke(); NSBezierPath(rect: bounds.insetBy(dx: 5, dy: 5)).stroke()
        let paragraph = NSMutableParagraphStyle(); paragraph.alignment = .center
        let font = copper(primary ? 23 : 17, bold: true)
        let attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: isEnabled ? (primary ? gold : parchment) : muted, .paragraphStyle: paragraph]
        (title as NSString).draw(in: NSRect(x: 8, y: (bounds.height - font.pointSize) / 2 - 2,
                                          width: bounds.width - 16, height: font.pointSize + 6), withAttributes: attrs)
    }
}
final class Backdrop: NSView {
    override var isFlipped: Bool { true }
    override func draw(_ dirtyRect: NSRect) {
        NSColor(white: 0.045, alpha: 1).setFill(); bounds.fill()
        let area = NSBezierPath(rect: NSRect(x: 0, y: 0, width: bounds.width, height: 230))
        NSGradient(starting: NSColor(srgbRed: 0.14, green: 0.10, blue: 0.06, alpha: 1),
                   ending: NSColor(white: 0.025, alpha: 1))!.draw(in: area, angle: 90)
        ("λ" as NSString).draw(at: NSPoint(x: 530, y: -30), withAttributes: [
            .font: NSFont.systemFont(ofSize: 220, weight: .bold), .foregroundColor: gold.withAlphaComponent(0.23)])
        ("BRUTAL" as NSString).draw(at: NSPoint(x: 40, y: 42), withAttributes: [.font: copper(47, bold: true), .foregroundColor: parchment])
        ("HALF-LIFE" as NSString).draw(at: NSPoint(x: 40, y: 96), withAttributes: [.font: copper(47, bold: true), .foregroundColor: parchment])
        ("VERSION 3 · MAC NATIVE" as NSString).draw(at: NSPoint(x: 44, y: 166), withAttributes: [.font: copper(14), .foregroundColor: gold])
        gold.withAlphaComponent(0.5).setFill(); NSRect(x: 40, y: 218, width: bounds.width - 80, height: 1).fill()
    }
}
