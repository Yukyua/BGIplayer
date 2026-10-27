import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

import '../models/game.dart';
import '../models/app_settings.dart';
import '../utils/palette.dart';

final bool fixedGamesDirectory = Platform.isIOS;

final bool desktopHost =
    Platform.isMacOS || Platform.isWindows || Platform.isLinux;

/// Only inspects directory entries and PE headers. Game data stays in place.
Future<Game> inspectGame(String path) async {
  final Directory directory = Directory(path);
  if (!await directory.exists()) throw const FormatException('游戏目录不存在。');
  final String canonical = await directory.resolveSymbolicLinks();
  final List<FileSystemEntity> entries =
      await Directory(canonical).list().toList();
  final List<File> archives = <File>[];
  final Set<EngineGeneration> generations = <EngineGeneration>{};
  for (final FileSystemEntity entry in entries) {
    if (entry is File) {
      final String extension = p.extension(entry.path).toLowerCase();
      if (extension == '.arc') archives.add(entry);
      if (extension == '.exe') {
        final EngineGeneration generation = await _peGeneration(entry);
        if (generation != EngineGeneration.unknown) generations.add(generation);
      }
    } else if (entry is Directory &&
        p.basename(entry.path).toLowerCase() == 'archive') {
      await for (final FileSystemEntity item in entry.list()) {
        if (item is File && p.extension(item.path).toLowerCase() == '.arc') {
          archives.add(item);
        }
      }
    }
  }
  if (!archives.any(
      (File file) => p.basename(file.path).toLowerCase() == 'sysprg.arc')) {
    throw const FormatException('未找到 sysprg.arc，请选择 BGI 游戏根目录。');
  }
  int bytes = 0;
  await for (final entry
      in Directory(canonical).list(recursive: true, followLinks: false)) {
    if (entry is File) bytes += await entry.length();
  }
  final String title = p.basename(canonical);
  return Game(
    id: canonical,
    title: title,
    subtitle: '${archives.length} 个资源包',
    path: canonical,
    sizeBytes: bytes,
    coverColors: colorsForName(title),
    engine:
        generations.length == 1 ? generations.single : EngineGeneration.unknown,
    synopsis: fixedGamesDirectory
        ? '由内嵌 BGIplayer 引擎运行。游戏资源和 UserData 存档保留在 Documents/Games 的游戏子目录。'
        : '由 BGIplayer SDL2 引擎运行。游戏资源和 UserData 存档保留在所选目录。',
  );
}

Future<EngineGeneration> _peGeneration(File file) async {
  final RandomAccessFile input = await file.open();
  try {
    final Uint8List dos = await input.read(64);
    if (dos.length != 64 || dos[0] != 0x4d || dos[1] != 0x5a) {
      return EngineGeneration.unknown;
    }
    final int offset = ByteData.sublistView(dos).getUint32(60, Endian.little);
    if (offset > await input.length() - 26) return EngineGeneration.unknown;
    await input.setPosition(offset);
    final Uint8List pe = await input.read(26);
    if (pe.length != 26 ||
        pe[0] != 80 ||
        pe[1] != 69 ||
        pe[2] != 0 ||
        pe[3] != 0) {
      return EngineGeneration.unknown;
    }
    return switch (ByteData.sublistView(pe).getUint16(24, Endian.little)) {
      0x10b => EngineGeneration.pe32,
      0x20b => EngineGeneration.pe64,
      _ => EngineGeneration.unknown,
    };
  } finally {
    await input.close();
  }
}

Future<File> _libraryFile() async {
  final Directory support = await getApplicationSupportDirectory();
  await support.create(recursive: true);
  return File(p.join(support.path, 'library.json'));
}

Future<List<Game>> readLibrary() async {
  if (fixedGamesDirectory) {
    final Directory documents = await getApplicationDocumentsDirectory();
    final Directory games = Directory(p.join(documents.path, 'Games'));
    await games.create(recursive: true);
    final List<Game> found = <Game>[];
    final List<FileSystemEntity> entries =
        await games.list(followLinks: false).toList();
    entries.sort(
        (FileSystemEntity a, FileSystemEntity b) => a.path.compareTo(b.path));
    for (final FileSystemEntity entry in entries) {
      if (entry is! Directory) continue;
      try {
        found.add(await readGameRun(await inspectGame(entry.path)));
      } on FormatException {/* A non-game folder is not a library entry. */}
    }
    return found;
  }
  final File file = await _libraryFile();
  if (!await file.exists()) return <Game>[];
  final Object? decoded = jsonDecode(await file.readAsString());
  if (decoded is! List) throw const FormatException('游戏库文件格式不正确。');
  final games = decoded.map((dynamic row) {
    final Map<String, dynamic> data = Map<String, dynamic>.from(row as Map);
    final String path = data['path'] as String;
    final String title = data['title'] as String;
    return Game(
        id: path,
        title: title,
        subtitle: data['subtitle'] as String? ?? '',
        path: path,
        sizeBytes: data['sizeBytes'] as int? ?? 0,
        coverColors: colorsForName(title),
        favorite: data['favorite'] == true,
        lastPlayed: DateTime.tryParse(data['lastPlayed'] as String? ?? ''),
        engine: EngineGeneration.values.firstWhere(
            (EngineGeneration g) => g.name == data['engine'],
            orElse: () => EngineGeneration.unknown));
  }).toList();
  return Future.wait(games.map(readGameRun));
}

Future<void> writeLibrary(List<Game> games) async {
  final File file = await _libraryFile();
  final File pending = File('${file.path}.tmp');
  await pending.writeAsString(
      jsonEncode(games
          .where((Game g) => !g.isDemo)
          .map((Game g) => <String, Object?>{
                'path': g.path,
                'title': g.title,
                'subtitle': g.subtitle,
                'sizeBytes': g.sizeBytes,
                'favorite': g.favorite,
                'lastPlayed': g.lastPlayed?.toIso8601String(),
                'engine': g.engine.name,
              })
          .toList()),
      flush: true);
  await pending.rename(file.path);
}

Future<bool> openHostFolder(String path) async {
  final String program = Platform.isMacOS
      ? '/usr/bin/open'
      : Platform.isWindows
          ? 'explorer.exe'
          : 'xdg-open';
  try {
    return (await Process.run(program, <String>[path])).exitCode == 0;
  } on ProcessException {
    return false;
  }
}

// Cache artifacts belong to the launcher, never to the game's resource/save directory.
String _runKey(String path) {
  int hash = 0x811c9dc5;
  final stablePath = fixedGamesDirectory ? 'Games/${p.basename(path)}' : path;
  for (final byte in utf8.encode(stablePath)) {
    hash = ((hash ^ byte) * 0x01000193) & 0xffffffff;
  }
  return hash.toRadixString(16);
}

Future<Directory> _runDirectory(Game game, [String? supportPath]) async {
  final support = supportPath ?? (await getApplicationSupportDirectory()).path;
  return Directory(p.join(support, 'runs', _runKey(game.path)))
      .create(recursive: true);
}

Future<Map<String, String>> prepareGameRun(Game game,
    {String? logPath, String? supportPath}) async {
  final directory = await _runDirectory(game, supportPath);
  final data = <String, String>{
    'path': game.path,
    'iconPath': p.join(directory.path, 'icon.png'),
    'logPath': logPath ??
        p.join(directory.path,
            'engine-${DateTime.now().microsecondsSinceEpoch}.log'),
    'lastPlayed': DateTime.now().toIso8601String()
  };
  await File(p.join(directory.path, 'latest.json'))
      .writeAsString(jsonEncode(data), flush: true);
  return data;
}

Future<Game> readGameRun(Game game, {String? supportPath}) async {
  final directory = await _runDirectory(game, supportPath);
  final file = File(p.join(directory.path, 'latest.json'));
  if (!await file.exists()) return game;
  try {
    final data = jsonDecode(await file.readAsString()) as Map;
    if (!fixedGamesDirectory && data['path'] != game.path) return game;
    final icon = fixedGamesDirectory
        ? p.join(directory.path, 'icon.png')
        : data['iconPath'] as String?;
    final savedLog = data['logPath'] as String?;
    final resolvedLog = fixedGamesDirectory && savedLog != null
        ? p.join(directory.path, p.basename(savedLog))
        : savedLog;
    return game.copyWith(
        lastPlayed: DateTime.tryParse(data['lastPlayed'] as String? ?? ''),
        iconPath: icon != null && await File(icon).exists() ? icon : null,
        lastLogPath: resolvedLog);
  } on FormatException {
    return game;
  }
}

Future<Uint8List?> readGameIcon(String? path) async {
  if (path == null) return null;
  final file = File(path);
  if (!await file.exists() || await file.length() > 4 * 1024 * 1024) {
    return null;
  }
  return file.readAsBytes();
}

Future<String> readLastGameLog(String? path) async {
  if (path == null) return '尚无运行日志';
  try {
    final input = await File(path).open();
    try {
      final length = await input.length();
      const limit = 128 * 1024;
      await input.setPosition(length > limit ? length - limit : 0);
      final text = utf8.decode(await input.read(limit), allowMalformed: true);
      return text.isEmpty
          ? '本次运行尚无日志输出'
          : '${length > limit ? '（仅显示最后 128 KB）\n' : ''}$text';
    } finally {
      await input.close();
    }
  } on FileSystemException {
    return '运行日志不存在或暂时无法读取';
  }
}

Future<AppSettings> readSettings() async {
  final file =
      File(p.join((await _libraryFile()).parent.path, 'settings.json'));
  if (!await file.exists()) return const AppSettings();
  final data = jsonDecode(await file.readAsString()) as Map;
  return AppSettings(
      renderer: RendererMode.values.firstWhere(
          (r) => r.name == data['renderer'],
          orElse: () => RendererMode.gpuFast),
      defaultFont: data['defaultFont'] as String?,
      defaultFontName: data['defaultFontName'] as String?);
}

Future<void> writeSettings(AppSettings settings) async {
  final file =
      File(p.join((await _libraryFile()).parent.path, 'settings.json'));
  await file.writeAsString(
      jsonEncode({
        'renderer': settings.renderer.name,
        'defaultFont': settings.defaultFont,
        'defaultFontName': settings.defaultFontName
      }),
      flush: true);
}
