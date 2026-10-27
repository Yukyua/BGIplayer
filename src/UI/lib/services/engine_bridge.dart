import 'dart:async';

import 'package:flutter/services.dart';

import '../models/game.dart';
import '../models/app_settings.dart';
import 'host_service.dart';

enum EngineEventType { launching, ready, paused, resumed, exited, error }

class EngineEvent {
  const EngineEvent(this.type, [this.message]);

  final EngineEventType type;
  final String? message;
}

class EngineBridgeUnavailable implements Exception {
  const EngineBridgeUnavailable(this.message);

  final String message;

  @override
  String toString() => message;
}

/// Host boundary used by the player screen.  A native shell implements this
/// over JNI/ObjC++ and forwards to the SDL2 game session.
abstract class EngineBridge {
  Stream<EngineEvent> get events;

  Future<void> launch(Game game);

  Future<void> pause();

  Future<void> resume();

  Future<void> stop();

  Future<void> dispose();
}

/// Default bridge for the UI demo.  It behaves like a very small engine:
/// launch takes a moment, then the player becomes interactive.
class DemoEngineBridge implements EngineBridge {
  final StreamController<EngineEvent> _controller =
      StreamController<EngineEvent>.broadcast();
  bool _disposed = false;

  @override
  Stream<EngineEvent> get events => _controller.stream;

  @override
  Future<void> launch(Game game) async {
    _emit(EngineEventType.launching);
    await Future<void>.delayed(const Duration(milliseconds: 900));
    if (!_disposed) {
      _emit(EngineEventType.ready);
    }
  }

  @override
  Future<void> pause() async {
    _emit(EngineEventType.paused);
  }

  @override
  Future<void> resume() async {
    _emit(EngineEventType.resumed);
  }

  @override
  Future<void> stop() async {
    _emit(EngineEventType.exited);
  }

  @override
  Future<void> dispose() async {
    _disposed = true;
    await _controller.close();
  }

  void _emit(EngineEventType type, [String? message]) {
    if (!_disposed) {
      _controller.add(EngineEvent(type, message));
    }
  }
}

/// Real native bridge contract.  The Dart side is deliberately thin so the
/// same channel can be implemented by Android (JNI + SDLActivity) and iOS
/// (ObjC++ + SDL view controller).
///
/// MethodChannel `bgiplayer/engine`:
///   launch(path, title, demo, renderer) -> null
///   pause() / resume() / stop() -> null
///
/// EventChannel `bgiplayer/engine/events`:
///   { "type": "ready" | "paused" | "resumed" | "exited" | "error",
///     "message": "..." }
class PlatformEngineBridge implements EngineBridge {
  PlatformEngineBridge(
      {this.renderer = RendererMode.gpuFast, this.defaultFont}) {
    try {
      _subscription = _events.receiveBroadcastStream().listen(
        _onNativeEvent,
        onError: (Object error, StackTrace stackTrace) {
          _emit(EngineEventType.error, error.toString());
        },
      );
    } on Object catch (error) {
      _emit(EngineEventType.error, error.toString());
    }
  }

  final RendererMode renderer;
  final String? defaultFont;

  static const MethodChannel _methods = MethodChannel('bgiplayer/engine');
  static const EventChannel _events = EventChannel('bgiplayer/engine/events');

  final StreamController<EngineEvent> _controller =
      StreamController<EngineEvent>.broadcast();
  StreamSubscription<dynamic>? _subscription;
  bool _disposed = false;

  @override
  Stream<EngineEvent> get events => _controller.stream;

  @override
  Future<void> launch(Game game) async {
    try {
      final run = await prepareGameRun(game);
      await _methods.invokeMethod<void>('launch', <String, Object?>{
        'path': game.path,
        'title': game.title,
        'demo': game.isDemo,
        'renderer': renderer.name,
        'defaultFont': defaultFont,
        'logPath': run['logPath'],
        'iconPath': run['iconPath'],
      });
    } on MissingPluginException {
      throw const EngineBridgeUnavailable(
        '未检测到原生引擎桥接。请先在 Android/iOS 端实现 '
        '`bgiplayer/engine` 通道，或切换到演示引擎。',
      );
    } on PlatformException catch (error) {
      throw EngineBridgeUnavailable(error.message ?? '原生引擎启动失败（${error.code}）');
    }
  }

  @override
  Future<void> pause() => _invokeQuietly('pause');

  @override
  Future<void> resume() => _invokeQuietly('resume');

  @override
  Future<void> stop() async {
    await _invokeQuietly('stop');
    _emit(EngineEventType.exited);
  }

  @override
  Future<void> dispose() async {
    _disposed = true;
    await _subscription?.cancel();
    await _controller.close();
  }

  Future<void> _invokeQuietly(String method) async {
    try {
      await _methods.invokeMethod<void>(method);
    } on MissingPluginException {
      // The player will already be showing the bridge-unavailable state.
    } on PlatformException {
      // Keep the UI responsive; the native side owns detailed error reporting.
    }
  }

  void _onNativeEvent(dynamic event) {
    if (event is! Map) return;
    final String? name = event['type']?.toString();
    final String? message = event['message']?.toString();
    final EngineEventType? type = switch (name) {
      'launching' => EngineEventType.launching,
      'ready' => EngineEventType.ready,
      'paused' => EngineEventType.paused,
      'resumed' => EngineEventType.resumed,
      'exited' => EngineEventType.exited,
      'error' => EngineEventType.error,
      _ => null,
    };
    if (type != null) {
      _emit(type, message);
    }
  }

  void _emit(EngineEventType type, [String? message]) {
    if (!_disposed) {
      _controller.add(EngineEvent(type, message));
    }
  }
}
