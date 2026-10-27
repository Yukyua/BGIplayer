import 'package:flutter/material.dart';
import '../core/app_scope.dart';
import '../models/app_settings.dart';
import '../services/system_fonts.dart';

class SettingsScreen extends StatelessWidget {
  const SettingsScreen({super.key});
  Future<void> _selectFont(BuildContext context) async {
    final controller = AppScope.of(context);
    try {
      final fonts = await listSystemFonts();
      if (!context.mounted) return;
      String query = '';
      final selected = await showDialog<SystemFontFace>(
          context: context,
          builder: (context) => StatefulBuilder(builder: (context, setState) {
                final filtered = fonts
                    .where((f) =>
                        f.name.toLowerCase().contains(query.toLowerCase()))
                    .toList();
                return AlertDialog(
                    title: const Text('默认对话字体'),
                    content: SizedBox(
                        width: 480,
                        height: 420,
                        child: Column(children: [
                          TextField(
                              decoration:
                                  const InputDecoration(hintText: '搜索字体'),
                              onChanged: (value) =>
                                  setState(() => query = value)),
                          Expanded(
                              child: ListView(children: [
                            ListTile(
                                title: const Text('系统自动选择'),
                                onTap: () => Navigator.pop(
                                    context, const SystemFontFace('', ''))),
                            ...filtered.map((font) => ListTile(
                                title: Text(font.name),
                                onTap: () => Navigator.pop(context, font)))
                          ]))
                        ])),
                    actions: [
                      TextButton(
                          onPressed: () => Navigator.pop(context),
                          child: const Text('取消'))
                    ]);
              }));
      if (selected != null) {
        controller.updateSettings(controller.settings.copyWith(
            defaultFont: selected.path,
            defaultFontName: selected.name,
            clearDefaultFont: selected.path.isEmpty));
      }
    } on Object catch (error) {
      if (context.mounted) {
        ScaffoldMessenger.of(context)
            .showSnackBar(SnackBar(content: Text('无法读取系统字体：$error')));
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    final controller = AppScope.of(context);
    return Scaffold(
        appBar: AppBar(title: const Text('启动器设置')),
        body: ListView(padding: const EdgeInsets.all(24), children: <Widget>[
          DropdownButtonFormField<RendererMode>(
            initialValue: controller.settings.renderer,
            decoration: const InputDecoration(labelText: '渲染模式'),
            items: RendererMode.values
                .map((RendererMode value) => DropdownMenuItem<RendererMode>(
                    value: value, child: Text(value.label)))
                .toList(),
            onChanged: (RendererMode? value) {
              if (value != null) {
                controller.updateSettings(
                    controller.settings.copyWith(renderer: value));
              }
            },
          ),
          const SizedBox(height: 16),
          ListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('默认对话字体'),
              subtitle: Text(controller.settings.defaultFontName ?? '系统自动选择'),
              trailing: const Icon(Icons.chevron_right),
              onTap: () => _selectFont(context)),
          const ListTile(
              contentPadding: EdgeInsets.zero,
              title: Text('程序版本'),
              trailing: Text('0.11')),
        ]));
  }
}
