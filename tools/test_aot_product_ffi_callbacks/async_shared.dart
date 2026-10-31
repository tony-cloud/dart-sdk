import 'dart:ffi';

@Native<Int64 Function()>(symbol: 'AsyncStaticInitializer')
external int initializeSharedValue();

@pragma('vm:shared')
final int sharedValue = initializeSharedValue();
typedef Group = Int64 Function(Int64, Int64);
int target(int a, int b) => sharedValue + a + b;
late NativeCallable<Group> callback;
@pragma('vm:entry-point')
int prepare() {
  // Initialize in the owning isolate; the group callback must read shared
  // storage, not a value folded from a constant initializer.
  if (sharedValue != 1234) throw StateError('shared initializer failed');
  callback =
      NativeCallable<Group>.isolateGroupBound(target, exceptionalReturn: -1981);
  return callback.nativeFunction.address;
}

@pragma('vm:entry-point')
void close() {
  callback.close();
}

void main() {}
