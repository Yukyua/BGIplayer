import 'package:flutter/material.dart';

import '../theme/app_theme.dart';
import '../services/host_service.dart';

class EmptyLibrary extends StatelessWidget {
  const EmptyLibrary({
    super.key,
    required this.onImport,
    this.hasQuery = false,
  });

  final VoidCallback onImport;
  final bool hasQuery;

  @override
  Widget build(BuildContext context) {
    final TextTheme text = Theme.of(context).textTheme;
    return Center(
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 460),
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 40),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              SizedBox(
                width: 108,
                height: 108,
                child: Stack(
                  alignment: Alignment.center,
                  children: <Widget>[
                    Container(
                      width: 96,
                      height: 96,
                      decoration: BoxDecoration(
                        shape: BoxShape.circle,
                        gradient: LinearGradient(
                          begin: Alignment.topLeft,
                          end: Alignment.bottomRight,
                          colors: <Color>[
                            AppColors.primary.withAlpha(42),
                            AppColors.secondary.withAlpha(20),
                          ],
                        ),
                        border: Border.all(color: AppColors.border),
                      ),
                    ),
                    const Icon(
                      Icons.collections_bookmark_outlined,
                      size: 40,
                      color: AppColors.primary,
                    ),
                    Positioned(
                      right: 0,
                      top: 14,
                      child: Container(
                        padding: const EdgeInsets.all(7),
                        decoration: const BoxDecoration(
                          color: AppColors.secondary,
                          shape: BoxShape.circle,
                        ),
                        child: const Icon(
                          Icons.add_rounded,
                          size: 14,
                          color: Color(0xFF1A1205),
                        ),
                      ),
                    ),
                  ],
                ),
              ),
              const SizedBox(height: 24),
              Text(
                hasQuery ? '没有匹配的游戏' : '游戏库还是空的',
                style: text.headlineSmall,
                textAlign: TextAlign.center,
              ),
              const SizedBox(height: 10),
              Text(
                hasQuery
                    ? '换个关键词，或者清除筛选条件再试试。'
                    : fixedGamesDirectory
                        ? '请通过“文件”将游戏放入 BGIplayer/Games，每个游戏使用独立子目录，然后重新打开应用。'
                        : '导入游戏目录，添加到游戏库。',
                textAlign: TextAlign.center,
                style: text.bodyMedium?.copyWith(color: AppColors.muted),
              ),
              const SizedBox(height: 24),
              if (!hasQuery && !fixedGamesDirectory)
                FilledButton.icon(
                  onPressed: onImport,
                  icon: const Icon(Icons.add_rounded),
                  label: const Text('导入第一个游戏'),
                ),
            ],
          ),
        ),
      ),
    );
  }
}
