import AppKit
import Darwin

// Run the game in another instance of the bundle's executable, so macOS sees
// the actual window owner as our game. Keep the launcher monitor and data lock.
func runBundledEngine() -> Never {
    guard let resources = Bundle.main.resourceURL else {
        fputs("BHL native loader: missing app resources\n", stderr); exit(1)
    }
    let path = resources.appendingPathComponent("Runtime/libxash.dylib").path
    guard let engine = dlopen(path, RTLD_NOW | RTLD_LOCAL) else {
        fputs("BHL native loader: \(String(cString: dlerror()))\n", stderr); exit(1)
    }
    typealias ChangeGame = @convention(c) (UnsafePointer<CChar>?) -> Void
    typealias HostMain = @convention(c) (Int32, UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?, UnsafePointer<CChar>?, Int32, ChangeGame?) -> Int32
    typealias HostShutdown = @convention(c) () -> Void
    guard let entry = dlsym(engine, "Host_Main"), let shutdown = dlsym(engine, "Host_Shutdown") else {
        fputs("BHL native loader: missing engine entry points\n", stderr); exit(1)
    }
    // Xash's signal handler cannot symbolize this Mach-O build and consumes
    // fatal signals before macOS can create its native crash report.
    var arguments = ([CommandLine.arguments[0], "-noch"] + CommandLine.arguments.dropFirst(2)).map { strdup($0) }
    arguments.append(nil)
    let count = Int32(arguments.count - 1)
    let fullscreenObserver = NotificationCenter.default.addObserver(forName: NSWindow.didEnterFullScreenNotification, object: nil, queue: nil) { _ in
        fputs("BHL macOS native fullscreen entered\n", stderr)
    }
    fputs("BHL game app identity: \(Bundle.main.bundleIdentifier ?? "missing")\n", stderr)
    fputs("BHL engine process: \(getpid())\n", stderr)
    let status = arguments.withUnsafeMutableBufferPointer { argv in
        "valve".withCString { game in
            unsafeBitCast(entry, to: HostMain.self)(count, argv.baseAddress, game, 0, nil)
        }
    }
    unsafeBitCast(shutdown, to: HostShutdown.self)()
    NotificationCenter.default.removeObserver(fullscreenObserver)
    for argument in arguments { free(argument) }
    dlclose(engine)
    exit(status)
}
