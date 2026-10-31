import 'dart:ffi';
import 'dart:isolate';
typedef Callback = Int64 Function(Int64, Int64);
typedef VoidCallback = Void Function();
@pragma('vm:entry-point')
int killTarget(int a, int b) {
  Isolate.current.kill(priority: Isolate.immediate);
  return a + b;
}
late NativeCallable<Callback> callback;
late NativeCallable<VoidCallback> voidCallback;
@pragma('vm:entry-point')
int prepare(bool handle) {
  if (handle) {
    voidCallback = NativeCallable<VoidCallback>.isolateLocal(() {
      Isolate.current.kill(priority: Isolate.immediate);
    });
    return voidCallback.nativeFunction.address;
  }
  callback = NativeCallable<Callback>.isolateLocal(killTarget, exceptionalReturn: -19001);
  return callback.nativeFunction.address;
}
@pragma('vm:entry-point')
void trigger(String library, bool handle) {
  final lib = DynamicLibrary.open(library);
  if (handle) {
    final call = lib.lookupFunction<Handle Function(Handle, Pointer<NativeFunction<VoidCallback>>), Object? Function(Object?, Pointer<NativeFunction<VoidCallback>>)>('CallHandleKill');
    call(<int>[42], voidCallback.nativeFunction);
  } else {
    final call = lib.lookupFunction<Int64 Function(Pointer<NativeFunction<Callback>>), int Function(Pointer<NativeFunction<Callback>>)>('CallKill');
    call(callback.nativeFunction);
  }
  print('FORBIDDEN_OUTER_DART_AFTER_KILL');
  throw StateError('isolate termination was swallowed');
}
void main() {}
