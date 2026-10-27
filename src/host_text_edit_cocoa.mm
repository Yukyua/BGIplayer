#ifdef __APPLE__
#include "bgiplayer/host_text_edit_bridge.hpp"
#include <SDL_syswm.h>
#import <AppKit/AppKit.h>
#include <algorithm>
#include <iostream>
#include <deque>
#include <cmath>

static const char* bgi_utf8(NSString* value) {
    const char* text = [value UTF8String];
    return text ? text : "";
}

@interface BGITextView : NSTextView <NSTextViewDelegate> {
@public
    bgiplayer::HostTextEdit* state;
    NSView* gameView;
    bool pasting;
}
@end
@implementation BGITextView
- (void)textDidChange:(NSNotification*)notification {
    (void)notification;
    if (state && ![self hasMarkedText]) state->commit_text(bgi_utf8([self string]));
}
- (void)insertText:(id)value replacementRange:(NSRange)replacement {
    NSString* string = [value isKindOfClass:[NSAttributedString class]] ? [value string] : value;
    string = [[string stringByReplacingOccurrencesOfString:@"\r\n" withString:@"\n"]
        stringByReplacingOccurrencesOfString:@"\n" withString:@"\r\n"];
    NSRange range = replacement.location == NSNotFound ?
        ([self hasMarkedText] ? [self markedRange] : [self selectedRange]) : replacement;
    if (!state || !state->permits_replacement(bgi_utf8([self string]),
        static_cast<uint32_t>(range.location), static_cast<uint32_t>(range.length),
        bgi_utf8(string), !pasting)) return;
    [super insertText:string replacementRange:replacement];
    if (![self hasMarkedText]) state->commit_text(bgi_utf8([self string]));
}
- (void)paste:(id)sender {
    (void)sender;
    NSString* string = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    if (!string) return;
    pasting = true;
    @try { [self insertText:string replacementRange:NSMakeRange(NSNotFound, 0)]; }
    @finally { pasting = false; }
}
- (void)insertNewline:(id)sender {
    (void)sender;
    if ([self hasMarkedText]) { [super insertNewline:sender]; return; }
    [self setSelectedRange:NSMakeRange(0, [[self string] length])];
    if (state) state->finish_edit(false);
    [[self window] makeFirstResponder:gameView];
}
- (void)insertTab:(id)sender {
    (void)sender;
    if (state) state->finish_edit(true);
    [[self window] makeFirstResponder:gameView];
}
@end

namespace bgiplayer {
namespace {
class CocoaTextEdit final : public HostTextEditBridge {
    HostTextEdit& state_;
    SDL_Renderer* renderer_;
    NSView* game_ = nil;
    BGITextView* edit_ = nil;
    NSClipView* viewport_ = nil;
    std::string committed_;
    std::string created_face_;
    uint32_t created_size_=0, created_width_=0, created_alignment_=0;
    bool created_multiline_=false;
    uint32_t painted_color_=0;
    int painted_scale_=0;
    struct OwnedKey { uint32_t type, timestamp; SDL_Scancode scan; };
    std::deque<OwnedKey> owned_keys_;
    uint32_t window_id_=0;
    bool pointer_capture_=false;
    static int watch_event(void* context, SDL_Event* event) {
        auto* self=static_cast<CocoaTextEdit*>(context);
        // SDL Cocoa queues keyboard events before NSApp forwards the same
        // NSEvent to its native first responder. Capture ownership at that
        // point; Enter/Tab may have changed focus before SDL_PollEvent returns.
        if ([NSThread isMainThread] && self->edit_ && self->state_.visible() &&
            [[self->edit_ window] firstResponder]==self->edit_ &&
            (event->type==SDL_KEYDOWN || event->type==SDL_KEYUP) &&
            event->key.windowID==self->window_id_)
            self->owned_keys_.push_back({event->type,event->key.timestamp,event->key.keysym.scancode});
        return 1;
    }
    bool inside(int x,int y) const {
        return state_.is_open() && state_.visible() && x>=int(state_.x()) &&
            y>=int(state_.y()) && x<int(state_.x()+state_.width()) &&
            y<int(state_.y()+state_.height());
    }
    void destroy() {
        if (edit_) {
            if ([[edit_ window] firstResponder] == edit_)
                [[edit_ window] makeFirstResponder:game_];
            edit_->state = nullptr;
            [viewport_ setDocumentView:nil];
            [viewport_ removeFromSuperview];
            [edit_ release];
            [viewport_ release];
            viewport_ = nil;
            edit_ = nil;
        }
    }
    void create() {
        destroy();
        if (!state_.is_open()) return;
        viewport_ = [[NSClipView alloc] initWithFrame:NSZeroRect];
        [viewport_ setDrawsBackground:NO];
        edit_ = [[BGITextView alloc] initWithFrame:NSZeroRect];
        edit_->state = &state_;
        edit_->gameView = game_;
        edit_->pasting = false;
        [edit_ setDelegate:edit_];
        [edit_ setFocusRingType:NSFocusRingTypeNone];
        [viewport_ setFocusRingType:NSFocusRingTypeNone];
        const bool multiline = state_.height() / state_.edit_font_size() > 1;
        created_face_=state_.font_name();
        created_size_=state_.edit_font_size();
        created_width_=state_.active_width_percent();
        created_alignment_=state_.active_alignment();
        created_multiline_=multiline;
        [edit_ setHorizontallyResizable:NO];
        [edit_ setVerticallyResizable:NO];
        [edit_ setAutoresizingMask:NSViewNotSizable];
        [[edit_ textContainer] setWidthTracksTextView:YES];
        [[edit_ textContainer] setHeightTracksTextView:NO];
        if (!multiline) {
            [[edit_ textContainer] setMaximumNumberOfLines:1];
            [[edit_ textContainer] setLineBreakMode:NSLineBreakByClipping];
        }
        [edit_ setRichText:NO];
        [edit_ setImportsGraphics:NO];
        [edit_ setDrawsBackground:NO];
        [edit_ setTextContainerInset:NSZeroSize];
        [[edit_ textContainer] setLineFragmentPadding:0];
        [edit_ setAutomaticQuoteSubstitutionEnabled:NO];
        [edit_ setAutomaticDashSubstitutionEnabled:NO];
        [edit_ setAutomaticTextReplacementEnabled:NO];
        [edit_ setAutomaticSpellingCorrectionEnabled:NO];
        [edit_ setContinuousSpellCheckingEnabled:NO];
        [edit_ setAlignment:state_.active_alignment() == 1 ? NSTextAlignmentCenter :
             state_.active_alignment() == 2 ? NSTextAlignmentRight : NSTextAlignmentLeft];
        committed_ = state_.window_text().value_or("");
        [edit_ setString:[NSString stringWithUTF8String:committed_.c_str()]];
        [viewport_ setDocumentView:edit_];
        [game_ addSubview:viewport_ positioned:NSWindowAbove relativeTo:nil];
        style();
        synchronize();
        [edit_ setSelectedRange:NSMakeRange(0, [[edit_ string] length])];
        if (state_.focus_child() && state_.visible()) [[edit_ window] makeFirstResponder:edit_];
    }
    void style() {
        if (!edit_) return;
        int x0, y0, x1, y1;
        SDL_RenderLogicalToWindow(renderer_, 0, 0, &x0, &y0);
        SDL_RenderLogicalToWindow(renderer_, 100, 100, &x1, &y1);
        const CGFloat size = static_cast<CGFloat>(state_.edit_font_size()) * std::abs(y1 - y0) / 100.0;
        painted_scale_=std::abs(y1-y0);
        painted_color_=state_.color();
        NSString* name = [NSString stringWithUTF8String:state_.font_name().c_str()];
        NSFont* font = [NSFont fontWithName:name size:size];
        if (!font) font = [NSFont userFixedPitchFontOfSize:size];
        CGFloat matrix[6];
        std::copy_n([font matrix],6,matrix);
        const CGFloat ratio=state_.active_width_percent()/100.0;
        matrix[0]*=ratio;matrix[2]*=ratio;matrix[4]*=ratio;
        NSFont* scaled=[NSFont fontWithName:[font fontName] matrix:matrix];
        [edit_ setFont:scaled ? scaled : font];
        const uint32_t bgr = state_.color();
        NSColor* color = [NSColor colorWithSRGBRed:(bgr & 255) / 255.0
            green:((bgr >> 8) & 255) / 255.0 blue:((bgr >> 16) & 255) / 255.0 alpha:1];
        [edit_ setTextColor:color];
        [edit_ setInsertionPointColor:color];
    }
public:
    CocoaTextEdit(HostTextEdit& state, SDL_Window* window, SDL_Renderer* renderer,
                  NSView* game) : state_(state), renderer_(renderer), game_(game) {
        window_id_=SDL_GetWindowID(window);
        SDL_AddEventWatch(watch_event,this);
        SDL_StopTextInput();
        state_.set_host_observer([this](HostTextEdit::HostChange change) {
            @autoreleasepool {
                if (change == HostTextEdit::HostChange::Create) create();
                else if (change == HostTextEdit::HostChange::Destroy) destroy();
                else { style(); synchronize(); }
            }
        });
        if (state_.is_open()) create();
    }
    ~CocoaTextEdit() override {
        SDL_DelEventWatch(watch_event,this);
        state_.set_host_observer({}); destroy();
    }
    void synchronize() override {
        @autoreleasepool {
            if (!state_.is_open()) { destroy(); return; }
            if (!edit_) { create(); return; }
            if (created_face_!=state_.font_name() || created_size_!=state_.edit_font_size() ||
                created_width_!=state_.active_width_percent() ||
                created_alignment_!=state_.active_alignment() ||
                created_multiline_!=(state_.height()/state_.edit_font_size()>1)) {
                create(); return;
            }
            int x0,y0,x1,y1;
            SDL_RenderLogicalToWindow(renderer_, state_.x(), state_.y(), &x0,&y0);
            SDL_RenderLogicalToWindow(renderer_, state_.x()+state_.width(),
                                      state_.y()+state_.height(), &x1,&y1);
            const NSRect bounds = [game_ bounds];
            [viewport_ setFrame:NSMakeRect(x0, [game_ isFlipped] ? y0 : NSHeight(bounds)-y1,
                                          x1-x0,y1-y0)];
            int sx0,sy0,sx1,sy1;
            SDL_RenderLogicalToWindow(renderer_,0,0,&sx0,&sy0);
            SDL_RenderLogicalToWindow(renderer_,100,100,&sx1,&sy1);
            if (painted_color_!=state_.color() || painted_scale_!=std::abs(sy1-sy0)) style();
            [viewport_ setHidden:!state_.visible()];
            if (!state_.visible() && [[edit_ window] firstResponder] == edit_)
                [[edit_ window] makeFirstResponder:game_];
            const auto text = state_.window_text().value_or("");
            if (text != committed_) {
                // Script/snapshot replacement cancels host-only preedit.
                if (text != bgi_utf8([edit_ string])) {
                    [edit_ unmarkText];
                    [edit_ setString:[NSString stringWithUTF8String:text.c_str()]];
                }
                committed_ = text;
            } else if (![edit_ hasMarkedText]) {
                committed_ = bgi_utf8([edit_ string]);
                state_.commit_text(committed_);
            }
            const NSSize intrinsic=[[edit_ textStorage] size];
            const CGFloat width=created_multiline_ ? x1-x0 :
                std::max<CGFloat>(x1-x0,std::ceil(intrinsic.width)+2);
            // Win32 EDIT owns a fixed child rectangle. Its text may scroll;
            // native layout must never expand that rectangle over adjacent UI.
            [[edit_ textContainer] setContainerSize:NSMakeSize(width,CGFLOAT_MAX)];
            [[edit_ layoutManager] ensureLayoutForTextContainer:[edit_ textContainer]];
            const NSRect used=[[edit_ layoutManager] usedRectForTextContainer:[edit_ textContainer]];
            const CGFloat height=created_multiline_ ?
                std::max<CGFloat>(y1-y0,std::ceil(NSMaxY(used))) : y1-y0;
            [edit_ setMinSize:NSMakeSize(width,height)];
            [edit_ setMaxSize:NSMakeSize(width,height)];
            [edit_ setFrame:NSMakeRect(0,0,width,height)];
            if (state_.visible() && [[edit_ window] firstResponder]==edit_)
                [edit_ scrollRangeToVisible:[edit_ selectedRange]];
        }
    }
    bool handle_event(const SDL_Event& event) override {
        if(event.type==SDL_WINDOWEVENT && event.window.event==SDL_WINDOWEVENT_FOCUS_LOST)
            pointer_capture_=false;
        if (event.type==SDL_KEYDOWN || event.type==SDL_KEYUP) {
            const auto found=std::find_if(owned_keys_.begin(),owned_keys_.end(),[&](const auto& key){
                return key.type==event.type && key.timestamp==event.key.timestamp &&
                    key.scan==event.key.keysym.scancode;
            });
            if(found!=owned_keys_.end()){owned_keys_.erase(found);return true;}
        }
        if(event.type==SDL_MOUSEBUTTONDOWN && inside(event.button.x,event.button.y)) {
            pointer_capture_=true;return true;
        }
        if(event.type==SDL_MOUSEBUTTONUP &&
            (pointer_capture_ || inside(event.button.x,event.button.y))) {
            pointer_capture_=false;return true;
        }
        if(event.type==SDL_MOUSEMOTION &&
            (pointer_capture_ || inside(event.motion.x,event.motion.y)))return true;
        if(event.type==SDL_MOUSEWHEEL && state_.is_open() && state_.visible()) {
            int x,y;SDL_GetMouseState(&x,&y);float logical_x,logical_y;
            SDL_RenderWindowToLogical(renderer_,x,y,&logical_x,&logical_y);
            if(inside(int(logical_x),int(logical_y)))return true;
        }
        const bool focused = edit_ && state_.visible() && [[edit_ window] firstResponder] == edit_;
        return focused && (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP ||
            event.type == SDL_TEXTINPUT || event.type == SDL_TEXTEDITING);
    }
};
}
std::unique_ptr<HostTextEditBridge> make_host_text_edit_bridge(
    HostTextEdit& state, SDL_Window* window, SDL_Renderer* renderer) {
    @autoreleasepool {
        SDL_SysWMinfo info{};
        SDL_VERSION(&info.version);
        if (window && SDL_GetWindowWMInfo(window, &info) && info.subsystem == SDL_SYSWM_COCOA)
            return std::make_unique<CocoaTextEdit>(state,window,renderer,
                                                 [info.info.cocoa.window contentView]);
    }
    return make_portable_text_edit_bridge(state,window,renderer);
}
} // namespace bgiplayer
#endif
