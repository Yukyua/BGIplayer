import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';

import '../core/app_controller.dart';
import '../core/app_scope.dart';
import '../models/game.dart';
import '../services/host_service.dart';
import '../theme/app_theme.dart';
import '../utils/formatters.dart';
import '../widgets/app_background.dart';
import '../widgets/empty_library.dart';
import '../widgets/game_card.dart';
import '../widgets/glass_panel.dart';
import '../widgets/import_sheet.dart';
import 'game_detail_screen.dart';
import 'player_screen.dart';
import 'settings_screen.dart';

enum _LibraryFilter { all, recent, favorites }

extension on _LibraryFilter {
  String get label => switch (this) {
        _LibraryFilter.all => '全部',
        _LibraryFilter.recent => '最近',
        _LibraryFilter.favorites => '收藏',
      };
}

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  final TextEditingController _search = TextEditingController();
  _LibraryFilter _filter = _LibraryFilter.all;
  String _query = '';

  @override
  void dispose() {
    _search.dispose();
    super.dispose();
  }

  List<Game> _filteredGames(AppController controller) {
    final String query = _query.trim().toLowerCase();
    final List<Game> result = controller.games.where((Game game) {
      final bool matches = query.isEmpty ||
          game.title.toLowerCase().contains(query) ||
          game.subtitle.toLowerCase().contains(query) ||
          game.path.toLowerCase().contains(query);
      if (!matches) return false;
      if (_filter == _LibraryFilter.favorites) return game.favorite;
      return true;
    }).toList();

    if (_filter == _LibraryFilter.recent) {
      result.sort((Game a, Game b) {
        final DateTime? left = a.lastPlayed;
        final DateTime? right = b.lastPlayed;
        if (left == null && right == null) return 0;
        if (left == null) return 1;
        if (right == null) return -1;
        return right.compareTo(left);
      });
    }
    return result;
  }

  Future<void> _import() async {
    final String? path = await showImportGameSheet(context);
    if (!mounted || path == null) return;
    final AppController controller = AppScope.of(context);
    try {
      final Game? game = await controller.importGame(path);
      if (mounted) {
        _snack(game == null ? '这个目录已经在游戏库里了。' : '已导入「${game.title}」。');
      }
    } on Object catch (error) {
      if (mounted) _snack('导入失败：$error');
    }
  }

  void _play(Game game) {
    final AppController controller = AppScope.of(context);
    controller.markPlayed(game.id);
    Navigator.of(context).push(
      PlayerScreen.route(
        game: game,
        usePlatformBridge: controller.settings.usePlatformBridge,
        renderer: controller.settings.renderer,
      ),
    );
  }

  void _openDetails(Game game) {
    Navigator.of(context).push(
      MaterialPageRoute<void>(
        builder: (BuildContext context) => GameDetailScreen(game: game),
      ),
    );
  }

  Future<void> _delete(Game game) async {
    final bool? confirmed = await showDialog<bool>(
      context: context,
      builder: (BuildContext context) => AlertDialog(
        backgroundColor: AppColors.surfaceRaised,
        title: const Text('从库中移除？'),
        content: Text(
          '只会移除「${game.title}」的库记录。'
          '不会删除游戏文件。',
        ),
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
    if (confirmed != true || !mounted) return;
    AppScope.of(context).removeGame(game.id);
    _snack('已从库中移除「${game.title}」。');
  }

  void _settings() {
    Navigator.of(context).push(
      MaterialPageRoute<void>(
        builder: (BuildContext context) => const SettingsScreen(),
      ),
    );
  }

  void _snack(String message) {
    final ScaffoldMessengerState messenger = ScaffoldMessenger.of(context);
    messenger
      ..hideCurrentSnackBar()
      ..showSnackBar(SnackBar(content: Text(message)));
  }

  @override
  Widget build(BuildContext context) {
    final AppController controller = AppScope.of(context);
    final List<Game> games = _filteredGames(controller);
    final bool filtering =
        _query.trim().isNotEmpty || _filter != _LibraryFilter.all;

    return Scaffold(
      backgroundColor: AppColors.background,
      body: AppBackground(
        child: SafeArea(
          bottom: false,
          child: CustomScrollView(
            slivers: <Widget>[
              SliverToBoxAdapter(
                child: _Toolbar(
                  search: _search,
                  onSettings: _settings,
                  query: _query,
                  filter: _filter,
                  count: games.length,
                  onQueryChanged: (String value) {
                    setState(() => _query = value);
                  },
                  onClearQuery: () {
                    _search.clear();
                    setState(() => _query = '');
                  },
                  onFilterChanged: (_LibraryFilter value) {
                    setState(() => _filter = value);
                  },
                  onImport: _import,
                ),
              ),
              if (controller.importing)
                SliverToBoxAdapter(
                  child: _ImportProgress(
                    path: controller.importingPath ?? '',
                    progress: controller.importProgress,
                  ),
                ),
              if (games.isEmpty)
                SliverToBoxAdapter(
                  child: EmptyLibrary(onImport: _import, hasQuery: filtering),
                )
              else
                SliverLayoutBuilder(
                  builder:
                      (BuildContext context, SliverConstraints constraints) {
                    final double horizontal = math.max(
                      24.0,
                      (constraints.crossAxisExtent - 1180) / 2,
                    );
                    return SliverPadding(
                      padding: EdgeInsets.fromLTRB(
                        horizontal,
                        22,
                        horizontal,
                        0,
                      ),
                      sliver: SliverGrid(
                        gridDelegate:
                            const SliverGridDelegateWithMaxCrossAxisExtent(
                          maxCrossAxisExtent: 390,
                          mainAxisExtent: 210,
                          crossAxisSpacing: 18,
                          mainAxisSpacing: 18,
                        ),
                        delegate: SliverChildBuilderDelegate((
                          BuildContext context,
                          int index,
                        ) {
                          final Game game = games[index];
                          return GameCard(
                            game: game,
                            onPlay: () => _play(game),
                            onDetails: () => _openDetails(game),
                            onToggleFavorite: () =>
                                AppScope.of(context).toggleFavorite(game.id),
                            onDelete: () => _delete(game),
                          );
                        }, childCount: games.length),
                      ),
                    );
                  },
                ),
              if (controller.libraryWarning != null)
                SliverToBoxAdapter(
                    child: Padding(
                        padding: const EdgeInsets.all(24),
                        child: Text(controller.libraryWarning!,
                            style: const TextStyle(color: AppColors.danger)))),
              const SliverToBoxAdapter(child: SizedBox(height: 44)),
            ],
          ),
        ),
      ),
    );
  }
}

class _Toolbar extends StatelessWidget {
  const _Toolbar({
    required this.search,
    required this.onSettings,
    required this.query,
    required this.filter,
    required this.count,
    required this.onQueryChanged,
    required this.onClearQuery,
    required this.onFilterChanged,
    required this.onImport,
  });

  final TextEditingController search;
  final VoidCallback onSettings;
  final String query;
  final _LibraryFilter filter;
  final int count;
  final ValueChanged<String> onQueryChanged;
  final VoidCallback onClearQuery;
  final ValueChanged<_LibraryFilter> onFilterChanged;
  final VoidCallback onImport;

  @override
  Widget build(BuildContext context) {
    final TextTheme text = Theme.of(context).textTheme;
    return Padding(
      padding: const EdgeInsets.fromLTRB(24, 28, 24, 0),
      child: Center(
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 1180),
          child: LayoutBuilder(
            builder: (BuildContext context, BoxConstraints constraints) {
              final bool compact = constraints.maxWidth < 680;
              final Widget searchField = TextField(
                controller: search,
                onChanged: onQueryChanged,
                textInputAction: TextInputAction.search,
                decoration: InputDecoration(
                  hintText: '搜索',
                  prefixIcon: const Icon(Icons.search_rounded),
                  suffixIcon: query.isEmpty
                      ? null
                      : IconButton(
                          tooltip: '清除搜索',
                          onPressed: onClearQuery,
                          icon: const Icon(Icons.close_rounded, size: 18),
                        ),
                ),
              );
              final Widget importButton = FilledButton.icon(
                onPressed: onImport,
                icon: const Icon(Icons.add_rounded),
                label: const Text('导入游戏目录'),
              );

              return Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: <Widget>[
                  if (compact)
                    Column(
                      children: <Widget>[
                        Row(children: [
                          Expanded(child: searchField),
                          const SizedBox(width: 12),
                          IconButton(
                              tooltip: '设置',
                              onPressed: onSettings,
                              icon: const Icon(Icons.tune_rounded))
                        ]),
                        if (!fixedGamesDirectory) const SizedBox(height: 12),
                        if (!fixedGamesDirectory)
                          SizedBox(width: double.infinity, child: importButton),
                      ],
                    )
                  else
                    Row(
                      children: <Widget>[
                        Expanded(child: searchField),
                        if (!fixedGamesDirectory) const SizedBox(width: 12),
                        if (!fixedGamesDirectory) importButton,
                        const SizedBox(width: 12),
                        IconButton(
                            tooltip: '设置',
                            onPressed: onSettings,
                            icon: const Icon(Icons.tune_rounded)),
                      ],
                    ),
                  const SizedBox(height: 18),
                  Row(
                    children: <Widget>[
                      Expanded(
                        child: Wrap(
                          spacing: 8,
                          runSpacing: 8,
                          children: _LibraryFilter.values
                              .map(
                                (_LibraryFilter item) => ChoiceChip(
                                  label: Text(item.label),
                                  selected: filter == item,
                                  showCheckmark: false,
                                  onSelected: (_) => onFilterChanged(item),
                                ),
                              )
                              .toList(),
                        ),
                      ),
                      const SizedBox(width: 12),
                      Text(
                        '$count 个条目',
                        style: text.bodySmall?.copyWith(color: AppColors.muted),
                      ),
                    ],
                  ),
                ],
              );
            },
          ),
        ),
      ),
    );
  }
}

class _ImportProgress extends StatelessWidget {
  const _ImportProgress({required this.path, required this.progress});

  final String path;
  final double progress;

  @override
  Widget build(BuildContext context) {
    final TextTheme text = Theme.of(context).textTheme;
    return Padding(
      padding: const EdgeInsets.fromLTRB(24, 18, 24, 0),
      child: Center(
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 1180),
          child: GlassPanel(
            padding: const EdgeInsets.all(16),
            radius: 18,
            boxShadow: false,
            child: Row(
              children: <Widget>[
                const SizedBox(
                  width: 20,
                  height: 20,
                  child: CircularProgressIndicator(strokeWidth: 2),
                ),
                const SizedBox(width: 14),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[
                      Text(
                        '正在导入 ${basename(path)}',
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: text.titleSmall,
                      ),
                      const SizedBox(height: 8),
                      ClipRRect(
                        borderRadius: BorderRadius.circular(999),
                        child: LinearProgressIndicator(
                          value: progress > 0 ? progress : null,
                          minHeight: 5,
                          backgroundColor: Colors.white.withAlpha(16),
                        ),
                      ),
                    ],
                  ),
                ),
                const SizedBox(width: 14),
                Text(
                  progress > 0 ? '${(progress * 100).round()}%' : '扫描中',
                  style: text.labelMedium?.copyWith(color: AppColors.primary),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
