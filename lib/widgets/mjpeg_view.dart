import 'dart:async';
import 'dart:io';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';

/// Live view of an MJPEG stream, such as the ESP32-CAM's `:81/stream`.
///
/// The ESP32 serves `multipart/x-mixed-replace`, which `Image.network` cannot
/// display — it expects one image per response, not an endless sequence. So we
/// hold the connection open ourselves and pull JPEG frames out of the byte
/// stream as they arrive.
///
/// Deliberately built on `dart:io` alone: adding a package for this would mean
/// a `pub get` on a machine that may not have the plugin toolchain set up, and
/// the parsing involved is about forty lines.
class MjpegView extends StatefulWidget {
  const MjpegView({
    super.key,
    required this.url,
    this.fit = BoxFit.cover,
    this.timeout = const Duration(seconds: 8),
    this.isLive = true,
  });

  final String url;
  final BoxFit fit;
  final Duration timeout;

  /// Set false to tear the connection down (e.g. the card is off-screen).
  /// The ESP32 has a small socket pool, so an abandoned stream costs a slot
  /// that a later reconnect may not get back.
  final bool isLive;

  @override
  State<MjpegView> createState() => _MjpegViewState();
}

class _MjpegViewState extends State<MjpegView> {
  static const int _jpegSoi = 0xD8; // start of image, preceded by 0xFF
  static const int _jpegEoi = 0xD9; // end of image, preceded by 0xFF

  /// A QQVGA JPEG is a few KB. If we get far past this without a complete
  /// frame we are not looking at MJPEG, so drop the buffer rather than grow
  /// it forever.
  static const int _maxFrameBytes = 512 * 1024;

  HttpClient? _client;
  StreamSubscription<List<int>>? _subscription;
  Timer? _retryTimer;

  final List<int> _buffer = <int>[];
  ui.Image? _image;
  bool _decoding = false;
  String? _error;
  bool _connecting = false;
  int _failures = 0;
  int _framesSeen = 0;

  @override
  void initState() {
    super.initState();
    if (widget.isLive) _connect();
  }

  @override
  void didUpdateWidget(MjpegView oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.url != widget.url || oldWidget.isLive != widget.isLive) {
      _disconnect();
      if (widget.isLive) _connect();
    }
  }

  @override
  void dispose() {
    _disconnect();
    _image?.dispose();
    _image = null;
    super.dispose();
  }

  void _disconnect() {
    _retryTimer?.cancel();
    _retryTimer = null;
    _subscription?.cancel();
    _subscription = null;
    _client?.close(force: true);
    _client = null;
    _buffer.clear();
    _connecting = false;
  }

  Future<void> _connect() async {
    if (!mounted) return;
    setState(() {
      _connecting = true;
      _error = null;
    });

    try {
      final client = HttpClient()
        ..connectionTimeout = widget.timeout
        // The stream never completes by design, so an idle timeout would kill
        // a perfectly healthy connection.
        ..idleTimeout = const Duration(hours: 1);
      _client = client;

      final request = await client.getUrl(Uri.parse(widget.url));
      final response = await request.close().timeout(widget.timeout);

      if (response.statusCode != HttpStatus.ok) {
        throw HttpException('Camera returned HTTP ${response.statusCode}');
      }
      if (!mounted) {
        client.close(force: true);
        return;
      }

      _failures = 0;
      setState(() => _connecting = false);

      _subscription = response.listen(
        _consume,
        onError: (Object e) => _fail(e.toString()),
        onDone: () => _fail('Camera closed the connection'),
        cancelOnError: true,
      );
    } catch (e) {
      _fail(_friendlyError(e));
    }
  }

  void _fail(String message) {
    if (!mounted) return;
    _subscription?.cancel();
    _subscription = null;
    _client?.close(force: true);
    _client = null;
    _buffer.clear();

    _failures++;
    setState(() {
      _connecting = false;
      _error = message;
    });

    // Back off up to 10 s. The camera is often simply not powered yet, and
    // hammering it makes its small socket pool worse, not better.
    final delaySeconds = _failures.clamp(1, 10);
    _retryTimer?.cancel();
    _retryTimer = Timer(Duration(seconds: delaySeconds), () {
      if (mounted && widget.isLive) _connect();
    });
  }

  String _friendlyError(Object e) {
    if (e is SocketException) return 'Cannot reach the camera';
    if (e is TimeoutException) return 'Camera did not respond';
    if (e is HttpException) return e.message;
    return 'Stream error';
  }

  /// Pull whole JPEGs out of the byte stream.
  ///
  /// We scan for SOI/EOI markers rather than parsing the multipart boundaries
  /// and `Content-Length` headers. It resynchronises on its own if a chunk is
  /// lost, and it does not care how the ESP32 formats its part headers. Inside
  /// JPEG entropy data every 0xFF is byte-stuffed, so an 0xFFD9 pair can only
  /// be a real end-of-image.
  void _consume(List<int> chunk) {
    _buffer.addAll(chunk);

    while (true) {
      final start = _findMarker(_jpegSoi, 0);
      if (start < 0) {
        // No start yet. Keep the last byte only — a 0xFF may be split across
        // the chunk boundary and its 0xD8 arrive next time.
        if (_buffer.length > 1) {
          _buffer.removeRange(0, _buffer.length - 1);
        }
        return;
      }

      final end = _findMarker(_jpegEoi, start + 2);
      if (end < 0) {
        if (start > 0) _buffer.removeRange(0, start); // drop part headers
        if (_buffer.length > _maxFrameBytes) _buffer.clear();
        return;
      }

      final frame = Uint8List.fromList(_buffer.sublist(start, end + 2));
      _buffer.removeRange(0, end + 2);
      _framesSeen++;
      _present(frame);
    }
  }

  int _findMarker(int marker, int from) {
    for (var i = from; i + 1 < _buffer.length; i++) {
      if (_buffer[i] == 0xFF && _buffer[i + 1] == marker) return i;
    }
    return -1;
  }

  /// Decode straight to a `ui.Image` instead of handing bytes to
  /// `Image.memory`. Every frame is a distinct byte array, so `MemoryImage`
  /// would enter a new entry in Flutter's image cache ~20 times a second and
  /// the cache would balloon. Owning the decode lets us dispose each frame the
  /// moment the next one replaces it.
  Future<void> _present(Uint8List bytes) async {
    // Frames arrive faster than they decode; showing the newest one and
    // dropping the backlog is what you want from a live view.
    if (_decoding) return;
    _decoding = true;

    try {
      final codec = await ui.instantiateImageCodec(bytes);
      final frame = await codec.getNextFrame();
      codec.dispose();

      if (!mounted) {
        frame.image.dispose();
        return;
      }

      final previous = _image;
      setState(() {
        _image = frame.image;
        _error = null;
      });
      previous?.dispose();
    } catch (_) {
      // A torn frame is normal on a lossy link — just wait for the next one.
    } finally {
      _decoding = false;
    }
  }

  @override
  Widget build(BuildContext context) {
    final image = _image;

    if (image != null) {
      return Stack(
        fit: StackFit.expand,
        children: [
          RawImage(image: image, fit: widget.fit, isAntiAlias: true),
          // A stalled stream keeps showing its last frame, which is
          // indistinguishable from a working one. Say so explicitly.
          if (_error != null)
            const Positioned(
              left: 10,
              bottom: 10,
              child: _StreamBadge(
                label: 'Reconnecting',
                color: Color(0xFFFF6F00),
              ),
            )
          else
            const Positioned(
              left: 10,
              bottom: 10,
              child: _StreamBadge(label: 'LIVE', color: Color(0xFFE53935)),
            ),
        ],
      );
    }

    if (_connecting || (_error == null && _framesSeen == 0)) {
      return const _StreamMessage(
        icon: Icons.videocam_rounded,
        message: 'Connecting to camera...',
        spinner: true,
      );
    }

    return _StreamMessage(
      icon: Icons.videocam_off_rounded,
      message: _error ?? 'No video',
      detail: widget.url,
      onRetry: () {
        _failures = 0;
        _disconnect();
        _connect();
      },
    );
  }
}

class _StreamBadge extends StatelessWidget {
  const _StreamBadge({required this.label, required this.color});

  final String label;
  final Color color;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.55),
        borderRadius: BorderRadius.circular(999),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Container(
            width: 8,
            height: 8,
            decoration: BoxDecoration(color: color, shape: BoxShape.circle),
          ),
          const SizedBox(width: 7),
          Text(
            label,
            style: const TextStyle(
              color: Colors.white,
              fontSize: 11,
              fontWeight: FontWeight.w900,
              letterSpacing: 0.5,
            ),
          ),
        ],
      ),
    );
  }
}

class _StreamMessage extends StatelessWidget {
  const _StreamMessage({
    required this.icon,
    required this.message,
    this.detail,
    this.spinner = false,
    this.onRetry,
  });

  final IconData icon;
  final String message;
  final String? detail;
  final bool spinner;
  final VoidCallback? onRetry;

  @override
  Widget build(BuildContext context) {
    return Container(
      color: const Color(0xFF0E1B24),
      alignment: Alignment.center,
      padding: const EdgeInsets.all(18),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          if (spinner)
            const SizedBox(
              width: 28,
              height: 28,
              child: CircularProgressIndicator(
                strokeWidth: 2.5,
                color: Color(0xFFFFC928),
              ),
            )
          else
            Icon(icon, color: const Color(0xFFFFC928), size: 40),
          const SizedBox(height: 12),
          Text(
            message,
            textAlign: TextAlign.center,
            style: const TextStyle(
              color: Colors.white,
              fontSize: 15,
              fontWeight: FontWeight.w800,
            ),
          ),
          if (detail != null) ...[
            const SizedBox(height: 6),
            Text(
              detail!,
              textAlign: TextAlign.center,
              style: TextStyle(
                color: Colors.white.withValues(alpha: 0.6),
                fontSize: 11,
                fontWeight: FontWeight.w600,
              ),
            ),
          ],
          if (onRetry != null) ...[
            const SizedBox(height: 14),
            OutlinedButton.icon(
              onPressed: onRetry,
              style: OutlinedButton.styleFrom(
                foregroundColor: const Color(0xFFFFC928),
                side: const BorderSide(color: Color(0xFFFFC928)),
              ),
              icon: const Icon(Icons.refresh_rounded, size: 18),
              label: const Text('Retry'),
            ),
          ],
        ],
      ),
    );
  }
}
