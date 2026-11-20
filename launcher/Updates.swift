import AppKit

// Updates, as in the other native ports: install.sh gives the app an updater (Contents/MacOS/update) that fetches the
// branch the app follows from GitHub into a worktree of the checkout it was installed from and rebuilds the app from
// it, keeping the game data, saves and settings. The launch window asks GitHub (in the background, each time it
// opens) whether that branch moved past the commit the app was built from (Build.json's revision).

enum UpdateState { case unavailable, checking, current, available, failed }

struct Updater {
    let script: URL
    let branch: String
    let built: String

    /// The app's updater, when this copy was installed from a checkout of the GitHub repository.
    static func installed() -> Updater? {
        guard let macos = Bundle.main.executableURL?.deletingLastPathComponent(),
              let resources = Bundle.main.resourceURL else { return nil }
        let script = macos.appendingPathComponent("update")
        guard FileManager.default.isExecutableFile(atPath: script.path),
              let data = try? Data(contentsOf: resources.appendingPathComponent("Build.json")),
              let build = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let built = build["revision"] as? String else { return nil }
        let info = NSDictionary(contentsOf: resources.appendingPathComponent("Update.plist"))
        return Updater(script: script, branch: info?["branch"] as? String ?? "main", built: built)
    }

    /// The newest commit of the branch on GitHub; nil if it can't be reached.
    func check() -> String? {
        let task = Process(); task.executableURL = URL(fileURLWithPath: "/bin/bash"); task.arguments = [script.path, "--check"]
        let pipe = Pipe(); task.standardOutput = pipe; task.standardError = FileHandle.nullDevice
        guard (try? task.run()) != nil else { return nil }
        DispatchQueue.global().asyncAfter(deadline: .now() + 20) { if task.isRunning { task.terminate() } }
        let data = pipe.fileHandleForReading.readDataToEndOfFile(); task.waitUntilExit()
        let out = String(data: data, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        if task.terminationStatus != 0 || out.isEmpty { return nil }
        if out.count >= 40 { return String(out.prefix(40)) }                       // a commit (an app built from a checkout)
        return out.contains(where: { $0.isWhitespace }) ? nil : out                 // a release's tag (the downloaded app)
    }

    func choice(_ state: UpdateState, latest: String?) -> LaunchRow.Choice {
        func short(_ c: String?) -> String { String((c ?? "").prefix(7)) }
        switch state {
        case .unavailable: return .init(value: "", label: "Not available", note: "This copy wasn't installed from the GitHub repository")
        case .checking: return .init(value: "", label: "Checking...", note: "Asking GitHub for the newest \(branch)")
        case .current: return .init(value: "", label: "Up to date", note: "\(branch) \(short(built))")
        case .available: return .init(value: "", label: "Update available", note: "\(short(built)) -> \(short(latest)): press to install")
        case .failed: return .init(value: "", label: "Couldn't check", note: "GitHub unreachable or not signed in: press to retry")
        }
    }

    /// The phase of the updater's output, in words.
    static func phase(_ line: String, branch: String) -> String? {
        if branch == "release" {                                                    // the downloaded app's updater (recomp setup's update.sh)
            if line.hasPrefix("== downloading ") { return "Downloading " + line.dropFirst(15) + " from GitHub" }
            if line.hasPrefix("== unpacking") { return "Unpacking it" }
            if line.hasPrefix("== installing") { return "Installing the new app" }
            return nil
        }
        if line.hasPrefix("== fetching") { return "Downloading the update from GitHub" }
        if line.hasPrefix("== \(branch): ") { return "Got " + line.dropFirst(branch.count + 5) }
        if line.hasPrefix("prepare:") || line.hasPrefix("dependencies:") || line.hasPrefix("unpack:") || line.hasPrefix("relocate:") {
            return "Rebuilding from your game files: preparing (1 of 4)"
        }
        if line.hasPrefix("analyze-") || line.hasPrefix("lift-") { return "Rebuilding from your game files: translating to native code (2 of 4)" }
        if line.hasPrefix("runtime:") || line.hasPrefix("server-bridge:") || line.hasPrefix("client-bridge:") || line.hasPrefix("session:") {
            return "Rebuilding from your game files: compiling (3 of 4)"
        }
        if line.hasPrefix("== installing") { return "Installing the new app (4 of 4)" }
        return nil
    }

    /// Runs the updater with a progress window (modal); returns true when the app was rebuilt.
    func run(over parent: NSWindow?, log logURL: URL) -> Bool {
        let panel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 520, height: 170), styleMask: [.titled], backing: .buffered, defer: false)
        panel.title = "Updating"
        let title = NSTextField(labelWithString: "Updating to the newest version"); title.font = .boldSystemFont(ofSize: 14)
        title.frame = NSRect(x: 20, y: 128, width: 480, height: 22)
        let status = NSTextField(wrappingLabelWithString: "Starting..."); status.frame = NSRect(x: 20, y: 64, width: 480, height: 56)
        let spin = NSProgressIndicator(frame: NSRect(x: 20, y: 44, width: 480, height: 12)); spin.isIndeterminate = true; spin.startAnimation(nil)
        let time = NSTextField(labelWithString: "This usually takes several minutes. Your saves and settings are kept.")
        time.font = .systemFont(ofSize: 11); time.textColor = .secondaryLabelColor; time.frame = NSRect(x: 20, y: 14, width: 380, height: 18)
        let button = NSButton(title: "Close", target: nil, action: nil); button.frame = NSRect(x: 400, y: 8, width: 100, height: 30); button.isHidden = true
        for view in [title, status, spin, time, button] { panel.contentView?.addSubview(view) }
        if let parent = parent { panel.setFrameOrigin(NSPoint(x: parent.frame.midX - 260, y: parent.frame.midY - 85)) } else { panel.center() }

        FileManager.default.createFile(atPath: logURL.path, contents: nil)
        let log = try? FileHandle(forWritingTo: logURL)
        let task = Process(); task.executableURL = URL(fileURLWithPath: "/bin/bash"); task.arguments = [script.path]
        let pipe = Pipe(); task.standardOutput = pipe; task.standardError = pipe
        let lock = NSLock(); var pending = "", tail = ""
        pipe.fileHandleForReading.readabilityHandler = { handle in
            let data = handle.availableData; guard !data.isEmpty else { return }
            try? log?.write(contentsOf: data)
            lock.lock(); defer { lock.unlock() }
            pending += String(data: data, encoding: .utf8) ?? ""
            while let nl = pending.firstIndex(of: "\n") {
                let line = String(pending[..<nl]); pending.removeSubrange(...nl)
                tail = String((tail + line + "\n").suffix(600))
                if let words = Updater.phase(line, branch: branch) { DispatchQueue.main.async { status.stringValue = words } }
            }
        }
        var ok = false
        let target = BlockTarget { NSApp.stopModal() }          // a button's target is weak: kept alive here
        button.target = target; button.action = #selector(BlockTarget.fire)
        task.terminationHandler = { finished in
            let code = finished.terminationStatus
            DispatchQueue.main.async {
                pipe.fileHandleForReading.readabilityHandler = nil; try? log?.close()
                spin.stopAnimation(nil); spin.isHidden = true; button.isHidden = false
                if code == 0 {
                    ok = true; title.stringValue = "Updated"
                    status.stringValue = "The new version is installed. The app restarts when you press Restart."; button.title = "Restart"
                } else {
                    lock.lock(); let last = tail.replacingOccurrences(of: "\n", with: " "); lock.unlock()
                    title.stringValue = "The update didn't finish"
                    status.stringValue = "Nothing was changed if it stopped before installing. Details: \(logURL.path)  (\(String(last.suffix(160))))"
                    time.stringValue = ""
                }
            }
        }
        guard (try? task.run()) != nil else { try? log?.close(); return false }
        panel.makeKeyAndOrderFront(nil)
        NSApp.runModal(for: panel)
        panel.orderOut(nil)
        withExtendedLifetime(target) {}
        return ok
    }
}

final class BlockTarget: NSObject {
    let action: () -> Void
    init(_ action: @escaping () -> Void) { self.action = action }
    @objc func fire() { action() }
}
