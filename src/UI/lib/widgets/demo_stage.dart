import 'dart:async';
import 'dart:math';

import 'package:flutter/material.dart';

import '../theme/app_theme.dart';

class _DialogueLine {
  const _DialogueLine(this.name, this.text);

  final String name;
  final String text;
}

const List<_DialogueLine> _lines = <_DialogueLine>[
  _DialogueLine('ナナ', 'ここから、新しい物語が始まる。'),
  _DialogueLine('ナナ', '画面をタップすると、次のメッセージへ進むよ。'),
  _DialogueLine('デモ', 'この画面は Flutter 製の UI デモ。実際のゲームは SDL2 が描画する。'),
];

/// Asset-free visual-novel stage.  It is the stand-in for the SDL2 surface
/// until the native engine bridge is wired up.
class DemoStage extends StatefulWidget {
  const DemoStage({super.key, this.showTouchHints = true});

  final bool showTouchHints;

  @override
  State<DemoStage> createState() => _DemoStageState();
}

class _DemoStageState extends State<DemoStage> {
  int _index = 0;
  int _visible = 0;
  Timer? _timer;

  _DialogueLine get _line => _lines[_index];

  @override
  void initState() {
    super.initState();
    _startTyping();
  }

  void _startTyping() {
    _timer?.cancel();
    _timer = Timer.periodic(const Duration(milliseconds: 30), (Timer timer) {
      if (!mounted) {
        timer.cancel();
        return;
      }
      if (_visible >= _line.text.length) {
        timer.cancel();
        return;
      }
      setState(() => _visible++);
    });
  }

  void _advance() {
    if (_visible < _line.text.length) {
      _timer?.cancel();
      setState(() => _visible = _line.text.length);
      return;
    }
    setState(() {
      _index = (_index + 1) % _lines.length;
      _visible = 0;
    });
    _startTyping();
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (BuildContext context, BoxConstraints constraints) {
        final double width = constraints.maxWidth;
        final bool compact = width < 700;
        final double fontSize = (width * 0.018).clamp(13.0, 23.0).toDouble();
        final String text = _line.text.substring(
          0,
          _visible.clamp(0, _line.text.length).toInt(),
        );
        final bool complete = _visible >= _line.text.length;

        return GestureDetector(
          behavior: HitTestBehavior.opaque,
          onTap: _advance,
          child: Stack(
            fit: StackFit.expand,
            children: <Widget>[
              CustomPaint(painter: _ScenePainter()),
              Positioned(
                top: compact ? 12 : 18,
                left: compact ? 14 : 20,
                child: _SceneTag(label: 'SCENE 01 · NIGHT', compact: compact),
              ),
              Positioned(
                top: compact ? 12 : 18,
                right: compact ? 14 : 20,
                child: _SceneTag(
                  label: 'DEMO · ${width.round()}×${(width * 9 / 16).round()}',
                  compact: compact,
                ),
              ),
              Align(
                alignment: Alignment.bottomCenter,
                child: _DialogueBox(
                  name: _line.name,
                  text: text,
                  complete: complete,
                  index: _index,
                  total: _lines.length,
                  fontSize: fontSize,
                  compact: compact,
                  showHint: widget.showTouchHints,
                ),
              ),
            ],
          ),
        );
      },
    );
  }
}

class _SceneTag extends StatelessWidget {
  const _SceneTag({required this.label, required this.compact});

  final String label;
  final bool compact;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: EdgeInsets.symmetric(
        horizontal: compact ? 8 : 10,
        vertical: compact ? 4 : 5,
      ),
      decoration: BoxDecoration(
        color: Colors.black.withAlpha(90),
        borderRadius: BorderRadius.circular(999),
        border: Border.all(color: Colors.white.withAlpha(28)),
      ),
      child: Text(
        label,
        style: TextStyle(
          color: Colors.white.withAlpha(190),
          fontSize: compact ? 9 : 10,
          fontWeight: FontWeight.w700,
          letterSpacing: 0.8,
        ),
      ),
    );
  }
}

class _DialogueBox extends StatelessWidget {
  const _DialogueBox({
    required this.name,
    required this.text,
    required this.complete,
    required this.index,
    required this.total,
    required this.fontSize,
    required this.compact,
    required this.showHint,
  });

  final String name;
  final String text;
  final bool complete;
  final int index;
  final int total;
  final double fontSize;
  final bool compact;
  final bool showHint;

  @override
  Widget build(BuildContext context) {
    final TextTheme textTheme = Theme.of(context).textTheme;
    return Padding(
      padding: EdgeInsets.all(compact ? 10 : 20),
      child: Container(
        width: double.infinity,
        padding: EdgeInsets.fromLTRB(
          compact ? 14 : 20,
          compact ? 12 : 16,
          compact ? 14 : 20,
          compact ? 10 : 14,
        ),
        decoration: BoxDecoration(
          gradient: LinearGradient(
            begin: Alignment.topLeft,
            end: Alignment.bottomRight,
            colors: <Color>[
              Colors.black.withAlpha(214),
              const Color(0xFF0B0D16).withAlpha(226),
            ],
          ),
          borderRadius: BorderRadius.circular(compact ? 14 : 18),
          border: Border.all(color: Colors.white.withAlpha(28)),
          boxShadow: <BoxShadow>[
            BoxShadow(
              color: Colors.black.withAlpha(130),
              blurRadius: 30,
              offset: const Offset(0, 12),
            ),
          ],
        ),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            Row(
              children: <Widget>[
                Container(
                  padding: EdgeInsets.symmetric(
                    horizontal: compact ? 9 : 12,
                    vertical: compact ? 3 : 5,
                  ),
                  decoration: BoxDecoration(
                    gradient: const LinearGradient(
                      colors: <Color>[AppColors.secondary, Color(0xFFFFE7B0)],
                    ),
                    borderRadius: BorderRadius.circular(999),
                  ),
                  child: Text(
                    name,
                    style: TextStyle(
                      color: const Color(0xFF201505),
                      fontSize: compact ? 10 : 12,
                      fontWeight: FontWeight.w800,
                      letterSpacing: 0.4,
                    ),
                  ),
                ),
                const SizedBox(width: 10),
                Text(
                  'MESSAGE ${index + 1} / $total',
                  style: textTheme.labelSmall?.copyWith(
                    color: Colors.white.withAlpha(120),
                    fontSize: compact ? 8 : 9,
                    letterSpacing: 1,
                  ),
                ),
                const Spacer(),
                Text(
                  'TOUCH TO ADVANCE',
                  style: textTheme.labelSmall?.copyWith(
                    color: Colors.white.withAlpha(80),
                    fontSize: compact ? 7 : 8,
                    letterSpacing: 1,
                  ),
                ),
              ],
            ),
            SizedBox(height: compact ? 8 : 12),
            Text(
              complete ? text : '$text▍',
              style: TextStyle(
                color: AppColors.text,
                fontSize: fontSize,
                height: 1.55,
                letterSpacing: 0.2,
              ),
            ),
            SizedBox(height: compact ? 4 : 8),
            Row(
              mainAxisAlignment: MainAxisAlignment.end,
              children: <Widget>[
                if (showHint && complete)
                  Text(
                    '点击继续',
                    style: TextStyle(
                      color: Colors.white.withAlpha(95),
                      fontSize: compact ? 8 : 9,
                      letterSpacing: 0.8,
                    ),
                  ),
                if (showHint && complete) const SizedBox(width: 4),
                AnimatedOpacity(
                  opacity: complete ? 1 : 0.25,
                  duration: const Duration(milliseconds: 220),
                  child: Icon(
                    Icons.keyboard_arrow_down_rounded,
                    size: compact ? 16 : 18,
                    color: AppColors.secondary,
                  ),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

class _ScenePainter extends CustomPainter {
  _ScenePainter();

  @override
  void paint(Canvas canvas, Size size) {
    final Rect rect = Offset.zero & size;
    final Paint sky = Paint()
      ..shader = const LinearGradient(
        begin: Alignment.topCenter,
        end: Alignment.bottomCenter,
        colors: <Color>[
          Color(0xFF0A1028),
          Color(0xFF241431),
          Color(0xFF090A12),
        ],
        stops: <double>[0, 0.58, 1],
      ).createShader(rect);
    canvas.drawRect(rect, sky);

    final Random random = Random(20261025);
    for (int index = 0; index < 90; index++) {
      final double x = random.nextDouble() * size.width;
      final double y = random.nextDouble() * size.height * 0.64;
      final double radius = random.nextDouble() * 1.2 + 0.3;
      canvas.drawCircle(
        Offset(x, y),
        radius,
        Paint()..color = Colors.white.withAlpha(40 + random.nextInt(110)),
      );
    }

    final Offset moon = Offset(size.width * 0.76, size.height * 0.2);
    canvas.drawCircle(
      moon,
      size.width * 0.085,
      Paint()
        ..color = const Color(0xFFF7E4B8).withAlpha(24)
        ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 26),
    );
    canvas.drawCircle(
      moon,
      size.width * 0.036,
      Paint()..color = const Color(0xFFF8E7C2),
    );

    final Paint city = Paint()..color = const Color(0xFF080A12).withAlpha(236);
    final Path skyline = Path()..moveTo(0, size.height);
    final double ground = size.height * 0.73;
    double x = 0;
    while (x < size.width) {
      final double buildingWidth = 22 + random.nextDouble() * 46;
      final double buildingHeight = 18 + random.nextDouble() * 92;
      skyline
        ..lineTo(x, ground - buildingHeight)
        ..lineTo(x + buildingWidth, ground - buildingHeight)
        ..lineTo(x + buildingWidth, ground);
      x += buildingWidth + 4;
    }
    skyline
      ..lineTo(size.width, size.height)
      ..close();
    canvas.drawPath(skyline, city);

    final Paint window = Paint()..color = AppColors.secondary.withAlpha(96);
    for (int index = 0; index < 26; index++) {
      canvas.drawRect(
        Rect.fromLTWH(
          random.nextDouble() * size.width,
          ground - random.nextDouble() * 64,
          2.5,
          4,
        ),
        window,
      );
    }

    final Paint groundPaint = Paint()
      ..shader = const LinearGradient(
        begin: Alignment.topCenter,
        end: Alignment.bottomCenter,
        colors: <Color>[Color(0xFF0D0E1A), Color(0xFF05060A)],
      ).createShader(
        Rect.fromLTWH(0, ground, size.width, size.height - ground),
      );
    canvas.drawRect(
      Rect.fromLTWH(0, ground, size.width, size.height - ground),
      groundPaint,
    );

    _paintCharacter(canvas, size);

    final Paint vignette = Paint()
      ..shader = RadialGradient(
        radius: 0.9,
        colors: <Color>[Colors.transparent, Colors.black.withAlpha(150)],
        stops: const <double>[0.45, 1],
      ).createShader(rect);
    canvas.drawRect(rect, vignette);
  }

  void _paintCharacter(Canvas canvas, Size size) {
    final double cx = size.width * 0.5;
    final double headY = size.height * 0.33;
    final double headRadius = size.width * 0.034;

    canvas.drawOval(
      Rect.fromCenter(
        center: Offset(cx, size.height * 0.68),
        width: size.width * 0.36,
        height: size.height * 0.5,
      ),
      Paint()
        ..color = AppColors.primary.withAlpha(28)
        ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 36),
    );

    final Path coat = Path()
      ..moveTo(cx - size.width * 0.105, size.height * 0.98)
      ..quadraticBezierTo(
        cx - size.width * 0.095,
        size.height * 0.6,
        cx - size.width * 0.055,
        size.height * 0.48,
      )
      ..quadraticBezierTo(
        cx - size.width * 0.02,
        size.height * 0.42,
        cx,
        size.height * 0.42,
      )
      ..quadraticBezierTo(
        cx + size.width * 0.02,
        size.height * 0.42,
        cx + size.width * 0.055,
        size.height * 0.48,
      )
      ..quadraticBezierTo(
        cx + size.width * 0.095,
        size.height * 0.6,
        cx + size.width * 0.105,
        size.height * 0.98,
      )
      ..close();
    canvas.drawPath(
      coat,
      Paint()
        ..shader = const LinearGradient(
          begin: Alignment.topCenter,
          end: Alignment.bottomCenter,
          colors: <Color>[Color(0xFF1B2138), Color(0xFF07080F)],
        ).createShader(Rect.fromLTWH(0, 0, size.width, size.height)),
    );

    final Path hair = Path()
      ..moveTo(cx - headRadius * 1.45, headY + headRadius * 0.9)
      ..quadraticBezierTo(
        cx - headRadius * 1.65,
        headY - headRadius * 1.8,
        cx,
        headY - headRadius * 1.55,
      )
      ..quadraticBezierTo(
        cx + headRadius * 1.7,
        headY - headRadius * 1.75,
        cx + headRadius * 1.5,
        headY + headRadius * 1.15,
      )
      ..quadraticBezierTo(
        cx + headRadius * 0.8,
        headY - headRadius * 0.1,
        cx,
        headY - headRadius * 0.35,
      )
      ..quadraticBezierTo(
        cx - headRadius * 0.9,
        headY - headRadius * 0.1,
        cx - headRadius * 1.45,
        headY + headRadius * 0.9,
      )
      ..close();
    canvas.drawCircle(
      Offset(cx, headY),
      headRadius,
      Paint()..color = const Color(0xFF2A1E35),
    );
    canvas.drawPath(hair, Paint()..color = const Color(0xFF131522));

    final Paint rim = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.2
      ..shader = const LinearGradient(
        colors: <Color>[Colors.transparent, Color(0x66F0C987)],
      ).createShader(Rect.fromLTWH(0, 0, size.width, size.height));
    canvas.drawPath(coat, rim);
  }

  @override
  bool shouldRepaint(covariant _ScenePainter oldDelegate) => false;
}
