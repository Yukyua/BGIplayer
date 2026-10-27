import 'package:flutter/material.dart';

import '../theme/app_theme.dart';

/// Static ambient background.  It intentionally has no repeating animation so
/// widget tests can settle immediately.
class AppBackground extends StatelessWidget {
  const AppBackground({super.key, required this.child});

  final Widget child;

  @override
  Widget build(BuildContext context) {
    return Stack(
      fit: StackFit.expand,
      children: <Widget>[
        const DecoratedBox(
          decoration: BoxDecoration(
            gradient: LinearGradient(
              begin: Alignment.topLeft,
              end: Alignment.bottomRight,
              colors: <Color>[
                Color(0xFF0A0E18),
                AppColors.background,
                Color(0xFF0C0910),
              ],
              stops: <double>[0, 0.55, 1],
            ),
          ),
        ),
        Positioned.fill(
          child: IgnorePointer(child: CustomPaint(painter: _AmbientPainter())),
        ),
        child,
      ],
    );
  }
}

class _AmbientPainter extends CustomPainter {
  @override
  void paint(Canvas canvas, Size size) {
    final Paint glow = Paint()
      ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 96);
    glow.shader = RadialGradient(
      colors: <Color>[AppColors.primary.withAlpha(32), Colors.transparent],
    ).createShader(
      Rect.fromCircle(
        center: Offset(size.width * 0.82, size.height * 0.08),
        radius: size.width * 0.42,
      ),
    );
    canvas.drawCircle(
      Offset(size.width * 0.82, size.height * 0.08),
      size.width * 0.42,
      glow,
    );

    final Paint warm = Paint()
      ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 110);
    warm.shader = RadialGradient(
      colors: <Color>[
        AppColors.secondary.withAlpha(18),
        Colors.transparent,
      ],
    ).createShader(
      Rect.fromCircle(
        center: Offset(size.width * 0.06, size.height * 0.9),
        radius: size.width * 0.34,
      ),
    );
    canvas.drawCircle(
      Offset(size.width * 0.06, size.height * 0.9),
      size.width * 0.34,
      warm,
    );

    final Paint grid = Paint()
      ..color = Colors.white.withAlpha(6)
      ..strokeWidth = 1;
    for (double x = 0; x < size.width; x += 52) {
      canvas.drawLine(Offset(x, 0), Offset(x, size.height), grid);
    }
    for (double y = 0; y < size.height; y += 52) {
      canvas.drawLine(Offset(0, y), Offset(size.width, y), grid);
    }
  }

  @override
  bool shouldRepaint(covariant _AmbientPainter oldDelegate) => false;
}
