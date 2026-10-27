import 'package:flutter/material.dart';

/// Deterministic gradient for imported games without cover art.
List<Color> colorsForName(String name) {
  int hash = 0;
  for (final int codeUnit in name.codeUnits) {
    hash = (hash * 31 + codeUnit) & 0x7FFFFFFF;
  }
  final double hue = (hash % 360).toDouble();
  return <Color>[
    HSLColor.fromAHSL(1, hue, 0.58, 0.56).toColor(),
    HSLColor.fromAHSL(1, (hue + 42) % 360, 0.66, 0.34).toColor(),
  ];
}
