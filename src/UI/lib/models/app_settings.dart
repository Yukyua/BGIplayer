enum RendererMode { cpuExact, gpuExact, gpuFast }

enum ScalingMode { fit, fill }

enum FrameCap { fps60, fps120 }

enum TextureFilter { nearest, linear }

extension RendererModeLabel on RendererMode {
  String get label => switch (this) {
        RendererMode.gpuFast => 'GPU（快速）',
        RendererMode.cpuExact => 'CPU（精确）',
        RendererMode.gpuExact => 'GPU（精确）',
      };
}

extension ScalingModeLabel on ScalingMode {
  String get label => switch (this) {
        ScalingMode.fit => '完整显示（留黑边）',
        ScalingMode.fill => '填满屏幕（裁切）',
      };
}

extension FrameCapLabel on FrameCap {
  String get label => switch (this) {
        FrameCap.fps60 => '60 FPS',
        FrameCap.fps120 => '120 FPS',
      };
}

extension TextureFilterLabel on TextureFilter {
  String get label => switch (this) {
        TextureFilter.nearest => '最近邻（像素清晰）',
        TextureFilter.linear => '线性（平滑）',
      };
}

/// Host-side settings only.  Text speed, BGM volume and game screen mode stay
/// inside the game's own Config screen.
class AppSettings {
  const AppSettings({
    this.renderer = RendererMode.gpuFast,
    this.defaultFont,
    this.defaultFontName,
    this.scaling = ScalingMode.fit,
    this.frameCap = FrameCap.fps60,
    this.textureFilter = TextureFilter.linear,
    this.longPressRightClick = true,
    this.showTouchHints = true,
    this.usePlatformBridge = true,
  });

  final RendererMode renderer;
  final String? defaultFont;
  final String? defaultFontName;
  final ScalingMode scaling;
  final FrameCap frameCap;
  final TextureFilter textureFilter;
  final bool longPressRightClick;
  final bool showTouchHints;
  final bool usePlatformBridge;

  AppSettings copyWith({
    RendererMode? renderer,
    String? defaultFont,
    String? defaultFontName,
    bool clearDefaultFont = false,
    ScalingMode? scaling,
    FrameCap? frameCap,
    TextureFilter? textureFilter,
    bool? longPressRightClick,
    bool? showTouchHints,
    bool? usePlatformBridge,
  }) {
    return AppSettings(
      renderer: renderer ?? this.renderer,
      defaultFont: clearDefaultFont ? null : defaultFont ?? this.defaultFont,
      defaultFontName:
          clearDefaultFont ? null : defaultFontName ?? this.defaultFontName,
      scaling: scaling ?? this.scaling,
      frameCap: frameCap ?? this.frameCap,
      textureFilter: textureFilter ?? this.textureFilter,
      longPressRightClick: longPressRightClick ?? this.longPressRightClick,
      showTouchHints: showTouchHints ?? this.showTouchHints,
      usePlatformBridge: usePlatformBridge ?? this.usePlatformBridge,
    );
  }
}
