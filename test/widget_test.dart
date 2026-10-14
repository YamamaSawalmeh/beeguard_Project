import 'package:beeguard/main.dart';
import 'package:beeguard/screens/login_screen.dart';
import 'package:beeguard/screens/splash_screen.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  testWidgets('starts with splash screen and navigates to login screen', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(const BeeGuardApp());

    expect(find.byType(SplashScreen), findsOneWidget);

    await tester.pump(const Duration(seconds: 2));
    await tester.pumpAndSettle();

    expect(find.byType(LoginScreen), findsOneWidget);
  });
}
