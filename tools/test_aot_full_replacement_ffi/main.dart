// Unchanged consumer: omitted constructor defaults must come from B or U.
import 'package:full_ffi_plugin/plugin.dart';

int mainRuns = 0;
@pragma('vm:entry-point')
int probe() => const PluginConfig(7).value;
@pragma('vm:entry-point')
int entrypointRuns() => mainRuns;
void main() {
  ++mainRuns;
}
