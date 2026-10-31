// Separate compiled-only control: never part of the callback-free baseline.
import 'dart:ffi';
typedef I64 = Int64 Function(Int64, Int64);
typedef H = Handle Function(Handle);
typedef B = Box Function(Box);
typedef V = Void Function(Int64);
final class Box extends Struct { @Int64() external int value; }
int target(int a, int b) => a * 3 + b;
Object? identity(Object? value) => value;
Box boxTarget(Box value) { value.value += 7; return value; }
late DynamicLibrary lib;
late NativeCallable<I64> captured, group;
late NativeCallable<H> handles;
late NativeCallable<V> listener;
late NativeCallable<B> boxes;
int messages = 0;
@pragma('vm:entry-point')
List<int> prepare(String path) {
  lib = DynamicLibrary.open(path);
  final capture = <int>[40];
  captured = NativeCallable<I64>.isolateLocal((int a, int b) => capture[0] + a + b, exceptionalReturn: -1);
  group = NativeCallable<I64>.isolateGroupBound((int a, int b) => a + b, exceptionalReturn: -2);
  handles = NativeCallable<H>.isolateLocal(identity);
  boxes = NativeCallable<B>.isolateLocal(boxTarget);
  listener = NativeCallable<V>.listener((int value) { messages += value; });
  return [Pointer.fromFunction<I64>(target, -3).address, captured.nativeFunction.address,
    group.nativeFunction.address, handles.nativeFunction.address, boxes.nativeFunction.address,
    listener.nativeFunction.address];
}
void equal(Object? expected, Object? value) { if (expected != value) throw StateError('$expected != $value'); }
@pragma('vm:entry-point')
void run() {
  final scalar = lib.lookupFunction<Int64 Function(Pointer<NativeFunction<I64>>, Int64, Int64), int Function(Pointer<NativeFunction<I64>>, int, int)>('CallI64');
  final handle = lib.lookupFunction<Handle Function(Pointer<NativeFunction<H>>, Handle), Object? Function(Pointer<NativeFunction<H>>, Object?)>('CallHandle');
  final box = lib.lookupFunction<Int64 Function(Pointer<NativeFunction<B>>, Int64), int Function(Pointer<NativeFunction<B>>, int)>('CallBox');
  final post = lib.lookupFunction<Void Function(Pointer<NativeFunction<V>>, Int64), void Function(Pointer<NativeFunction<V>>, int)>('CallVoid');
  equal(19, scalar(Pointer.fromFunction<I64>(target, -3), 5, 4));
  equal(52, scalar(captured.nativeFunction, 5, 7));
  equal(12, scalar(group.nativeFunction, 5, 7));
  final object = <int>[5];
  equal(true, identical(object, handle(handles.nativeFunction, object)));
  equal(27, box(boxes.nativeFunction, 20));
  post(listener.nativeFunction, 17);
}
@pragma('vm:entry-point')
void finish() {
  equal(17, messages);
  captured.close(); group.close(); handles.close(); boxes.close(); listener.close();
}
void main() {}
