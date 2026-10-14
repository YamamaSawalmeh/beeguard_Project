/// Helpers for reading Firebase Realtime Database snapshots.
///
/// RTDB hands back `Object?` that is really a `Map<Object?, Object?>`, and the
/// ESP32 writes some fields as ints where the seed data has doubles (30 vs
/// 30.0). Every screen had its own private copy of these three functions; this
/// is the shared one.
library;

Map<dynamic, dynamic> asMap(Object? value) {
  if (value is Map) return value;
  return {};
}

double readDouble(Object? value, [double fallback = 0]) {
  if (value is num) return value.toDouble();
  return double.tryParse(value?.toString() ?? '') ?? fallback;
}

int readInt(Object? value, [int fallback = 0]) {
  if (value is num) return value.toInt();
  return int.tryParse(value?.toString() ?? '') ?? fallback;
}

String readString(Object? value, String fallback) {
  final text = value?.toString();
  if (text == null || text.isEmpty) return fallback;
  return text;
}

bool readBool(Object? value) => value == true;

/// "5 min ago" from a Unix timestamp in seconds.
///
/// The firmware stamps every alert and detection with an epoch alongside the
/// human-readable string, so the app can show elapsed time that keeps counting
/// up instead of a fixed string written once by the ESP32.
String timeAgo(int epochSeconds) {
  if (epochSeconds <= 0) return 'unknown';

  final then = DateTime.fromMillisecondsSinceEpoch(epochSeconds * 1000);
  final seconds = DateTime.now().difference(then).inSeconds;

  // A clock skew between the ESP32 and the phone can put an event slightly in
  // the future; that is not worth showing as a negative duration.
  if (seconds < 0) return 'just now';
  if (seconds < 45) return 'just now';
  if (seconds < 3600) return '${seconds ~/ 60} min ago';
  if (seconds < 86400) {
    final hours = seconds ~/ 3600;
    return hours == 1 ? '1 hour ago' : '$hours hours ago';
  }
  final days = seconds ~/ 86400;
  return days == 1 ? 'yesterday' : '$days days ago';
}
