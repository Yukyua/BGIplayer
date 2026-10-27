import 'dart:math';

import 'package:flutter/material.dart';

import '../utils/formatters.dart';

/// Abstract, asset-free cover art.  The real app can replace this with a
/// cached title bitmap extracted from the game archive.
class CoverArt extends StatelessWidget {
  const CoverArt({
    super.key,
    required this.title,
    required this.colors,
    required this.seed,
    this.showTitle = true,
  });

  final String title;
  final List<Color> colors;
  final int seed;
  final bool showTitle;

  @override
  Widget build(BuildContext context) {
    final List<Color> palette = colors.length >= 2
        ? colors
        : <Color>[const Color(0xFF404A68), const Color(0xFF171D2A)];
    return DecoratedBox(
      decoration: BoxDecoration(
        gradient: LinearGradient(
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
          colors: palette,
        ),
      ),
      child: Stack(
        fit: StackFit.expand,
        children: <Widget>[
          CustomPaint(painter: _CoverPainter(seed: seed)),
          if (showTitle)
            Align(
              alignment: Alignment.bottomLeft,
              child: Padding(
                padding: const EdgeInsets.all(16),
                child: Text(
                  initials(title),
                  style: TextStyle(
                    fontSize: 56,
                    height: 0.9,
                    fontWeight: FontWeight.w800,
                    color: Colors.white.withAlpha(30),
                  ),
                ),
              ),
            ),
          Positioned(
            top: 14,
            left: 14,
            child: Container(
              padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 5),
              decoration: BoxDecoration(
                color: Colors.black.withAlpha(90),
                borderRadius: BorderRadius.circular(999),
                border: Border.all(color: Colors.white.withAlpha(28)),
              ),
              child: const Text(
                'BGI',
                style: TextStyle(
                  color: Colors.white,
                  fontSize: 10,
                  fontWeight: FontWeight.w800,
                  letterSpacing: 1.2,
                ),
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _CoverPainter extends CustomPainter {
  _CoverPainter({required this.seed});

  final int seed;

  @override
  void paint(Canvas canvas, Size size) {
    final Random random = Random(seed & 0x7FFFFFFF);
    final Offset moon = Offset(size.width * 0.72, size.height * 0.28);

    final Paint glow = Paint()
      ..color = Colors.white.withAlpha(38)
      ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 28);
    canvas.drawCircle(moon, size.width * 0.17, glow);
    canvas.drawCircle(
      moon,
      size.width * 0.072,
      Paint()..color = Colors.white.withAlpha(214),
    );

    final Paint star = Paint()..color = Colors.white.withAlpha(120);
    for (int index = 0; index < 30; index++) {
      canvas.drawCircle(
        Offset(
          random.nextDouble() * size.width,
          random.nextDouble() * size.height * 0.62,
        ),
        random.nextDouble() * 1.4 + 0.4,
        star,
      );
    }

    final Paint line = Paint()
      ..color = Colors.white.withAlpha(34)
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.2;
    for (int index = 0; index < 4; index++) {
      final double y = size.height * (0.48 + index * 0.12);
      final Path path = Path()
        ..moveTo(0, y)
        ..quadraticBezierTo(
          size.width * 0.5,
          y - 20 - index * 8,
          size.width,
          y + 6,
        );
      canvas.drawPath(path, line);
    }

    final Path horizon = Path()
      ..moveTo(0, size.height * 0.78)
      ..lineTo(size.width * 0.18, size.height * 0.68)
      ..lineTo(size.width * 0.34, size.height * 0.8)
      ..lineTo(size.width * 0.52, size.height * 0.7)
      ..lineTo(size.width * 0.7, size.height * 0.8)
      ..lineTo(size.width, size.height * 0.72)
      ..lineTo(size.width, size.height)
      ..lineTo(0, size.height)
      ..close();
    canvas.drawPath(horizon, Paint()..color = Colors.black.withAlpha(70));
  }

  @override
  bool shouldRepaint(covariant _CoverPainter oldDelegate) =>
      oldDelegate.seed != seed;
}
