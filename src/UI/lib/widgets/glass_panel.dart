import 'package:flutter/material.dart';

import '../theme/app_theme.dart';

class GlassPanel extends StatelessWidget {
  const GlassPanel({
    super.key,
    required this.child,
    this.padding = const EdgeInsets.all(20),
    this.radius = 24,
    this.color,
    this.borderColor,
    this.gradient,
    this.boxShadow = true,
  });

  final Widget child;
  final EdgeInsetsGeometry padding;
  final double radius;
  final Color? color;
  final Color? borderColor;
  final Gradient? gradient;
  final bool boxShadow;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: padding,
      decoration: BoxDecoration(
        color: gradient == null
            ? (color ?? AppColors.surface.withAlpha(232))
            : null,
        gradient: gradient,
        borderRadius: BorderRadius.circular(radius),
        border: Border.all(color: borderColor ?? AppColors.border),
        boxShadow: boxShadow
            ? <BoxShadow>[
                BoxShadow(
                  color: Colors.black.withAlpha(70),
                  blurRadius: 32,
                  offset: const Offset(0, 18),
                ),
              ]
            : null,
      ),
      child: child,
    );
  }
}
