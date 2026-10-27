import '../models/app_settings.dart';
import 'engine_bridge.dart';
import 'engine_bridge_native_stub.dart'
    if (dart.library.io) 'engine_bridge_desktop.dart' as native;

EngineBridge createEngineBridge(
        {RendererMode renderer = RendererMode.gpuFast, String? defaultFont}) =>
    native.createEngineBridge(renderer: renderer, defaultFont: defaultFont);
