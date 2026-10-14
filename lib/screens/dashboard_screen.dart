import 'dart:async';

import 'package:firebase_database/firebase_database.dart';
import 'package:flutter/material.dart';

import '../theme/app_colors.dart';
import '../utils/app_routes.dart';
import '../widgets/beeguard_card.dart';
import '../widgets/section_header.dart';

class DashboardScreen extends StatefulWidget {
  const DashboardScreen({super.key});

  @override
  State<DashboardScreen> createState() => _DashboardScreenState();
}

class _DashboardScreenState extends State<DashboardScreen> {
  StreamSubscription<DatabaseEvent>? _hiveSubscription;
  StreamSubscription<DatabaseEvent>? _commandsSubscription;
  StreamSubscription<DatabaseEvent>? _alertsSubscription;

  double temperature = 0;
  double humidity = 0;
  double feedingLevel = 0;
  int healthScore = 0;
  bool hiveLocked = true;
  int unreadAlerts = 0;
  bool _loading = true;

  @override
  void initState() {
    super.initState();
    _hiveSubscription = FirebaseDatabase.instance
        .ref('hive_status')
        .onValue
        .listen((event) {
          final data = _asMap(event.snapshot.value);
          if (!mounted) return;
          setState(() {
            temperature = _readDouble(data['temperature']);
            humidity = _readDouble(data['humidity']);
            feedingLevel = _readDouble(data['water_level']);
            healthScore = _readInt(data['health_score']);
            _loading = false;
          });
        });
    _commandsSubscription = FirebaseDatabase.instance
        .ref('security')
        .onValue
        .listen((event) {
          final data = _asMap(event.snapshot.value);
          if (!mounted) return;
          setState(() {
            hiveLocked = data['locked'] == true;
          });
        });

    // Unread count for the bell badge. Limited to the recent window so the
    // dashboard never downloads a whole season of events just to draw a number.
    _alertsSubscription = FirebaseDatabase.instance
        .ref('alerts')
        .limitToLast(50)
        .onValue
        .listen((event) {
          final data = _asMap(event.snapshot.value);
          var unread = 0;
          for (final entry in data.entries) {
            if (_asMap(entry.value)['read'] != true) unread++;
          }
          if (!mounted) return;
          setState(() => unreadAlerts = unread);
        });
  }

  @override
  void dispose() {
    _hiveSubscription?.cancel();
    _commandsSubscription?.cancel();
    _alertsSubscription?.cancel();
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

    final status = _hiveStatus;
    final statusColor = _statusColor(status);
    final now = DateTime.now();

    final infoItems = [
      _InfoItem(
        icon: Icons.thermostat_rounded,
        title: 'Temperature',
        value: '${temperature.toStringAsFixed(0)} C',
        color: const Color(0xFFE53935),
      ),
      _InfoItem(
        icon: Icons.water_drop_rounded,
        title: 'Humidity',
        value: '${humidity.toStringAsFixed(0)}%',
        color: const Color(0xFF42A5F5),
      ),
      _InfoItem(
        icon: hiveLocked ? Icons.lock_rounded : Icons.lock_open_rounded,
        title: 'Hive Lock',
        value: hiveLocked ? 'Locked' : 'Unlocked',
        color: hiveLocked ? const Color(0xFFE53935) : AppColors.secondary,
      ),
      _InfoItem(
        icon: Icons.local_drink_rounded,
        title: 'Feeding Level',
        value: '${feedingLevel.toStringAsFixed(0)}%',
        color: feedingLevel < 20
            ? const Color(0xFFFF6F00)
            : const Color(0xFF42A5F5),
      ),
    ];

    return Scaffold(
      appBar: AppBar(
        title: const Text('Dashboard'),
        actions: [
          IconButton(
            tooltip: 'Notifications',
            onPressed: () =>
                Navigator.pushNamed(context, AppRoutes.notifications),
            icon: Badge.count(
              count: unreadAlerts,
              isLabelVisible: unreadAlerts > 0,
              backgroundColor: const Color(0xFFE53935),
              child: Icon(
                unreadAlerts > 0
                    ? Icons.notifications_active_rounded
                    : Icons.notifications_none_rounded,
              ),
            ),
          ),
          IconButton(
            tooltip: 'Profile',
            onPressed: () => Navigator.pushNamed(context, AppRoutes.profile),
            icon: const Icon(Icons.account_circle_outlined),
          ),
          const SizedBox(width: 8),
        ],
      ),
      body: SafeArea(
        top: false,
        child: ListView(
          padding: const EdgeInsets.fromLTRB(20, 8, 20, 24),
          children: [
            _DashboardHeader(now: now),
            const SizedBox(height: 18),
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
                      status == 'Healthy'
                          ? Icons.check_circle_rounded
                          : Icons.warning_rounded,
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
                          'Hive Status',
                          style: TextStyle(
                            color: AppColors.textSecondary,
                            fontWeight: FontWeight.w700,
                          ),
                        ),
                        const SizedBox(height: 6),
                        Text(
                          status,
                          style: TextStyle(
                            color: statusColor,
                            fontSize: 26,
                            fontWeight: FontWeight.w900,
                          ),
                        ),
                      ],
                    ),
                  ),
                  Text(
                    '$healthScore%',
                    style: const TextStyle(
                      color: AppColors.textPrimary,
                      fontSize: 18,
                      fontWeight: FontWeight.w900,
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 20),
            _InfoGrid(items: infoItems),
            const SizedBox(height: 28),
            const SectionHeader(
              title: 'Recent Alerts',
              subtitle: 'Latest important events',
            ),
            const SizedBox(height: 12),
            const _AlertRow(
              icon: Icons.local_drink_rounded,
              title: 'Low Feeding Level',
              time: '5 min ago',
              color: Color(0xFFFF6F00),
            ),
            const _AlertRow(
              icon: Icons.pest_control_rounded,
              title: 'Hornet Detected',
              time: '20 min ago',
              color: Color(0xFFE53935),
            ),
            const _AlertRow(
              icon: Icons.credit_card_rounded,
              title: 'RFID Access Granted',
              time: '50 min ago',
              color: AppColors.secondary,
            ),
            const SizedBox(height: 24),
            const SectionHeader(
              title: 'Quick Actions',
              subtitle: 'Primary hive operations',
            ),
            const SizedBox(height: 12),
            const _QuickActionsGrid(),
          ],
        ),
      ),
    );
  }

  String get _hiveStatus {
    if (healthScore < 50 || feedingLevel < 10) return 'Critical';
    if (healthScore < 70 || feedingLevel < 20 || temperature > 36) {
      return 'Warning';
    }
    return 'Healthy';
  }

  Color _statusColor(String status) {
    switch (status) {
      case 'Critical':
        return const Color(0xFFE53935);
      case 'Warning':
        return const Color(0xFFFF6F00);
      default:
        return AppColors.secondary;
    }
  }

  Map<dynamic, dynamic> _asMap(Object? value) {
    if (value is Map) return value;
    return {};
  }

  double _readDouble(Object? value) {
    if (value is num) return value.toDouble();
    return double.tryParse(value?.toString() ?? '') ?? 0;
  }

  int _readInt(Object? value) {
    if (value is num) return value.toInt();
    return int.tryParse(value?.toString() ?? '') ?? 0;
  }
}

class _DashboardHeader extends StatelessWidget {
  const _DashboardHeader({required this.now});

  final DateTime now;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(18),
      decoration: BoxDecoration(
        color: const Color(0xFFFFF7D1),
        borderRadius: BorderRadius.circular(22),
        border: Border.all(color: const Color(0xFFFFC928), width: 1.2),
      ),
      child: Row(
        children: [
          Container(
            width: 54,
            height: 54,
            alignment: Alignment.center,
            decoration: BoxDecoration(
              color: const Color(0xFFFFC928),
              borderRadius: BorderRadius.circular(18),
            ),
            child: Stack(
              alignment: Alignment.center,
              children: const [
                Icon(Icons.hexagon_rounded, color: Color(0xFFFFF7D6), size: 38),
                Icon(Icons.hive_rounded, color: Color(0xFF8A5A00), size: 27),
              ],
            ),
          ),
          const SizedBox(width: 14),
          const Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  'BeeGuard Hive 01',
                  style: TextStyle(
                    color: AppColors.textPrimary,
                    fontSize: 23,
                    fontWeight: FontWeight.w900,
                  ),
                ),
                SizedBox(height: 5),
                Text(
                  'Smart Beehive Monitoring',
                  style: TextStyle(
                    color: Color(0xFF8A5A00),
                    fontWeight: FontWeight.w800,
                  ),
                ),
              ],
            ),
          ),
          Text(
            '${now.month}/${now.day}/${now.year}\n${now.hour.toString().padLeft(2, '0')}:${now.minute.toString().padLeft(2, '0')}',
            textAlign: TextAlign.right,
            style: const TextStyle(
              color: AppColors.textSecondary,
              fontSize: 12,
              fontWeight: FontWeight.w700,
              height: 1.4,
            ),
          ),
        ],
      ),
    );
  }
}

class _InfoGrid extends StatelessWidget {
  const _InfoGrid({required this.items});

  final List<_InfoItem> items;

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        final width = (constraints.maxWidth - 14) / 2;
        return Wrap(
          spacing: 14,
          runSpacing: 14,
          children: items
              .map((item) => SizedBox(width: width, child: _InfoCard(item)))
              .toList(),
        );
      },
    );
  }
}

class _InfoCard extends StatelessWidget {
  const _InfoCard(this.item);

  final _InfoItem item;

  @override
  Widget build(BuildContext context) {
    return BeeGuardCard(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Icon(item.icon, color: item.color, size: 28),
          const SizedBox(height: 18),
          Text(
            item.value,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: const TextStyle(
              color: AppColors.textPrimary,
              fontSize: 22,
              fontWeight: FontWeight.w900,
            ),
          ),
          const SizedBox(height: 4),
          Text(
            item.title,
            style: const TextStyle(
              color: AppColors.textSecondary,
              fontSize: 13,
              fontWeight: FontWeight.w700,
            ),
          ),
        ],
      ),
    );
  }
}

class _AlertRow extends StatelessWidget {
  const _AlertRow({
    required this.icon,
    required this.title,
    required this.time,
    required this.color,
  });

  final IconData icon;
  final String title;
  final String time;
  final Color color;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 10),
      child: BeeGuardCard(
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
        child: Row(
          children: [
            Icon(icon, color: color, size: 24),
            const SizedBox(width: 14),
            Expanded(
              child: Text(
                title,
                style: Theme.of(context).textTheme.titleMedium,
              ),
            ),
            Text(
              time,
              style: const TextStyle(
                color: AppColors.textSecondary,
                fontSize: 12,
                fontWeight: FontWeight.w700,
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _QuickActionsGrid extends StatelessWidget {
  const _QuickActionsGrid();

  static const actions = [
    _ActionItem(
      icon: Icons.shield_rounded,
      title: 'Security',
      description: 'RFID and lock status',
      routeName: AppRoutes.security,
      color: AppColors.secondary,
    ),
    _ActionItem(
      icon: Icons.graphic_eq_rounded,
      title: 'AI Status',
      description: 'TinyML sound analysis',
      routeName: AppRoutes.aiStatus,
      color: Color(0xFF7B1FA2),
    ),
    _ActionItem(
      icon: Icons.inventory_2_rounded,
      title: 'Honey Collection',
      description: 'Door and smoke process',
      routeName: AppRoutes.hiveDetails,
      color: Color(0xFF8D6E63),
    ),
    _ActionItem(
      icon: Icons.local_drink_rounded,
      title: 'Feeding',
      description: 'Solution level and pump',
      routeName: AppRoutes.control,
      color: Color(0xFF42A5F5),
    ),
    _ActionItem(
      icon: Icons.pest_control_rounded,
      title: 'Hornet Detection',
      description: 'Detection and entrance servo',
      routeName: AppRoutes.hornetDetection,
      color: Color(0xFFFF6F00),
    ),
  ];

  @override
  Widget build(BuildContext context) {
    return Column(
      children: actions
          .map(
            (action) => Padding(
              padding: const EdgeInsets.only(bottom: 12),
              child: BeeGuardCard(
                padding: const EdgeInsets.all(16),
                onTap: () => Navigator.pushNamed(context, action.routeName),
                child: Row(
                  children: [
                    Container(
                      width: 46,
                      height: 46,
                      decoration: BoxDecoration(
                        color: action.color.withValues(alpha: 0.12),
                        borderRadius: BorderRadius.circular(16),
                      ),
                      child: Icon(action.icon, color: action.color),
                    ),
                    const SizedBox(width: 14),
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Text(
                            action.title,
                            style: Theme.of(context).textTheme.titleMedium,
                          ),
                          const SizedBox(height: 4),
                          Text(
                            action.description,
                            style: const TextStyle(
                              color: AppColors.textSecondary,
                              fontSize: 13,
                              fontWeight: FontWeight.w600,
                            ),
                          ),
                        ],
                      ),
                    ),
                    const Icon(
                      Icons.arrow_forward_ios_rounded,
                      color: AppColors.textSecondary,
                      size: 16,
                    ),
                  ],
                ),
              ),
            ),
          )
          .toList(),
    );
  }
}

class _InfoItem {
  const _InfoItem({
    required this.icon,
    required this.title,
    required this.value,
    required this.color,
  });

  final IconData icon;
  final String title;
  final String value;
  final Color color;
}

class _ActionItem {
  const _ActionItem({
    required this.icon,
    required this.title,
    required this.description,
    required this.routeName,
    required this.color,
  });

  final IconData icon;
  final String title;
  final String description;
  final String routeName;
  final Color color;
}
