import 'dart:async';

import 'package:firebase_database/firebase_database.dart';
import 'package:flutter/material.dart';

import '../theme/app_colors.dart';
import '../utils/db_read.dart';
import '../widgets/beeguard_card.dart';
import '../widgets/section_header.dart';

class SecurityScreen extends StatefulWidget {
  const SecurityScreen({super.key});

  @override
  State<SecurityScreen> createState() => _SecurityScreenState();
}

class _SecurityScreenState extends State<SecurityScreen> {
  StreamSubscription<DatabaseEvent>? _logsSubscription;
  StreamSubscription<DatabaseEvent>? _lockSubscription;

  List<_RfidLog> logs = const [];
  bool hiveLocked = true;
  String rfidStatus = 'Waiting for Card';

  @override
  void initState() {
    super.initState();
    // Only the last five rows are displayed, so fetch only what is shown.
    // /rfid_logs grows by one node per card scan and is never pruned; reading
    // the whole node meant re-downloading every scan ever made on each update.
    _logsSubscription = FirebaseDatabase.instance
        .ref('rfid_logs')
        .limitToLast(5)
        .onValue
        .listen((event) {
          final data = _asMap(event.snapshot.value);
          final nextLogs = data.entries.map((entry) {
            final row = _asMap(entry.value);
            // The firmware writes a single "timestamp" ("2026-08-20 14:33:10")
            // plus an "epoch". There is no separate "date" field, so the old
            // code fell back to the literal word "Today" on every row —
            // including entries from last week.
            final epoch = row['epoch'] is num
                ? (row['epoch'] as num).toInt()
                : 0;
            return _RfidLog(
              when: epoch > 0
                  ? timeAgo(epoch)
                  : (row['timestamp']?.toString() ??
                        row['time']?.toString() ??
                        'Unknown time'),
              stamp: row['timestamp']?.toString() ?? '',
              action: row['action']?.toString() ?? '',
              status: row['access']?.toString() ?? 'denied',
              cardId: row['card_id']?.toString() ?? 'Unknown',
            );
          }).toList();
          if (!mounted) return;
          setState(() => logs = nextLogs.reversed.take(5).toList());
        });

    // Hive lock + RFID status are owned by the ESP under /security.
    _lockSubscription = FirebaseDatabase.instance
        .ref('security')
        .onValue
        .listen((event) {
          final data = _asMap(event.snapshot.value);
          if (!mounted) return;
          setState(() {
            hiveLocked = data['locked'] == true;
            rfidStatus = data['rfid_status']?.toString() ?? rfidStatus;
          });
        });
  }

  @override
  void dispose() {
    _logsSubscription?.cancel();
    _lockSubscription?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final latestStatus = rfidStatus == 'granted'
        ? 'Access Granted'
        : rfidStatus == 'denied'
        ? 'Access Denied'
        : (logs.isNotEmpty && logs.first.status == 'granted')
        ? 'Access Granted'
        : 'Waiting for Card';

    return Scaffold(
      appBar: AppBar(title: const Text('Security')),
      body: SafeArea(
        top: false,
        child: ListView(
          padding: const EdgeInsets.fromLTRB(20, 8, 20, 24),
          children: [
            const Text(
              'Security System',
              style: TextStyle(
                color: AppColors.textPrimary,
                fontSize: 28,
                fontWeight: FontWeight.w900,
              ),
            ),
            const SizedBox(height: 18),
            _StatusCard(
              icon: Icons.credit_card_rounded,
              title: 'RFID Status',
              value: latestStatus,
              color: latestStatus == 'Access Denied'
                  ? const Color(0xFFE53935)
                  : AppColors.secondary,
            ),
            const SizedBox(height: 14),
            _StatusCard(
              icon: hiveLocked ? Icons.lock_rounded : Icons.lock_open_rounded,
              title: 'Hive Lock',
              value: hiveLocked ? 'Locked' : 'Unlocked',
              color: hiveLocked ? const Color(0xFFE53935) : AppColors.secondary,
            ),
            const SizedBox(height: 24),
            const SectionHeader(
              title: 'Recent Access History',
              subtitle: 'Latest RFID scans',
            ),
            const SizedBox(height: 12),
            if (logs.isEmpty)
              const BeeGuardCard(
                child: Text(
                  'No RFID scans yet.',
                  style: TextStyle(fontWeight: FontWeight.w700),
                ),
              )
            else
              ...logs.map(
                (log) => Padding(
                  padding: const EdgeInsets.only(bottom: 10),
                  child: _AccessRow(log: log),
                ),
              ),
            const SizedBox(height: 14),
            const BeeGuardCard(
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Icon(Icons.info_outline_rounded, color: AppColors.secondary),
                  SizedBox(width: 14),
                  Expanded(
                    child: Text(
                      'Scan an RFID card to unlock the hive. Scan again to lock it.',
                      style: TextStyle(
                        color: AppColors.textPrimary,
                        fontSize: 15,
                        fontWeight: FontWeight.w700,
                        height: 1.4,
                      ),
                    ),
                  ),
                ],
              ),
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
      padding: const EdgeInsets.all(18),
      child: Row(
        children: [
          Container(
            width: 50,
            height: 50,
            decoration: BoxDecoration(
              color: color.withValues(alpha: 0.12),
              borderRadius: BorderRadius.circular(16),
            ),
            child: Icon(icon, color: color),
          ),
          const SizedBox(width: 14),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  title,
                  style: const TextStyle(
                    color: AppColors.textSecondary,
                    fontWeight: FontWeight.w700,
                  ),
                ),
                const SizedBox(height: 5),
                Text(
                  value,
                  style: TextStyle(
                    color: color,
                    fontSize: 22,
                    fontWeight: FontWeight.w900,
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

class _AccessRow extends StatelessWidget {
  const _AccessRow({required this.log});

  final _RfidLog log;

  @override
  Widget build(BuildContext context) {
    final granted = log.status == 'granted';
    final color = granted ? AppColors.secondary : const Color(0xFFE53935);

    return BeeGuardCard(
      padding: const EdgeInsets.all(16),
      child: Row(
        children: [
          Icon(
            granted ? Icons.check_circle_rounded : Icons.cancel_rounded,
            color: color,
          ),
          const SizedBox(width: 14),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  'Card #${log.cardId}',
                  style: Theme.of(context).textTheme.titleMedium,
                ),
                const SizedBox(height: 4),
                Text(
                  log.stamp.isEmpty ? log.when : '${log.when}  -  ${log.stamp}',
                  style: const TextStyle(
                    color: AppColors.textSecondary,
                    fontSize: 12,
                    fontWeight: FontWeight.w700,
                  ),
                ),
              ],
            ),
          ),
          Column(
            crossAxisAlignment: CrossAxisAlignment.end,
            children: [
              Text(
                granted ? 'Granted' : 'Denied',
                style: TextStyle(color: color, fontWeight: FontWeight.w900),
              ),
              if (log.action.isNotEmpty) ...[
                const SizedBox(height: 3),
                Text(
                  // Any card toggles the lock, so which way it went is the
                  // part the beekeeper actually needs from the log.
                  log.action == 'locked' ? 'Locked' : 'Unlocked',
                  style: const TextStyle(
                    color: AppColors.textSecondary,
                    fontSize: 11,
                    fontWeight: FontWeight.w700,
                  ),
                ),
              ],
            ],
          ),
        ],
      ),
    );
  }
}

class _RfidLog {
  const _RfidLog({
    required this.when,
    required this.stamp,
    required this.action,
    required this.status,
    required this.cardId,
  });

  /// Elapsed time, e.g. "5 min ago".
  final String when;

  /// Absolute stamp from the firmware, e.g. "2026-08-20 14:33:10".
  final String stamp;

  /// "locked" or "unlocked" — which way this scan moved the lock.
  final String action;

  final String status;
  final String cardId;
}
