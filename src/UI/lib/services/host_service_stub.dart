import 'dart:typed_data';
import '../models/app_settings.dart';
import '../models/game.dart';

const bool privateGameCopies = false;
Future<void> removeHostGame(String path) async {}

const bool desktopHost = false;
const bool fixedGamesDirectory = false;
Future<Game> inspectGame(String path) async =>
    throw UnsupportedError('请使用桌面版本导入本地游戏。');
Future<List<Game>> readLibrary() async => <Game>[];
Future<void> writeLibrary(List<Game> games) async {}
Future<bool> openHostFolder(String path) async => false;

Future<Map<String, String>> prepareGameRun(Game game,
        {String? logPath, String? supportPath}) async =>
    {};
Future<Game> readGameRun(Game game, {String? supportPath}) async => game;
Future<Uint8List?> readGameIcon(String? path) async => null;
Future<String> readLastGameLog(String? path) async => '尚无运行日志';
Future<AppSettings> readSettings() async => const AppSettings();
Future<void> writeSettings(AppSettings settings) async {}
