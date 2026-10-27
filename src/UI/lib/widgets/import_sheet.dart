import 'package:flutter/material.dart';

import '../services/game_folder_picker.dart';
import '../theme/app_theme.dart';

Future<String?> showImportGameSheet(BuildContext context) {
  return showModalBottomSheet<String>(
    context: context,
    isScrollControlled: true,
    useSafeArea: true,
    backgroundColor: Colors.transparent,
    barrierColor: Colors.black.withAlpha(170),
    builder: (BuildContext context) => const ImportGameSheet(),
  );
}

class ImportGameSheet extends StatefulWidget {
  const ImportGameSheet({super.key});

  @override
  State<ImportGameSheet> createState() => _ImportGameSheetState();
}

class _ImportGameSheetState extends State<ImportGameSheet> {
  final TextEditingController _path = TextEditingController();
  bool _picking = false;

  @override
  void dispose() {
    _path.dispose();
    super.dispose();
  }

  void _submit() {
    final String value = _path.text.trim();
    if (value.isEmpty) return;
    Navigator.of(context).pop(value);
  }

  Future<void> _pickSystemFolder() async {
    if (_picking) return;
    setState(() => _picking = true);
    try {
      final String? path = await GameFolderPicker().pickGameFolder();
      if (mounted && path != null) Navigator.of(context).pop(path);
    } on Object catch (error) {
      if (mounted) {
        ScaffoldMessenger.of(context)
            .showSnackBar(SnackBar(content: Text('无法打开文件夹选择器：$error')));
      }
    } finally {
      if (mounted) setState(() => _picking = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final TextTheme text = Theme.of(context).textTheme;
    final double keyboard = MediaQuery.of(context).viewInsets.bottom;
    return Padding(
      padding: EdgeInsets.only(bottom: keyboard),
      child: Align(
        alignment: Alignment.bottomCenter,
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 660),
          child: Container(
            decoration: const BoxDecoration(
              color: AppColors.surface,
              borderRadius: BorderRadius.vertical(top: Radius.circular(28)),
              border: Border(top: BorderSide(color: AppColors.border)),
            ),
            padding: const EdgeInsets.fromLTRB(24, 12, 24, 20),
            child: SingleChildScrollView(
              child: Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: <Widget>[
                  Center(
                    child: Container(
                      width: 42,
                      height: 4,
                      decoration: BoxDecoration(
                        color: Colors.white.withAlpha(50),
                        borderRadius: BorderRadius.circular(999),
                      ),
                    ),
                  ),
                  const SizedBox(height: 22),
                  Row(
                    children: <Widget>[
                      Container(
                        width: 44,
                        height: 44,
                        decoration: BoxDecoration(
                          color: AppColors.primary.withAlpha(28),
                          borderRadius: BorderRadius.circular(14),
                        ),
                        child: const Icon(
                          Icons.create_new_folder_outlined,
                          color: AppColors.primary,
                        ),
                      ),
                      const SizedBox(width: 14),
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: <Widget>[
                            Text('导入游戏目录', style: text.titleLarge),
                            const SizedBox(height: 3),
                            Text(
                              '选择包含 sysprg.arc 的游戏根目录',
                              style: text.bodySmall?.copyWith(
                                color: AppColors.muted,
                              ),
                            ),
                          ],
                        ),
                      ),
                      IconButton(
                        tooltip: '关闭',
                        onPressed: () => Navigator.of(context).pop(),
                        icon: const Icon(Icons.close_rounded),
                      ),
                    ],
                  ),
                  const SizedBox(height: 20),
                  TextField(
                    controller: _path,
                    textInputAction: TextInputAction.done,
                    keyboardType: TextInputType.url,
                    onSubmitted: (_) => _submit(),
                    decoration: const InputDecoration(
                      labelText: '游戏目录路径',
                      hintText: '游戏根目录的绝对路径',
                      prefixIcon: Icon(Icons.folder_open_rounded),
                    ),
                  ),
                  const SizedBox(height: 12),
                  ValueListenableBuilder<TextEditingValue>(
                    valueListenable: _path,
                    builder: (
                      BuildContext context,
                      TextEditingValue value,
                      Widget? child,
                    ) {
                      return SizedBox(
                        width: double.infinity,
                        child: FilledButton.icon(
                          onPressed: value.text.trim().isEmpty ? null : _submit,
                          icon: const Icon(Icons.download_done_rounded),
                          label: const Text('导入此目录'),
                        ),
                      );
                    },
                  ),
                  const SizedBox(height: 10),
                  SizedBox(
                    width: double.infinity,
                    child: OutlinedButton.icon(
                      onPressed: _picking ? null : _pickSystemFolder,
                      icon: _picking
                          ? const SizedBox(
                              width: 16,
                              height: 16,
                              child: CircularProgressIndicator(strokeWidth: 2),
                            )
                          : const Icon(Icons.folder_rounded),
                      label: const Text('使用系统文件夹选择器'),
                    ),
                  ),
                  const SizedBox(height: 4),
                  const SizedBox(height: 8),
                  Container(
                    padding: const EdgeInsets.all(14),
                    decoration: BoxDecoration(
                      color: AppColors.surfaceRaised,
                      borderRadius: BorderRadius.circular(16),
                      border: Border.all(color: AppColors.border),
                    ),
                    child: Row(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: <Widget>[
                        const Icon(
                          Icons.info_outline_rounded,
                          size: 18,
                          color: AppColors.secondary,
                        ),
                        const SizedBox(width: 10),
                        Expanded(
                          child: Text(
                            '游戏文件保留在原目录，不会复制。'
                            '启动时由 SDL2 打开独立游戏窗口，存档仍由游戏管理。',
                            style: text.bodySmall?.copyWith(
                              color: AppColors.muted,
                              height: 1.5,
                            ),
                          ),
                        ),
                      ],
                    ),
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}
