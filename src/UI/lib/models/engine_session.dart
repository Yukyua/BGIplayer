import 'dart:async';

import 'package:flutter/foundation.dart';

import '../services/engine_bridge.dart';
import 'game.dart';

enum EngineStatus { idle, launching, running, paused, exited, error }

/// UI-side state machine for one game session.  The real implementation will
/// own (or attach to) the C++ `GameSession`; the interface stays the same.
class EngineSession extends ChangeNotifier {
  EngineSession({required this.game, required EngineBridge bridge})
      : _bridge = bridge;

  final Game game;

  EngineBridge _bridge;
  StreamSubscription<EngineEvent>? _subscription;
  EngineStatus _status = EngineStatus.idle;
  String? _error;
  String? diagnostic;
  bool _disposed = false;

  EngineStatus get status => _status;
  String? get error => _error;
  bool get isRunning => _status == EngineStatus.running;
  bool get hasError => _status == EngineStatus.error;

  Future<void> start() async {
    if (_disposed) return;
    await _subscription?.cancel();
    _subscription = _bridge.events.listen(_onEvent);
    _status = EngineStatus.launching;
    _error = null;
    _notify();
    try {
      await _bridge.launch(game);
    } on EngineBridgeUnavailable catch (error) {
      _status = EngineStatus.error;
      _error = error.message;
      _notify();
      return;
    } on Object catch (error) {
      _status = EngineStatus.error;
      _error = error.toString();
      _notify();
      return;
    }
    if (!_disposed && _status == EngineStatus.launching) {
      _status = EngineStatus.running;
      _notify();
    }
  }

  Future<void> pause() async {
    if (_disposed || _status != EngineStatus.running) return;
    _status = EngineStatus.paused;
    _notify();
    await _bridge.pause();
  }

  Future<void> resume() async {
    if (_disposed) return;
    _status = EngineStatus.running;
    _notify();
    await _bridge.resume();
  }

  Future<void> stop() async {
    if (_disposed) return;
    await _bridge.stop();
    _status = EngineStatus.exited;
    _notify();
  }

  Future<void> switchBridge(EngineBridge bridge) async {
    if (_disposed) return;
    await _subscription?.cancel();
    await _bridge.dispose();
    _bridge = bridge;
    _status = EngineStatus.idle;
    _error = null;
    _notify();
    await start();
  }

  void _onEvent(EngineEvent event) {
    if (_disposed) return;
    switch (event.type) {
      case EngineEventType.launching:
        _status = EngineStatus.launching;
        break;
      case EngineEventType.ready:
        diagnostic = event.message;
        _status = EngineStatus.running;
        break;
      case EngineEventType.paused:
        _status = EngineStatus.paused;
        break;
      case EngineEventType.resumed:
        _status = EngineStatus.running;
        break;
      case EngineEventType.exited:
        _status = EngineStatus.exited;
        break;
      case EngineEventType.error:
        _status = EngineStatus.error;
        _error = event.message;
        break;
    }
    _notify();
  }

  void _notify() {
    if (!_disposed) {
      notifyListeners();
    }
  }

  @override
  void dispose() {
    _disposed = true;
    final StreamSubscription<EngineEvent>? subscription = _subscription;
    if (subscription != null) {
      unawaited(subscription.cancel());
    }
    unawaited(_bridge.dispose());
    super.dispose();
  }
}
