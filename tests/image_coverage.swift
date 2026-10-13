// Reject blank renderer captures without adding a Python imaging dependency.
import Foundation
import CoreGraphics
import ImageIO

guard CommandLine.arguments.count == 2,
      let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: CommandLine.arguments[1]) as CFURL, nil),
      let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else {
    fputs("Cannot read capture\n", stderr); exit(1)
}
let width = image.width, height = image.height
var bytes = [UInt8](repeating: 0, count: width * height * 4)
let rendered = bytes.withUnsafeMutableBytes { buffer -> Bool in
    guard let context = CGContext(data: buffer.baseAddress, width: width, height: height,
                                  bitsPerComponent: 8, bytesPerRow: width * 4,
                                  space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return false }
    context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
    return true
}
guard rendered else { exit(1) }
var lit = 0
for i in stride(from: 0, to: bytes.count, by: 4) {
    if max(bytes[i], bytes[i + 1], bytes[i + 2]) > 16 { lit += 1 }
}
let coverage = Double(lit) / Double(width * height)
let report: [String: Any] = ["width": width, "height": height, "nonblack_fraction": coverage,
                            "passed": coverage > 0.05]
print(String(data: try JSONSerialization.data(withJSONObject: report, options: [.sortedKeys]), encoding: .utf8)!)
exit(coverage > 0.05 ? 0 : 1)
