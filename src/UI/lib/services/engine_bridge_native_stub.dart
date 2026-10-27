import '../models/app_settings.dart';
import 'engine_bridge.dart';

EngineBridge createEngineBridge(
        {required RendererMode renderer, String? defaultFont}) =>
    PlatformEngineBridge(renderer: renderer, defaultFont: defaultFont);
