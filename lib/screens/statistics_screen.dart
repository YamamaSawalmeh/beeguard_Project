import 'dart:async';

import 'package:firebase_database/firebase_database.dart';
import 'package:flutter/material.dart';

import '../theme/app_colors.dart';
import '../utils/db_read.dart';
import '../widgets/beeguard_card.dart';
import '../widgets/section_header.dart';

class StatisticsScreen extends StatefulWidget {
  const StatisticsScreen({super.key});

  @override
  State<StatisticsScreen> createState() => _StatisticsScreenState();
}

class _StatisticsScreenState extends State<StatisticsScreen> {
  StreamSubscription<DatabaseEvent>? _hiveSubscription;
  StreamSubscription<DatabaseEvent>? _productionSubscription;

  double currentHoneyWeight = 0;
  double estimatedHoney = 0;
  double todayProduction = 0;
  double weeklyProduction = 0;
  double monthlyProduction = 0;
  double totalHarvested = 0;
  List<_DayPoint> weekPoints = const [];
  bool _loading = true;

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
            currentHoneyWeight = readDouble(data['weight']);
            _loading = false;
          });
        });
    _productionSubscription = FirebaseDatabase.instance
        .ref('production')
        .onValue
        .listen((event) {
          final data = asMap(event.snapshot.value);
          if (!mounted) return;
          setState(() {
            // Fall back to weight above a ~4 kg baseline so the headline is
            // never a bare 0 before the controller writes estimated_honey.
            final espEstimate = readDouble(data['estimated_honey']);
            final baseline = readDouble(data['baseline_weight']) > 0
                ? readDouble(data['baseline_weight'])
                : 4.0;
            estimatedHoney = espEstimate > 0
                ? espEstimate
                : (currentHoneyWeight > baseline
                      ? (currentHoneyWeight - baseline) * 0.85
                      : 0);
            todayProduction = readDouble(data['today_production']);
            weeklyProduction = readDouble(data['weekly_production']);
            monthlyProduction = readDouble(data['monthly_production']);
            totalHarvested = readDouble(data['total_harvested']);
            weekPoints = _buildWeek(
              asMap(data['history']),
              readDouble(data['today_production']),
            );
          });
        });
  }

  /// Last seven calendar days from /production/history, which the ESP writes
  /// one entry per day keyed "YYYY-MM-DD".
  ///
  /// The previous version had no history to read, so it drew
  /// `[today*0.5, today*0.7, today*0.6, ...]` — a fixed wave that always had
  /// the same shape regardless of what the hive did. Days with no recorded
  /// entry are now plotted as a genuine zero.
  List<_DayPoint> _buildWeek(Map<dynamic, dynamic> history, double today) {
    const labels = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];
    final now = DateTime.now();
    final points = <_DayPoint>[];

    for (var i = 6; i >= 0; i--) {
      final day = DateTime(now.year, now.month, now.day - i);
      final key =
          '${day.year.toString().padLeft(4, '0')}-'
          '${day.month.toString().padLeft(2, '0')}-'
          '${day.day.toString().padLeft(2, '0')}';

      double value;
      if (i == 0) {
        // Today has not been rolled into history yet — it is still running.
        value = today;
      } else {
        value = readDouble(asMap(history[key])['gain']);
      }
      points.add(_DayPoint(labels[day.weekday - 1], value));
    }
    return points;
  }

  @override
  void dispose() {
    _hiveSubscription?.cancel();
    _productionSubscription?.cancel();
    super.dispose();
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

    final hasHistory = weekPoints.any((p) => p.value > 0);

    return Scaffold(
      appBar: AppBar(title: const Text('Honey Production')),
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
                      color: const Color(0xFF8D6E63).withValues(alpha: 0.12),
                      borderRadius: BorderRadius.circular(18),
                    ),
                    child: const Icon(
                      Icons.scale_rounded,
                      color: Color(0xFF8D6E63),
                      size: 32,
                    ),
                  ),
                  const SizedBox(width: 16),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        const Text(
                          'Estimated Honey Ready',
                          style: TextStyle(
                            color: AppColors.textSecondary,
                            fontWeight: FontWeight.w700,
                          ),
                        ),
                        const SizedBox(height: 6),
                        Text(
                          '${estimatedHoney.toStringAsFixed(2)} kg',
                          style: const TextStyle(
                            color: AppColors.textPrimary,
                            fontSize: 28,
                            fontWeight: FontWeight.w900,
                          ),
                        ),
                        const SizedBox(height: 4),
                        Text(
                          'Hive on scale: ${currentHoneyWeight.toStringAsFixed(1)} kg',
                          style: const TextStyle(
                            color: AppColors.textSecondary,
                            fontSize: 12.5,
                            fontWeight: FontWeight.w600,
                          ),
                        ),
                      ],
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 18),
            _StatsGrid(
              items: [
                _StatItem('Today', todayProduction),
                _StatItem('This Week', weeklyProduction),
                _StatItem('This Month', monthlyProduction),
              ],
            ),
            if (totalHarvested > 0) ...[
              const SizedBox(height: 14),
              BeeGuardCard(
                child: Row(
                  children: [
                    const Icon(
                      Icons.inventory_2_rounded,
                      color: Color(0xFF8D6E63),
                    ),
                    const SizedBox(width: 14),
                    const Expanded(
                      child: Text(
                        'Total harvested',
                        style: TextStyle(
                          color: AppColors.textPrimary,
                          fontSize: 15,
                          fontWeight: FontWeight.w800,
                        ),
                      ),
                    ),
                    Text(
                      '${totalHarvested.toStringAsFixed(2)} kg',
                      style: const TextStyle(
                        color: AppColors.textPrimary,
                        fontSize: 18,
                        fontWeight: FontWeight.w900,
                      ),
                    ),
                  ],
                ),
              ),
            ],
            const SizedBox(height: 24),
            const SectionHeader(
              title: 'Last Seven Days',
              subtitle: 'Daily weight gain recorded by the hive scale',
            ),
            const SizedBox(height: 12),
            BeeGuardCard(
              padding: const EdgeInsets.all(18),
              child: hasHistory
                  ? SizedBox(
                      height: 200,
                      child: CustomPaint(
                        painter: _LineChartPainter(points: weekPoints),
                        child: const SizedBox.expand(),
                      ),
                    )
                  : const _NoHistoryYet(),
            ),
          ],
        ),
      ),
    );
  }
}

class _NoHistoryYet extends StatelessWidget {
  const _NoHistoryYet();

  @override
  Widget build(BuildContext context) {
    return const Padding(
      padding: EdgeInsets.symmetric(vertical: 26),
      child: Column(
        children: [
          Icon(Icons.show_chart_rounded, color: Color(0xFFFFC928), size: 44),
          SizedBox(height: 12),
          Text(
            'No daily history yet',
            style: TextStyle(
              color: AppColors.textPrimary,
              fontSize: 16,
              fontWeight: FontWeight.w800,
            ),
          ),
          SizedBox(height: 6),
          Text(
            'The hive records one entry per day. The chart fills in as the '
            'days pass.',
            textAlign: TextAlign.center,
            style: TextStyle(
              color: AppColors.textSecondary,
              fontSize: 13,
              height: 1.35,
              fontWeight: FontWeight.w600,
            ),
          ),
        ],
      ),
    );
  }
}

class _DayPoint {
  const _DayPoint(this.label, this.value);
  final String label;
  final double value;
}

class _StatsGrid extends StatelessWidget {
  const _StatsGrid({required this.items});

  final List<_StatItem> items;

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        final width = (constraints.maxWidth - 28) / 3;
        return Wrap(
          spacing: 14,
          runSpacing: 14,
          children: items
              .map((item) => SizedBox(width: width, child: _StatCard(item)))
              .toList(),
        );
      },
    );
  }
}

class _StatCard extends StatelessWidget {
  const _StatCard(this.item);

  final _StatItem item;

  @override
  Widget build(BuildContext context) {
    return BeeGuardCard(
      padding: const EdgeInsets.all(14),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Icon(Icons.inventory_2_rounded, color: AppColors.secondary),
          const SizedBox(height: 14),
          Text(
            '${item.value.toStringAsFixed(1)} kg',
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: const TextStyle(
              color: AppColors.textPrimary,
              fontSize: 18,
              fontWeight: FontWeight.w900,
            ),
          ),
          const SizedBox(height: 4),
          Text(
            item.title,
            style: const TextStyle(
              color: AppColors.textSecondary,
              fontSize: 12,
              fontWeight: FontWeight.w700,
            ),
          ),
        ],
      ),
    );
  }
}

class _LineChartPainter extends CustomPainter {
  const _LineChartPainter({required this.points});

  final List<_DayPoint> points;

  static const double _labelBand = 22; // reserved for the weekday row
  static const double _topPad = 10;

  @override
  void paint(Canvas canvas, Size size) {
    if (points.isEmpty) return;

    final plotHeight = size.height - _labelBand - _topPad;
    if (plotHeight <= 0) return;

    final axisPaint = Paint()
      ..color = AppColors.textSecondary.withValues(alpha: 0.18)
      ..strokeWidth = 1;
    final linePaint = Paint()
      ..color = AppColors.secondary
      ..strokeWidth = 3
      ..style = PaintingStyle.stroke
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round;
    final dotPaint = Paint()..color = AppColors.secondary;
    final fillPaint = Paint()
      ..color = AppColors.secondary.withValues(alpha: 0.12)
      ..style = PaintingStyle.fill;

    for (var i = 0; i <= 4; i++) {
      final y = _topPad + plotHeight * i / 4;
      canvas.drawLine(Offset(0, y), Offset(size.width, y), axisPaint);
    }

    // Scale to the tallest bar, with a floor so a week of near-zero gains does
    // not get amplified into a dramatic-looking chart.
    var maxValue = 0.0;
    for (final p in points) {
      if (p.value > maxValue) maxValue = p.value;
    }
    if (maxValue < 0.5) maxValue = 0.5;

    final dx = points.length > 1 ? size.width / (points.length - 1) : 0.0;
    final offsets = <Offset>[];
    for (var i = 0; i < points.length; i++) {
      final x = points.length > 1 ? i * dx : size.width / 2;
      final y = _topPad + plotHeight - (points[i].value / maxValue) * plotHeight;
      offsets.add(Offset(x, y));
    }

    final area = Path()..moveTo(offsets.first.dx, _topPad + plotHeight);
    for (final o in offsets) {
      area.lineTo(o.dx, o.dy);
    }
    area
      ..lineTo(offsets.last.dx, _topPad + plotHeight)
      ..close();
    canvas.drawPath(area, fillPaint);

    final line = Path()..moveTo(offsets.first.dx, offsets.first.dy);
    for (var i = 1; i < offsets.length; i++) {
      line.lineTo(offsets[i].dx, offsets[i].dy);
    }
    canvas.drawPath(line, linePaint);

    for (final o in offsets) {
      canvas.drawCircle(o, 4, dotPaint);
    }

    for (var i = 0; i < points.length; i++) {
      final painter = TextPainter(
        text: TextSpan(
          text: points[i].label,
          style: const TextStyle(
            color: AppColors.textSecondary,
            fontSize: 11,
            fontWeight: FontWeight.w700,
          ),
        ),
        textDirection: TextDirection.ltr,
      )..layout();

      // Clamp so the first and last labels stay inside the canvas instead of
      // being clipped at the edges.
      final cx = (offsets[i].dx - painter.width / 2).clamp(
        0.0,
        size.width - painter.width,
      );
      painter.paint(canvas, Offset(cx, size.height - _labelBand + 6));
    }
  }

  @override
  bool shouldRepaint(covariant _LineChartPainter oldDelegate) {
    if (oldDelegate.points.length != points.length) return true;
    for (var i = 0; i < points.length; i++) {
      if (oldDelegate.points[i].value != points[i].value) return true;
      if (oldDelegate.points[i].label != points[i].label) return true;
    }
    return false;
  }
}

class _StatItem {
  const _StatItem(this.title, this.value);

  final String title;
  final double value;
}
