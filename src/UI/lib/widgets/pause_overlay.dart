import 'dart:ui' show ImageFilter;

import 'package:flutter/material.dart';

import '../theme/app_theme.dart';
import 'glass_panel.dart';
import 'pills.dart';

class PauseOverlay extends StatelessWidget {
  const PauseOverlay({
    super.key,
    required this.onResume,
    required this.onSettings,
    required this.onExit,
  });

  final VoidCallback onResume;
  final VoidCallback onSettings;
  final VoidCallback onExit;

  @override
  Widget build(BuildContext context) {
    final TextTheme text = Theme.of(context).textTheme;
    return Positioned.fill(
      child: ClipRect(
        child: BackdropFilter(
          filter: ImageFilter.blur(sigmaX: 18, sigmaY: 18),
          child: ColoredBox(
            color: Colors.black.withAlpha(148),
            child: SafeArea(
              child: Center(
                child: ConstrainedBox(
                  constraints: const BoxConstraints(maxWidth: 380),
                  child: GlassPanel(
                    padding: const EdgeInsets.fromLTRB(24, 26, 24, 22),
                    radius: 28,
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      children: <Widget>[
                        Container(
                          width: 54,
                          height: 54,
                          decoration: BoxDecoration(
                            color: AppColors.primary.withAlpha(26),
                            shape: BoxShape.circle,
                            border: Border.all(color: AppColors.border),
                          ),
                          child: const Icon(
                            Icons.pause_rounded,
                            color: AppColors.primary,
                            size: 26,
                          ),
                        ),
                        const SizedBox(height: 16),
                        Text('暂停', style: text.headlineSmall),
                        const SizedBox(height: 6),
                        Text(
                          '引擎已挂起，音频已暂停。',
                          style: text.bodySmall?.copyWith(
                            color: AppColors.muted,
                          ),
                        ),
                        const SizedBox(height: 18),
                        const DemoBadge(label: 'FLUTTER UI DEMO'),
                        const SizedBox(height: 22),
                        SizedBox(
                          width: double.infinity,
                          child: FilledButton.icon(
                            onPressed: onResume,
                            icon: const Icon(Icons.play_arrow_rounded),
                            label: const Text('继续游戏'),
                          ),
                        ),
                        const SizedBox(height: 10),
                        SizedBox(
                          width: double.infinity,
                          child: OutlinedButton.icon(
                            onPressed: onSettings,
                            icon: const Icon(Icons.tune_rounded),
                            label: const Text('Host 设置'),
                          ),
                        ),
                        const SizedBox(height: 4),
                        TextButton.icon(
                          onPressed: onExit,
                          icon: const Icon(Icons.logout_rounded, size: 18),
                          label: const Text('退出游戏'),
                          style: TextButton.styleFrom(
                            foregroundColor: AppColors.danger,
                          ),
                        ),
                      ],
                    ),
                  ),
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}
