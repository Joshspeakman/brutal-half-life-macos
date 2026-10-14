import AppKit

// The launch window, laid out like the other native ports': a banner (the mod's menu art), rows that cycle through
// their choices, and a button for each group of settings that opens it as a panel over the window (Back replaces
// Quit while one is open). Key Guide and Updates run their own action. Arrows, Return and Escape work as in the ports.

final class LaunchRow {
    struct Choice { let value: String; let label: String; let note: String }
    let title: String
    var choices: [Choice]
    var index: Int
    var activate: (() -> Void)?            // a row that runs something instead of cycling (Updates, Files)
    init(_ title: String, _ choices: [Choice], index: Int = 0) {
        self.title = title; self.choices = choices; self.index = min(max(index, 0), choices.count - 1)
    }
    var value: String { choices[index].value }
    var label: String { choices[index].label }
}

final class LaunchSection {
    let title: String
    let rows: [LaunchRow]
    var activate: (() -> Void)?
    var summary: (() -> String)?
    init(_ title: String, rows: [LaunchRow] = []) { self.title = title; self.rows = rows }
}

final class LaunchView: NSView {
    var top: [LaunchRow] = []
    var sections: [LaunchSection] = []
    var open = -1                          // the open group, -1 none
    var focus = 0, hover = hitNone, pressed = hitNone
    var backdrop: NSImage?
    var note = ""
    var playEnabled = true
    var onPlay: (() -> Void)?, onQuit: (() -> Void)?, onChange: (() -> Void)?

    static let hitNone = -1, hitPlay = 100, hitQuit = 101, hitBack = 102, hitSection = 200   // rows: 10*row + 0/1/2
    let bannerHeight: CGFloat = 252, topY: CGFloat = 270

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    var rows: [LaunchRow] { open >= 0 ? sections[open].rows : top }
    var items: Int { rows.count + (open >= 0 ? 0 : sections.count) }   // what the keyboard moves between
    var flyRect: NSRect {                                             // an open group's panel, sized to its rows, above Back and Play
        let need = 90 + 74 * CGFloat(open >= 0 ? sections[open].rows.count : 0), h = min(max(need, 300), bounds.height - 200)
        return NSRect(x: 40, y: bounds.height - 106 - h, width: bounds.width - 80, height: h)
    }
    func rowRect(_ k: Int) -> NSRect {
        let y = open >= 0 ? flyRect.minY + 70 : topY + 14
        return NSRect(x: 70, y: y + 74 * CGFloat(k), width: bounds.width - 140, height: 64)
    }
    func arrow(_ k: Int, right: Bool) -> NSRect {
        let r = rowRect(k); return NSRect(x: right ? r.maxX - 34 : r.maxX - 330, y: r.minY + 6, width: 30, height: 30)
    }
    func valueRect(_ k: Int) -> NSRect { let r = rowRect(k); return NSRect(x: r.maxX - 300, y: r.minY + 7, width: 262, height: 30) }
    func sectionRect(_ i: Int) -> NSRect {
        let per = 2, line = i / per, inLine = min(per, sections.count - line * per), col = i % per
        let w: CGFloat = 290, g: CGFloat = 28, x0 = (bounds.width - (CGFloat(inLine) * w + CGFloat(inLine - 1) * g)) / 2
        return NSRect(x: x0 + CGFloat(col) * (w + g), y: topY + 74 * CGFloat(top.count) + 44 + CGFloat(line) * 100, width: w, height: 54)
    }
    var playRect: NSRect { NSRect(x: bounds.midX + 12, y: bounds.height - 80, width: 220, height: 52) }
    var quitRect: NSRect { NSRect(x: bounds.midX - 232, y: bounds.height - 80, width: 220, height: 52) }

    func hit(_ p: NSPoint) -> Int {
        if playRect.contains(p) { return LaunchView.hitPlay }
        if quitRect.contains(p) { return open >= 0 ? LaunchView.hitBack : LaunchView.hitQuit }
        if open < 0 { for i in sections.indices where sectionRect(i).contains(p) { return LaunchView.hitSection + i } }
        for k in rows.indices {
            if arrow(k, right: false).contains(p) { return 10 * k }
            if arrow(k, right: true).contains(p) { return 10 * k + 2 }
            if valueRect(k).contains(p) { return 10 * k + 1 }
        }
        if open >= 0 && !flyRect.contains(p) { return LaunchView.hitBack }   // a click outside the panel closes it
        return LaunchView.hitNone
    }
    func openSection(_ i: Int) {
        let s = sections[i]; focus = top.count + i
        if let run = s.activate { run(); needsDisplay = true; return }
        open = i; focus = 0; hover = LaunchView.hitNone; needsDisplay = true
    }
    func closeSection() {
        guard open >= 0 else { return }
        focus = top.count + open; open = -1; hover = LaunchView.hitNone; needsDisplay = true
    }
    override func updateTrackingAreas() {
        for t in trackingAreas { removeTrackingArea(t) }
        addTrackingArea(NSTrackingArea(rect: bounds, options: [.mouseMoved, .activeAlways, .inVisibleRect], owner: self, userInfo: nil))
    }
    override func mouseMoved(with e: NSEvent) {
        let h = hit(convert(e.locationInWindow, from: nil)); if h != hover { hover = h; needsDisplay = true }
    }
    override func mouseDown(with e: NSEvent) { pressed = hit(convert(e.locationInWindow, from: nil)); needsDisplay = true }
    override func mouseUp(with e: NSEvent) {
        let h = hit(convert(e.locationInWindow, from: nil)), p = pressed; pressed = LaunchView.hitNone; needsDisplay = true
        guard h == p, h != LaunchView.hitNone else { return }
        switch h {
        case LaunchView.hitPlay: if playEnabled { onPlay?() }
        case LaunchView.hitQuit: onQuit?()
        case LaunchView.hitBack: closeSection()
        case LaunchView.hitSection...: openSection(h - LaunchView.hitSection)
        default:
            let k = h / 10; focus = k
            if let run = rows[k].activate { run(); needsDisplay = true; return }
            step(k, by: h % 10 == 0 ? -1 : 1)
        }
    }
    func step(_ k: Int, by dir: Int) {
        if k >= rows.count { let n = items; focus = (focus + dir + n) % n; needsDisplay = true; return }   // the group buttons
        let r = rows[k]
        if let run = r.activate { run(); needsDisplay = true; return }
        r.index = (r.index + dir + r.choices.count) % r.choices.count; needsDisplay = true
        onChange?()
    }
    override func keyDown(with e: NSEvent) {
        let n = items, rc = rows.count
        switch e.keyCode {
        case 0x24, 0x4c:                                           // Return, Enter
            if focus >= rc { openSection(focus - rc); return }
            if let run = rows[focus].activate { run(); needsDisplay = true; return }
            if open >= 0 { closeSection(); return }
            if playEnabled { onPlay?() }
        case 0x35: if open >= 0 { closeSection() } else { onQuit?() }   // Escape
        case 0x7e: focus = (focus + n - 1) % n; needsDisplay = true
        case 0x7d: focus = (focus + 1) % n; needsDisplay = true
        case 0x7b: step(focus, by: -1)
        case 0x7c: step(focus, by: 1)
        default: super.keyDown(with: e)
        }
    }

    // ------------------------------------------------------------ drawing
    private func shadow(_ blur: CGFloat = 3, dy: CGFloat = -1) -> NSShadow {
        let s = NSShadow(); s.shadowColor = NSColor(white: 0, alpha: 0.9); s.shadowOffset = NSSize(width: 0, height: dy); s.shadowBlurRadius = blur; return s
    }
    private func text(_ s: String, _ font: NSFont, _ color: NSColor, _ r: NSRect, align: NSTextAlignment, shadow sh: NSShadow? = nil, kern: CGFloat = 0) {
        let ps = NSMutableParagraphStyle(); ps.alignment = align; ps.lineBreakMode = .byTruncatingTail
        var a: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: color, .paragraphStyle: ps, .kern: kern]
        if let sh = sh { a[.shadow] = sh }
        (s as NSString).draw(in: r, withAttributes: a)
    }
    /// corner brackets, like the marks of the ports' launchers
    private func brackets(_ r: NSRect, _ len: CGFloat, _ w: CGFloat, _ c: NSColor) {
        c.setStroke(); let p = NSBezierPath(); p.lineWidth = w; p.lineCapStyle = .square
        let (x0, y0, x1, y1) = (r.minX, r.minY, r.maxX, r.maxY)
        p.move(to: NSPoint(x: x0, y: y0 + len)); p.line(to: NSPoint(x: x0, y: y0)); p.line(to: NSPoint(x: x0 + len, y: y0))
        p.move(to: NSPoint(x: x1 - len, y: y0)); p.line(to: NSPoint(x: x1, y: y0)); p.line(to: NSPoint(x: x1, y: y0 + len))
        p.move(to: NSPoint(x: x1, y: y1 - len)); p.line(to: NSPoint(x: x1, y: y1)); p.line(to: NSPoint(x: x1 - len, y: y1))
        p.move(to: NSPoint(x: x0 + len, y: y1)); p.line(to: NSPoint(x: x0, y: y1)); p.line(to: NSPoint(x: x0, y: y1 - len))
        p.stroke()
    }
    private func fillOver(_ r: NSRect) { r.fill(using: .sourceOver) }
    private func button(_ r: NSRect, _ title: String, code: Int, primary: Bool, enabled: Bool = true) {
        let hot = enabled && hover == code, down = enabled && pressed == code
        (down ? accent.withAlphaComponent(0.45) : hot ? accent.withAlphaComponent(0.28) : primary ? accentDim.withAlphaComponent(0.35) : NSColor(white: 0, alpha: 0.6)).setFill()
        fillOver(r)
        let line = enabled && (primary || hot) ? accent : accentDim
        line.setStroke(); let b = NSBezierPath(rect: r.insetBy(dx: 0.5, dy: 0.5)); b.lineWidth = 1; b.stroke()
        brackets(r.insetBy(dx: -4, dy: -4), 10, 2, line)
        let font = din(primary ? 30 : 26)
        let attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: !enabled ? dimColor : hot || primary ? NSColor.white : textColor,
                                                    .shadow: shadow(2, dy: -1.5), .kern: 4]
        let size = (title as NSString).size(withAttributes: attrs)          // the capitals centred on the button
        (title as NSString).draw(at: NSPoint(x: r.midX - size.width / 2, y: (r.midY + font.capHeight / 2 - font.ascender).rounded()), withAttributes: attrs)
    }
    private func drawArrow(_ r: NSRect, right: Bool, lit: Bool) {
        let p = NSBezierPath(), x0 = r.minX + 9, x1 = r.maxX - 9, y0 = r.minY + 7, y1 = r.maxY - 7, ym = r.midY
        if right { p.move(to: NSPoint(x: x0, y: y0)); p.line(to: NSPoint(x: x1, y: ym)); p.line(to: NSPoint(x: x0, y: y1)) }
        else { p.move(to: NSPoint(x: x1, y: y0)); p.line(to: NSPoint(x: x0, y: ym)); p.line(to: NSPoint(x: x1, y: y1)) }
        p.close(); (lit ? NSColor.white : accent).setFill(); p.fill()
    }
    private func drawRows(_ sh: NSShadow) {
        for (k, row) in rows.enumerated() {
            let r = rowRect(k), choice = row.choices[row.index]
            if k == focus {
                accent.withAlphaComponent(0.12).setFill(); fillOver(r.insetBy(dx: -10, dy: -2))
                accent.setFill(); NSRect(x: r.minX - 10, y: r.minY - 2, width: 3, height: r.height + 4).fill()
            }
            text(row.title.uppercased(), din(24), accent, NSRect(x: r.minX + 4, y: r.minY + 10, width: 300, height: 30), align: .left, shadow: sh, kern: 2)
            if row.activate == nil {
                drawArrow(arrow(k, right: false), right: false, lit: hover == 10 * k)
                drawArrow(arrow(k, right: true), right: true, lit: hover == 10 * k + 2)
            }
            text(choice.label, body(20, bold: true), hover == 10 * k + 1 ? .white : textColor, valueRect(k), align: .center, shadow: sh, kern: 0.5)
            text(choice.note, body(12.5), dimColor, NSRect(x: r.maxX - 340, y: r.minY + 40, width: 350, height: 18), align: .center)
            if k + 1 < rows.count { accentDim.withAlphaComponent(0.35).setFill(); fillOver(NSRect(x: r.minX, y: r.maxY + 5, width: r.width, height: 1)) }
        }
    }
    private func drawPanel(_ panel: NSRect) {
        NSColor(white: 0, alpha: 0.62).setFill(); fillOver(panel)
        accentDim.withAlphaComponent(0.8).setStroke(); let p = NSBezierPath(rect: panel.insetBy(dx: 0.5, dy: 0.5)); p.lineWidth = 1; p.stroke()
        brackets(panel.insetBy(dx: -5, dy: -5), 18, 2.5, accent)
    }
    /// an image filling a rectangle, its aspect kept (cropped around the given point of the image, 0...1)
    private func cover(_ img: NSImage, _ r: NSRect, fraction: CGFloat, focus: NSPoint = NSPoint(x: 0.5, y: 0.5)) {
        let s = img.size, sc = max(r.width / s.width, r.height / s.height), w = r.width / sc, h = r.height / sc
        let x = min(max(s.width * focus.x - w / 2, 0), s.width - w), y = min(max(s.height * focus.y - h / 2, 0), s.height - h)
        img.draw(in: r, from: NSRect(x: x, y: s.height - y - h, width: w, height: h), operation: .sourceOver, fraction: fraction,
                 respectFlipped: true, hints: [.interpolation: NSImageInterpolation.high])
    }
    private func drawBanner(_ br: NSRect, _ sh: NSShadow) {
        NSColor.black.setFill(); br.fill()
        // the mod's key art: the skull-faced Gordon on the right, the λ logo behind the title
        if let art = backdrop { cover(art, br, fraction: 1, focus: NSPoint(x: 0.62, y: 0.32)) }
        NSGradient(colorsAndLocations: (NSColor(white: 0, alpha: 0.78), 0), (NSColor(white: 0, alpha: 0.35), 0.45), (NSColor(white: 0, alpha: 0.05), 0.75))!
            .draw(in: br, angle: 0)
        let lr = NSRect(x: 34, y: 54, width: 360, height: 150)
        text("BRUTAL", din(66), .white, NSRect(x: lr.minX, y: lr.minY, width: lr.width, height: 70), align: .left, shadow: shadow(14, dy: 0), kern: 8)
        text("HALF-LIFE", din(66), accent, NSRect(x: lr.minX, y: lr.minY + 62, width: lr.width, height: 70), align: .left, shadow: shadow(14, dy: 0), kern: 8)
        text("VERSION 3 · NATIVE EDITION FOR macOS", din(16), textColor, NSRect(x: lr.minX + 2, y: lr.maxY + 2, width: lr.width + 60, height: 22), align: .left, shadow: sh, kern: 3)
        NSGradient(starting: NSColor(white: 0, alpha: 0), ending: NSColor(white: 0, alpha: 0.85))!.draw(in: NSRect(x: 0, y: bannerHeight - 36, width: br.width, height: 36), angle: 90)
        accentDim.setFill(); NSRect(x: 0, y: bannerHeight, width: br.width, height: 1.5).fill()
    }
    override func draw(_ dirtyRect: NSRect) {
        let b = bounds, sh = shadow()
        NSColor(white: 0.04, alpha: 1).setFill(); b.fill()
        if let art = backdrop { cover(art, b, fraction: 0.32, focus: NSPoint(x: 0.25, y: 0.5)) }   // darker below the banner
        NSGradient(colorsAndLocations: (NSColor(white: 0, alpha: 0.35), 0), (NSColor(white: 0, alpha: 0.82), 1))!
            .draw(in: b, relativeCenterPosition: NSPoint(x: 0, y: -0.1))
        drawBanner(NSRect(x: 0, y: 0, width: b.width, height: bannerHeight), sh)
        // the top level: the rows, then a button for each group with a summary under it
        let fly = open, savedFocus = focus, savedHover = hover; open = -1
        if fly >= 0 { focus = LaunchView.hitNone; hover = LaunchView.hitNone }   // under the panel: nothing lit
        drawPanel(NSRect(x: 54, y: topY, width: b.width - 108, height: 74 * CGFloat(top.count) + 14))
        drawRows(sh)
        for (i, s) in sections.enumerated() {
            let r = sectionRect(i)
            if fly < 0 && focus == top.count + i { accent.withAlphaComponent(0.22).setFill(); fillOver(r.insetBy(dx: -8, dy: -7)) }
            button(r, s.title.uppercased(), code: LaunchView.hitSection + i, primary: false)
            let summary = s.summary?() ?? s.rows.map { $0.label }.joined(separator: " · ")
            text(summary, body(12.5), dimColor, NSRect(x: r.minX - 10, y: r.maxY + 8, width: r.width + 20, height: 32), align: .center, shadow: sh)
        }
        if !note.isEmpty, let last = sections.indices.last {
            text(note, body(12.5), dimColor, NSRect(x: 54, y: sectionRect(last).minY + 98, width: b.width - 108, height: 34), align: .center, shadow: sh)
        }
        open = fly; focus = savedFocus; hover = savedHover
        // an open group: a panel over the window
        if open >= 0 {
            NSColor(white: 0, alpha: 0.6).setFill(); fillOver(b)
            let f = flyRect
            NSColor(white: 0.05, alpha: 0.985).setFill(); fillOver(f)
            accentDim.setStroke(); let fp = NSBezierPath(rect: f.insetBy(dx: 0.5, dy: 0.5)); fp.lineWidth = 1; fp.stroke()
            brackets(f.insetBy(dx: -5, dy: -5), 22, 3, accent)
            text(sections[open].title.uppercased(), din(30), accent, NSRect(x: f.minX, y: f.minY + 20, width: f.width, height: 36), align: .center, shadow: sh, kern: 5)
            accentDim.withAlphaComponent(0.6).setFill(); fillOver(NSRect(x: f.minX + 30, y: f.minY + 60, width: f.width - 60, height: 1))
            drawRows(sh)
        }
        button(quitRect, open >= 0 ? "BACK" : "QUIT", code: open >= 0 ? LaunchView.hitBack : LaunchView.hitQuit, primary: false)
        button(playRect, "PLAY", code: LaunchView.hitPlay, primary: true, enabled: playEnabled)
    }
}
