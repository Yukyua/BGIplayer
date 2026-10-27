import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

import '../models/app_settings.dart';
import '../models/game.dart';
import 'engine_bridge.dart';
import 'host_service.dart';

EngineBridge createEngineBridge(
        {required RendererMode renderer, String? defaultFont}) =>
    Platform.isMacOS || Platform.isLinux || Platform.isWindows
        ? DesktopEngineBridge(renderer: renderer, defaultFont: defaultFont)
        : PlatformEngineBridge(renderer: renderer, defaultFont: defaultFont);

/// Launch only our native host, passing game paths as argument-array elements.
/// The engine owns its SDL window, input, scheduling, rendering and saves.
class DesktopEngineBridge implements EngineBridge {
  DesktopEngineBridge(
      {this.renderer = RendererMode.gpuFast,
      this.executable,
      this.defaultFont,
      this.supportDirectory});
  final RendererMode renderer;
  final String? defaultFont;
  final String? executable;
  final Directory? supportDirectory;
  final StreamController<EngineEvent> _events =
      StreamController<EngineEvent>.broadcast();
  Process? _process;
  Future<void>? _completion;
  bool _disposed = false;
  bool _stopping = false;
  bool _launching = false;
  bool _paused = false;
  String? logPath;

  @override
  Stream<EngineEvent> get events => _events.stream;

  Future<String> _findExecutable() async {
    final String? override =
        executable ?? Platform.environment['BGIPLAYER_ENGINE'];
    if (override != null && override.isNotEmpty) {
      if (await File(override).exists()) return p.absolute(override);
      throw EngineBridgeUnavailable('找不到指定的 BGIplayer 引擎：$override');
    }
    final String name = Platform.isWindows ? 'bgiplayer.exe' : 'bgiplayer';
    final Directory binaryDirectory = File(Platform.resolvedExecutable).parent;
    final List<String> candidates = <String>[
      p.join(binaryDirectory.parent.path, 'Resources', 'engine', name),
      p.join(binaryDirectory.path, 'engine', name),
    ];
    // flutter run starts in src/UI. Also supports starting from repository root.
    Directory directory = Directory.current;
    for (int i = 0; i < 5; i++) {
      candidates.add(p.join(directory.path, 'build', name));
      directory = directory.parent;
    }
    for (final String candidate in candidates) {
      if (await File(candidate).exists()) {
        return p.normalize(p.absolute(candidate));
      }
    }
    throw const EngineBridgeUnavailable(
        '找不到 BGIplayer 引擎。请先在仓库运行 make，或设置 BGIPLAYER_ENGINE 为引擎绝对路径。');
  }

  @override
  Future<void> launch(Game game) async {
    if (_disposed || _launching || _process != null) {
      throw StateError('引擎已启动或正在启动。');
    }
    _launching = true;
    _stopping = false;
    _emit(EngineEventType.launching);
    IOSink? log;
    try {
      if (!await Directory(game.path).exists()) {
        throw const EngineBridgeUnavailable('游戏目录不存在，请重新导入。');
      }
      final String engine = await _findExecutable();
      final Directory support =
          supportDirectory ?? await getApplicationSupportDirectory();
      final Directory logs =
          await Directory(p.join(support.path, 'logs')).create(recursive: true);
      logPath = p.join(
          logs.path, 'engine-${DateTime.now().microsecondsSinceEpoch}.log');
      final run = await prepareGameRun(game,
          logPath: logPath, supportPath: support.path);
      final IOSink activeLog = File(logPath!).openWrite();
      log = activeLog;
      final Process process = await Process.start(
          engine,
          <String>[
            '--game',
            game.path,
            '--backend',
            'sdl2',
            if (defaultFont != null) ...['--default-font', defaultFont!]
          ],
          workingDirectory: File(engine).parent.path,
          environment: <String, String>{
            'BGI_GAME_ICON_OUTPUT': run['iconPath']!,
            'BGI_GPU': renderer == RendererMode.cpuExact ? '0' : '1',
            'BGI_RENDER_MODE':
                renderer == RendererMode.gpuFast ? 'fast' : 'exact'
          });
      if (_disposed) {
        process.kill();
      }
      _process = process;
      final List<String> tail = <String>[];
      Future<void> drain(Stream<List<int>> stream) async {
        await for (final String line in stream
            .transform(const Utf8Decoder(allowMalformed: true))
            .transform(const LineSplitter())) {
          activeLog.writeln(line);
          tail.add(line);
          if (tail.length > 24) tail.removeAt(0);
        }
      }

      final Future<void> stdout = drain(process.stdout);
      final Future<void> stderr = drain(process.stderr);
      _completion = () async {
        final int code = await process.exitCode;
        await Future.wait(<Future<void>>[stdout, stderr]);
        await activeLog.flush();
        await activeLog.close();
        if (identical(_process, process)) _process = null;
        if (code == 0 || _stopping) {
          _emit(EngineEventType.exited);
        } else {
          _emit(EngineEventType.error,
              '引擎退出（$code）。日志：$logPath\n${tail.join('\n')}');
        }
      }();
      _emit(EngineEventType.ready, logPath);
    } on Object {
      if (_process == null) await log?.close();
      rethrow;
    } finally {
      _launching = false;
    }
  }

  @override
  Future<void> pause() async {
    if (_process == null || _paused) return;
    if (Platform.isWindows) {
      throw const EngineBridgeUnavailable('此平台尚未接入引擎暂停接口。');
    }
    if (!_process!.kill(ProcessSignal.sigstop)) throw StateError('无法暂停引擎。');
    _paused = true;
    _emit(EngineEventType.paused);
  }

  @override
  Future<void> resume() async {
    if (_process == null || !_paused) return;
    if (!_process!.kill(ProcessSignal.sigcont)) throw StateError('无法继续引擎。');
    _paused = false;
    _emit(EngineEventType.resumed);
  }

  @override
  Future<void> stop() async {
    _stopping = true;
    if (_paused) await resume();
    _process?.kill();
    final Future<void>? completion = _completion;
    if (completion != null) {
      try {
        await completion.timeout(const Duration(seconds: 3));
      } on TimeoutException {
        _process?.kill(ProcessSignal.sigkill);
        await completion;
      }
    }
  }

  @override
  Future<void> dispose() async {
    _disposed = true;
    await stop();
    await _events.close();
  }

  void _emit(EngineEventType type, [String? message]) {
    if (!_disposed) _events.add(EngineEvent(type, message));
  }
}
