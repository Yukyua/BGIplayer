import 'dart:async';
import 'package:flutter/foundation.dart';

import '../models/app_settings.dart';
import '../models/game.dart';
import '../services/host_service.dart';

class AppController extends ChangeNotifier {
  AppController(
      {required List<Game> games,
      AppSettings settings = const AppSettings(),
      this.persistent = false,
      Future<void> Function(String)? removeImportedGame})
      : _games = List<Game>.of(games),
        _settings = settings,
        _removeImportedGame = removeImportedGame ?? removeHostGame;

  factory AppController.seeded() => AppController(games: <Game>[]);

  static Future<AppController> open() async {
    final AppController controller =
        AppController(games: <Game>[], persistent: true);
    try {
      controller._games.addAll(await readLibrary());
      controller._settings = await readSettings();
    } on Object catch (error) {
      controller.libraryWarning = '无法读取游戏库：$error。为保留原文件，当前不保存更改；请检查游戏库文件后重试。';
      controller._libraryWritable = false;
    }
    return controller;
  }

  final bool persistent;
  final Future<void> Function(String) _removeImportedGame;
  final List<Game> _games;
  AppSettings _settings;
  bool _importing = false;
  String? _importingPath;
  bool _disposed = false;
  bool _libraryWritable = true;
  String? libraryWarning;
  Future<void> _saveQueue = Future<void>.value();

  List<Game> get games => List<Game>.unmodifiable(_games);
  AppSettings get settings => _settings;
  bool get importing => _importing;
  double get importProgress => 0;
  String? get importingPath => _importingPath;

  Future<Game?> importGame(String path) async {
    if (_disposed || _importing || path.trim().isEmpty) return null;
    _importing = true;
    _importingPath = path.trim();
    _notify();
    try {
      final Game game = await inspectGame(path.trim());
      if (_disposed || _games.any((Game item) => item.path == game.path)) {
        return null;
      }
      _games.insert(0, game);
      _save();
      return game;
    } finally {
      _importing = false;
      _importingPath = null;
      _notify();
    }
  }

  void toggleFavorite(String id) {
    final int index = _games.indexWhere((Game game) => game.id == id);
    if (index == -1) return;
    _games[index] = _games[index].copyWith(favorite: !_games[index].favorite);
    _save();
    _notify();
  }

  Future<void> removeGame(String id) async {
    final matches = _games.where((game) => game.id == id);
    if (matches.isEmpty) return;
    await _removeImportedGame(matches.first.path);
    if (_disposed) return;
    _games.removeWhere((Game game) => game.id == id);
    _save();
    _notify();
  }

  void clearLibrary() {
    _libraryWritable = true;
    libraryWarning = null;
    _games.clear();
    _save();
    _notify();
  }

  void markPlayed(String id) {
    final int index = _games.indexWhere((Game game) => game.id == id);
    if (index == -1) return;
    _games[index] = _games[index].copyWith(lastPlayed: DateTime.now());
    _save();
    _notify();
  }

  Future<void> refreshRun(String id) async {
    final index = _games.indexWhere((g) => g.id == id);
    if (index < 0) return;
    final updated = await readGameRun(_games[index]);
    if (_disposed) return;
    final current = _games.indexWhere((g) => g.id == id);
    if (current < 0) return;
    _games[current] = updated.copyWith(favorite: _games[current].favorite);
    _save();
    _notify();
  }

  void updateSettings(AppSettings value) {
    _settings = value;
    if (persistent) {
      unawaited(writeSettings(value).catchError((Object error) {
        libraryWarning = '无法保存设置：$error';
        _notify();
      }));
    }
    _notify();
  }

  void _save() {
    if (!persistent || !_libraryWritable) return;
    final List<Game> snapshot = games;
    _saveQueue = _saveQueue
        .then((_) => writeLibrary(snapshot))
        .catchError((Object error) {
      libraryWarning = '无法保存游戏库：$error';
      _notify();
    });
  }

  void _notify() {
    if (!_disposed) notifyListeners();
  }

  @override
  void dispose() {
    _disposed = true;
    super.dispose();
  }
}
