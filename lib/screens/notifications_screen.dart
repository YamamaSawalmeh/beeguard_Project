import 'dart:async';

import 'package:firebase_database/firebase_database.dart';
import 'package:flutter/material.dart';

import '../theme/app_colors.dart';
import '../utils/db_read.dart';
import '../widgets/beeguard_card.dart';

class NotificationsScreen extends StatefulWidget {
  const NotificationsScreen({super.key});

  @override
  State<NotificationsScreen> createState() => _NotificationsScreenState();
}

class _NotificationsScreenState extends State<NotificationsScreen> {
  static const Color honeyYellow = Color(0xFFFFC928);

  StreamSubscription<DatabaseEvent>? _subscription;
  Timer? _tick;
  List<_AlertEntry> alerts = const [];
  String selectedFilter = 'all';
  bool _loading = true;

  @override
  void initState() {
    super.initState();
    // Only the newest 100 events are pulled. The hive writes an entry for
    // every door move, feed, scan and reading, so over a season /alerts grows
    // into thousands of nodes — without the limit the app would download the
    // whole history on every change.
    _subscription = FirebaseDatabase.instance
        .ref('alerts')
        .limitToLast(100)
        .onValue
        .listen((event) {
          final data = asMap(event.snapshot.value);
          final next = data.entries.map((entry) {
            final row = asMap(entry.value);
            return _AlertEntry(
              id: entry.key.toString(),
              type: readString(row['type'], 'system').toLowerCase(),
              severity: readString(row['severity'], 'info').toLowerCase(),
              message: readString(row['message'], 'No message'),
              timestamp: readString(row['timestamp'], ''),
              epoch: readInt(row['epoch']),
              read: readBool(row['read']),
            );
          }).toList();

          // Sort by epoch rather than trusting key order: alerts written
          // before the ESP32 got its NTP sync carry a fallback key that would
          // otherwise sit in the wrong place.
          next.sort((a, b) => b.epoch.compareTo(a.epoch));

          if (!mounted) return;
          setState(() {
            alerts = next;
            _loading = false;
          });
        });

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

  Future<void> _markAllRead() async {
    final unread = alerts.where((a) => !a.read).toList();
    if (unread.isEmpty) return;

    // One multi-path update instead of one write per alert.
    final updates = <String, Object?>{
      for (final a in unread) '${a.id}/read': true,
    };
    await FirebaseDatabase.instance.ref('alerts').update(updates);

    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text('${unread.length} alerts marked as read')),
    );
  }

  List<_AlertEntry> get _filtered {
    switch (selectedFilter) {
      case 'all':
        return alerts;
      case 'unread':
        return alerts.where((a) => !a.read).toList();
      case 'critical':
        return alerts.where((a) => a.severity == 'critical').toList();
      default:
        final group = _AlertMeta.groupOf(selectedFilter);
        return alerts
            .where((a) => _AlertMeta.groupOf(a.type) == group)
            .toList();
    }
  }

  @override
  Widget build(BuildContext context) {
    final unreadCount = alerts.where((a) => !a.read).length;
    final filtered = _filtered;

    return Scaffold(
      appBar: AppBar(
        title: Row(
          children: [
            const Icon(Icons.notifications_rounded, color: honeyYellow),
            const SizedBox(width: 10),
            const Text('Alerts'),
            if (unreadCount > 0) ...[
              const SizedBox(width: 8),
              Container(
                padding: const EdgeInsets.symmetric(
                  horizontal: 8,
                  vertical: 3,
                ),
                decoration: BoxDecoration(
                  color: const Color(0xFFE53935),
                  borderRadius: BorderRadius.circular(999),
                ),
                child: Text(
                  '$unreadCount',
                  style: const TextStyle(
                    color: Colors.white,
                    fontSize: 12,
                    fontWeight: FontWeight.w900,
                  ),
                ),
              ),
            ],
          ],
        ),
        actions: [
          if (unreadCount > 0)
            IconButton(
              tooltip: 'Mark all as read',
              onPressed: _markAllRead,
              icon: const Icon(Icons.done_all_rounded),
            ),
          const SizedBox(width: 6),
        ],
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator(color: honeyYellow))
          : SafeArea(
              top: false,
              child: ListView(
                padding: const EdgeInsets.fromLTRB(20, 8, 20, 24),
                children: [
                  _FilterChips(
                    selected: selectedFilter,
                    unreadCount: unreadCount,
                    onSelected: (filter) =>
                        setState(() => selectedFilter = filter),
                  ),
                  const SizedBox(height: 18),
                  if (filtered.isEmpty)
                    _EmptyAlerts(filtered: selectedFilter != 'all')
                  else
                    ...filtered.map(
                      (alert) => Padding(
                        padding: const EdgeInsets.only(bottom: 12),
                        child: _AlertCard(alert: alert),
                      ),
                    ),
                ],
              ),
            ),
    );
  }
}

/// Presentation for every event type the firmware emits.
///
/// Kept in one place so adding an event on the ESP32 side means adding one row
/// here rather than editing three parallel switch statements. An unknown type
/// still renders sensibly instead of disappearing.
class _AlertMeta {
  const _AlertMeta(this.title, this.icon, this.color, this.group);

  final String title;
  final IconData icon;
  final Color color;
  final String group;

  static const Color _red = Color(0xFFE53935);
  static const Color _orange = Color(0xFFFF6F00);
  static const Color _yellow = Color(0xFFFFC928);
  static const Color _blue = Color(0xFF42A5F5);
  static const Color _brown = Color(0xFF8D6E63);

  static const Map<String, _AlertMeta> _table = {
    'temperature': _AlertMeta(
      'Temperature Alert',
      Icons.thermostat_rounded,
      _red,
      'climate',
    ),
    'humidity': _AlertMeta(
      'Humidity Alert',
      Icons.water_drop_rounded,
      _blue,
      'climate',
    ),
    'sound': _AlertMeta(
      'Sound Analysis',
      Icons.graphic_eq_rounded,
      _yellow,
      'climate',
    ),
    'water': _AlertMeta(
      'Feeding Level',
      Icons.local_drink_rounded,
      _blue,
      'feeding',
    ),
    'feeding': _AlertMeta('Feeding', Icons.water_rounded, _blue, 'feeding'),
    'hornet': _AlertMeta(
      'Hornet Detected',
      Icons.pest_control_rounded,
      _orange,
      'hornet',
    ),
    'hornet_clear': _AlertMeta(
      'Hornet Cleared',
      Icons.verified_rounded,
      AppColors.secondary,
      'hornet',
    ),
    'entrance_narrow': _AlertMeta(
      'Entrance Narrowed',
      Icons.door_sliding_rounded,
      _orange,
      'hornet',
    ),
    'entrance_open': _AlertMeta(
      'Entrance Opened',
      Icons.door_front_door_rounded,
      AppColors.secondary,
      'hornet',
    ),
    'rfid': _AlertMeta(
      'RFID Access',
      Icons.credit_card_rounded,
      _red,
      'security',
    ),
    'security': _AlertMeta('Security', Icons.lock_rounded, _red, 'security'),
    'door_opened': _AlertMeta(
      'Door Opened',
      Icons.door_sliding_rounded,
      _brown,
      'honey',
    ),
    'door_closed': _AlertMeta(
      'Door Closed',
      Icons.door_front_door_rounded,
      _brown,
      'honey',
    ),
    'smoke': _AlertMeta('Smoke Pump', Icons.air_rounded, _yellow, 'honey'),
    'harvest': _AlertMeta(
      'Harvest Recorded',
      Icons.inventory_2_rounded,
      _brown,
      'honey',
    ),
    'calibration': _AlertMeta(
      'Calibration',
      Icons.straighten_rounded,
      AppColors.secondary,
      'system',
    ),
    'sensor_fault': _AlertMeta(
      'Sensor Fault',
      Icons.report_problem_rounded,
      _orange,
      'system',
    ),
    'node_offline': _AlertMeta(
      'Node Offline',
      Icons.wifi_off_rounded,
      _orange,
      'system',
    ),
    'system': _AlertMeta('System', Icons.memory_rounded, _blue, 'system'),
  };

  static _AlertMeta of(String type) =>
      _table[type] ??
      const _AlertMeta(
        'Hive Alert',
        Icons.notifications_rounded,
        AppColors.secondary,
        'system',
      );

  // Named groupOf, not group: Dart keeps static and instance members in one
  // namespace, so a static `group(...)` would collide with the `group` field.
  static String groupOf(String type) => of(type).group;
}

class _FilterChips extends StatelessWidget {
  const _FilterChips({
    required this.selected,
    required this.unreadCount,
    required this.onSelected,
  });

  final String selected;
  final int unreadCount;
  final ValueChanged<String> onSelected;

  // Grouped rather than one chip per event type: the firmware now emits close
  // to twenty types, and a twenty-chip strip is not something anyone scrolls.
  static const filters = [
    _FilterItem('all', 'All', Icons.apps_rounded),
    _FilterItem('unread', 'Unread', Icons.mark_email_unread_rounded),
    _FilterItem('critical', 'Critical', Icons.priority_high_rounded),
    _FilterItem('hornet', 'Hornet', Icons.pest_control_rounded),
    _FilterItem('water', 'Feeding', Icons.local_drink_rounded),
    _FilterItem('temperature', 'Climate', Icons.thermostat_rounded),
    _FilterItem('rfid', 'Security', Icons.lock_rounded),
    _FilterItem('door_opened', 'Honey', Icons.hive_rounded),
    _FilterItem('system', 'System', Icons.memory_rounded),
  ];

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      height: 42,
      child: ListView.separated(
        scrollDirection: Axis.horizontal,
        itemCount: filters.length,
        separatorBuilder: (_, _) => const SizedBox(width: 8),
        itemBuilder: (context, index) {
          final filter = filters[index];
          final active = selected == filter.value;
          final showCount = filter.value == 'unread' && unreadCount > 0;

          return ChoiceChip(
            avatar: Icon(
              filter.icon,
              size: 18,
              color: active ? const Color(0xFF2B1A05) : AppColors.secondary,
            ),
            label: Text(
              showCount ? '${filter.label} ($unreadCount)' : filter.label,
            ),
            selected: active,
            selectedColor: const Color(0xFFFFC928),
            backgroundColor: AppColors.card,
            showCheckmark: false,
            side: BorderSide(
              color: active
                  ? const Color(0xFFFFC928)
                  : AppColors.textSecondary.withValues(alpha: 0.3),
            ),
            labelStyle: const TextStyle(
              color: AppColors.textPrimary,
              fontWeight: FontWeight.w800,
            ),
            onSelected: (_) => onSelected(filter.value),
          );
        },
      ),
    );
  }
}

class _AlertCard extends StatelessWidget {
  const _AlertCard({required this.alert});

  final _AlertEntry alert;

  @override
  Widget build(BuildContext context) {
    final meta = _AlertMeta.of(alert.type);
    final critical = alert.severity == 'critical';
    final color = critical ? const Color(0xFFE53935) : meta.color;

    return BeeGuardCard(
      padding: EdgeInsets.zero,
      onTap: alert.read
          ? null
          : () async {
              await FirebaseDatabase.instance
                  .ref('alerts/${alert.id}/read')
                  .set(true);
              if (context.mounted) {
                ScaffoldMessenger.of(context).showSnackBar(
                  const SnackBar(content: Text('Alert marked as read')),
                );
              }
            },
      child: Container(
        decoration: BoxDecoration(
          color: alert.read ? AppColors.card : const Color(0xFFFFF3B8),
          borderRadius: BorderRadius.circular(22),
        ),
        child: IntrinsicHeight(
          child: Row(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Container(
                width: 4,
                decoration: BoxDecoration(
                  color: color,
                  borderRadius: const BorderRadius.only(
                    topLeft: Radius.circular(22),
                    bottomLeft: Radius.circular(22),
                  ),
                ),
              ),
              Expanded(
                child: Padding(
                  padding: const EdgeInsets.all(16),
                  child: Row(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Container(
                        width: 44,
                        height: 44,
                        decoration: BoxDecoration(
                          color: color.withValues(alpha: 0.12),
                          borderRadius: BorderRadius.circular(14),
                        ),
                        child: Icon(meta.icon, color: color, size: 23),
                      ),
                      const SizedBox(width: 14),
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Row(
                              children: [
                                Flexible(
                                  child: Text(
                                    meta.title,
                                    style: const TextStyle(
                                      color: AppColors.textPrimary,
                                      fontSize: 16,
                                      fontWeight: FontWeight.w900,
                                    ),
                                  ),
                                ),
                                if (critical) ...[
                                  const SizedBox(width: 8),
                                  const _SeverityTag(
                                    label: 'CRITICAL',
                                    color: Color(0xFFE53935),
                                  ),
                                ] else if (alert.severity == 'warning') ...[
                                  const SizedBox(width: 8),
                                  const _SeverityTag(
                                    label: 'WARNING',
                                    color: Color(0xFFFF6F00),
                                  ),
                                ],
                              ],
                            ),
                            const SizedBox(height: 6),
                            Text(
                              alert.message,
                              style: const TextStyle(
                                color: AppColors.textSecondary,
                                fontSize: 13,
                                height: 1.35,
                                fontWeight: FontWeight.w600,
                              ),
                            ),
                            const SizedBox(height: 8),
                            Text(
                              alert.displayTime,
                              style: TextStyle(
                                color: AppColors.textSecondary.withValues(
                                  alpha: 0.8,
                                ),
                                fontSize: 12,
                                fontWeight: FontWeight.w700,
                              ),
                            ),
                          ],
                        ),
                      ),
                    ],
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _SeverityTag extends StatelessWidget {
  const _SeverityTag({required this.label, required this.color});

  final String label;
  final Color color;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.15),
        borderRadius: BorderRadius.circular(999),
      ),
      child: Text(
        label,
        style: TextStyle(
          color: color,
          fontSize: 9.5,
          fontWeight: FontWeight.w900,
          letterSpacing: 0.5,
        ),
      ),
    );
  }
}

class _EmptyAlerts extends StatelessWidget {
  const _EmptyAlerts({required this.filtered});

  final bool filtered;

  @override
  Widget build(BuildContext context) {
    return BeeGuardCard(
      padding: const EdgeInsets.all(28),
      child: Column(
        children: [
          const Icon(Icons.hive_rounded, color: Color(0xFFFFC928), size: 58),
          const SizedBox(height: 14),
          Text(
            filtered
                ? 'Nothing in this category.'
                : 'No alerts yet. Your hive is happy!',
            textAlign: TextAlign.center,
            style: const TextStyle(
              color: AppColors.textPrimary,
              fontSize: 17,
              fontWeight: FontWeight.w800,
            ),
          ),
        ],
      ),
    );
  }
}

class _AlertEntry {
  const _AlertEntry({
    required this.id,
    required this.type,
    required this.severity,
    required this.message,
    required this.timestamp,
    required this.epoch,
    required this.read,
  });

  final String id;
  final String type;
  final String severity;
  final String message;
  final String timestamp;
  final int epoch;
  final bool read;

  /// Elapsed time when the firmware supplied a real epoch, otherwise whatever
  /// string it wrote. Alerts raised before the ESP32's NTP sync completes have
  /// no usable epoch, and "56 years ago" would be worse than the raw text.
  String get displayTime {
    if (epoch > 0) return timeAgo(epoch);
    return timestamp.isEmpty ? 'Unknown time' : timestamp;
  }
}

class _FilterItem {
  const _FilterItem(this.value, this.label, this.icon);
  final String value;
  final String label;
  final IconData icon;
}
