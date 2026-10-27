import 'package:flutter/material.dart';

import '../models/game.dart';

/// A folder node used by the in-app demo browser.  The real app receives a
/// security-scoped URL / SAF tree from the system picker and copies it into
/// the app sandbox; this tree only exists to make the UI demo explorable.
class DemoFolder {
  const DemoFolder({
    required this.name,
    required this.path,
    this.children = const <DemoFolder>[],
    this.files = const <String>[],
  });

  final String name;
  final String path;
  final List<DemoFolder> children;
  final List<String> files;

  int get totalFiles =>
      files.length +
      children.fold<int>(
        0,
        (int sum, DemoFolder child) => sum + child.totalFiles,
      );

  int get totalFolders => children.fold<int>(
        0,
        (int sum, DemoFolder child) => sum + 1 + child.totalFolders,
      );
}

const DemoFolder demoRoot = DemoFolder(
  name: '设备存储',
  path: '/storage/emulated/0',
  children: <DemoFolder>[
    DemoFolder(
      name: 'Games',
      path: '/storage/emulated/0/Games',
      children: <DemoFolder>[
        DemoFolder(
          name: '放課後シンデレラ',
          path: '/storage/emulated/0/Games/放課後シンデレラ',
          files: <String>[
            'system.arc',
            'data01xxx.arc',
            'data02xxx.arc',
            '放課後シンデレラ.exe',
          ],
          children: <DemoFolder>[
            DemoFolder(
              name: 'UserData',
              path: '/storage/emulated/0/Games/放課後シンデレラ/UserData',
              files: <String>['OneNightAfter900.sud', 'config.dat'],
            ),
          ],
        ),
        DemoFolder(
          name: '１／１彼氏彼女MiniFanDisc',
          path: '/storage/emulated/0/Games/１／１彼氏彼女MiniFanDisc',
          files: <String>['system.arc', 'data01xxx.arc', '１／１彼氏彼女.exe'],
          children: <DemoFolder>[
            DemoFolder(
              name: 'UserData',
              path: '/storage/emulated/0/Games/１／１彼氏彼女MiniFanDisc/UserData',
              files: <String>['config.dat'],
            ),
          ],
        ),
        DemoFolder(
          name: 'BGI Demo Scene',
          path: '/storage/emulated/0/Games/BGI Demo Scene',
          files: <String>['demo.scene', 'README.txt'],
        ),
      ],
    ),
    DemoFolder(
      name: 'Download',
      path: '/storage/emulated/0/Download',
      files: <String>['bgi_patch.zip', 'not-a-game.arc'],
    ),
    DemoFolder(
      name: 'Documents',
      path: '/storage/emulated/0/Documents',
      files: <String>['saves_backup.zip'],
    ),
    DemoFolder(name: 'Android', path: '/storage/emulated/0/Android'),
  ],
);

/// Seed data.  These are UI records only; they do not represent files on the
/// device and no game data is bundled with this demo.
List<Game> seedGames() {
  final DateTime now = DateTime.now();
  return <Game>[
    Game(
      id: 'cinderella',
      title: '放課後シンデレラ',
      subtitle: 'HOOKSOFT · PE64 · 示例库条目',
      path: '/storage/emulated/0/Games/放課後シンデレラ',
      sizeBytes: 4200000000,
      coverColors: const <Color>[Color(0xFF6A5CFF), Color(0xFFFF8FB1)],
      lastPlayed: now.subtract(const Duration(hours: 2)),
      favorite: true,
      engine: EngineGeneration.pe64,
      synopsis: '示例条目。真实版本中，这里显示从游戏目录读取的标题、封面、'
          '引擎代际和运行状态；启动按钮会通过 C ABI 调用 SDL2 宿主。',
    ),
    Game(
      id: 'minifan',
      title: '１／１彼氏彼女 MiniFanDisc',
      subtitle: 'PE64 · Fan disc · 示例库条目',
      path: '/storage/emulated/0/Games/１／１彼氏彼女MiniFanDisc',
      sizeBytes: 1800000000,
      coverColors: const <Color>[Color(0xFF2AA8A0), Color(0xFF275D9E)],
      lastPlayed: now.subtract(const Duration(days: 2)),
      engine: EngineGeneration.pe64,
      synopsis: '示例条目。它演示了多个游戏共存时的库列表、筛选和存储统计。',
    ),
    const Game(
      id: 'demo-scene',
      title: 'BGI Demo Scene',
      subtitle: '内置演示场景 · 无需游戏文件',
      path: 'demo://first-scene',
      sizeBytes: 0,
      coverColors: <Color>[Color(0xFFF0C987), Color(0xFFB34B5E)],
      engine: EngineGeneration.unknown,
      isDemo: true,
      synopsis: '这是 UI demo 自带的模拟场景，用来演示全屏播放、点击推进、'
          '暂停、横屏沉浸和退出流程。它不包含任何商业游戏素材。',
    ),
  ];
}
