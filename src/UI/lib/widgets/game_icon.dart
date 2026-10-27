import 'dart:typed_data';
import 'package:flutter/material.dart';
import '../services/host_service.dart';

class GameIcon extends StatefulWidget {
  const GameIcon({super.key, this.path, this.size = 72});
  final String? path;
  final double size;
  @override
  State<GameIcon> createState() => _GameIconState();
}

class _GameIconState extends State<GameIcon> {
  late Future<Uint8List?> _image;
  @override
  void initState() {
    super.initState();
    _image = readGameIcon(widget.path);
  }

  @override
  void didUpdateWidget(GameIcon oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.path != widget.path) _image = readGameIcon(widget.path);
  }

  @override
  Widget build(BuildContext context) => SizedBox(
      width: widget.size,
      height: widget.size,
      child: FutureBuilder<Uint8List?>(
          future: _image,
          builder: (context, snapshot) {
            Widget fallback() => Image.asset('assets/default_game_icon.png',
                fit: BoxFit.contain);
            return snapshot.data == null
                ? fallback()
                : Image.memory(snapshot.data!,
                    fit: BoxFit.contain,
                    errorBuilder: (context, error, stack) => fallback());
          }));
}
