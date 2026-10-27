import Cocoa
import FlutterMacOS
import CoreText

class MainFlutterWindow: NSWindow {
  override func awakeFromNib() {
    let flutterViewController = FlutterViewController()
    let windowFrame = self.frame
    self.contentViewController = flutterViewController
    self.setFrame(windowFrame, display: true)

    let fontChannel = FlutterMethodChannel(name: "bgiplayer/fonts", binaryMessenger: flutterViewController.engine.binaryMessenger)
    fontChannel.setMethodCallHandler { call, result in
      guard call.method == "list" else { result(FlutterMethodNotImplemented); return }
      let collection = CTFontCollectionCreateFromAvailableFonts(nil)
      let descriptors = CTFontCollectionCreateMatchingFontDescriptors(collection) as? [CTFontDescriptor] ?? []
      var seen = Set<String>()
      var fonts = [[String: String]]()
      for descriptor in descriptors {
        guard let url = CTFontDescriptorCopyAttribute(descriptor, kCTFontURLAttribute) as? URL,
              let name = CTFontDescriptorCopyAttribute(descriptor, kCTFontDisplayNameAttribute) as? String,
              FileManager.default.isReadableFile(atPath: url.path), seen.insert(url.path).inserted else { continue }
        fonts.append(["name": name, "path": url.path])
      }
      result(fonts.sorted { $0["name"]! < $1["name"]! })
    }

    RegisterGeneratedPlugins(registry: flutterViewController)

    super.awakeFromNib()
  }
}
