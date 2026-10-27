import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../core/app_controller.dart';
import '../core/app_scope.dart';
import '../models/game.dart';
import '../services/game_folder_picker.dart';
import '../theme/app_theme.dart';
import '../utils/formatters.dart';
import '../widgets/app_background.dart';
import '../widgets/game_icon.dart';
import '../services/host_service.dart';
import '../widgets/glass_panel.dart';
import '../widgets/pills.dart';
import 'player_screen.dart';

class GameDetailScreen extends StatelessWidget {
  const GameDetailScreen({super.key, required this.game});

  final Game game;

  void _play(BuildContext context, Game current) {
    final AppController controller = AppScope.of(context);
    controller.markPlayed(current.id);
    Navigator.of(context).push(
      PlayerScreen.route(
        game: current,
        usePlatformBridge: controller.settings.usePlatformBridge,
        renderer: controller.settings.renderer,
      ),
    );
  }

  Future<void> _openFolder(BuildContext context, Game current) async {
    final bool opened = await GameFolderPicker().openFolder(current.path);
    if (!context.mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(opened ? '已请求系统打开目录。' : '当前平台没有文件管理器桥接。')),
    );
  }

  Future<void> _copyPath(BuildContext context, Game current) async {
    await Clipboard.setData(ClipboardData(text: current.path));
    if (!context.mounted) return;
    ScaffoldMessenger.of(context)
        .showSnackBar(const SnackBar(content: Text('路径已复制。')));
  }

  Future<void> _delete(BuildContext context, Game current) async {
    final bool? confirmed = await showDialog<bool>(
      context: context,
      builder: (BuildContext context) => AlertDialog(
        backgroundColor: AppColors.surfaceRaised,
        title: const Text('从库中移除？'),
        content: Text('只会移除「${current.title}」的库记录，不会删除游戏文件。'),
        actions: <Widget>[
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: const Text('取消'),
          ),
          FilledButton(
            onPressed: () => Navigator.of(context).pop(true),
            child: const Text('移除'),
          ),
        ],
      ),
    );
    if (confirmed != true || !context.mounted) return;
    AppScope.of(context).removeGame(current.id);
    Navigator.of(context).pop();
  }

  @override
  Widget build(BuildContext context) {
    final AppController controller = AppScope.of(context);
    Game? current;
    for (final Game item in controller.games) {
      if (item.id == game.id) {
        current = item;
        break;
      }
    }
    if (current == null) {
      return Scaffold(
        backgroundColor: AppColors.background,
        appBar: AppBar(title: const Text('游戏详情')),
        body: const Center(child: Text('这个条目已经从库中移除。')),
      );
    }

    final TextTheme text = Theme.of(context).textTheme;
    final Game item = current;
    return Scaffold(
      backgroundColor: AppColors.background,
      body: AppBackground(
        child: SafeArea(
          bottom: false,
          child: CustomScrollView(
            slivers: <Widget>[
              SliverAppBar(
                pinned: true,
                backgroundColor: AppColors.background.withAlpha(224),
                surfaceTintColor: Colors.transparent,
                leading: IconButton(
                  tooltip: '返回',
                  onPressed: () => Navigator.of(context).pop(),
                  icon: const Icon(Icons.arrow_back_rounded),
                ),
                title: const Text('游戏详情'),
              ),
              SliverToBoxAdapter(
                child: Center(
                  child: ConstrainedBox(
                    constraints: const BoxConstraints(maxWidth: 980),
                    child: Padding(
                      padding: const EdgeInsets.fromLTRB(24, 8, 24, 44),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: <Widget>[
                          AspectRatio(
                            aspectRatio: 16 / 9,
                            child: ClipRRect(
                              borderRadius: BorderRadius.circular(28),
                              child: Stack(
                                fit: StackFit.expand,
                                children: <Widget>[
                                  Center(
                                      child: GameIcon(
                                          path: item.iconPath, size: 128)),
                                  Positioned(
                                    bottom: 16,
                                    left: 16,
                                    child: Wrap(
                                      spacing: 8,
                                      runSpacing: 8,
                                      children: <Widget>[
                                        EngineBadge(generation: item.engine),
                                        if (item.isDemo) const DemoBadge(),
                                        if (item.favorite)
                                          const Pill(
                                            label: '收藏',
                                            icon: Icons.star_rounded,
                                            color: Color(0x30F0C987),
                                            foreground: AppColors.secondary,
                                            compact: true,
                                          ),
                                      ],
                                    ),
                                  ),
                                ],
                              ),
                            ),
                          ),
                          const SizedBox(height: 24),
                          Row(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: <Widget>[
                              Expanded(
                                child: Column(
                                  crossAxisAlignment: CrossAxisAlignment.start,
                                  children: <Widget>[
                                    Text(
                                      item.title,
                                      style: text.headlineMedium,
                                    ),
                                    const SizedBox(height: 6),
                                    Text(
                                      item.subtitle,
                                      style: text.bodyMedium?.copyWith(
                                        color: AppColors.muted,
                                      ),
                                    ),
                                  ],
                                ),
                              ),
                              IconButton(
                                tooltip: item.favorite ? '取消收藏' : '收藏',
                                onPressed: () =>
                                    controller.toggleFavorite(item.id),
                                icon: Icon(
                                  item.favorite
                                      ? Icons.star_rounded
                                      : Icons.star_border_rounded,
                                  color: item.favorite
                                      ? AppColors.secondary
                                      : AppColors.muted,
                                ),
                              ),
                              PopupMenuButton<String>(
                                tooltip: '更多',
                                color: AppColors.surfaceRaised,
                                onSelected: (String value) {
                                  if (value == 'delete') {
                                    _delete(context, item);
                                  }
                                },
                                itemBuilder: (BuildContext context) =>
                                    <PopupMenuEntry<String>>[
                                  const PopupMenuItem<String>(
                                    value: 'delete',
                                    child: Row(
                                      children: <Widget>[
                                        Icon(Icons.delete_outline_rounded),
                                        SizedBox(width: 10),
                                        Text('从库中移除'),
                                      ],
                                    ),
                                  ),
                                ],
                              ),
                            ],
                          ),
                          const SizedBox(height: 16),
                          Wrap(
                            spacing: 8,
                            runSpacing: 8,
                            children: <Widget>[
                              Pill(
                                icon: Icons.sd_storage_rounded,
                                label: item.sizeBytes == 0
                                    ? '大小待扫描'
                                    : formatBytes(item.sizeBytes),
                              ),
                              Pill(
                                icon: Icons.history_rounded,
                                label: formatRelativeTime(item.lastPlayed),
                              ),
                              Pill(
                                icon: Icons.folder_open_rounded,
                                label: item.isDemo ? '内置场景' : '沙盒目录',
                              ),
                            ],
                          ),
                          const SizedBox(height: 24),
                          Wrap(
                            spacing: 12,
                            runSpacing: 12,
                            children: <Widget>[
                              FilledButton.icon(
                                onPressed: () => _play(context, item),
                                icon: const Icon(Icons.play_arrow_rounded),
                                label: const Text('启动游戏'),
                              ),
                              OutlinedButton.icon(
                                onPressed: () => _openFolder(context, item),
                                icon: const Icon(Icons.folder_open_rounded),
                                label: const Text('打开目录'),
                              ),
                              OutlinedButton.icon(
                                onPressed: () => _copyPath(context, item),
                                icon: const Icon(Icons.copy_rounded),
                                label: const Text('复制路径'),
                              ),
                            ],
                          ),
                          const SizedBox(height: 30),
                          GlassPanel(
                            padding: const EdgeInsets.all(16),
                            radius: 18,
                            boxShadow: false,
                            child: Row(
                              children: <Widget>[
                                const Icon(
                                  Icons.route_rounded,
                                  size: 18,
                                  color: AppColors.primary,
                                ),
                                const SizedBox(width: 12),
                                Expanded(
                                  child: Text(
                                    item.path,
                                    maxLines: 2,
                                    overflow: TextOverflow.ellipsis,
                                    style: text.bodySmall?.copyWith(
                                      color: AppColors.muted,
                                      height: 1.5,
                                    ),
                                  ),
                                ),
                              ],
                            ),
                          ),
                          const SizedBox(height: 28),
                          Text('技术信息', style: text.titleMedium),
                          const SizedBox(height: 12),
                          Wrap(
                            spacing: 12,
                            runSpacing: 12,
                            children: <Widget>[
                              _InfoTile(
                                label: '引擎代际',
                                value: item.engine.label,
                              ),
                              _InfoTile(
                                label: '运行方式',
                                value: item.isDemo
                                    ? 'Flutter 演示场景'
                                    : 'SDL2 + C++ 引擎',
                              ),
                              _InfoTile(
                                label: '存档目录',
                                value: item.isDemo ? '无' : 'game/UserData',
                              ),
                              const _InfoTile(
                                  label: '导入策略', value: '复制到 App 沙盒'),
                            ],
                          ),
                          const SizedBox(height: 28),
                          Text('上次运行日志', style: text.titleMedium),
                          const SizedBox(height: 12),
                          _LastRunLog(path: item.lastLogPath),
                        ],
                      ),
                    ),
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _InfoTile extends StatelessWidget {
  const _InfoTile({required this.label, required this.value});

  final String label;
  final String value;

  @override
  Widget build(BuildContext context) {
    final TextTheme text = Theme.of(context).textTheme;
    return SizedBox(
      width: 210,
      child: GlassPanel(
        padding: const EdgeInsets.all(14),
        radius: 16,
        boxShadow: false,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Text(
              label,
              style: text.labelSmall?.copyWith(color: AppColors.muted),
            ),
            const SizedBox(height: 5),
            Text(
              value,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: text.bodyMedium,
            ),
          ],
        ),
      ),
    );
  }
}

class _LastRunLog extends StatefulWidget {
  const _LastRunLog({this.path});
  final String? path;
  @override
  State<_LastRunLog> createState() => _LastRunLogState();
}

class _LastRunLogState extends State<_LastRunLog> {
  late Future<String> _log;
  @override
  void initState() {
    super.initState();
    _log = readLastGameLog(widget.path);
  }

  @override
  void didUpdateWidget(_LastRunLog oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.path != widget.path) _log = readLastGameLog(widget.path);
  }

  @override
  Widget build(BuildContext context) => GlassPanel(
      radius: 18,
      child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        Align(
            alignment: Alignment.centerRight,
            child: IconButton(
                tooltip: '刷新日志',
                onPressed: () =>
                    setState(() => _log = readLastGameLog(widget.path)),
                icon: const Icon(Icons.refresh))),
        FutureBuilder<String>(
            future: _log,
            builder: (context, snapshot) => ConstrainedBox(
                constraints: const BoxConstraints(maxHeight: 360),
                child: SingleChildScrollView(
                    child: SelectableText(snapshot.data ?? '读取中…',
                        style: const TextStyle(
                            fontFamily: 'monospace', fontSize: 12))))),
      ]));
}
