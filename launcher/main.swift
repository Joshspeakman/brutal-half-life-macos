import AppKit
import Darwin

func failure(_ message: String) -> NSError {
    NSError(domain: "BrutalHalfLife", code: 1, userInfo: [NSLocalizedDescriptionKey: message])
}

struct Installation {
    let resources: URL
    let data: URL
    var runtime: URL { resources.appendingPathComponent("Runtime") }
    var mod: URL { data.appendingPathComponent("bhl") }
    var saves: URL { mod.appendingPathComponent("save") }
    var log: URL { data.appendingPathComponent("Last Run.log") }
    func openPlayLog(for process: Process) throws -> FileHandle {
        let fm = FileManager.default
        if fm.fileExists(atPath: log.path), (try log.resourceValues(forKeys: [.fileSizeKey])).fileSize ?? 0 > 0 {
            let history = data.appendingPathComponent("Play Logs", isDirectory: true)
            try fm.createDirectory(at: history, withIntermediateDirectories: true)
            let date = (try log.resourceValues(forKeys: [.contentModificationDateKey])).contentModificationDate ?? Date()
            let stamp = ISO8601DateFormatter().string(from: date).replacingOccurrences(of: ":", with: "-")
            try fm.copyItem(at: log, to: history.appendingPathComponent("\(stamp)-\(UUID().uuidString).log"))
        }
        guard fm.createFile(atPath: log.path, contents: nil) else { throw failure("Could not open the play log.") }
        let handle = try FileHandle(forWritingTo: log)
        try handle.truncate(atOffset: 0)
        let build = try String(contentsOf: resources.appendingPathComponent("Build.json"), encoding: .utf8)
        let header = "BHL play started: \(ISO8601DateFormatter().string(from: Date()))\nBuild: \(build)\nArguments: \(process.arguments ?? [])\n"
        try handle.write(contentsOf: Data(header.utf8))
        return handle
    }
    init() throws {
        guard let resource = Bundle.main.resourceURL else { throw failure("Run the installed app bundle.") }
        resources = resource
        let path = ProcessInfo.processInfo.environment["BHL_DATA_DIR"] ?? Bundle.main.object(forInfoDictionaryKey: "BHLDataDirectory") as? String
        data = path.map { URL(fileURLWithPath: $0) } ?? FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/Brutal Half-Life")
    }
    func validate() throws {
        for url in [runtime.appendingPathComponent("libxash.dylib"), runtime.appendingPathComponent("client_arm64.dylib"),
                    runtime.appendingPathComponent("libref_metal.dylib"), runtime.appendingPathComponent("libEGL.dylib"),
                    runtime.appendingPathComponent("libGLESv2.dylib"),
                    runtime.appendingPathComponent("hl_arm64.dylib"), resources.appendingPathComponent("Images/hl.dll"),
                    data.appendingPathComponent("native-install.json"), data.appendingPathComponent("valve/halflife.wad"),
                    mod.appendingPathComponent("maps/start.bsp")] {
            guard FileManager.default.fileExists(atPath: url.path) else { throw failure("Missing \(url.lastPathComponent). Rebuild this app with your Half-Life files and the BHL v3 archive.") }
        }
    }
    func latestSave() -> String? {
        let files = (try? FileManager.default.contentsOfDirectory(at: saves, includingPropertiesForKeys: [.contentModificationDateKey])) ?? []
        return files.filter { $0.pathExtension.lowercased() == "sav" && $0.deletingPathExtension().lastPathComponent.range(of: "^[A-Za-z0-9_-]+$", options: .regularExpression) != nil }
            .max { a, b in
                let ad = (try? a.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
                let bd = (try? b.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
                return ad < bd
            }?.deletingPathExtension().lastPathComponent
    }
    func process(mode: Int, resolution: Int, fullscreen: Bool, difficulty: Int, frameRate: Int = 0, vsync: Bool = true, aroundNotch: Bool = false, testScript: String? = nil) throws -> Process {
        try validate()
        // The window-owning process must retain our app identity and Game Mode
        // metadata. A bare Runtime/xash3d child does not inherit them.
        let p = Process(); p.executableURL = Bundle.main.executableURL; p.currentDirectoryURL = data
        var env = ProcessInfo.processInfo.environment
        for key in env.keys where key.hasPrefix("BHL_") || key.hasPrefix("SDL_") || key.hasPrefix("DYLD_") || key.hasPrefix("XASH3D_") { env.removeValue(forKey: key) }
        env["BHL_IMAGE_DIR"] = resources.appendingPathComponent("Images").path
        env["XASH3D_BASEDIR"] = data.path
        env["XASH3D_EXTRAS_PAK1"] = resources.appendingPathComponent("extras.pk3").path
        env["DYLD_LIBRARY_PATH"] = runtime.path
        env["SDL_EGL_LIBRARY"] = runtime.appendingPathComponent("libEGL.dylib").path
        env["SDL_OPENGL_LIBRARY"] = runtime.appendingPathComponent("libGLESv2.dylib").path
        env["ANGLE_DEFAULT_PLATFORM"] = "metal"
        // A macOS fullscreen Space always keeps the picture below the camera
        // housing. Covering the whole display instead measured about 7 ms less
        // presentation latency on the development M1 Pro; Game Mode still
        // activates for this fullscreen window.
        let fullDisplay = fullscreen && aroundNotch
        env["SDL_VIDEO_MAC_FULLSCREEN_SPACES"] = fullDisplay ? "0" : "1"
        let sizes = [(1280, 720), (1600, 900), (1920, 1080), (2560, 1440)]
        let size = sizes[min(max(resolution, 0), sizes.count - 1)]
        var args = ["--run-engine", "-game", "bhl", "-ref", "metal", "-metal", "-dll", runtime.appendingPathComponent("hl_arm64.dylib").path,
                    "-clientlib", runtime.appendingPathComponent("client_arm64.dylib").path,
                    "-menulib", runtime.appendingPathComponent("libmenu.dylib").path,
                    fullscreen ? "-borderless" : "-windowed", "-width", String(size.0), "-height", String(size.1),
                    "-noip", "-noip6", "-nomaster", "+sv_lan", "1", "+cl_lw", "0", "+skill", String(difficulty + 1)]
        if let script = testScript {
            guard script.range(of: "^[A-Za-z0-9_-]+\\.cfg$", options: .regularExpression) != nil,
                  FileManager.default.fileExists(atPath: mod.appendingPathComponent(script).path) else { throw failure("Invalid acceptance script.") }
            env["SDL_VIDEODRIVER"] = "cocoa"; env["SDL_AUDIODRIVER"] = "dummy"
            env["DYLD_PRINT_LIBRARIES"] = "1"
            if !CommandLine.arguments.contains("--engine-test-visible") { args += ["-hidden"] }
            args += ["-nomouse", "-nowriteconfig", "-dev", "3", "+exec", script]
        } else {
            let limits = [120, 60, 1000]
            args += ["+fps_max", String(limits[min(max(frameRate, 0), limits.count - 1)]), "+gl_vsync", vsync ? "1" : "0"]
            // With VSync, start each frame when at most one finished frame is
            // waiting for the display, instead of letting the swap chain run
            // two frames ahead. Measured only for the full-display window.
            args += ["+host_vsync_pacing", vsync && fullDisplay ? "1" : "0"]
            switch mode {
            case 1: args += ["+map", "start"]
            case 2: args += ["+map", "_zoo"]
            case 3:
                guard let save = latestSave() else { throw failure("There are no saved games yet.") }
                args += ["+load", save]
            default: break
            }
        }
        p.environment = env; p.arguments = args
        return p
    }
}

final class Launcher: NSObject, NSApplicationDelegate, NSWindowDelegate {
    var installation: Installation!
    var window: NSWindow!
    var content: Backdrop!
    var mode: NSPopUpButton!, display: NSPopUpButton!, difficulty: NSPopUpButton!, frameRate: NSPopUpButton!
    var fullscreen: NSButton!, vsync: NSButton!, aroundNotch: NSButton!, play: MetalButton!, status: NSTextField!
    var child: Process?
    var output: FileHandle?
    var sessionLock: Int32 = -1
    var crashMonitor: Timer?
    var lastFailure: String?
    let defaults = UserDefaults.standard
    var preview: String? {
        let args = CommandLine.arguments
        guard let i = args.firstIndex(of: "--preview"), i + 1 < args.count else { return nil }
        return args[i + 1]
    }
    func applicationDidFinishLaunching(_ notification: Notification) {
        do {
            defaults.register(defaults: ["difficulty": 1, "vsync": true, "aroundNotch": true])
            if let i = CommandLine.arguments.firstIndex(of: "--write-iconset"), i + 1 < CommandLine.arguments.count {
                try writeIconset(URL(fileURLWithPath: CommandLine.arguments[i + 1])); exit(0)
            }
            installation = try Installation()
            if CommandLine.arguments.contains("--validate-install") { try installation.validate(); print("Installation validated"); exit(0) }
            if let i = CommandLine.arguments.firstIndex(of: "--engine-test"), i + 1 < CommandLine.arguments.count {
                try start(testScript: CommandLine.arguments[i + 1]); return
            }
            NSApp.setActivationPolicy(preview == nil ? .regular : .accessory)
            let menu = NSMenu(), item = NSMenuItem(); menu.addItem(item); item.submenu = NSMenu()
            item.submenu?.addItem(withTitle: "Quit Brutal Half-Life", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
            NSApp.mainMenu = menu
            buildWindow()
            if let path = preview {
                content.layoutSubtreeIfNeeded()
                guard let rep = content.bitmapImageRepForCachingDisplay(in: content.bounds) else { throw failure("Cannot render preview") }
                content.cacheDisplay(in: content.bounds, to: rep)
                try rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: path))
                exit(0)
            }
            do { try installation.validate() } catch { status.stringValue = error.localizedDescription; play.isEnabled = false }
            window.center(); window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
        } catch {
            if CommandLine.arguments.count > 1 { fputs(error.localizedDescription + "\n", stderr); exit(1) }
            NSAlert(error: error).runModal(); NSApp.terminate(nil)
        }
    }
    func button(_ title: String, _ rect: NSRect, _ action: Selector) -> MetalButton {
        let b = MetalButton(frame: rect); b.title = title; b.target = self; b.action = action; b.isBordered = false
        b.setAccessibilityLabel(title); content.addSubview(b); return b
    }
    func picker(_ title: String, _ choices: [String], y: CGFloat, key: String) -> NSPopUpButton {
        content.addSubview(label(title, NSRect(x: 42, y: y + 5, width: 215, height: 25), size: 18, color: gold))
        let p = NSPopUpButton(frame: NSRect(x: 265, y: y, width: 430, height: 32)); p.addItems(withTitles: choices)
        p.selectItem(at: min(max(defaults.integer(forKey: key), 0), choices.count - 1))
        p.font = copper(16); p.setAccessibilityLabel(title); content.addSubview(p); return p
    }
    func buildWindow() {
        content = Backdrop(frame: NSRect(x: 0, y: 0, width: 740, height: 703))
        window = NSWindow(contentRect: content.bounds, styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
        window.title = "Brutal Half-Life"; window.titleVisibility = .hidden; window.titlebarAppearsTransparent = true
        window.appearance = NSAppearance(named: .darkAqua); window.isReleasedWhenClosed = false; window.delegate = self; window.contentView = content
        mode = picker("START", ["Game menu", "New campaign", "Weapon showcase", "Continue latest save"], y: 245, key: "mode")
        display = picker("RESOLUTION", ["1280 × 720", "1600 × 900", "1920 × 1080", "2560 × 1440"], y: 294, key: "resolution")
        display.toolTip = "Window size. Native fullscreen uses your display's desktop resolution."
        frameRate = picker("FRAME RATE", ["120 FPS limit", "60 FPS limit", "Uncapped"], y: 343, key: "frameRate")
        difficulty = picker("DIFFICULTY", ["Easy", "Normal", "Hard"], y: 392, key: "difficulty")
        fullscreen = NSButton(checkboxWithTitle: "Fullscreen", target: nil, action: nil)
        fullscreen.toolTip = "Use macOS fullscreen, with automatic Game Mode when enabled in macOS."
        fullscreen.frame = NSRect(x: 265, y: 438, width: 130, height: 30); fullscreen.state = defaults.bool(forKey: "fullscreen") ? .on : .off
        content.addSubview(fullscreen)
        aroundNotch = NSButton(checkboxWithTitle: "Around notch", target: nil, action: nil)
        aroundNotch.toolTip = "Fullscreen covers the whole display, including the area beside the camera. Lowest latency; the camera hides a small strip at the top centre. Off keeps the picture below the camera in a macOS fullscreen Space."
        aroundNotch.frame = NSRect(x: 400, y: 438, width: 160, height: 30); aroundNotch.state = defaults.bool(forKey: "aroundNotch") ? .on : .off
        content.addSubview(aroundNotch)
        vsync = NSButton(checkboxWithTitle: "VSync", target: nil, action: nil)
        vsync.toolTip = "Steady frames without tearing. In fullscreen around the notch, frames are paced for low latency."
        vsync.frame = NSRect(x: 570, y: 438, width: 125, height: 30); vsync.state = defaults.bool(forKey: "vsync") ? .on : .off
        content.addSubview(vsync)
        _ = button("CONTROLS", NSRect(x: 40, y: 494, width: 208, height: 46), #selector(controls))
        _ = button("SAVED GAMES", NSRect(x: 266, y: 494, width: 208, height: 46), #selector(openSaves))
        _ = button("FILES & LOG", NSRect(x: 492, y: 494, width: 208, height: 46), #selector(openFiles))
        status = label("Metal graphics · Local development build", NSRect(x: 40, y: 565, width: 660, height: 42), size: 12, color: muted)
        status.alignment = .center; content.addSubview(status)
        let quit = button("QUIT", NSRect(x: 141, y: 632, width: 220, height: 52), #selector(quitApp)); quit.keyEquivalent = "\u{1b}"
        play = button("PLAY", NSRect(x: 379, y: 632, width: 220, height: 52), #selector(playGame)); play.primary = true; play.keyEquivalent = "\r"
    }
    @objc func controls() {
        let alert = NSAlert(); alert.messageText = "Brutal Half-Life controls"
        alert.informativeText = "WASD — Move\nMouse — Aim\nLeft click — Fire\nMiddle click or X — Alternate fire\nRight click — Weapon special action\nR — Reload     E — Use     Space — Jump\nControl / C — Crouch     F — Flashlight\nMouse wheel — Change weapon\nF5 — Quick save     F6 — Quick load\nEscape — Game menu\n\nThese are v3's starting bindings. Change controls, mouse sensitivity, sound and graphics in the game menu. Use Fn with function keys when needed."
        alert.runModal()
    }
    @objc func openSaves() {
        do { try FileManager.default.createDirectory(at: installation.saves, withIntermediateDirectories: true); NSWorkspace.shared.open(installation.saves) }
        catch { NSAlert(error: error).runModal() }
    }
    @objc func openFiles() { NSWorkspace.shared.open(installation.data) }
    @objc func quitApp() { NSApp.terminate(nil) }
    @objc func playGame() {
        do {
            defaults.set(mode.indexOfSelectedItem, forKey: "mode"); defaults.set(display.indexOfSelectedItem, forKey: "resolution")
            defaults.set(difficulty.indexOfSelectedItem, forKey: "difficulty"); defaults.set(fullscreen.state == .on, forKey: "fullscreen")
            defaults.set(frameRate.indexOfSelectedItem, forKey: "frameRate"); defaults.set(vsync.state == .on, forKey: "vsync")
            defaults.set(aroundNotch.state == .on, forKey: "aroundNotch")
            try start()
        } catch { NSAlert(error: error).runModal() }
    }
    func start(testScript: String? = nil) throws {
        guard child == nil else { return }
        try installation.validate()
        sessionLock = open(installation.data.appendingPathComponent(".game.lock").path, O_CREAT | O_RDWR, 0o600)
        guard sessionLock >= 0, flock(sessionLock, LOCK_EX | LOCK_NB) == 0 else {
            if sessionLock >= 0 { close(sessionLock); sessionLock = -1 }
            throw failure("This installation is already running.")
        }
        do {
            let p = try installation.process(mode: mode?.indexOfSelectedItem ?? 0, resolution: display?.indexOfSelectedItem ?? 0,
                                             fullscreen: testScript != nil ? CommandLine.arguments.contains("--engine-test-fullscreen") : fullscreen?.state == .on,
                                             difficulty: difficulty?.indexOfSelectedItem ?? 1,
                                             frameRate: frameRate?.indexOfSelectedItem ?? 0, vsync: vsync?.state != .off,
                                             aroundNotch: testScript != nil ? CommandLine.arguments.contains("--engine-test-notch") : aroundNotch?.state == .on, testScript: testScript)
            let handle = try installation.openPlayLog(for: p)
            p.standardOutput = handle; p.standardError = handle; output = handle
            // Inherit the same lock through stdin. If the launcher crashes,
            // its still-running engine continues to own this installation.
            p.standardInput = FileHandle(fileDescriptor: sessionLock, closeOnDealloc: false)
            lastFailure = nil
            p.terminationHandler = { result in DispatchQueue.main.async {
                self.crashMonitor?.invalidate(); self.crashMonitor = nil
                let reason = result.terminationReason == .uncaughtSignal ? "signal" : "exit"
                try? handle.write(contentsOf: Data("\nBHL play ended: \(ISO8601DateFormatter().string(from: Date())) — \(reason) \(result.terminationStatus)\n".utf8))
                try? handle.close(); self.output = nil; self.child = nil
                flock(self.sessionLock, LOCK_UN); close(self.sessionLock); self.sessionLock = -1
                if testScript != nil { exit(result.terminationStatus) }
                self.window.makeKeyAndOrderFront(nil); NSApp.unhide(nil); NSApp.activate(ignoringOtherApps: true)
                if result.terminationStatus != 0 || self.lastFailure != nil || self.runtimeFailure() != nil {
                    let alert = NSAlert(); alert.messageText = "Brutal Half-Life stopped unexpectedly"
                    alert.informativeText = "Open Files & Log to inspect Last Run.log and earlier Play Logs. macOS crash reports are in Console. Your saves remain in Saved Games."; alert.runModal()
                }
            }}
            child = p; try p.run()
            if testScript == nil {
                window.orderOut(nil); NSApp.hide(nil)
                crashMonitor = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
                    guard let self = self, p.isRunning, self.lastFailure == nil, let failure = self.runtimeFailure() else { return }
                    self.lastFailure = failure
                    // Xash catches some fatal signals and waits for a key in
                    // its console. A failed game must return to the launcher.
                    p.terminate()
                    DispatchQueue.main.asyncAfter(deadline: .now() + 3) { if p.isRunning { kill(p.processIdentifier, SIGKILL) } }
                }
            }
        } catch {
            child = nil; try? output?.close(); output = nil
            flock(sessionLock, LOCK_UN); close(sessionLock); sessionLock = -1; throw error
        }
    }
    func runtimeFailure() -> String? {
        guard let handle = try? FileHandle(forReadingFrom: installation.log) else { return nil }
        defer { try? handle.close() }
        guard let length = try? handle.seekToEnd() else { return nil }
        try? handle.seek(toOffset: length > 262144 ? length - 262144 : 0)
        guard let bytes = try? handle.readToEnd(), let text = String(data: bytes, encoding: .utf8) else { return nil }
        return text.components(separatedBy: .newlines).first {
            $0.range(of: "BHL.*(unimplemented|failed|not implemented)|Crash:|Xash Error:|BHL native loader:|Host_Error:", options: .regularExpression) != nil
        }
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { child == nil }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if let p = child, p.isRunning { NSRunningApplication(processIdentifier: p.processIdentifier)?.activate(options: []); return .terminateCancel }
        return .terminateNow
    }
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if let p = child { NSRunningApplication(processIdentifier: p.processIdentifier)?.activate(options: []) }
        else { window?.makeKeyAndOrderFront(nil) }
        return true
    }
}
if CommandLine.arguments.dropFirst().first == "--run-engine" { runBundledEngine() }
let app = NSApplication.shared
let delegate = Launcher(); app.delegate = delegate; app.run()
