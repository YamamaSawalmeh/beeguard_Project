import 'dart:async';

import 'package:firebase_database/firebase_database.dart';
import 'package:flutter/material.dart';

import '../theme/app_colors.dart';
import '../utils/db_read.dart';
import '../widgets/beeguard_card.dart';

class LiveMonitoringScreen extends StatefulWidget {
  const LiveMonitoringScreen({super.key});

  @override
  State<LiveMonitoringScreen> createState() => _LiveMonitoringScreenState();
}

class _LiveMonitoringScreenState extends State<LiveMonitoringScreen> {
  StreamSubscription<DatabaseEvent>? _subscription;
  Timer? _tick;

  bool _loading = true;
  double temperature = 0;
  double humidity = 0;
  double hiveWeight = 0;
  double waterLevel = 0;
  double waterMl = 0;
  String soundResult = 'Unknown';
  int soundConfidence = 0;
  String lastSync = 'Waiting for data';
  int lastSyncEpoch = 0;
  bool waterSensorOk = true;
  bool audioOnline = false;

  @override
  void initState() {
    super.initState();
    _subscription = FirebaseDatabase.instance.ref('hive_status').onValue.listen(
      (event) {
        final data = asMap(event.snapshot.value);
        if (!mounted) return;

        setState(() {
          temperature = readDouble(data['temperature']);
          humidity = readDouble(data['humidity']);
          hiveWeight = readDouble(data['weight']);
          waterLevel = readDouble(data['water_level']);
          waterMl = readDouble(data['water_remaining_ml']);
          soundResult = readString(data['sound_result'], 'Unknown');
          soundConfidence = readInt(data['sound_confidence']);
          lastSync = readString(data['last_sync'], 'No sync yet');
          lastSyncEpoch = readInt(data['last_sync_epoch']);
          // Absent on older firmware; assume healthy rather than showing a
          // false fault on a hive that has not been reflashed yet.
          waterSensorOk = data['water_sensor_ok'] != false;
          audioOnline = readBool(data['audio_node_online']);
          _loading = false;
        });
      },
    );

    _tick = Timer.periodic(const Duration(seconds: 30), (_) {
      if (mounted) setState(() {});
    });
  }

  @override
  void dispose() {
    _subscription?.cancel();
    _tick?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (_loading) {
      return const Scaffold(
        body: Center(
          child: CircularProgressIndicator(color: Color(0xFFFFC928)),
        ),
      );
    }

    // "Unknown" means the audio board has not reported yet. Flagging that as a
    // warning would put a permanent orange badge on a healthy hive, so it is
    // shown as a distinct "waiting" state instead.
    final soundKnown =
        soundResult.isNotEmpty && soundResult.toLowerCase() != 'unknown';

    final sensors = [
      _SensorItem(
        name: 'Temperature',
        value: audioOnline || temperature > 0
            ? '${temperature.toStringAsFixed(1)} C'
            : 'Waiting',
        icon: Icons.thermostat_rounded,
        color: const Color(0xFFE53935),
        warning: temperature > 36 || (temperature > 0 && temperature < 32),
        subtitle: audioOnline ? null : 'Audio/DHT board offline',
      ),
      _SensorItem(
        name: 'Humidity',
        value: audioOnline || humidity > 0
            ? '${humidity.toStringAsFixed(0)}%'
            : 'Waiting',
        icon: Icons.water_drop_rounded,
        color: const Color(0xFF42A5F5),
        warning: humidity > 75 || (humidity > 0 && humidity < 40),
        subtitle: audioOnline ? null : 'Audio/DHT board offline',
      ),
      _SensorItem(
        name: 'Hive Weight',
        value: '${hiveWeight.toStringAsFixed(1)} kg',
        icon: Icons.scale_rounded,
        color: const Color(0xFF8D6E63),
        warning: false,
        subtitle: 'Whole hive on the load cell',
      ),
      _SensorItem(
        name: 'Sound Analysis',
        value: soundKnown ? soundResult : 'Waiting for analysis',
        icon: Icons.graphic_eq_rounded,
        color: AppColors.secondary,
        warning: soundKnown && soundResult.toLowerCase() != 'normal',
        subtitle: soundKnown && soundConfidence > 0
            ? '$soundConfidence% confidence'
            : 'Runs every 15 minutes',
      ),
      _SensorItem(
        name: 'Feeding Level',
        value: waterSensorOk
            ? '${waterLevel.toStringAsFixed(0)}%'
            : 'Sensor fault',
        icon: Icons.local_drink_rounded,
        color: const Color(0xFF42A5F5),
        warning: !waterSensorOk || waterLevel < 20,
        subtitle: waterSensorOk && waterMl > 0
            ? '${waterMl.toStringAsFixed(0)} ml remaining'
            : (waterSensorOk ? null : 'Ultrasonic not responding'),
      ),
    ];

    return Scaffold(
      appBar: AppBar(
        title: const Text('Live Monitoring'),
        leading: IconButton(
          color: const Color(0xFFFFC928),
          onPressed: () => Navigator.pop(context),
          icon: const Icon(Icons.arrow_back_rounded),
        ),
      ),
      body: SafeArea(
        top: false,
        child: ListView(
          padding: const EdgeInsets.fromLTRB(20, 8, 20, 24),
          children: [
            _LiveStatusRow(lastSyncEpoch: lastSyncEpoch),
            const SizedBox(height: 18),
            ...sensors.map(
              (sensor) => Padding(
                padding: const EdgeInsets.only(bottom: 14),
                child: _SensorCard(sensor: sensor),
              ),
            ),
            const SizedBox(height: 6),
            _ConnectionStatusCard(
              lastSync: lastSync,
              lastSyncEpoch: lastSyncEpoch,
              audioOnline: audioOnline,
            ),
          ],
        ),
      ),
    );
  }
}

class _LiveStatusRow extends StatelessWidget {
  const _LiveStatusRow({required this.lastSyncEpoch});

  final int lastSyncEpoch;

  @override
  Widget build(BuildContext context) {
    // This row used to read "Live - just now" as a hard-coded string, so it
    // said the same thing whether the hive had reported a second ago or gone
    // offline days earlier. The controller uploads every 30 s, so anything
    // past ~2 minutes means the link is down.
    final stale =
        lastSyncEpoch <= 0 ||
        DateTime.now()
                .difference(
                  DateTime.fromMillisecondsSinceEpoch(lastSyncEpoch * 1000),
                )
                .inSeconds >
            120;

    return Row(
      children: [
        _StatusDot(
          color: stale ? const Color(0xFFFF6F00) : AppColors.secondary,
          size: 9,
        ),
        const SizedBox(width: 8),
        Text(
          lastSyncEpoch <= 0
              ? 'Waiting for the hive'
              : (stale
                    ? 'Last update ${timeAgo(lastSyncEpoch)}'
                    : 'Live - ${timeAgo(lastSyncEpoch)}'),
          style: const TextStyle(
            color: AppColors.textSecondary,
            fontSize: 13,
            fontWeight: FontWeight.w600,
          ),
        ),
      ],
    );
  }
}

class _SensorCard extends StatelessWidget {
  const _SensorCard({required this.sensor});

  final _SensorItem sensor;

  @override
  Widget build(BuildContext context) {
    final statusColor = sensor.warning
        ? const Color(0xFFFFC928)
        : AppColors.secondary;

    return BeeGuardCard(
      padding: EdgeInsets.zero,
      child: Column(
        children: [
          Padding(
            padding: const EdgeInsets.all(18),
            child: Row(
              children: [
                Container(
                  width: 54,
                  height: 54,
                  decoration: BoxDecoration(
                    color: sensor.color.withValues(alpha: 0.14),
                    borderRadius: BorderRadius.circular(18),
                  ),
                  child: Icon(sensor.icon, color: sensor.color, size: 30),
                ),
                const SizedBox(width: 16),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        sensor.name,
                        style: const TextStyle(
                          color: AppColors.textPrimary,
                          fontSize: 15,
                          fontWeight: FontWeight.w800,
                        ),
                      ),
                      const SizedBox(height: 6),
                      Text(
                        sensor.value,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: const TextStyle(
                          color: AppColors.textPrimary,
                          fontSize: 25,
                          fontWeight: FontWeight.w900,
                        ),
                      ),
                      if (sensor.subtitle != null) ...[
                        const SizedBox(height: 4),
                        Text(
                          sensor.subtitle!,
                          maxLines: 2,
                          style: const TextStyle(
                            color: AppColors.textSecondary,
                            fontSize: 12,
                            fontWeight: FontWeight.w600,
                          ),
                        ),
                      ],
                    ],
                  ),
                ),
                _StatusChip(
                  label: sensor.warning ? 'Warning' : 'Normal',
                  color: statusColor,
                ),
              ],
            ),
          ),
          Container(
            height: 4,
            decoration: BoxDecoration(
              color: statusColor,
              borderRadius: const BorderRadius.only(
                bottomLeft: Radius.circular(22),
                bottomRight: Radius.circular(22),
              ),
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
      padding: const EdgeInsets.symmetric(horizontal: 11, vertical: 7),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.12),
        borderRadius: BorderRadius.circular(999),
      ),
      child: Text(
        label,
        style: TextStyle(
          color: color,
          fontSize: 12,
          fontWeight: FontWeight.w800,
        ),
      ),
    );
  }
}

class _ConnectionStatusCard extends StatelessWidget {
  const _ConnectionStatusCard({
    required this.lastSync,
    required this.lastSyncEpoch,
    required this.audioOnline,
  });

  final String lastSync;
  final int lastSyncEpoch;
  final bool audioOnline;

  @override
  Widget build(BuildContext context) {
    // Previously this card always said "ESP32 Connected / Signal Strong",
    // regardless of state. It now reports the two boards separately, because
    // they fail independently: the controller can be online and publishing
    // while the audio board is dead, and the temperature would simply freeze.
    final controllerOnline =
        lastSyncEpoch > 0 &&
        DateTime.now()
                .difference(
                  DateTime.fromMillisecondsSinceEpoch(lastSyncEpoch * 1000),
                )
                .inSeconds <=
            120;

    return BeeGuardCard(
      padding: EdgeInsets.zero,
      child: Container(
        padding: const EdgeInsets.all(20),
        decoration: BoxDecoration(
          color: AppColors.textPrimary,
          borderRadius: BorderRadius.circular(22),
        ),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(
                  controllerOnline
                      ? Icons.wifi_rounded
                      : Icons.wifi_off_rounded,
                  color: const Color(0xFFFFC928),
                  size: 28,
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: Text(
                    controllerOnline
                        ? 'Hive controller connected'
                        : 'Hive controller not reporting',
                    style: const TextStyle(
                      color: Colors.white,
                      fontSize: 17,
                      fontWeight: FontWeight.w800,
                    ),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 16),
            _NodeRow(
              label: 'Main controller',
              online: controllerOnline,
            ),
            const SizedBox(height: 10),
            _NodeRow(
              label: 'Audio / DHT board',
              online: audioOnline,
            ),
            const SizedBox(height: 14),
            Text(
              lastSyncEpoch > 0
                  ? 'Last sync: ${timeAgo(lastSyncEpoch)}  ($lastSync)'
                  : 'Last sync: $lastSync',
              style: TextStyle(
                color: Colors.white.withValues(alpha: 0.72),
                fontSize: 12.5,
                fontWeight: FontWeight.w600,
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _NodeRow extends StatelessWidget {
  const _NodeRow({required this.label, required this.online});

  final String label;
  final bool online;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        _StatusDot(
          color: online ? AppColors.secondary : const Color(0xFFFF6F00),
          size: 10,
        ),
        const SizedBox(width: 8),
        Expanded(
          child: Text(
            label,
            style: const TextStyle(
              color: Colors.white,
              fontSize: 14,
              fontWeight: FontWeight.w700,
            ),
          ),
        ),
        Text(
          online ? 'Online' : 'Offline',
          style: TextStyle(
            color: online
                ? AppColors.secondary
                : const Color(0xFFFF6F00),
            fontSize: 13,
            fontWeight: FontWeight.w800,
          ),
        ),
      ],
    );
  }
}

class _StatusDot extends StatelessWidget {
  const _StatusDot({required this.color, required this.size});

  final Color color;
  final double size;

  @override
  Widget build(BuildContext context) {
    return Container(
      width: size,
      height: size,
      decoration: BoxDecoration(color: color, shape: BoxShape.circle),
    );
  }
}

class _SensorItem {
  const _SensorItem({
    required this.name,
    required this.value,
    required this.icon,
    required this.color,
    required this.warning,
    this.subtitle,
  });

  final String name;
  final String value;
  final IconData icon;
  final Color color;
  final bool warning;

  /// Extra line under the value: units, confidence, or why a reading is
  /// missing. Without it a stale number looks exactly like a live one.
  final String? subtitle;
}
