#ifdef __APPLE__
#include "bgiplayer/game_window.hpp"
#import <AppKit/AppKit.h>
#include <cstring>
namespace bgiplayer {
void set_game_dock_icon(const GameWindowIcon& icon) {
    @autoreleasepool {
        NSBitmapImageRep* rep=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:icon.width pixelsHigh:icon.height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:icon.width*4 bitsPerPixel:32];
        if(!rep)return;
        std::memcpy([rep bitmapData],icon.rgba.data(),icon.rgba.size());
        NSImage* image=[[NSImage alloc] initWithSize:NSMakeSize(icon.width,icon.height)];
        [image addRepresentation:rep];[NSApp setApplicationIconImage:image];
        [image release];[rep release];
    }
}
}
#endif
