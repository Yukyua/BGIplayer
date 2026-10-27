#import "BGIEnginePlugin.h"
#import <UIKit/UIKit.h>
#import <CoreText/CoreText.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "bgiplayer/application.hpp"
#if TARGET_OS_SIMULATOR && defined(BGI_IOS_VM_POLICY_PROBE)
#include "bgiplayer/vm.hpp"
#endif
#include "bgiplayer/game_window.hpp"
#include "bgiplayer/host_text_edit_bridge.hpp"
#include "bgiplayer/keyboard_input.hpp"
#include "bgiplayer/sdl_window_mode.hpp"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_system.h>
namespace {
struct EngineLogScope {
    int output=-1,error=-1;
    explicit EngineLogScope(NSString* path) {
        if (![path isKindOfClass:NSString.class] || !path.length) return;
        const int fd=open(path.fileSystemRepresentation,O_WRONLY|O_CREAT|O_TRUNC,0600);
        if (fd<0) throw std::runtime_error("cannot open engine session log");
        output=dup(STDOUT_FILENO);error=dup(STDERR_FILENO);
        if (output<0 || error<0) { if(output>=0)close(output);if(error>=0)close(error);close(fd);throw std::runtime_error("cannot preserve engine log streams"); }
        std::cout.flush();std::cerr.flush();fflush(nullptr);
        const bool failed=dup2(fd,STDOUT_FILENO)<0 || dup2(fd,STDERR_FILENO)<0;
        close(fd);
        if (failed) {restore();throw std::runtime_error("cannot redirect engine log streams");}
    }
    void restore() {
        std::cout.flush();std::cerr.flush();fflush(nullptr);
        if(output>=0) {dup2(output,STDOUT_FILENO);close(output);output=-1;}
        if(error>=0) {dup2(error,STDERR_FILENO);close(error);error=-1;}
    }
    ~EngineLogScope() {restore();}
};
struct IconOutputScope {
    std::optional<std::string> previous;
    explicit IconOutputScope(NSString* path) {
        if (const char* old=getenv("BGI_GAME_ICON_OUTPUT")) previous=old;
        if ([path isKindOfClass:NSString.class] && path.length) setenv("BGI_GAME_ICON_OUTPUT",path.fileSystemRepresentation,1);
        else unsetenv("BGI_GAME_ICON_OUTPUT");
    }
    ~IconOutputScope() {
        if(previous) setenv("BGI_GAME_ICON_OUTPUT",previous->c_str(),1);
        else unsetenv("BGI_GAME_ICON_OUTPUT");
    }
};
NSArray* availableFontFiles() {
    NSMutableArray* result=[NSMutableArray array];NSMutableSet* seen=[NSMutableSet set];
    CFArrayRef names=CTFontManagerCopyAvailablePostScriptNames();
    if (!names) return result;
    for (CFIndex index=0;index<CFArrayGetCount(names);++index) {
        CFStringRef name=(CFStringRef)CFArrayGetValueAtIndex(names,index);
        CTFontRef font=CTFontCreateWithName(name,16,nullptr);if(!font)continue;
        CFTypeRef attribute=CTFontCopyAttribute(font,kCTFontURLAttribute);
        CFStringRef fullName=CTFontCopyFullName(font);
        if (attribute && CFGetTypeID(attribute)==CFURLGetTypeID()) {
            NSURL* url=(__bridge NSURL*)attribute;
            NSString* label=fullName?(__bridge NSString*)fullName:(__bridge NSString*)name;
            NSString* key=url.path;
            if (url.isFileURL && [[NSFileManager defaultManager] isReadableFileAtPath:url.path] && ![seen containsObject:key]) {
                [seen addObject:key];[result addObject:@{@"name":label,@"path":url.path}];
            }
        }
        if(attribute)CFRelease(attribute);if(fullName)CFRelease(fullName);CFRelease(font);
    }
    CFRelease(names);
    [result sortUsingComparator:^NSComparisonResult(NSDictionary* a,NSDictionary* b) {return [a[@"name"] localizedCaseInsensitiveCompare:b[@"name"]];}];
    return result;
}
}
namespace bgiplayer {
HostKeyboardLayout host_keyboard_layout() { return HostKeyboardLayout::Logical; }
uint16_t host_keyboard_modifiers(uint16_t fallback) { return fallback; }
bool host_window_has_modal_dialog(SDL_Window*) {
    for (UIWindowScene* scene in UIApplication.sharedApplication.connectedScenes)
        if ([scene isKindOfClass:UIWindowScene.class])
            for (UIWindow* window in scene.windows)
                if (window.isKeyWindow && window.rootViewController.presentedViewController) return true;
    return false;
}
void set_game_dock_icon(const GameWindowIcon&) {} // iOS has no per-window Dock icon API.
std::unique_ptr<HostTextEditBridge> make_host_text_edit_bridge(HostTextEdit& state, SDL_Window* window, SDL_Renderer* renderer) {
    return make_portable_text_edit_bridge(state,window,renderer);
}
}
@implementation BGIEnginePlugin {
    FlutterEventSink _events;
    BOOL _running;
    BOOL _ready;
    UIWindow* _libraryWindow;
    NSTimer* _readyTimer;
}
+ (void)registerWithRegistrar:(NSObject<FlutterPluginRegistrar>*)registrar {
    BGIEnginePlugin* plugin=[BGIEnginePlugin new];
    FlutterMethodChannel* channel=[FlutterMethodChannel methodChannelWithName:@"bgiplayer/engine" binaryMessenger:registrar.messenger];
    [registrar addMethodCallDelegate:plugin channel:channel];
    FlutterMethodChannel* fonts=[FlutterMethodChannel methodChannelWithName:@"bgiplayer/fonts" binaryMessenger:registrar.messenger];
    [fonts setMethodCallHandler:^(FlutterMethodCall* call,FlutterResult result) {
        if ([call.method isEqualToString:@"list"]) result(availableFontFiles());
        else result(FlutterMethodNotImplemented);
    }];
    [[FlutterEventChannel eventChannelWithName:@"bgiplayer/engine/events" binaryMessenger:registrar.messenger] setStreamHandler:plugin];
#if TARGET_OS_SIMULATOR
    // Simulator-only CLI smoke test uses exactly the public launch handler.
    if (const char* game=getenv("BGI_IOS_TEST_GAME")) {
#ifdef BGI_IOS_VM_POLICY_PROBE
        // Opt-in only: requires the static engine to match the current VM header.
        // Verify state preservation in a real iOS VM, not a desktop macro mock.
        bgiplayer::VM check(std::vector<uint8_t>{0x01});
        check.set_window_mode_size(6,1280,720);
        check.select_window_mode(0,0,6);check.set_client_size(1280,720);
        int applied=0;
        check.set_window_mode_applier([&](const bgiplayer::VM::WindowModeRequest&) { ++applied;return true; });
        const bool reject_full=!check.select_window_mode(1,0,6);
        const bool reject_size=!check.select_window_mode(0,0,8);
        check.set_client_size(1920,1080);check.set_window_mode_size(6,640,480);
        const bool retained=check.window_fullscreen()==0 && check.window_mode_index()==6 &&
            check.client_width()==1280 && check.client_height()==720 &&
            check.window_width(6)==1280 && check.window_height(6)==720 && applied==0;
        if (!reject_full || !reject_size || !retained) {
            NSLog(@"BGI_IOS_TEST mobile window policy FAILED");return;
        }
        NSLog(@"BGI_IOS_TEST mobile window policy PASS (mode, fullscreen, client dimensions retained)");
#endif
        setvbuf(stdout,nullptr,_IOLBF,0);
        NSArray* fontFiles=availableFontFiles();
        NSLog(@"BGI_IOS_TEST readable font files: %lu",(unsigned long)fontFiles.count);
        if (!fontFiles.count) { NSLog(@"BGI_IOS_TEST font enumeration FAILED");return; }
        NSString* path=[NSString stringWithUTF8String:game];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_main_queue(), ^{
            NSMutableDictionary* arguments=[@{@"path":path,@"renderer":@"cpuExact",
                @"logPath":[NSHomeDirectory() stringByAppendingPathComponent:@"Documents/ios-engine-session.log"],
                @"iconPath":[NSHomeDirectory() stringByAppendingPathComponent:@"Documents/ios-game-icon.png"]} mutableCopy];
            if (const char* override=getenv("BGI_IOS_TEST_FONT")) arguments[@"defaultFont"]=[NSString stringWithUTF8String:override];
            else for (NSDictionary* file in fontFiles) {
                if ([file[@"path"] rangeOfString:@"hiragino" options:NSCaseInsensitiveSearch].location!=NSNotFound ||
                    [file[@"name"] containsString:@"ヒラギノ"]) {arguments[@"defaultFont"]=file[@"path"];break;}
            }
            if (!arguments[@"defaultFont"]) {NSLog(@"BGI_IOS_TEST no Japanese default font file found");return;}
            NSLog(@"BGI_IOS_TEST defaultFont file provided=YES");
            [plugin handleMethodCall:[FlutterMethodCall methodCallWithMethodName:@"launch" arguments:arguments] result:^(id result) { NSLog(@"BGI_IOS_TEST launch result: %@",result); }];
        });
    }
#endif
}
- (FlutterError*)onListenWithArguments:(id)args eventSink:(FlutterEventSink)sink { _events=[sink copy];return nil; }
- (FlutterError*)onCancelWithArguments:(id)args { _events=nil;return nil; }
- (void)emit:(NSString*)type message:(NSString*)message {
    if ([type isEqualToString:@"error"]) NSLog(@"BGI_ENGINE error %@",message?:@"");
#if TARGET_OS_SIMULATOR
    else if (getenv("BGI_IOS_TEST_GAME")) NSLog(@"BGI_ENGINE %@ %@",type,message?:@"");
#endif
    if (_events) _events(@{@"type":type,@"message":message?:@""});
}
- (void)stopGame { SDL_Event event{};event.type=SDL_QUIT;SDL_PushEvent(&event); }
- (void)handleMethodCall:(FlutterMethodCall*)call result:(FlutterResult)result {
    if ([call.method isEqualToString:@"stop"]) { [self stopGame];result(nil);return; }
    if ([call.method isEqualToString:@"pause"] || [call.method isEqualToString:@"resume"]) {
        SDL_Event event{};event.type=[call.method isEqualToString:@"pause"]?SDL_APP_WILLENTERBACKGROUND:SDL_APP_DIDENTERFOREGROUND;
        SDL_PushEvent(&event);result(nil);return;
    }
    if (![call.method isEqualToString:@"launch"]) {result(FlutterMethodNotImplemented);return;}
    if (_running) {result([FlutterError errorWithCode:@"busy" message:@"游戏正在运行" details:nil]);return;}
    NSString* path=call.arguments[@"path"];
    if (![path isKindOfClass:NSString.class] || ![[NSFileManager defaultManager] fileExistsAtPath:[path stringByAppendingPathComponent:@"sysprg.arc"]]) {
        // Archive/sysprg.arc is also a valid engine layout.
        if (![path isKindOfClass:NSString.class] || ![[NSFileManager defaultManager] fileExistsAtPath:[path stringByAppendingPathComponent:@"Archive/sysprg.arc"]]) {
            result([FlutterError errorWithCode:@"directory" message:@"游戏目录缺少 sysprg.arc" details:nil]);return;
        }
    }
    for (UIWindowScene* scene in UIApplication.sharedApplication.connectedScenes)
        if ([scene isKindOfClass:UIWindowScene.class])
            for (UIWindow* window in scene.windows) if (window.isKeyWindow) _libraryWindow=window;
    _running=YES;_ready=NO;result(nil);[self emit:@"launching" message:nil];
    NSString* gamePath=[path copy];
    NSString* rendering=call.arguments[@"renderer"];
    NSString* defaultFont=call.arguments[@"defaultFont"];
    NSString* logPath=call.arguments[@"logPath"];
    NSString* iconPath=call.arguments[@"iconPath"];
    dispatch_async(dispatch_get_main_queue(), ^{
        SDL_SetMainReady();SDL_iPhoneSetEventPump(SDL_TRUE);
        SDL_SetHint(SDL_HINT_ORIENTATIONS,"LandscapeLeft LandscapeRight");
        self->_readyTimer=[NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES block:^(NSTimer* timer) {
            if (self->_ready || !SDL_GetKeyboardFocus()) return;
            self->_ready=YES;[self emit:@"ready" message:nil];
            for (UIWindowScene* scene in UIApplication.sharedApplication.connectedScenes)
                if ([scene isKindOfClass:UIWindowScene.class])
                    for (UIWindow* window in scene.windows) if (window.isKeyWindow && window!=self->_libraryWindow) {
                        UIButton* close=[UIButton buttonWithType:UIButtonTypeSystem];
                        [close setTitle:@"×" forState:UIControlStateNormal];close.frame=CGRectMake(8,8,44,44);
                        close.backgroundColor=[UIColor colorWithWhite:0 alpha:0.4];close.tintColor=UIColor.whiteColor;
                        [close addTarget:self action:@selector(stopGame) forControlEvents:UIControlEventTouchUpInside];
                        [window.rootViewController.view addSubview:close];
                    }
            [timer invalidate];self->_readyTimer=nil;
        }];
        try { EngineLogScope log(logPath);IconOutputScope icon(iconPath);
            bgiplayer::Options options;
            if ([defaultFont isKindOfClass:NSString.class] && defaultFont.length) options.default_font_file=defaultFont.fileSystemRepresentation;options.game=gamePath.UTF8String;options.backend="sdl2";options.render_mode=[rendering isEqualToString:@"gpuFast"]?"fast":"exact";setenv("BGI_GPU",[rendering isEqualToString:@"cpuExact"]?"0":"1",1);
#if TARGET_OS_SIMULATOR
            if (const char* seconds=getenv("BGI_IOS_TEST_SECONDS")) options.runtime_seconds=std::stod(seconds);
            if (const char* replay=getenv("BGI_IOS_TEST_REPLAY")) options.replay_clicks=replay;
            if (const char* screenshot=getenv("BGI_IOS_TEST_SCREENSHOT")) options.screenshot=screenshot;
#endif
            try { bgiplayer::run(options); }
            catch (const std::exception& error) { std::cerr << "BGIplayer: " << error.what() << std::endl;throw; }
            [self emit:@"exited" message:nil]; }
        catch (const std::exception& error) { [self emit:@"error" message:[NSString stringWithUTF8String:error.what()]]; }
        [self->_readyTimer invalidate];self->_readyTimer=nil;self->_running=NO;
        [self->_libraryWindow makeKeyAndVisible];self->_libraryWindow=nil;
#if TARGET_OS_SIMULATOR
        if (getenv("BGI_IOS_TEST_GAME")) {
            fprintf(stdout,"BGI_IOS_TEST stdout restored\n");fflush(stdout);
            fprintf(stderr,"BGI_IOS_TEST stderr restored; icon env restored=%s\n",getenv("BGI_GAME_ICON_OUTPUT")?:"<unset>");
        }
#endif
    });
}
@end
