import 'package:firebase_core/firebase_core.dart';
import 'package:firebase_database/firebase_database.dart';
import 'package:flutter/material.dart';

import 'firebase_options.dart';
import 'screens/ai_status_screen.dart';
import 'screens/control_screen.dart';
import 'screens/dashboard_screen.dart';
import 'screens/hive_details_screen.dart';
import 'screens/hornet_detection_screen.dart';
import 'screens/live_monitoring_screen.dart';
import 'screens/login_screen.dart';
import 'screens/notifications_screen.dart';
import 'screens/profile_screen.dart';
import 'screens/security_screen.dart';
import 'screens/settings_screen.dart';
import 'screens/splash_screen.dart';
import 'screens/statistics_screen.dart';
import 'theme/app_theme.dart';
import 'utils/app_routes.dart';

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await _initializeFirebase();
  runApp(const BeeGuardApp());
}

Future<void> _initializeFirebase() async {
  try {
    await Firebase.initializeApp(
      options: DefaultFirebaseOptions.currentPlatform,
    );
    await _seedInitialData();
  } catch (error, stackTrace) {
    // The app is still usable offline (screens show their empty states), so a
    // Firebase problem should surface in the log rather than kill startup.
    debugPrint('Firebase startup skipped: $error');
    debugPrintStack(stackTrace: stackTrace);
  }
}

/// Writes the node skeleton on a brand-new database so screens have something
/// to bind to before the ESP32 first reports.
///
/// This is structure, not fake readings: seeding a plausible temperature would
/// show a hive that is not connected as a healthy one. Everything the hardware
/// measures starts empty and is filled in by the controller. Runs only when
/// /hive_status is absent, so it never overwrites live data.
Future<void> _seedInitialData() async {
  try {
    final database = FirebaseDatabase.instance.ref();
    final hiveStatusSnapshot = await database.child('hive_status').get();

    if (hiveStatusSnapshot.exists) {
      return;
    }

    await database.update({
      'hive_status': {
        'temperature': 0,
        'humidity': 0,
        'weight': 0,
        'water_level': 0,
        'water_height_cm': 0,
        'water_remaining_ml': 0,
        'water_sensor_ok': true,
        'sound_result': 'Unknown',
        'sound_confidence': 0,
        'health_score': 0,
        'door_open': false,
        'locked': true,
        'entrance_status': 'open',
        'audio_node_online': false,
        'camera_node_online': false,
        'last_sync': 'Waiting for hive',
        'last_sync_epoch': 0,
      },
      'ai_status': {
        'sound_result': 'Unknown',
        'confidence': 0,
        'last_analysis': 'No analysis yet',
        'model_version': 'Edge Impulse v1.0',
        'node_online': false,
      },
      // Command contract shared with the ESP32 main controller.
      // App WRITES: collect_honey, feed_ml, entrance, tare_scale, set_baseline.
      // ESP echoes back: smoke_pump, pump (for live status display).
      'commands': {
        'collect_honey': false,
        'smoke_pump': false,
        'pump': false,
        'feed_ml': 0,
        'entrance': 'open',
        'tare_scale': false,
        'set_baseline': false,
      },
      // RFID-driven hive lock. ESP owns these; the app only reads them.
      'security': {
        'locked': true,
        'rfid_status': 'Waiting for Card',
        'last_card': '',
        'last_time': '',
      },
      'hornet_detection': {
        'detected': false,
        'entrance_status': 'open',
        'last_detection': 'No detection yet',
        'last_detection_epoch': 0,
        'last_confirmed': false,
        'detection_count': 0,
        'camera_online': false,
        'image_url': '',
      },
      // Where the app finds the ESP32-CAM. The controller republishes this at
      // boot, so changing the camera's address does not need an app rebuild.
      'camera': {
        'stream_url': 'http://192.168.137.150/',
        'status_url': 'http://192.168.137.150:81/status',
        'online': false,
        'last_seen': '',
      },
      'production': {
        'hive_weight': 0,
        'baseline_weight': 4.0,
        'estimated_honey': 0,
        'today_production': 0,
        'today_start_weight': 0,
        'today_date': '',
        'weekly_production': 0,
        'monthly_production': 0,
        'total_harvested': 0,
        'last_harvest_kg': 0,
        'last_harvest_time': '',
      },
      // Physical measurements the controller reads back at boot. Editing these
      // re-calibrates the hive without reflashing.
      'calibration': {
        'scale_factor': -7050.0,
        'tare_offset': 0,
        'container_height_cm': 3.5,
        'sensor_to_top_cm': 4.0,
        'container_diameter_cm': 9.0,
        'honey_fraction': 0.85,
      },
      'settings': {
        'temp_max': 36,
        'temp_min': 32,
        'humidity_max': 75,
        'humidity_min': 40,
        'notifications_enabled': true,
      },
    });
  } catch (error, stackTrace) {
    debugPrint('Initial data seed skipped: $error');
    debugPrintStack(stackTrace: stackTrace);
  }
}

class BeeGuardApp extends StatelessWidget {
  const BeeGuardApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'BeeGuard',
      debugShowCheckedModeBanner: false,
      theme: AppTheme.lightTheme,
      initialRoute: AppRoutes.splash,
      routes: {
        AppRoutes.splash: (context) => const SplashScreen(),
        AppRoutes.login: (context) => const LoginScreen(),
        AppRoutes.dashboard: (context) => const DashboardScreen(),
        AppRoutes.hiveDetails: (context) => const HiveDetailsScreen(),
        AppRoutes.liveMonitoring: (context) => const LiveMonitoringScreen(),
        AppRoutes.aiStatus: (context) => const AiStatusScreen(),
        AppRoutes.control: (context) => const ControlScreen(),
        AppRoutes.security: (context) => const SecurityScreen(),
        AppRoutes.hornetDetection: (context) => const HornetDetectionScreen(),
        AppRoutes.notifications: (context) => const NotificationsScreen(),
        AppRoutes.statistics: (context) => const StatisticsScreen(),
        AppRoutes.profile: (context) => const ProfileScreen(),
        AppRoutes.settings: (context) => const SettingsScreen(),
      },
    );
  }
}
