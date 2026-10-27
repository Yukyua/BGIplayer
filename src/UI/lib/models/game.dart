import 'package:flutter/material.dart';

/// The engine generation detected from the game executable.  The real host
/// scans the PE header; the UI only presents the result.
enum EngineGeneration { pe32, pe64, unknown }

extension EngineGenerationLabel on EngineGeneration {
  String get label => switch (this) {
        EngineGeneration.pe32 => 'PE32',
        EngineGeneration.pe64 => 'PE64',
        EngineGeneration.unknown => 'ENGINE ?',
      };
}

/// A library entry. [path] is the selected canonical game directory.
class Game {
  const Game({
    required this.id,
    required this.title,
    required this.subtitle,
    required this.path,
    required this.sizeBytes,
    required this.coverColors,
    this.lastPlayed,
    this.favorite = false,
    this.engine = EngineGeneration.unknown,
    this.isDemo = false,
    this.synopsis = '',
    this.iconPath,
    this.lastLogPath,
  });

  final String id;
  final String title;
  final String subtitle;
  final String path;
  final int sizeBytes;
  final List<Color> coverColors;
  final DateTime? lastPlayed;
  final bool favorite;
  final EngineGeneration engine;
  final bool isDemo;
  final String synopsis;
  final String? iconPath;
  final String? lastLogPath;

  Game copyWith({
    String? title,
    String? subtitle,
    String? path,
    int? sizeBytes,
    List<Color>? coverColors,
    DateTime? lastPlayed,
    bool? favorite,
    EngineGeneration? engine,
    bool? isDemo,
    String? synopsis,
    String? iconPath,
    String? lastLogPath,
  }) {
    return Game(
      id: id,
      title: title ?? this.title,
      subtitle: subtitle ?? this.subtitle,
      path: path ?? this.path,
      sizeBytes: sizeBytes ?? this.sizeBytes,
      coverColors: coverColors ?? this.coverColors,
      lastPlayed: lastPlayed ?? this.lastPlayed,
      favorite: favorite ?? this.favorite,
      engine: engine ?? this.engine,
      isDemo: isDemo ?? this.isDemo,
      synopsis: synopsis ?? this.synopsis,
      iconPath: iconPath ?? this.iconPath,
      lastLogPath: lastLogPath ?? this.lastLogPath,
    );
  }
}
