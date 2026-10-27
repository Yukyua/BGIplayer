#ifdef __APPLE__
#include "bgiplayer/keyboard_input.hpp"
#import <AppKit/AppKit.h>
#include <Carbon/Carbon.h>
namespace bgiplayer {
HostKeyboardLayout host_keyboard_layout() {
    @autoreleasepool {
        NSTextInputContext* context = [NSTextInputContext currentInputContext];
        NSResponder* responder = [[NSApp keyWindow] firstResponder];
        if (!context && [responder respondsToSelector:@selector(inputContext)])
            context = [(NSView*)responder inputContext];
        NSString* source = [context selectedKeyboardInputSource];
        if ([source hasPrefix:@"com.apple.keylayout.German"]) return HostKeyboardLayout::German;
        if ([source hasPrefix:@"com.apple.keylayout.French"]) return HostKeyboardLayout::French;
        if ([source hasPrefix:@"com.apple.keylayout.Japanese"] ||
            [source hasPrefix:@"com.apple.inputmethod.Kotoeri"])
            return KBGetLayoutType(LMGetKbdType()) == kKeyboardJIS ?
                HostKeyboardLayout::Japanese : HostKeyboardLayout::Logical;
        if ([source isEqualToString:@"com.apple.keylayout.US"] ||
            [source isEqualToString:@"com.apple.keylayout.ABC"]) return HostKeyboardLayout::Us;
        return HostKeyboardLayout::Logical;
    }
}
uint16_t host_keyboard_modifiers(uint16_t fallback) {
    @autoreleasepool {
        // Cocoa lock flags describe the current toggle independently of key
        // press events, including changes while a native child owns focus.
        fallback &= static_cast<uint16_t>(~KMOD_CAPS);
        if (([NSEvent modifierFlags] & NSEventModifierFlagCapsLock) != 0)
            fallback |= KMOD_CAPS;
        return fallback;
    }
}
} // namespace bgiplayer
#endif
