import 'dart:async';

import 'package:flutter/material.dart';

import '../models/app_settings.dart';
import '../core/app_scope.dart';
import '../models/engine_session.dart';
import '../models/game.dart';
import '../services/engine_bridge_factory.dart';

/// The SDL window is the actual player. Flutter only shows process state.
class DesktopPlayerScreen extends StatefulWidget {
  const DesktopPlayerScreen(
      {super.key, required this.game, required this.renderer});
  final Game game;
  final RendererMode renderer;
  @override
  State<DesktopPlayerScreen> createState() => _DesktopPlayerScreenState();
}

class _DesktopPlayerScreenState extends State<DesktopPlayerScreen> {
  late final EngineSession _session;
  @override
  void initState() {
    super.initState();
    _session = EngineSession(
        game: widget.game,
        bridge: createEngineBridge(
            renderer: widget.renderer,
            defaultFont: AppScope.read(context).settings.defaultFont));
    _session.addListener(_changed);
    unawaited(_session.start());
  }

  void _changed() {
    if (mounted) {
      setState(() {});
      if (_session.status == EngineStatus.running ||
          _session.status == EngineStatus.exited ||
          _session.hasError) {
        unawaited(AppScope.of(context).refreshRun(widget.game.id));
      }
    }
  }

  @override
  void dispose() {
    _session.removeListener(_changed);
    _session.dispose();
    super.dispose();
  }

  Future<void> _leave() async {
    if (_session.status != EngineStatus.exited && !_session.hasError) {
      final bool? stop = await showDialog<bool>(
          context: context,
          builder: (BuildContext context) => AlertDialog(
                title: const Text('结束游戏？'),
                content: const Text('请先在游戏内存档，结束进程会丢失未保存的进度。'),
                actions: <Widget>[
                  TextButton(
                      onPressed: () => Navigator.pop(context, false),
                      child: const Text('继续游戏')),
                  FilledButton(
                      onPressed: () => Navigator.pop(context, true),
                      child: const Text('结束游戏')),
                ],
              ));
      if (stop != true) return;
      await _session.stop();
    }
    if (mounted) Navigator.pop(context);
  }

  @override
  Widget build(BuildContext context) => PopScope(
        canPop: false,
        onPopInvokedWithResult: (bool didPop, dynamic result) {
          if (!didPop) unawaited(_leave());
        },
        child: Scaffold(
          appBar: AppBar(
              title: Text(widget.game.title), automaticallyImplyLeading: false),
          body: Center(
              child: SingleChildScrollView(
                  padding: const EdgeInsets.all(32),
                  child: ConstrainedBox(
                    constraints: const BoxConstraints(maxWidth: 720),
                    child: Column(
                        mainAxisSize: MainAxisSize.min,
                        children: <Widget>[
                          Icon(
                              _session.hasError
                                  ? Icons.error_outline
                                  : Icons.sports_esports_outlined,
                              size: 64),
                          const SizedBox(height: 24),
                          Text(
                              switch (_session.status) {
                                EngineStatus.error => '启动或运行失败',
                                EngineStatus.exited => '游戏已结束',
                                EngineStatus.launching ||
                                EngineStatus.idle =>
                                  '正在启动引擎…',
                                _ => '游戏正在独立的 SDL2 窗口运行',
                              },
                              style: Theme.of(context).textTheme.headlineSmall,
                              textAlign: TextAlign.center),
                          const SizedBox(height: 16),
                          SelectableText(_session.error ?? widget.game.path),
                          if (_session.diagnostic != null)
                            Padding(
                                padding: const EdgeInsets.only(top: 12),
                                child: SelectableText(
                                    '日志：${_session.diagnostic}')),
                          const SizedBox(height: 24),
                          FilledButton(
                              onPressed: _leave,
                              child: Text(_session.hasError ||
                                      _session.status == EngineStatus.exited
                                  ? '返回游戏库'
                                  : '结束游戏并返回')),
                        ]),
                  ))),
        ),
      );
}
