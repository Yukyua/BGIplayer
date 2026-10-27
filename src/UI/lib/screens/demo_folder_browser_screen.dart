import 'package:flutter/material.dart';

import '../services/demo_data.dart';
import '../theme/app_theme.dart';
import '../widgets/app_background.dart';

/// A read-only folder browser used when no native document picker is
/// available (web preview, desktop debug run, widget tests).
class DemoFolderBrowserScreen extends StatefulWidget {
  const DemoFolderBrowserScreen({super.key});

  @override
  State<DemoFolderBrowserScreen> createState() =>
      _DemoFolderBrowserScreenState();
}

class _DemoFolderBrowserScreenState extends State<DemoFolderBrowserScreen> {
  final List<DemoFolder> _trail = <DemoFolder>[demoRoot];

  DemoFolder get _current => _trail.last;

  void _open(DemoFolder folder) {
    setState(() => _trail.add(folder));
  }

  void _back() {
    if (_trail.length > 1) {
      setState(() => _trail.removeLast());
    } else {
      Navigator.of(context).pop();
    }
  }

  @override
  Widget build(BuildContext context) {
    final TextTheme text = Theme.of(context).textTheme;
    final bool canSelect = !identical(_current, demoRoot);
    return Scaffold(
      backgroundColor: AppColors.background,
      body: AppBackground(
        child: SafeArea(
          child: Column(
            children: <Widget>[
              Padding(
                padding: const EdgeInsets.fromLTRB(12, 10, 18, 0),
                child: Row(
                  children: <Widget>[
                    IconButton(
                      tooltip: '返回',
                      onPressed: _back,
                      icon: const Icon(Icons.arrow_back_rounded),
                    ),
                    const SizedBox(width: 4),
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: <Widget>[
                          Text('选择游戏目录', style: text.titleLarge),
                          const SizedBox(height: 2),
                          Text(
                            _current.path,
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                            style: text.bodySmall?.copyWith(
                              color: AppColors.muted,
                              fontSize: 11,
                            ),
                          ),
                        ],
                      ),
                    ),
                    OutlinedButton(
                      onPressed: () => Navigator.of(context).pop(),
                      child: const Text('取消'),
                    ),
                  ],
                ),
              ),
              Padding(
                padding: const EdgeInsets.fromLTRB(20, 16, 20, 8),
                child: Container(
                  padding: const EdgeInsets.all(14),
                  decoration: BoxDecoration(
                    color: AppColors.surfaceRaised,
                    borderRadius: BorderRadius.circular(18),
                    border: Border.all(color: AppColors.border),
                  ),
                  child: Row(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[
                      const Icon(
                        Icons.folder_copy_outlined,
                        size: 18,
                        color: AppColors.primary,
                      ),
                      const SizedBox(width: 10),
                      Expanded(
                        child: Text(
                          '这是演示目录。真实设备上，iOS 通过 UIDocumentPicker / '
                          'fileImporter 选择文件夹，Android 通过 '
                          'ACTION_OPEN_DOCUMENT_TREE 选择，然后复制到 App 沙盒。',
                          style: text.bodySmall?.copyWith(
                            color: AppColors.muted,
                            height: 1.5,
                          ),
                        ),
                      ),
                    ],
                  ),
                ),
              ),
              Expanded(
                child: ListView(
                  padding: const EdgeInsets.fromLTRB(20, 8, 20, 20),
                  children: <Widget>[
                    if (_current.children.isNotEmpty) ...<Widget>[
                      const _SectionLabel('文件夹'),
                      ..._current.children.map(
                        (DemoFolder folder) => _FolderTile(
                          folder: folder,
                          onTap: () => _open(folder),
                        ),
                      ),
                    ],
                    if (_current.files.isNotEmpty) ...<Widget>[
                      const _SectionLabel('文件'),
                      ..._current.files.map(
                        (String file) => _FileTile(name: file),
                      ),
                    ],
                    if (_current.children.isEmpty && _current.files.isEmpty)
                      const _EmptyFolder(),
                  ],
                ),
              ),
              Container(
                padding: const EdgeInsets.fromLTRB(20, 12, 20, 12),
                decoration: BoxDecoration(
                  color: AppColors.background.withAlpha(236),
                  border: const Border(
                    top: BorderSide(color: AppColors.border),
                  ),
                ),
                child: Row(
                  children: <Widget>[
                    Expanded(
                      child: Text(
                        canSelect
                            ? '${_current.name} · ${_current.totalFiles} 个文件'
                            : '请选择具体的游戏文件夹',
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: text.bodySmall?.copyWith(color: AppColors.muted),
                      ),
                    ),
                    const SizedBox(width: 12),
                    FilledButton.icon(
                      onPressed: canSelect
                          ? () => Navigator.of(context).pop(_current.path)
                          : null,
                      icon: const Icon(Icons.check_rounded),
                      label: const Text('选择此目录'),
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _FolderTile extends StatelessWidget {
  const _FolderTile({required this.folder, required this.onTap});

  final DemoFolder folder;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final TextTheme text = Theme.of(context).textTheme;
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Material(
        color: AppColors.surface,
        borderRadius: BorderRadius.circular(18),
        child: InkWell(
          onTap: onTap,
          borderRadius: BorderRadius.circular(18),
          child: Container(
            padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 12),
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(18),
              border: Border.all(color: AppColors.border),
            ),
            child: Row(
              children: <Widget>[
                Container(
                  width: 42,
                  height: 42,
                  decoration: BoxDecoration(
                    color: AppColors.primary.withAlpha(24),
                    borderRadius: BorderRadius.circular(13),
                  ),
                  child: const Icon(
                    Icons.folder_rounded,
                    color: AppColors.primary,
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[
                      Text(
                        folder.name,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: text.titleSmall,
                      ),
                      const SizedBox(height: 2),
                      Text(
                        '${folder.children.length} 个文件夹 · '
                        '${folder.files.length} 个文件',
                        style: text.bodySmall?.copyWith(
                          color: AppColors.muted,
                          fontSize: 11,
                        ),
                      ),
                    ],
                  ),
                ),
                const Icon(Icons.chevron_right_rounded, color: AppColors.muted),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _FileTile extends StatelessWidget {
  const _FileTile({required this.name});

  final String name;

  @override
  Widget build(BuildContext context) {
    final TextTheme text = Theme.of(context).textTheme;
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 12),
        decoration: BoxDecoration(
          color: AppColors.surface.withAlpha(140),
          borderRadius: BorderRadius.circular(18),
          border: Border.all(color: AppColors.border),
        ),
        child: Row(
          children: <Widget>[
            const Icon(
              Icons.insert_drive_file_outlined,
              color: AppColors.muted,
              size: 22,
            ),
            const SizedBox(width: 14),
            Expanded(
              child: Text(
                name,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: text.bodyMedium,
              ),
            ),
            Text(
              '资源文件',
              style: text.labelSmall?.copyWith(color: AppColors.muted),
            ),
          ],
        ),
      ),
    );
  }
}

class _SectionLabel extends StatelessWidget {
  const _SectionLabel(this.text);

  final String text;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(4, 14, 4, 10),
      child: Text(
        text.toUpperCase(),
        style: const TextStyle(
          color: AppColors.muted,
          fontSize: 11,
          fontWeight: FontWeight.w700,
          letterSpacing: 1.4,
        ),
      ),
    );
  }
}

class _EmptyFolder extends StatelessWidget {
  const _EmptyFolder();

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 60),
      child: Column(
        children: <Widget>[
          const Icon(
            Icons.folder_off_outlined,
            size: 36,
            color: AppColors.muted,
          ),
          const SizedBox(height: 12),
          Text(
            '这个目录是空的',
            style: Theme.of(context)
                .textTheme
                .bodyMedium
                ?.copyWith(color: AppColors.muted),
          ),
        ],
      ),
    );
  }
}
