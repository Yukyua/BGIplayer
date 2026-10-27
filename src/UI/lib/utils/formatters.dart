/// Small formatting helpers shared by cards and detail pages.
String formatBytes(int bytes) {
  if (bytes <= 0) return '0 B';
  const List<String> units = <String>['B', 'KB', 'MB', 'GB', 'TB'];
  double value = bytes.toDouble();
  int unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }
  final String digits = value >= 100 || unit == 0
      ? value.toStringAsFixed(0)
      : value.toStringAsFixed(1);
  return '$digits ${units[unit]}';
}

String formatRelativeTime(DateTime? time) {
  if (time == null) return '从未游玩';
  final Duration diff = DateTime.now().difference(time);
  if (diff.isNegative) return '刚刚';
  if (diff.inMinutes < 1) return '刚刚';
  if (diff.inMinutes < 60) return '${diff.inMinutes} 分钟前';
  if (diff.inHours < 24) return '${diff.inHours} 小时前';
  if (diff.inDays < 30) return '${diff.inDays} 天前';
  final String month = time.month.toString().padLeft(2, '0');
  final String day = time.day.toString().padLeft(2, '0');
  return '${time.year}-$month-$day';
}

/// Handles both POSIX and Windows separators without importing `path`.
String basename(String path) {
  String value = path.replaceAll('\\', '/');
  while (value.endsWith('/') && value.length > 1) {
    value = value.substring(0, value.length - 1);
  }
  final int index = value.lastIndexOf('/');
  return index == -1 ? value : value.substring(index + 1);
}

String initials(String text) {
  final String trimmed = text.trim();
  if (trimmed.isEmpty) return 'B';
  return String.fromCharCode(trimmed.runes.first);
}
