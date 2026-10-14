import 'dart:async';

import 'package:firebase_database/firebase_database.dart';
import 'package:flutter/material.dart';

import '../theme/app_colors.dart';
import '../widgets/beeguard_card.dart';

class AiStatusScreen extends StatefulWidget {
  const AiStatusScreen({super.key});

  @override
  State<AiStatusScreen> createState() => _AiStatusScreenState();
}

class _AiStatusScreenState extends State<AiStatusScreen> {
  StreamSubscription<DatabaseEvent>? _statusSubscription;
  StreamSubscription<DatabaseEvent>? _historySubscription;

  bool _loading = true;
  String soundResult = 'Unknown';
  int confidence = 0;
  String lastAnalysis = 'No analysis yet';
  String modelVersion = 'Unknown model';
  List<_HistoryEntry> history = const [];

  @override
  void initState() {
    super.initState();
    _statusSubscription = FirebaseDatabase.instance
        .ref('ai_status')
        .onValue
        .listen((event) {
          final data = _asMap(event.snapshot.value);
          if (!mounted) return;

          setState(() {
            soundResult = data['sound_result']?.toString() ?? 'Unknown';
            confidence = _readInt(data['confidence']);
            lastAnalysis =
                data['last_analysis']?.toString() ?? 'No analysis yet';
            modelVersion = data['model_version']?.toString() ?? 'Unknown model';
            _loading = false;
          });
        });

    // The audio board now classifies automatically every 15 minutes, so
    // ai_history gains ~96 entries a day. Reading the whole node would mean
    // re-downloading a growing archive on every change just to show three
    // rows; limitToLast keeps it to the newest handful.
    _historySubscription = FirebaseDatabase.instance
        .ref('ai_history')
        .limitToLast(10)
        .onValue
        .listen((event) {
          final data = _asMap(event.snapshot.value);
          final entries = data.entries.map((entry) {
            final row = _asMap(entry.value);
            return _HistoryEntry(
              time: row['time']?.toString() ?? 'Unknown time',
              result: row['result']?.toString() ?? 'Unknown',
              confidence: _readInt(row['confidence']),
            );
          }).toList();

          if (!mounted) return;
          setState(() => history = entries.reversed.take(3).toList());
        });
  }

  @override
  void dispose() {
    _statusSubscription?.cancel();
    _historySubscription?.cancel();
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

    final color = _resultColor(soundResult);

    return Scaffold(
      appBar: AppBar(title: const Text('AI Status')),
      body: SafeArea(
        top: false,
        child: ListView(
          padding: const EdgeInsets.fromLTRB(20, 8, 20, 24),
          children: [
            _SoundAnalysisCard(
              result: soundResult,
              confidence: confidence,
              color: color,
            ),
            const SizedBox(height: 18),
            _LastAnalysisCard(
              lastAnalysis: lastAnalysis,
              modelVersion: modelVersion,
            ),
            const SizedBox(height: 18),
            _HistoryCard(history: history),
          ],
        ),
      ),
    );
  }

  Color _resultColor(String result) {
    final normalized = result.toLowerCase();
    if (normalized == 'normal') return AppColors.secondary;
    if (normalized == 'swarming') return const Color(0xFFFFC928);
    if (normalized == 'queen loss') return const Color(0xFFE53935);
    return AppColors.textSecondary;
  }

  Map<dynamic, dynamic> _asMap(Object? value) {
    if (value is Map) return value;
    return {};
  }

  int _readInt(Object? value) {
    if (value is num) return value.toInt();
    return int.tryParse(value?.toString() ?? '') ?? 0;
  }
}

class _SoundAnalysisCard extends StatelessWidget {
  const _SoundAnalysisCard({
    required this.result,
    required this.confidence,
    required this.color,
  });

  final String result;
  final int confidence;
  final Color color;

  @override
  Widget build(BuildContext context) {
    return BeeGuardCard(
      padding: const EdgeInsets.all(24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.center,
        children: [
          ClipPath(
            clipper: _HexagonClipper(),
            child: Container(
              width: 92,
              height: 92,
              color: const Color(0xFFFFC928),
              child: const Icon(
                Icons.hive_rounded,
                color: AppColors.textPrimary,
                size: 50,
              ),
            ),
          ),
          const SizedBox(height: 20),
          const Text(
            'Sound Analysis Result',
            textAlign: TextAlign.center,
            style: TextStyle(
              color: AppColors.textPrimary,
              fontSize: 17,
              fontWeight: FontWeight.w700,
            ),
          ),
          const SizedBox(height: 10),
          Text(
            result,
            textAlign: TextAlign.center,
            style: TextStyle(
              color: color,
              fontSize: 30,
              fontWeight: FontWeight.w900,
            ),
          ),
          const SizedBox(height: 18),
          ClipRRect(
            borderRadius: BorderRadius.circular(999),
            child: LinearProgressIndicator(
              value: (confidence.clamp(0, 100)) / 100,
              minHeight: 10,
              backgroundColor: const Color(0xFFFFC928).withValues(alpha: 0.22),
              color: color,
            ),
          ),
          const SizedBox(height: 10),
          Text(
            'Confidence: $confidence%',
            style: const TextStyle(
              color: AppColors.textSecondary,
              fontSize: 14,
              fontWeight: FontWeight.w700,
            ),
          ),
          const SizedBox(height: 18),
          _StatusChip(label: result.toUpperCase(), color: color),
        ],
      ),
    );
  }
}

class _LastAnalysisCard extends StatelessWidget {
  const _LastAnalysisCard({
    required this.lastAnalysis,
    required this.modelVersion,
  });

  final String lastAnalysis;
  final String modelVersion;

  @override
  Widget build(BuildContext context) {
    return BeeGuardCard(
      padding: const EdgeInsets.all(18),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Container(
            width: 48,
            height: 48,
            decoration: BoxDecoration(
              color: const Color(0xFFFFC928).withValues(alpha: 0.18),
              borderRadius: BorderRadius.circular(16),
            ),
            child: const Icon(
              Icons.access_time_rounded,
              color: AppColors.textPrimary,
              size: 26,
            ),
          ),
          const SizedBox(width: 14),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  'Last Analysis: $lastAnalysis',
                  style: const TextStyle(
                    color: AppColors.textPrimary,
                    fontSize: 15,
                    fontWeight: FontWeight.w800,
                  ),
                ),
                const SizedBox(height: 8),
                Text(
                  'Model: $modelVersion',
                  style: const TextStyle(
                    color: AppColors.textSecondary,
                    fontSize: 14,
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

class _HistoryCard extends StatelessWidget {
  const _HistoryCard({required this.history});

  final List<_HistoryEntry> history;

  @override
  Widget build(BuildContext context) {
    // No placeholder rows here. This card used to fall back to three
    // hard-coded results (13:00 Normal 94%, ...) whenever history was empty,
    // which is indistinguishable from real analysis output — a hive whose
    // audio board was offline looked like one reporting a healthy colony.
    return BeeGuardCard(
      padding: const EdgeInsets.all(18),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text(
            'Recent AI Results',
            style: TextStyle(
              color: AppColors.textPrimary,
              fontSize: 18,
              fontWeight: FontWeight.w800,
            ),
          ),
          const SizedBox(height: 14),
          if (history.isEmpty)
            const Padding(
              padding: EdgeInsets.symmetric(vertical: 10),
              child: Text(
                'No analysis recorded yet. The hive analyses its sound '
                'automatically every 15 minutes.',
                style: TextStyle(
                  color: AppColors.textSecondary,
                  fontSize: 13.5,
                  height: 1.4,
                  fontWeight: FontWeight.w600,
                ),
              ),
            )
          else
            for (var i = 0; i < history.length; i++) ...[
              _HistoryRow(entry: history[i]),
              if (i < history.length - 1) const Divider(height: 22),
            ],
        ],
      ),
    );
  }
}

class _HistoryRow extends StatelessWidget {
  const _HistoryRow({required this.entry});

  final _HistoryEntry entry;

  @override
  Widget build(BuildContext context) {
    final color = entry.result.toLowerCase() == 'normal'
        ? AppColors.secondary
        : const Color(0xFFFFC928);

    return Row(
      children: [
        SizedBox(
          width: 72,
          child: Text(
            entry.time,
            style: const TextStyle(
              color: AppColors.textSecondary,
              fontSize: 13,
              fontWeight: FontWeight.w700,
            ),
          ),
        ),
        Expanded(
          child: Text(
            entry.result,
            style: TextStyle(
              color: color,
              fontSize: 15,
              fontWeight: FontWeight.w800,
            ),
          ),
        ),
        Text(
          '${entry.confidence}%',
          style: const TextStyle(
            color: AppColors.textPrimary,
            fontSize: 15,
            fontWeight: FontWeight.w800,
          ),
        ),
      ],
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
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 9),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.14),
        borderRadius: BorderRadius.circular(999),
      ),
      child: Text(
        label,
        style: TextStyle(
          color: color,
          fontSize: 13,
          fontWeight: FontWeight.w900,
        ),
      ),
    );
  }
}

class _HexagonClipper extends CustomClipper<Path> {
  @override
  Path getClip(Size size) {
    final w = size.width;
    final h = size.height;
    return Path()
      ..moveTo(w * 0.5, 0)
      ..lineTo(w, h * 0.25)
      ..lineTo(w, h * 0.75)
      ..lineTo(w * 0.5, h)
      ..lineTo(0, h * 0.75)
      ..lineTo(0, h * 0.25)
      ..close();
  }

  @override
  bool shouldReclip(covariant CustomClipper<Path> oldClipper) => false;
}

class _HistoryEntry {
  const _HistoryEntry({
    required this.time,
    required this.result,
    required this.confidence,
  });

  final String time;
  final String result;
  final int confidence;
}
