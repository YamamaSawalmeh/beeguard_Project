import 'dart:async';

import 'package:firebase_database/firebase_database.dart';
import 'package:flutter/material.dart';

import '../theme/app_colors.dart';
import '../utils/db_read.dart';
import '../widgets/beeguard_card.dart';

class HiveDetailsScreen extends StatefulWidget {
  const HiveDetailsScreen({super.key});

  @override
  State<HiveDetailsScreen> createState() => _HiveDetailsScreenState();
}

class _HiveDetailsScreenState extends State<HiveDetailsScreen> {
  StreamSubscription<DatabaseEvent>? _hiveSubscription;
  StreamSubscription<DatabaseEvent>? _commandsSubscription;
  StreamSubscription<DatabaseEvent>? _productionSubscription;

  double hiveWeight = 0;
  double estimatedHoney = 0;
  double baselineWeight = 0;
  double todayProduction = 0;
  double lastHarvestKg = 0;
  String lastHarvestTime = '';
  bool smokePumpRunning = false;
  bool doorOpen = false;
  bool _loading = true;
  bool _collecting = false;
  int _step = 0;
  bool _busy = false;

  @override
  void initState() {
    super.initState();
    _hiveSubscription = FirebaseDatabase.instance
        .ref('hive_status')
        .onValue
        .listen((event) {
          final data = asMap(event.snapshot.value);
          if (!mounted) return;
          setState(() {
            hiveWeight = readDouble(data['weight']);
            // The ESP echoes the physical door position here.
            doorOpen = readBool(data['door_open']);
            _loading = false;
          });
        });
    _commandsSubscription = FirebaseDatabase.instance
        .ref('commands')
        .onValue
        .listen((event) {
          final data = asMap(event.snapshot.value);
          if (!mounted) return;
          setState(() {
            smokePumpRunning = readBool(data['smoke_pump']);
          });
        });
    _productionSubscription = FirebaseDatabase.instance
        .ref('production')
        .onValue
        .listen((event) {
          final data = asMap(event.snapshot.value);
          if (!mounted) return;
          setState(() {
            // Default to ~4 kg (empty box) when no baseline is set, so the
            // estimate shows a number rather than a dash. Set Baseline can
            // still record a real, measured value later.
            final rawBaseline = readDouble(data['baseline_weight']);
            baselineWeight = rawBaseline > 0 ? rawBaseline : 4.0;
            todayProduction = readDouble(data['today_production']);
            lastHarvestKg = readDouble(data['last_harvest_kg']);
            lastHarvestTime = readString(data['last_harvest_time'], '');
            // Prefer the ESP's figure. It applies the honey fraction from
            // /calibration, so recalculating here would drift from the value
            // shown everywhere else the moment that fraction is tuned.
            estimatedHoney = readDouble(data['estimated_honey']);
          });
        });
  }

  @override
  void dispose() {
    _hiveSubscription?.cancel();
    _commandsSubscription?.cancel();
    _productionSubscription?.cancel();
    super.dispose();
  }

  // Tell the ESP to treat the current weight as "no harvestable honey".
  // Without a baseline the estimate is meaningless — it would report the
  // weight of the boxes, frames and bees as honey.
  Future<void> _setBaseline() async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Set honey baseline?'),
        content: Text(
          'The hive currently weighs ${hiveWeight.toStringAsFixed(1)} kg.\n\n'
          'Everything above this weight from now on will be counted as '
          'harvestable honey. Do this when the hive has no honey to collect.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context, false),
            child: const Text('Cancel'),
          ),
          FilledButton(
            onPressed: () => Navigator.pop(context, true),
            child: const Text('Set Baseline'),
          ),
        ],
      ),
    );
    if (confirmed != true) return;

    await FirebaseDatabase.instance.ref('commands/set_baseline').set(true);
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('Baseline will update on the next reading')),
    );
  }

  // Start collection: open the door and run the smoke pump.
  // The door is intentionally LEFT OPEN — there is no automatic closing step.
  // The ESP handles the physical sequence (open door -> 4s smoke -> smoke off).
  Future<void> _collectHoney() async {
    if (_busy || doorOpen) return;
    setState(() {
      _busy = true;
      _collecting = true;
      _step = 0;
    });

    await FirebaseDatabase.instance.ref('commands/collect_honey').set(true);

    // Local timeline just mirrors the ESP sequence for visual feedback.
    await Future<void>.delayed(const Duration(seconds: 2));
    if (mounted) setState(() => _step = 1); // smoke running
    await Future<void>.delayed(const Duration(seconds: 4));
    if (mounted) {
      setState(() {
        _collecting = false;
        _busy = false;
      });
    }
  }

  // Manual close (the automatic closing step was removed by request).
  Future<void> _closeDoor() async {
    if (_busy) return;
    setState(() => _busy = true);
    await FirebaseDatabase.instance.ref('commands/collect_honey').set(false);
    await Future<void>.delayed(const Duration(seconds: 2));
    if (mounted) {
      setState(() {
        _busy = false;
        _collecting = false;
        _step = 0;
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

    return Scaffold(
      appBar: AppBar(title: const Text('Honey Collection')),
      body: SafeArea(
        top: false,
        child: ListView(
          padding: const EdgeInsets.fromLTRB(20, 8, 20, 24),
          children: [
            const Text(
              'Honey Collection',
              style: TextStyle(
                color: AppColors.textPrimary,
                fontSize: 28,
                fontWeight: FontWeight.w900,
              ),
            ),
            const SizedBox(height: 18),
            _StatusCard(
              icon: doorOpen
                  ? Icons.door_sliding_rounded
                  : Icons.door_front_door_rounded,
              title: 'Door Status',
              value: _busy && !doorOpen
                  ? 'Opening'
                  : _busy && doorOpen
                  ? 'Closing'
                  : (doorOpen ? 'Open' : 'Closed'),
              color: doorOpen ? AppColors.secondary : AppColors.textSecondary,
            ),
            const SizedBox(height: 14),
            _StatusCard(
              icon: Icons.air_rounded,
              title: 'Smoke Pump',
              value: smokePumpRunning ? 'Running' : 'Stopped',
              color: smokePumpRunning
                  ? const Color(0xFFFF6F00)
                  : AppColors.textSecondary,
            ),
            const SizedBox(height: 18),
            BeeGuardCard(
              child: Column(
                children: [
                  _ProcessStep(
                    active: _collecting && _step == 0,
                    done: doorOpen || (_collecting && _step > 0),
                    title: 'Opening Door',
                  ),
                  const _ProcessDivider(),
                  _ProcessStep(
                    active: _collecting && _step == 1,
                    done: false,
                    title: 'Smoke Pump Running (4s)',
                  ),
                ],
              ),
            ),
            const SizedBox(height: 18),
            SizedBox(
              height: 56,
              // Toggle button: opens for collection, or closes the door manually.
              // There is no automatic closing step anymore — the door stays open
              // after collection until the farmer taps "Close Door".
              child: doorOpen
                  ? ElevatedButton.icon(
                      onPressed: _busy ? null : _closeDoor,
                      style: ElevatedButton.styleFrom(
                        backgroundColor: const Color(0xFF8D6E63),
                        foregroundColor: Colors.white,
                        shape: RoundedRectangleBorder(
                          borderRadius: BorderRadius.circular(16),
                        ),
                      ),
                      icon: _busy
                          ? const SizedBox(
                              width: 18,
                              height: 18,
                              child: CircularProgressIndicator(
                                strokeWidth: 2,
                                color: Colors.white,
                              ),
                            )
                          : const Icon(Icons.door_front_door_rounded),
                      label: Text(_busy ? 'Closing Door' : 'Close Door'),
                    )
                  : ElevatedButton.icon(
                      onPressed: _busy ? null : _collectHoney,
                      style: ElevatedButton.styleFrom(
                        backgroundColor: AppColors.textPrimary,
                        foregroundColor: Colors.white,
                        shape: RoundedRectangleBorder(
                          borderRadius: BorderRadius.circular(16),
                        ),
                      ),
                      icon: _collecting
                          ? const SizedBox(
                              width: 18,
                              height: 18,
                              child: CircularProgressIndicator(
                                strokeWidth: 2,
                                color: Colors.white,
                              ),
                            )
                          : const Icon(Icons.inventory_2_rounded),
                      label: Text(
                        _collecting ? 'Collecting Honey' : 'Collect Honey',
                      ),
                    ),
            ),
            const SizedBox(height: 18),

            // This card used to read "Current Honey Weight" but showed
            // hive_status/weight — the whole hive on the load cell: boxes,
            // frames, brood and bees included. On a healthy colony that is
            // 20-plus kg, none of which can be collected. The headline figure
            // is now the estimate of what is actually harvestable, with the
            // raw scale reading kept underneath as supporting detail.
            _HoneyEstimateCard(
              // Prefer the ESP's figure; fall back to weight above baseline so
              // the card is never blank before the controller writes one.
              estimatedHoney: estimatedHoney > 0
                  ? estimatedHoney
                  : (hiveWeight > baselineWeight
                        ? (hiveWeight - baselineWeight) * 0.85
                        : 0),
              hiveWeight: hiveWeight,
              baselineWeight: baselineWeight,
              todayProduction: todayProduction,
              onSetBaseline: _setBaseline,
            ),

            if (lastHarvestKg > 0) ...[
              const SizedBox(height: 14),
              BeeGuardCard(
                child: Row(
                  children: [
                    const Icon(
                      Icons.inventory_2_rounded,
                      color: Color(0xFF8D6E63),
                      size: 26,
                    ),
                    const SizedBox(width: 14),
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Text(
                            'Last harvest: ${lastHarvestKg.toStringAsFixed(2)} kg',
                            style: const TextStyle(
                              color: AppColors.textPrimary,
                              fontSize: 15,
                              fontWeight: FontWeight.w800,
                            ),
                          ),
                          if (lastHarvestTime.isNotEmpty) ...[
                            const SizedBox(height: 4),
                            Text(
                              lastHarvestTime,
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
                  ],
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }
}

class _HoneyEstimateCard extends StatelessWidget {
  const _HoneyEstimateCard({
    required this.estimatedHoney,
    required this.hiveWeight,
    required this.baselineWeight,
    required this.todayProduction,
    required this.onSetBaseline,
  });

  final double estimatedHoney;
  final double hiveWeight;
  final double baselineWeight;
  final double todayProduction;
  final VoidCallback onSetBaseline;

  @override
  Widget build(BuildContext context) {
    // With no baseline the estimate would be the full hive weight, which is
    // badly wrong rather than merely imprecise — so prompt instead of showing
    // a confident-looking number.
    final needsBaseline = baselineWeight <= 0;

    return BeeGuardCard(
      padding: const EdgeInsets.all(20),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Container(
                width: 52,
                height: 52,
                decoration: BoxDecoration(
                  color: const Color(0xFFFFC928).withValues(alpha: 0.18),
                  borderRadius: BorderRadius.circular(16),
                ),
                child: const Icon(
                  Icons.hive_rounded,
                  color: Color(0xFF8D6E63),
                  size: 28,
                ),
              ),
              const SizedBox(width: 15),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const Text(
                      'Estimated Honey Ready',
                      style: TextStyle(
                        color: AppColors.textSecondary,
                        fontSize: 13,
                        fontWeight: FontWeight.w800,
                      ),
                    ),
                    const SizedBox(height: 5),
                    Text(
                      needsBaseline
                          ? '--'
                          : '${estimatedHoney.toStringAsFixed(2)} kg',
                      style: const TextStyle(
                        color: AppColors.textPrimary,
                        fontSize: 30,
                        fontWeight: FontWeight.w900,
                      ),
                    ),
                  ],
                ),
              ),
            ],
          ),
          const SizedBox(height: 16),
          const Divider(height: 1),
          const SizedBox(height: 14),
          _DetailRow(
            label: 'Hive weight on the scale',
            value: '${hiveWeight.toStringAsFixed(1)} kg',
          ),
          const SizedBox(height: 9),
          _DetailRow(
            label: 'Baseline (empty of honey)',
            value: needsBaseline
                ? 'not set'
                : '${baselineWeight.toStringAsFixed(1)} kg',
          ),
          const SizedBox(height: 9),
          _DetailRow(
            label: 'Gained today',
            value: '${todayProduction >= 0 ? '+' : ''}'
                '${todayProduction.toStringAsFixed(2)} kg',
            highlight: todayProduction > 0,
          ),
          if (needsBaseline) ...[
            const SizedBox(height: 14),
            Container(
              padding: const EdgeInsets.all(13),
              decoration: BoxDecoration(
                color: const Color(0xFFFF6F00).withValues(alpha: 0.09),
                borderRadius: BorderRadius.circular(14),
              ),
              child: const Text(
                'Set a baseline so the estimate counts only new stores, not '
                'the weight of the hive itself.',
                style: TextStyle(
                  color: AppColors.textSecondary,
                  fontSize: 12.5,
                  height: 1.35,
                  fontWeight: FontWeight.w600,
                ),
              ),
            ),
          ],
          const SizedBox(height: 12),
          Align(
            alignment: Alignment.centerLeft,
            child: TextButton.icon(
              onPressed: onSetBaseline,
              icon: const Icon(Icons.straighten_rounded, size: 18),
              label: Text(
                needsBaseline ? 'Set baseline now' : 'Reset baseline',
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _DetailRow extends StatelessWidget {
  const _DetailRow({
    required this.label,
    required this.value,
    this.highlight = false,
  });

  final String label;
  final String value;
  final bool highlight;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Expanded(
          child: Text(
            label,
            style: const TextStyle(
              color: AppColors.textSecondary,
              fontSize: 13.5,
              fontWeight: FontWeight.w600,
            ),
          ),
        ),
        Text(
          value,
          style: TextStyle(
            color: highlight ? AppColors.secondary : AppColors.textPrimary,
            fontSize: 14.5,
            fontWeight: FontWeight.w900,
          ),
        ),
      ],
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

class _ProcessStep extends StatelessWidget {
  const _ProcessStep({
    required this.active,
    required this.done,
    required this.title,
  });

  final bool active;
  final bool done;
  final String title;

  @override
  Widget build(BuildContext context) {
    final color = done || active
        ? AppColors.secondary
        : AppColors.textSecondary;
    return Row(
      children: [
        if (active)
          const SizedBox(
            width: 24,
            height: 24,
            child: CircularProgressIndicator(
              strokeWidth: 2,
              color: AppColors.secondary,
            ),
          )
        else
          Icon(
            done ? Icons.check_circle_rounded : Icons.circle_outlined,
            color: color,
          ),
        const SizedBox(width: 14),
        Expanded(
          child: Text(
            title,
            style: TextStyle(
              color: color,
              fontSize: 15,
              fontWeight: FontWeight.w800,
            ),
          ),
        ),
      ],
    );
  }
}

class _ProcessDivider extends StatelessWidget {
  const _ProcessDivider();

  @override
  Widget build(BuildContext context) {
    return const Padding(
      padding: EdgeInsets.only(left: 11, top: 8, bottom: 8),
      child: Align(
        alignment: Alignment.centerLeft,
        child: SizedBox(
          height: 18,
          child: VerticalDivider(width: 1, thickness: 1),
        ),
      ),
    );
  }
}
