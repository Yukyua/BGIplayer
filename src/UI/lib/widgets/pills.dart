import 'package:flutter/material.dart';

import '../models/game.dart';
import '../theme/app_theme.dart';

class Pill extends StatelessWidget {
  const Pill({
    super.key,
    required this.label,
    this.icon,
    this.color,
    this.foreground,
    this.compact = false,
  });

  final String label;
  final IconData? icon;
  final Color? color;
  final Color? foreground;
  final bool compact;

  @override
  Widget build(BuildContext context) {
    final Color background = color ?? AppColors.surfaceRaised;
    final Color content = foreground ?? AppColors.text;
    return Container(
      padding: EdgeInsets.symmetric(
        horizontal: compact ? 9 : 12,
        vertical: compact ? 5 : 7,
      ),
      decoration: BoxDecoration(
        color: background,
        borderRadius: BorderRadius.circular(999),
        border: Border.all(color: AppColors.border),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: <Widget>[
          if (icon != null) ...<Widget>[
            Icon(icon, size: compact ? 12 : 14, color: content),
            const SizedBox(width: 6),
          ],
          Text(
            label,
            style: TextStyle(
              color: content,
              fontSize: compact ? 10 : 11,
              fontWeight: FontWeight.w700,
              letterSpacing: 0.3,
            ),
          ),
        ],
      ),
    );
  }
}

class EngineBadge extends StatelessWidget {
  const EngineBadge({super.key, required this.generation});

  final EngineGeneration generation;

  @override
  Widget build(BuildContext context) {
    final (String label, Color color) = switch (generation) {
      EngineGeneration.pe32 => ('PE32', AppColors.secondary),
      EngineGeneration.pe64 => ('PE64', AppColors.success),
      EngineGeneration.unknown => ('ENGINE ?', AppColors.muted),
    };
    return Pill(
      label: label,
      color: color.withAlpha(26),
      foreground: color,
      compact: true,
    );
  }
}

class DemoBadge extends StatelessWidget {
  const DemoBadge({super.key, this.label = 'DEMO'});

  final String label;

  @override
  Widget build(BuildContext context) {
    return Pill(
      label: label,
      icon: Icons.science_outlined,
      color: AppColors.secondary.withAlpha(24),
      foreground: AppColors.secondary,
      compact: true,
    );
  }
}
