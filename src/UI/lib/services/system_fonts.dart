import 'package:flutter/services.dart';

class SystemFontFace {
  const SystemFontFace(this.name, this.path);
  final String name, path;
}

Future<List<SystemFontFace>> listSystemFonts() async {
  final rows = await const MethodChannel('bgiplayer/fonts')
      .invokeListMethod<dynamic>('list');
  return (rows ?? [])
      .map(
          (row) => SystemFontFace(row['name'] as String, row['path'] as String))
      .toList();
}
