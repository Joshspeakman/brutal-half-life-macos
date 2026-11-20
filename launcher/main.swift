import AppKit
import Darwin

func failure(_ message: String) -> NSError {
    NSError(domain: "BrutalHalfLife", code: 1, userInfo: [NSLocalizedDescriptionKey: message])
}

struct Installation {
    let resources: URL
    let data: URL
    var runtime: URL { resources.appendingPathComponent("Runtime") }
    /// The drag-and-drop app: the game's library and images, set up on this Mac (tools/dropin_build.py), else nil
    var gameLib: URL? { ProcessInfo.processInfo.environment["BHL_GAME_LIB"].map { URL(fileURLWithPath: $0) } }
    var images: URL { gameLib?.appendingPathComponent("Images") ?? resources.appendingPathComponent("Images") }
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
                    runtime.appendingPathComponent("hl_arm64.dylib"), images.appendingPathComponent("hl.dll"),
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
        env["BHL_IMAGE_DIR"] = images.path
        env["XASH3D_BASEDIR"] = data.path
        env["XASH3D_EXTRAS_PAK1"] = resources.appendingPathComponent("extras.pk3").path
        env["DYLD_LIBRARY_PATH"] = gameLib.map { runtime.path + ":" + $0.path } ?? runtime.path   /* (the downloaded app: + the game's library) */
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
    var view: LaunchView!
    // Rows backed by the launcher's existing preference keys, so earlier choices carry over.
    var startRow: LaunchRow!, difficultyRow: LaunchRow!
    var fullscreenRow: LaunchRow!, notchRow: LaunchRow!, sizeRow: LaunchRow!, frameRateRow: LaunchRow!, vsyncRow: LaunchRow!
    var updater: Updater?
    var updateState = UpdateState.unavailable, latestCommit: String?
    var child: Process?
    var output: FileHandle?
    var sessionLock: Int32 = -1
    var crashMonitor: Timer?
    var lastFailure: String?
    let defaults = UserDefaults.standard
    func argument(_ name: String) -> String? {
        let args = CommandLine.arguments
        guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
        return args[i + 1]
    }
    var preview: String? { argument("--preview") }
    func applicationDidFinishLaunching(_ notification: Notification) {
        do {
            defaults.register(defaults: ["difficulty": 1, "vsync": true, "aroundNotch": true])
            if let path = argument("--write-iconset") { try writeIconset(URL(fileURLWithPath: path)); exit(0) }
            installation = try Installation()
            if CommandLine.arguments.contains("--validate-install") { try installation.validate(); print("Installation validated"); exit(0) }
            if let script = argument("--engine-test") { try start(testScript: script); return }
            NSApp.setActivationPolicy(preview == nil ? .regular : .accessory)
            let menu = NSMenu(), item = NSMenuItem(); menu.addItem(item); item.submenu = NSMenu()
            item.submenu?.addItem(withTitle: "Quit Brutal Half-Life", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
            NSApp.mainMenu = menu
            buildWindow()
            do { try installation.validate() } catch { view.note = error.localizedDescription; view.playEnabled = false }
            if let path = preview {
                // development: the window as an image (--preview-open <group>)
                if let name = argument("--preview-open"), let i = view.sections.firstIndex(where: { $0.title.caseInsensitiveCompare(name) == .orderedSame }) { view.open = i }
                guard let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { throw failure("Cannot render preview") }
                view.cacheDisplay(in: view.bounds, to: rep)
                try rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: path))
                exit(0)
            }
            window.center(); window.makeKeyAndOrderFront(nil); window.makeFirstResponder(view); NSApp.activate(ignoringOtherApps: true)
            checkForUpdate()
        } catch {
            if CommandLine.arguments.count > 1 { fputs(error.localizedDescription + "\n", stderr); exit(1) }
            NSAlert(error: error).runModal(); NSApp.terminate(nil)
        }
    }
    func buildWindow() {
        typealias C = LaunchRow.Choice
        let hz = NSScreen.main?.maximumFramesPerSecond ?? 60
        startRow = LaunchRow("Start", [C(value: "menu", label: "Game Menu", note: "New game, chapters, saves and options"),
                                       C(value: "campaign", label: "New Campaign", note: "Straight into the campaign hub"),
                                       C(value: "showcase", label: "Weapon Showcase", note: "Every weapon, in v3's test range"),
                                       C(value: "latest", label: "Continue", note: "Your most recent save")],
                             index: defaults.integer(forKey: "mode"))
        difficultyRow = LaunchRow("Difficulty", [C(value: "1", label: "Easy", note: "For a new game"),
                                                 C(value: "2", label: "Normal", note: "For a new game"),
                                                 C(value: "3", label: "Hard", note: "For a new game")],
                                  index: defaults.integer(forKey: "difficulty"))
        fullscreenRow = LaunchRow("Screen", [C(value: "window", label: "Window", note: "A window of the size below"),
                                             C(value: "fullscreen", label: "Fullscreen", note: "Your display's resolution; Game Mode turns on")],
                                  index: defaults.bool(forKey: "fullscreen") ? 1 : 0)
        notchRow = LaunchRow("Notch", [C(value: "around", label: "Around the Notch", note: "The whole display; lowest latency"),
                                       C(value: "below", label: "Below the Notch", note: "A macOS fullscreen Space under the camera")],
                             index: defaults.bool(forKey: "aroundNotch") ? 0 : 1)
        sizeRow = LaunchRow("Window Size", ["1280 × 720", "1600 × 900", "1920 × 1080", "2560 × 1440"].map { C(value: $0, label: $0, note: "Windowed play") },
                            index: defaults.integer(forKey: "resolution"))
        frameRateRow = LaunchRow("Frame Rate", [C(value: "120", label: "120 FPS", note: hz >= 120 ? "This display's refresh rate" : "A limit above this display's \(hz) Hz"),
                                                C(value: "60", label: "60 FPS", note: "Cooler and quieter"),
                                                C(value: "uncapped", label: "Uncapped", note: "As fast as the game runs")],
                                 index: defaults.integer(forKey: "frameRate"))
        vsyncRow = LaunchRow("VSync", [C(value: "on", label: "On", note: "No tearing; paced for low latency around the notch"),
                                       C(value: "off", label: "Off", note: "Tears and presents frames unevenly")],
                             index: defaults.bool(forKey: "vsync") ? 0 : 1)
        let display = LaunchSection("Display", rows: [fullscreenRow, notchRow, sizeRow, frameRateRow, vsyncRow])
        display.summary = { [unowned self] in
            let screen = self.fullscreenRow.value == "fullscreen" ? (self.notchRow.value == "around" ? "Fullscreen around the notch" : "Fullscreen below the notch") : "Window " + self.sizeRow.label
            return "\(screen) · \(self.frameRateRow.label) · VSync \(self.vsyncRow.label)"
        }
        let guide = LaunchSection("Key Guide")
        guide.summary = { "WASD · Mouse · R reload · E use · F flashlight · F5 / F6" }
        guide.activate = { [unowned self] in self.controls() }
        let saves = LaunchRow("Saved Games", [C(value: "", label: "Open in Finder", note: "Your saves, kept across updates")])
        saves.activate = { [unowned self] in self.openSaves() }
        let files = LaunchRow("Files & Log", [C(value: "", label: "Open in Finder", note: "Game data, settings and play logs")])
        files.activate = { [unowned self] in self.openFiles() }
        let filesSection = LaunchSection("Files", rows: [saves, files])
        filesSection.summary = { "Saved games · Settings · Play logs" }
        updater = Updater.installed()
        updateState = updater == nil ? .unavailable : .checking
        let updatesSection = LaunchSection("Updates")
        updatesSection.summary = { [unowned self] in let c = self.updateChoice(); return c.note.isEmpty ? c.label : "\(c.label) · \(c.note)" }
        updatesSection.activate = { [unowned self] in self.updatePressed() }
        view = LaunchView(frame: NSRect(x: 0, y: 0, width: 780, height: 800))   // the ports' 760, plus room for the second top row
        view.top = [startRow, difficultyRow]
        view.sections = [display, guide, filesSection, updatesSection]
        view.backdrop = launcherArt("backdrop")
        view.note = "Metal graphics. Controls, sound and video detail are in the game's own Options."
        view.onPlay = { [unowned self] in self.playGame() }
        view.onQuit = { NSApp.terminate(nil) }
        window = NSWindow(contentRect: view.frame, styleMask: [.titled, .closable, .miniaturizable, .fullSizeContentView], backing: .buffered, defer: false)
        window.title = "Brutal Half-Life"; window.titleVisibility = .hidden; window.titlebarAppearsTransparent = true
        window.isMovableByWindowBackground = true; window.backgroundColor = .black
        window.appearance = NSAppearance(named: .darkAqua); window.isReleasedWhenClosed = false; window.delegate = self; window.contentView = view
    }
    func updateChoice() -> LaunchRow.Choice {
        updater?.choice(updateState, latest: latestCommit) ?? Updater(script: URL(fileURLWithPath: "/"), branch: "main", built: "").choice(.unavailable, latest: nil)
    }
    func refreshUpdates() {
        view.needsDisplay = true                             // the Updates summary reads the state
    }
    func checkForUpdate() {
        guard let updater = updater else { return }
        updateState = .checking; refreshUpdates()
        DispatchQueue.global(qos: .utility).async {
            let latest = updater.check()
            DispatchQueue.main.async {
                self.latestCommit = latest
                self.updateState = latest == nil ? .failed : latest == updater.built ? .current : .available
                self.refreshUpdates()
            }
        }
    }
    func updatePressed() {
        guard let updater = updater, updateState != .checking, child == nil else { return }   // never under a running game
        guard updateState == .available, let latest = latestCommit else { checkForUpdate(); return }
        let alert = NSAlert(); alert.messageText = updater.branch == "nightly" ? "Update to the newest Nightly?" : "Update to the newest version?"
        alert.informativeText = "The \(updater.branch) branch (\(latest.prefix(7))) is downloaded from GitHub and the app is rebuilt from your own copy of the game, "
            + "which usually takes several minutes. Your game data, saves and settings are kept. The app restarts when it is done."
        alert.addButton(withTitle: "Update"); alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        if updater.run(over: window, log: installation.data.appendingPathComponent("Update.log")) {
            // restart: the rebuilt app, then this one ends
            let open = Process(); open.executableURL = URL(fileURLWithPath: "/usr/bin/open"); open.arguments = ["-n", Bundle.main.bundlePath]
            try? open.run(); open.waitUntilExit()
            exit(0)
        }
        checkForUpdate()
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
    func playGame() {
        do {
            defaults.set(startRow.index, forKey: "mode"); defaults.set(sizeRow.index, forKey: "resolution")
            defaults.set(difficultyRow.index, forKey: "difficulty"); defaults.set(fullscreenRow.value == "fullscreen", forKey: "fullscreen")
            defaults.set(frameRateRow.index, forKey: "frameRate"); defaults.set(vsyncRow.value == "on", forKey: "vsync")
            defaults.set(notchRow.value == "around", forKey: "aroundNotch")
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
            let p = try installation.process(mode: startRow?.index ?? 0, resolution: sizeRow?.index ?? 0,
                                             fullscreen: testScript != nil ? CommandLine.arguments.contains("--engine-test-fullscreen") : fullscreenRow?.value == "fullscreen",
                                             difficulty: difficultyRow?.index ?? 1,
                                             frameRate: frameRateRow?.index ?? 0, vsync: vsyncRow?.value != "off",
                                             aroundNotch: testScript != nil ? CommandLine.arguments.contains("--engine-test-notch") : notchRow?.value == "around", testScript: testScript)
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
#if DROPIN   // (only tools/make_dropin.sh links the setup window: swiftc -D DROPIN)
/* The drag-and-drop app (tools/make_dropin.sh): its setup window (recomp setup's setup_ui.m, linked in) builds the game from the
 * player's copy first, then says where it is; this process never loads the game (game.json's bound_symbol is ""). */
@_silgen_name("ds_easy_install") func ds_easy_install(_ exe: UnsafeMutablePointer<UnsafePointer<CChar>?>, _ game: UnsafeMutablePointer<UnsafePointer<CChar>?>,
                                                      _ data: UnsafeMutablePointer<UnsafePointer<CChar>?>) -> Int32
if let res = Bundle.main.resourceURL, FileManager.default.fileExists(atPath: res.appendingPathComponent("setup/game.json").path),
   ProcessInfo.processInfo.environment["BHL_DATA_DIR"] == nil, !CommandLine.arguments.dropFirst().contains(where: { $0.hasPrefix("--") }) {   // (a plain launch: not --write-iconset or a test)
    var exe: UnsafePointer<CChar>? = nil, game: UnsafePointer<CChar>? = nil, data: UnsafePointer<CChar>? = nil
    if ds_easy_install(&exe, &game, &data) > 0, let exe, let data {
        setenv("BHL_DATA_DIR", data, 1)
        setenv("BHL_GAME_LIB", (String(cString: exe) as NSString).deletingLastPathComponent, 1)
    }
}
#endif
let app = NSApplication.shared
let delegate = Launcher(); app.delegate = delegate; app.run()
