import 'package:flutter/material.dart';
import '../models/game.dart';
import '../theme/app_theme.dart';
import '../utils/formatters.dart';
import 'game_icon.dart';

class GameCard extends StatelessWidget {
  const GameCard(
      {super.key,
      required this.game,
      required this.onPlay,
      required this.onDetails,
      required this.onToggleFavorite,
      required this.onDelete});
  final Game game;
  final VoidCallback onPlay, onDetails, onToggleFavorite, onDelete;
  @override
  Widget build(BuildContext context) => Card(
      color: AppColors.surface,
      clipBehavior: Clip.antiAlias,
      child: InkWell(
          onTap: onDetails,
          child: Padding(
              padding: const EdgeInsets.all(18),
              child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(children: [
                      GameIcon(path: game.iconPath),
                      const SizedBox(width: 14),
                      Expanded(
                          child: Text(game.title,
                              maxLines: 2,
                              overflow: TextOverflow.ellipsis,
                              style: Theme.of(context).textTheme.titleMedium)),
                      PopupMenuButton<String>(
                          tooltip: '更多',
                          onSelected: (value) {
                            if (value == 'favorite') onToggleFavorite();
                            if (value == 'delete') onDelete();
                          },
                          itemBuilder: (_) => [
                                PopupMenuItem(
                                    value: 'favorite',
                                    child: Text(game.favorite ? '取消收藏' : '收藏')),
                                const PopupMenuItem(
                                    value: 'delete', child: Text('从库中移除')),
                              ]),
                    ]),
                    const Spacer(),
                    Text(
                        '${formatBytes(game.sizeBytes)} · ${game.lastPlayed == null ? '尚未运行' : '上次运行 ${formatRelativeTime(game.lastPlayed)}'}',
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: Theme.of(context).textTheme.bodySmall),
                    const SizedBox(height: 14),
                    Align(
                        alignment: Alignment.centerRight,
                        child: FilledButton.icon(
                            onPressed: onPlay,
                            icon:
                                const Icon(Icons.play_arrow_rounded, size: 18),
                            label: const Text('启动'))),
                  ]))));
}
