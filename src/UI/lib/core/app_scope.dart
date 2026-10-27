import 'package:flutter/widgets.dart';

import 'app_controller.dart';

/// Tiny dependency-injection scope.  No provider/riverpod dependency is
/// required for the demo, but the controller boundary is the same one a
/// native shell would call through the C ABI.
class AppScope extends InheritedNotifier<AppController> {
  const AppScope({
    super.key,
    required AppController controller,
    required super.child,
  }) : super(notifier: controller);

  static AppController read(BuildContext context) =>
      context.getInheritedWidgetOfExactType<AppScope>()!.notifier!;

  static AppController of(BuildContext context) {
    final AppScope? scope =
        context.dependOnInheritedWidgetOfExactType<AppScope>();
    assert(scope != null, 'AppScope was not found above this widget.');
    return scope!.notifier!;
  }
}
