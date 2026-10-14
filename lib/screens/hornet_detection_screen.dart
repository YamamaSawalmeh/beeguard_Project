import 'dart:async';

import 'package:firebase_database/firebase_database.dart';
import 'package:flutter/material.dart';

import '../theme/app_colors.dart';
import '../utils/db_read.dart';
import '../widgets/beeguard_card.dart';
import '../widgets/mjpeg_view.dart';

class HornetDetectionScreen extends StatefulWidget {
  const HornetDetectionScreen({super.key});

  @override
  State<HornetDetectionScreen> createState() => _HornetDetectionScreenState();
}

class _HornetDetectionScreenState extends State<HornetDetectionScreen> {
  /// Used until the controller publishes /camera/stream_url. Keeping the URL
  /// in the database means the camera can move without rebuilding the app.
  static const String _fallbackStreamUrl = 'http://192.168.137.150/';

  StreamSubscription<DatabaseEvent>? _hornetSubscription;
  StreamSubscription<DatabaseEvent>? _cameraSubscription;
  Timer? _tick;

  bool detected = false;
  String entranceStatus = 'open';
  String lastDetection = 'No detection yet';
  int lastDetectionEpoch = 0;
  bool lastConfirmed = false;
  int detectionCount = 0;
  bool cameraOnline = false;
  String streamUrl = _fallbackStreamUrl;
  bool _loading = true;
  bool _sending = false;

  @override
  void initState() {
    super.initState();

    _hornetSubscription = FirebaseDatabase.instance
        .ref('hornet_detection')
        .onValue
        .listen((event) {
          final data = asMap(event.snapshot.value);
          if (!mounted) return;
          setState(() {
            detected = readBool(data['detected']);
            entranceStatus = readString(data['entrance_status'], 'open');
            lastDetection = readString(
              data['last_detection'],
              'No detection yet',
            );
            lastDetectionEpoch = readInt(data['last_detection_epoch']);
            lastConfirmed = readBool(data['last_confirmed']);
            detectionCount = readInt(data['detection_count']);
            _loading = false;
          });
        });

    _cameraSubscription = FirebaseDatabase.instance.ref('camera').onValue.listen(
      (event) {
        final data = asMap(event.snapshot.value);
        if (!mounted) return;
        setState(() {
          streamUrl = readString(data['stream_url'], _fallbackStreamUrl);
          cameraOnline = readBool(data['online']);
        });
      },
    );

    // "12 min ago" has to keep counting even when nothing new arrives from
    // Firebase, so the elapsed label is refreshed locally.
    _tick = Timer.periodic(const Duration(seconds: 30), (_) {
      if (mounted) setState(() {});
    });
  }

  @override
  void dispose() {
    _hornetSubscription?.cancel();
    _cameraSubscription?.cancel();
    _tick?.cancel();
    super.dispose();
  }

  Future<void> _setEntrance(String value) async {
    if (_sending) return;
    setState(() => _sending = true);
    try {
      await FirebaseDatabase.instance.ref('commands/entrance').set(value);
      // The ESP echoes entrance_status back once the servo has actually moved;
      // this is only so the button state does not look stuck in the meantime.
      if (mounted) setState(() => entranceStatus = value);
    } finally {
      if (mounted) setState(() => _sending = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    if (_loading) {
      return const Scaffold(
        body: Center(
          child: CircularProgressIndicator(color: AppColors.secondary),
        ),
      );
    }

    final narrowed = entranceStatus == 'narrow';
    final statusColor = detected
        ? const Color(0xFFE53935)
        : AppColors.secondary;

    return Scaffold(
      appBar: AppBar(title: const Text('Hornet Detection')),
      body: SafeArea(
        top: false,
        child: ListView(
          padding: const EdgeInsets.fromLTRB(20, 8, 20, 24),
          children: [
            BeeGuardCard(
              padding: const EdgeInsets.all(22),
              child: Row(
                children: [
                  Container(
                    width: 58,
                    height: 58,
                    decoration: BoxDecoration(
                      color: statusColor.withValues(alpha: 0.12),
                      borderRadius: BorderRadius.circular(18),
                    ),
                    child: Icon(
                      detected
                          ? Icons.pest_control_rounded
                          : Icons.check_circle_rounded,
                      color: statusColor,
                      size: 32,
                    ),
                  ),
                  const SizedBox(width: 16),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        const Text(
                          'Detection Status',
                          style: TextStyle(
                            color: AppColors.textSecondary,
                            fontWeight: FontWeight.w700,
                          ),
                        ),
                        const SizedBox(height: 8),
                        _StatusChip(
                          label: detected
                              ? (detectionCount > 1
                                    ? '$detectionCount Hornets Detected'
                                    : 'Hornet Detected')
                              : 'No Hornet',
                          color: statusColor,
                        ),
                      ],
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 16),

            // ── Live camera + last detection, one section ──────────
            _CameraSection(
              streamUrl: streamUrl,
              cameraOnline: cameraOnline,
              detected: detected,
              lastDetection: lastDetection,
              lastDetectionEpoch: lastDetectionEpoch,
              lastConfirmed: lastConfirmed,
              detectionCount: detectionCount,
            ),

            const SizedBox(height: 16),
            BeeGuardCard(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    children: [
                      Icon(
                        narrowed
                            ? Icons.door_sliding_rounded
                            : Icons.door_front_door_rounded,
                        color: narrowed
                            ? const Color(0xFFFF6F00)
                            : AppColors.secondary,
                      ),
                      const SizedBox(width: 12),
                      Text(
                        narrowed ? 'Entrance: Narrow' : 'Entrance: Open',
                        style: Theme.of(context).textTheme.titleMedium,
                      ),
                    ],
                  ),
                  const SizedBox(height: 6),
                  Text(
                    narrowed
                        ? 'The gate is narrowed so hornets cannot get in. Bees can still pass.'
                        : 'The gate is fully open for normal foraging traffic.',
                    style: const TextStyle(
                      color: AppColors.textSecondary,
                      fontSize: 13,
                      fontWeight: FontWeight.w600,
                      height: 1.35,
                    ),
                  ),
                  const SizedBox(height: 16),
                  Row(
                    children: [
                      Expanded(
                        child: ElevatedButton(
                          onPressed: _sending || !narrowed
                              ? null
                              : () => _setEntrance('open'),
                          child: const Text('Open Entrance'),
                        ),
                      ),
                      const SizedBox(width: 10),
                      Expanded(
                        child: OutlinedButton(
                          onPressed: _sending || narrowed
                              ? null
                              : () => _setEntrance('narrow'),
                          child: const Text('Narrow Entrance'),
                        ),
                      ),
                    ],
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

/// The live stream and the last-detection record, deliberately in one card.
///
/// Split across two cards they read as unrelated facts; together the question
/// a beekeeper actually has — "is something at the entrance right now, and
/// when was the last time there was?" — is answered in one glance.
class _CameraSection extends StatelessWidget {
  const _CameraSection({
    required this.streamUrl,
    required this.cameraOnline,
    required this.detected,
    required this.lastDetection,
    required this.lastDetectionEpoch,
    required this.lastConfirmed,
    required this.detectionCount,
  });

  final String streamUrl;
  final bool cameraOnline;
  final bool detected;
  final String lastDetection;
  final int lastDetectionEpoch;
  final bool lastConfirmed;
  final int detectionCount;

  @override
  Widget build(BuildContext context) {
    final hasDetection = lastConfirmed && lastDetectionEpoch > 0;
    final accent = detected
        ? const Color(0xFFE53935)
        : (hasDetection ? const Color(0xFFFF6F00) : AppColors.secondary);

    return BeeGuardCard(
      padding: EdgeInsets.zero,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(18, 16, 18, 12),
            child: Row(
              children: [
                const Icon(
                  Icons.videocam_rounded,
                  color: AppColors.secondary,
                  size: 22,
                ),
                const SizedBox(width: 10),
                const Expanded(
                  child: Text(
                    'Entrance Camera',
                    style: TextStyle(
                      color: AppColors.textPrimary,
                      fontSize: 17,
                      fontWeight: FontWeight.w900,
                    ),
                  ),
                ),
                _StatusChip(
                  label: cameraOnline ? 'Online' : 'Offline',
                  color: cameraOnline
                      ? AppColors.secondary
                      : AppColors.textSecondary,
                ),
              ],
            ),
          ),
          ClipRRect(
            child: SizedBox(
              height: 220,
              width: double.infinity,
              child: MjpegView(url: streamUrl, fit: BoxFit.contain),
            ),
          ),
          Container(
            width: double.infinity,
            padding: const EdgeInsets.fromLTRB(18, 16, 18, 18),
            decoration: BoxDecoration(
              color: accent.withValues(alpha: 0.07),
              borderRadius: const BorderRadius.only(
                bottomLeft: Radius.circular(22),
                bottomRight: Radius.circular(22),
              ),
            ),
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Container(
                  width: 42,
                  height: 42,
                  decoration: BoxDecoration(
                    color: accent.withValues(alpha: 0.15),
                    borderRadius: BorderRadius.circular(13),
                  ),
                  child: Icon(
                    hasDetection
                        ? Icons.pest_control_rounded
                        : Icons.shield_rounded,
                    color: accent,
                    size: 22,
                  ),
                ),
                const SizedBox(width: 14),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      const Text(
                        'Last Detection',
                        style: TextStyle(
                          color: AppColors.textSecondary,
                          fontSize: 12,
                          fontWeight: FontWeight.w800,
                          letterSpacing: 0.3,
                        ),
                      ),
                      const SizedBox(height: 5),
                      Text(
                        hasDetection
                            ? timeAgo(lastDetectionEpoch)
                            : 'No hornet detected yet',
                        style: TextStyle(
                          color: accent,
                          fontSize: 19,
                          fontWeight: FontWeight.w900,
                        ),
                      ),
                      if (hasDetection) ...[
                        const SizedBox(height: 5),
                        Text(
                          lastDetection,
                          style: const TextStyle(
                            color: AppColors.textPrimary,
                            fontSize: 13,
                            fontWeight: FontWeight.w700,
                          ),
                        ),
                        const SizedBox(height: 3),
                        Text(
                          detected
                              ? 'Confirmed hornet - still visible now'
                              : 'Confirmed hornet'
                                    '${detectionCount > 0 ? ' - $detectionCount in frame' : ''}',
                          style: const TextStyle(
                            color: AppColors.textSecondary,
                            fontSize: 12,
                            fontWeight: FontWeight.w600,
                          ),
                        ),
                      ] else ...[
                        const SizedBox(height: 4),
                        const Text(
                          'The camera watches the entrance continuously, '
                          'whether or not this screen is open.',
                          style: TextStyle(
                            color: AppColors.textSecondary,
                            fontSize: 12,
                            height: 1.35,
                            fontWeight: FontWeight.w600,
                          ),
                        ),
                      ],
                    ],
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _StatusChip extends StatelessWidget {
  const _StatusChip({required this.label, required this.color});

  final String label;
  final Color color;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 8),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.12),
        borderRadius: BorderRadius.circular(999),
      ),
      child: Text(
        label,
        style: TextStyle(color: color, fontWeight: FontWeight.w900),
      ),
    );
  }
}
