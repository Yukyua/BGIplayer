import 'dart:io';
import 'package:flutter/services.dart';
import 'package:file_selector/file_selector.dart';
import 'host_service.dart';

class GameFolderPicker {
  Future<String?> pickGameFolder() => Platform.isAndroid
      ? const MethodChannel('bgiplayer/storage')
          .invokeMethod<String>('importGame')
      : getDirectoryPath(confirmButtonText: '选择游戏目录');
  Future<bool> openFolder(String path) => openHostFolder(path);
}
