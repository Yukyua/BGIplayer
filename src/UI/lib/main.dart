import 'package:flutter/material.dart';

import 'app.dart';
import 'core/app_controller.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  final AppController controller = await AppController.open();
  runApp(BgiDemoApp(controller: controller));
}
