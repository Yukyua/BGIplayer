import 'package:flutter/material.dart';

import 'core/app_controller.dart';
import 'core/app_scope.dart';
import 'screens/home_screen.dart';
import 'theme/app_theme.dart';

class BgiDemoApp extends StatefulWidget {
  const BgiDemoApp({super.key, this.controller});

  final AppController? controller;

  @override
  State<BgiDemoApp> createState() => _BgiDemoAppState();
}

class _BgiDemoAppState extends State<BgiDemoApp> {
  late final AppController _controller;

  @override
  void initState() {
    super.initState();
    _controller = widget.controller ?? AppController.seeded();
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AppScope(
      controller: _controller,
      child: MaterialApp(
        title: 'BGIplayer Library',
        debugShowCheckedModeBanner: false,
        theme: buildAppTheme(),
        home: const HomeScreen(),
      ),
    );
  }
}
