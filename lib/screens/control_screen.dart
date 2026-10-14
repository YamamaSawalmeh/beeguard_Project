import 'dart:async';

import 'package:firebase_database/firebase_database.dart';
import 'package:flutter/material.dart';

import '../theme/app_colors.dart';
import '../widgets/beeguard_card.dart';

class ControlScreen extends StatefulWidget {
  const ControlScreen({super.key});

  @override
  State<ControlScreen> createState() => _ControlScreenState();
}

class _ControlScreenState extends State<ControlScreen> {
  StreamSubscription<DatabaseEvent>? _commandsSubscription;
  StreamSubscription<DatabaseEvent>? _hiveSubscription;

  bool pumpRunning = false;
  double feedingLevel = 0;
  double waterHeight = 0;
  double remainingMl = 0;
  bool sensorOk = true;
  bool _loading = true;
  bool _feeding = false;
  int _feedingMl = 0;

  // Volume -> pump run time. 50 ml ≈ 6 s (measured); 100 ml ≈ 12 s (linear).
  // The ESP performs the real timing; this only drives the UI spinner.
  static const Map<int, int> _feedSeconds = {50: 6, 100: 12};

  @override
  void initState() {
    super.initState();
    _commandsSubscription = FirebaseDatabase.instance
        .ref('commands/pump')
        .onValue
        .listen((event) {
          if (!mounted) return;
          setState(() => pumpRunning = event.snapshot.value == true);
        });
    _hiveSubscription = FirebaseDatabase.instance
        .ref('hive_status')
        .onValue
        .listen((event) {
          final data = _asMap(event.snapshot.value);
          if (!mounted) return;
          setState(() {
            feedingLevel = _readDouble(data['water_level']);
            waterHeight = _readDouble(
              data['water_height_cm'] ?? data['water_height'],
            );
            remainingMl = _readDouble(data['water_remaining_ml']);
            // Absent on firmware that predates the sensor-fault flag; treat
            // that as healthy rather than showing a fault that isn't there.
            sensorOk = data['water_sensor_ok'] != false;
            _loading = false;
          });
        });
  }

  @override
  void dispose() {
    _commandsSubscription?.cancel();
    _hiveSubscription?.cancel();
    super.dispose();
  }

  // Dispense a fixed volume. We only write the requested amount; the ESP runs
  // the pump for the mapped time and resets commands/feed_ml back to 0 itself.
  Future<void> _feed(int ml) async {
    if (_feeding) return;
    setState(() {
      _feeding = true;
      _feedingMl = ml;
    });
    await FirebaseDatabase.instance.ref('commands/feed_ml').set(ml);
    final seconds = _feedSeconds[ml] ?? 6;
    await Future<void>.delayed(Duration(seconds: seconds));
    if (mounted) {
      setState(() {
        _feeding = false;
        _feedingMl = 0;
      });
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

    final level = feedingLevel.clamp(0, 100).toDouble();
    final warning = level < 20;
    final critical = level < 10;

    return Scaffold(
      appBar: AppBar(title: const Text('Feeding')),
      body: SafeArea(
        top: false,
        child: ListView(
          padding: const EdgeInsets.fromLTRB(20, 8, 20, 24),
          children: [
            const Text(
              'Feeding Solution',
              style: TextStyle(
                color: AppColors.textPrimary,
                fontSize: 28,
                fontWeight: FontWeight.w900,
              ),
            ),
            const SizedBox(height: 18),
            BeeGuardCard(
              padding: const EdgeInsets.all(24),
              child: Column(
                children: [
                  SizedBox(
                    width: 180,
                    height: 180,
                    child: Stack(
                      fit: StackFit.expand,
                      children: [
                        CircularProgressIndicator(
                          value: level / 100,
                          strokeWidth: 14,
                          backgroundColor: const Color(
                            0xFF42A5F5,
                          ).withValues(alpha: 0.14),
                          color: critical
                              ? const Color(0xFFE53935)
                              : warning
                              ? const Color(0xFFFF6F00)
                              : const Color(0xFF42A5F5),
                        ),
                        Center(
                          child: Column(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              const Text(
                                'Feeding Level',
                                style: TextStyle(
                                  color: AppColors.textSecondary,
                                  fontWeight: FontWeight.w700,
                                ),
                              ),
                              const SizedBox(height: 6),
                              Text(
                                '${level.toStringAsFixed(0)}%',
                                style: const TextStyle(
                                  color: AppColors.textPrimary,
                                  fontSize: 34,
                                  fontWeight: FontWeight.w900,
                                ),
                              ),
                            ],
                          ),
                        ),
                      ],
                    ),
                  ),
                  const SizedBox(height: 22),
                  // Volume is what actually decides whether another dose fits;
                  // a percentage of a 3.5 cm tank does not tell you if there
                  // is enough left for a 100 ml feed.
                  Text(
                    remainingMl > 0
                        ? '${remainingMl.toStringAsFixed(0)} ml remaining'
                        : 'Remaining height: ${waterHeight.toStringAsFixed(1)} cm',
                    style: const TextStyle(
                      color: AppColors.textPrimary,
                      fontSize: 18,
                      fontWeight: FontWeight.w900,
                    ),
                  ),
                  if (remainingMl > 0) ...[
                    const SizedBox(height: 5),
                    Text(
                      'Depth ${waterHeight.toStringAsFixed(1)} cm',
                      style: const TextStyle(
                        color: AppColors.textSecondary,
                        fontSize: 13,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                  ],
                ],
              ),
            ),
            const SizedBox(height: 16),
            if (!sensorOk) ...[
              const _LevelNotice(
                icon: Icons.sensors_off_rounded,
                title: 'Level sensor not responding',
                message:
                    'The ultrasonic sensor stopped answering, so the level '
                    'shown is the last good reading. Check its wiring.',
                color: Color(0xFFE53935),
              ),
              const SizedBox(height: 16),
            ],
            _StatusCard(
              icon: Icons.water_rounded,
              title: 'Pump Status',
              value: pumpRunning ? 'Running' : 'Stopped',
              color: pumpRunning
                  ? AppColors.secondary
                  : AppColors.textSecondary,
            ),
            const SizedBox(height: 20),
            const Text(
              'Feed Bees',
              style: TextStyle(
                color: AppColors.textPrimary,
                fontSize: 18,
                fontWeight: FontWeight.w900,
              ),
            ),
            const SizedBox(height: 4),
            const Text(
              'Choose how much solution to dispense',
              style: TextStyle(
                color: AppColors.textSecondary,
                fontWeight: FontWeight.w600,
              ),
            ),
            const SizedBox(height: 12),
            Row(
              children: [
                Expanded(
                  child: _FeedButton(
                    ml: 50,
                    seconds: _feedSeconds[50] ?? 6,
                    busy: _feeding,
                    isActive: _feeding && _feedingMl == 50,
                    onTap: () => _feed(50),
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: _FeedButton(
                    ml: 100,
                    seconds: _feedSeconds[100] ?? 12,
                    busy: _feeding,
                    isActive: _feeding && _feedingMl == 100,
                    onTap: () => _feed(100),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 16),
            if (critical)
              const _LevelNotice(
                icon: Icons.error_rounded,
                title: 'Critical Feeding Level',
                message: 'Refill the feeding container immediately.',
                color: Color(0xFFE53935),
              )
            else if (warning)
              const _LevelNotice(
                icon: Icons.warning_rounded,
                title: 'Low Feeding Level',
                message: 'Refill is recommended soon.',
                color: Color(0xFFFF6F00),
              ),
          ],
        ),
      ),
    );
  }

  Map<dynamic, dynamic> _asMap(Object? value) {
    if (value is Map) return value;
    return {};
  }

  double _readDouble(Object? value) {
    if (value is num) return value.toDouble();
    return double.tryParse(value?.toString() ?? '') ?? 0;
  }
}

class _FeedButton extends StatelessWidget {
  const _FeedButton({
    required this.ml,
    required this.seconds,
    required this.busy,
    required this.isActive,
    required this.onTap,
  });

  final int ml;
  final int seconds;
  final bool busy; // any feed currently running
  final bool isActive; // this specific volume is the one running
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      height: 66,
      child: ElevatedButton(
        onPressed: busy ? null : onTap,
        style: ElevatedButton.styleFrom(
          backgroundColor: AppColors.textPrimary,
          foregroundColor: Colors.white,
          disabledBackgroundColor: AppColors.textPrimary.withValues(alpha: 0.4),
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(16),
          ),
        ),
        child: isActive
            ? const Row(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  SizedBox(
                    width: 18,
                    height: 18,
                    child: CircularProgressIndicator(
                      strokeWidth: 2,
                      color: Colors.white,
                    ),
                  ),
                  SizedBox(width: 10),
                  Text(
                    'Feeding...',
                    style: TextStyle(fontWeight: FontWeight.w800),
                  ),
                ],
              )
            : Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  Text(
                    'Feed $ml ml',
                    style: const TextStyle(
                      fontSize: 17,
                      fontWeight: FontWeight.w900,
                    ),
                  ),
                  const SizedBox(height: 2),
                  Text(
                    '~$seconds s',
                    style: const TextStyle(
                      fontSize: 12,
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                ],
              ),
      ),
    );
  }
}

class _StatusCard extends StatelessWidget {
  const _StatusCard({
    required this.icon,
    required this.title,
    required this.value,
    required this.color,
  });

  final IconData icon;
  final String title;
  final String value;
  final Color color;

  @override
  Widget build(BuildContext context) {
    return BeeGuardCard(
      child: Row(
        children: [
          Icon(icon, color: color, size: 30),
          const SizedBox(width: 14),
          Expanded(
            child: Text(title, style: Theme.of(context).textTheme.titleMedium),
          ),
          Text(
            value,
            style: TextStyle(color: color, fontWeight: FontWeight.w900),
          ),
        ],
      ),
    );
  }
}

class _LevelNotice extends StatelessWidget {
  const _LevelNotice({
    required this.icon,
    required this.title,
    required this.message,
    required this.color,
  });

  final IconData icon;
  final String title;
  final String message;
  final Color color;

  @override
  Widget build(BuildContext context) {
    return BeeGuardCard(
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Icon(icon, color: color),
          const SizedBox(width: 14),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  title,
                  style: TextStyle(
                    color: color,
                    fontSize: 16,
                    fontWeight: FontWeight.w900,
                  ),
                ),
                const SizedBox(height: 5),
                Text(
                  message,
                  style: const TextStyle(
                    color: AppColors.textSecondary,
                    fontWeight: FontWeight.w600,
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
