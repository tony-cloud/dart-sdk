import 'dart:ffi';
import 'dart:isolate';
typedef Callback = Handle Function(Int64,Int64);
late NativeCallable<Callback> callback;
late Object? Function(Pointer<NativeFunction<Callback>>) invoke;
@pragma('vm:entry-point')
int prepare(String path) {
  final lib=DynamicLibrary.open(path);
  invoke=lib.lookupFunction<Handle Function(Pointer<NativeFunction<Callback>>),Object? Function(Pointer<NativeFunction<Callback>>)>('HandleCallbackKill');
  final capture=<int>[17];
  callback=NativeCallable<Callback>.isolateLocal((int a,int b) {
    if(capture[0]!=17)throw StateError('capture');
    Isolate.current.kill(priority:Isolate.immediate);
    return a+b;
  });
  return callback.nativeFunction.address;
}
@pragma('vm:entry-point')
void trigger() {
  invoke(callback.nativeFunction);
  print('FORBIDDEN_OUTER_DART_AFTER_KILL');
  throw StateError('Handle-result callback swallowed termination');
}
void main() {}
