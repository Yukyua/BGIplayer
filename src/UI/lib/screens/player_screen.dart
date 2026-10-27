import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../core/app_scope.dart';
import '../models/engine_session.dart';
import '../models/game.dart';
import '../models/app_settings.dart';
import '../services/engine_bridge_factory.dart';
import '../services/host_service.dart';
import 'desktop_player_screen.dart';
import '../services/engine_bridge.dart';
import '../theme/app_theme.dart';
import '../widgets/demo_stage.dart';
import '../widgets/glass_panel.dart';
import '../widgets/pause_overlay.dart';
import '../widgets/pills.dart';
import 'settings_screen.dart';

class PlayerScreen extends StatefulWidget {
  const PlayerScreen({
    super.key,
    required this.game,
    required this.usePlatformBridge,
    this.renderer = RendererMode.gpuFast,
  });

  final Game game;
  final bool usePlatformBridge;
  final RendererMode renderer;

  static Route<void> route({
    required Game game,
    required bool usePlatformBridge,
    RendererMode renderer = RendererMode.gpuFast,
  }) {
    if (desktopHost && usePlatformBridge) {
      return MaterialPageRoute<void>(
          builder: (_) => DesktopPlayerScreen(game: game, renderer: renderer));
    }
    return PageRouteBuilder<void>(
      transitionDuration: const Duration(milliseconds: 360),
      reverseTransitionDuration: const Duration(milliseconds: 260),
      pageBuilder: (
        BuildContext context,
        Animation<double> animation,
        Animation<double> secondaryAnimation,
      ) {
        return PlayerScreen(
          game: game,
          usePlatformBridge: usePlatformBridge,
          renderer: renderer,
        );
      },
      transitionsBuilder: (
        BuildContext context,
        Animation<double> animation,
        Animation<double> secondaryAnimation,
        Widget child,
      ) {
        return FadeTransition(
          opacity: CurvedAnimation(
            parent: animation,
            curve: Curves.easeOutCubic,
          ),
          child: child,
        );
      },
    );
  }

  @override
  State<PlayerScreen> createState() => _PlayerScreenState();
}

class _PlayerScreenState extends State<PlayerScreen>
    with WidgetsBindingObserver {
  late final EngineSession _session;
  bool _paused = false;
  bool _exiting = false;
  bool _showLoading = true;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _session = EngineSession(
      game: widget.game,
      bridge: widget.usePlatformBridge
          ? createEngineBridge(
              renderer: widget.renderer,
              defaultFont: AppScope.read(context).settings.defaultFont)
          : DemoEngineBridge(),
    );
    _session.addListener(_onSessionChanged);
    unawaited(_enterPlayerMode());
    unawaited(_session.start());
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _session.removeListener(_onSessionChanged);
    _session.dispose();
    unawaited(_restorePlayerMode());
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.inactive ||
        state == AppLifecycleState.paused) {
      if (_session.status == EngineStatus.running && !_paused) {
        _pause();
      }
    }
  }

  void _onSessionChanged() {
    if (!mounted) return;
    final EngineStatus status = _session.status;
    if (status == EngineStatus.running ||
        status == EngineStatus.exited ||
        status == EngineStatus.error) {
      unawaited(AppScope.of(context).refreshRun(widget.game.id));
    }
    if (_showLoading &&
        (status == EngineStatus.running ||
            status == EngineStatus.error ||
            status == EngineStatus.exited)) {
      setState(() => _showLoading = false);
    }
    if (status == EngineStatus.exited && !_exiting) {
      _exiting = true;
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted) {
          Navigator.of(context).pop();
        }
      });
    }
  }

  Future<void> _enterPlayerMode() async {
    try {
      await SystemChrome.setEnabledSystemUIMode(SystemUiMode.immersiveSticky);
      await SystemChrome.setPreferredOrientations(const <DeviceOrientation>[
        DeviceOrientation.landscapeLeft,
        DeviceOrientation.landscapeRight,
      ]);
      SystemChrome.setSystemUIOverlayStyle(
        const SystemUiOverlayStyle(
          statusBarColor: Colors.transparent,
          statusBarIconBrightness: Brightness.light,
          statusBarBrightness: Brightness.dark,
          systemNavigationBarColor: Colors.black,
          systemNavigationBarIconBrightness: Brightness.light,
        ),
      );
    } on Object catch (_) {
      // Desktop/web: system UI calls are no-ops or unsupported.
    }
  }

  Future<void> _restorePlayerMode() async {
    try {
      await SystemChrome.setEnabledSystemUIMode(SystemUiMode.edgeToEdge);
      await SystemChrome.setPreferredOrientations(const <DeviceOrientation>[]);
      SystemChrome.setSystemUIOverlayStyle(SystemUiOverlayStyle.light);
    } on Object catch (_) {
      // Ignore unsupported platforms.
    }
  }

  void _pause() {
    if (_paused || _session.hasError) return;
    setState(() => _paused = true);
    if (_session.status == EngineStatus.running) {
      unawaited(_session.pause());
    }
  }

  void _resume() {
    setState(() => _paused = false);
    if (_session.status == EngineStatus.paused) {
      unawaited(_session.resume());
    }
  }

  Future<void> _openSettings() async {
    _pause();
    await Navigator.of(context).push(
      MaterialPageRoute<void>(
        builder: (BuildContext context) => const SettingsScreen(),
      ),
    );
  }

  Future<void> _useDemoEngine() async {
    setState(() => _showLoading = true);
    await _session.switchBridge(DemoEngineBridge());
  }

  Future<void> _exit() async {
    if (_exiting) return;
    final bool? confirmed = await showDialog<bool>(
      context: context,
      builder: (BuildContext context) => AlertDialog(
        backgroundColor: AppColors.surfaceRaised,
        title: const Text('退出游戏？'),
        content: const Text('如果游戏内还没有存档，当前进度可能会丢失。'),
        actions: <Widget>[
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: const Text('继续游戏'),
          ),
          FilledButton(
            onPressed: () => Navigator.of(context).pop(true),
            child: const Text('退出'),
          ),
        ],
      ),
    );
    if (confirmed != true || !mounted) return;
    setState(() {
      _exiting = true;
      _paused = false;
    });
    await _session.stop();
    if (!mounted) return;
    Navigator.of(context).pop();
  }

  void _onBack() {
    if (_session.hasError) {
      Navigator.of(context).pop();
      return;
    }
    if (_paused) {
      unawaited(_exit());
      return;
    }
    _pause();
  }

  @override
  Widget build(BuildContext context) {
    final bool showHints = AppScope.of(context).settings.showTouchHints;
    return PopScope(
      canPop: false,
      onPopInvokedWithResult: (bool didPop, dynamic result) {
        if (didPop) return;
        if (_session.hasError) {
          Navigator.of(context).pop();
          return;
        }
        if (_paused) {
          unawaited(_exit());
          return;
        }
        _pause();
      },
      child: Scaffold(
        backgroundColor: Colors.black,
        body: Stack(
          fit: StackFit.expand,
          children: <Widget>[
            AnimatedBuilder(
              animation: _session,
              builder: (BuildContext context, Widget? child) {
                if (_session.hasError) {
                  return _EngineErrorView(
                    error: _session.error,
                    onUseDemo: () {
                      unawaited(_useDemoEngine());
                    },
                    onBack: () => Navigator.of(context).pop(),
                  );
                }
                return LayoutBuilder(
                  builder: (BuildContext context, BoxConstraints constraints) {
                    final double width = math.min(
                      constraints.maxWidth,
                      constraints.maxHeight * 16 / 9,
                    );
                    final double height = width * 9 / 16;
                    return Center(
                      child: SizedBox(
                        width: width,
                        height: height,
                        child: ClipRect(
                          child: DemoStage(showTouchHints: showHints),
                        ),
                      ),
                    );
                  },
                );
              },
            ),
            _PlayerTopBar(game: widget.game, onBack: _onBack, onPause: _pause),
            if (_showLoading && _session.status == EngineStatus.launching)
              _LaunchOverlay(game: widget.game),
            if (_paused)
              PauseOverlay(
                onResume: _resume,
                onSettings: _openSettings,
                onExit: _exit,
              ),
          ],
        ),
      ),
    );
  }
}

class _PlayerTopBar extends StatelessWidget {
  const _PlayerTopBar({
    required this.game,
    required this.onBack,
    required this.onPause,
  });

  final Game game;
  final VoidCallback onBack;
  final VoidCallback onPause;

  @override
  Widget build(BuildContext context) {
    final TextTheme text = Theme.of(context).textTheme;
    return Positioned(
      top: 0,
      left: 0,
      right: 0,
      child: Container(
        padding: EdgeInsets.only(
          top: MediaQuery.of(context).padding.top + 4,
          left: 6,
          right: 6,
          bottom: 24,
        ),
        decoration: BoxDecoration(
          gradient: LinearGradient(
            begin: Alignment.topCenter,
            end: Alignment.bottomCenter,
            colors: <Color>[Colors.black.withAlpha(196), Colors.transparent],
          ),
        ),
        child: Row(
          children: <Widget>[
            IconButton(
              tooltip: '返回',
              onPressed: onBack,
              icon: const Icon(Icons.arrow_back_rounded),
            ),
            const SizedBox(width: 4),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: <Widget>[
                  Text(
                    game.title,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: text.titleSmall,
                  ),
                  const SizedBox(height: 2),
                  Text(
                    game.subtitle,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: text.bodySmall?.copyWith(
                      color: Colors.white.withAlpha(120),
                      fontSize: 10,
                    ),
                  ),
                ],
              ),
            ),
            if (game.isDemo) ...<Widget>[
              const DemoBadge(),
              const SizedBox(width: 6),
            ],
            IconButton(
              tooltip: '暂停',
              onPressed: onPause,
              icon: const Icon(Icons.pause_rounded),
            ),
          ],
        ),
      ),
    );
  }
}

class _LaunchOverlay extends StatelessWidget {
  const _LaunchOverlay({required this.game});

  final Game game;

  @override
  Widget build(BuildContext context) {
    final TextTheme text = Theme.of(context).textTheme;
    return Positioned.fill(
      child: ColoredBox(
        color: Colors.black.withAlpha(194),
        child: Center(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              const SizedBox(
                width: 42,
                height: 42,
                child: CircularProgressIndicator(strokeWidth: 3),
              ),
              const SizedBox(height: 22),
              Text('正在启动引擎…', style: text.titleMedium),
              const SizedBox(height: 8),
              Text(
                game.title,
                style: text.bodySmall?.copyWith(color: AppColors.muted),
              ),
              const SizedBox(height: 4),
              Text(
                'SDL2 host · Flutter overlay',
                style: text.labelSmall?.copyWith(
                  color: Colors.white.withAlpha(80),
                  letterSpacing: 1,
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _EngineErrorView extends StatelessWidget {
  const _EngineErrorView({
    required this.error,
    required this.onUseDemo,
    required this.onBack,
  });

  final String? error;
  final VoidCallback onUseDemo;
  final VoidCallback onBack;

  @override
  Widget build(BuildContext context) {
    final TextTheme text = Theme.of(context).textTheme;
    return ColoredBox(
      color: Colors.black,
      child: SafeArea(
        child: Center(
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 440),
            child: Padding(
              padding: const EdgeInsets.all(24),
              child: GlassPanel(
                padding: const EdgeInsets.fromLTRB(24, 26, 24, 22),
                radius: 26,
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: <Widget>[
                    Container(
                      width: 54,
                      height: 54,
                      decoration: BoxDecoration(
                        color: AppColors.danger.withAlpha(24),
                        shape: BoxShape.circle,
                        border: Border.all(color: AppColors.border),
                      ),
                      child: const Icon(
                        Icons.link_off_rounded,
                        color: AppColors.danger,
                      ),
                    ),
                    const SizedBox(height: 16),
                    Text('引擎桥接不可用', style: text.headlineSmall),
                    const SizedBox(height: 8),
                    Text(
                      error ??
                          '原生端还没有实现 bgiplayer/engine 通道。'
                              '你可以先使用内置演示引擎预览 UI 流程。',
                      textAlign: TextAlign.center,
                      style: text.bodySmall?.copyWith(
                        color: AppColors.muted,
                        height: 1.6,
                      ),
                    ),
                    const SizedBox(height: 22),
                    SizedBox(
                      width: double.infinity,
                      child: FilledButton.icon(
                        onPressed: onUseDemo,
                        icon: const Icon(Icons.science_outlined),
                        label: const Text('使用演示引擎'),
                      ),
                    ),
                    const SizedBox(height: 8),
                    SizedBox(
                      width: double.infinity,
                      child: OutlinedButton.icon(
                        onPressed: onBack,
                        icon: const Icon(Icons.arrow_back_rounded),
                        label: const Text('返回游戏库'),
                      ),
                    ),
                  ],
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}
