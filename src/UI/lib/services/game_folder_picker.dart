import 'package:file_selector/file_selector.dart';
import 'host_service.dart';

class GameFolderPicker {
  Future<String?> pickGameFolder() =>
      getDirectoryPath(confirmButtonText: '选择游戏目录');
  Future<bool> openFolder(String path) => openHostFolder(path);
}
