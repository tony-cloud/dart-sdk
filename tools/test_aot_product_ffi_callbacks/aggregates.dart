// Loaded as a new ordinary URI into the callback-free released PRODUCT group.
import 'dart:ffi';
import 'dart:typed_data';

final class Pair extends Struct { @Int64() external int a; @Int64() external int b; }
final class HfaF extends Struct { @Float() external double a; @Float() external double b; @Float() external double c; }
final class HfaD extends Struct { @Double() external double a; @Double() external double b; @Double() external double c; @Double() external double d; }
final class Large extends Struct { @Int64() external int a; @Int64() external int b; @Int64() external int c; }
final class Envelope extends Struct { @Int64() external int prefix; external Large payload; }
final class Word extends Union { @Uint64() external int bits; @Double() external double floating; }
final class Nested extends Struct { external Pair inner; @Array(3) external Array<Int64> items; }
final class Node extends Struct { external Pointer<Int64> pointer; @Int64() external int value; }
@Packed(1)
final class Odd3 extends Struct { @Uint8() external int tag; @Uint16() external int value; }
@Packed(1)
final class Odd5 extends Struct { @Uint8() external int tag; @Uint32() external int value; }
@Packed(1)
final class Odd7 extends Struct { @Uint8() external int tag; @Uint16() external int middle; @Uint32() external int value; }
@Packed(1)
final class Odd9 extends Struct { @Uint8() external int tag; @Uint64() external int value; }
@Packed(1)
final class Odd17 extends Struct { @Uint8() external int tag; @Uint64() external int a; @Uint64() external int b; }

typedef PairSig = Pair Function(Pair);
typedef HfaFSig = HfaF Function(HfaF);
typedef HfaDSig = HfaD Function(HfaD);
typedef LargeSig = Large Function(Large);
typedef WordSig = Word Function(Word);
typedef NestedSig = Nested Function(Nested);
typedef NodeSig = Node Function(Node);
typedef HandleSig = Handle Function(Handle);
typedef HandleVarSig = Handle Function(Int64,VarArgs<(Handle,Pair,Large)>);
typedef HandleResultSig = Handle Function();
typedef HandleInputSig = Int64 Function(Handle);
typedef SpillSig = Pair Function(Int64,Int64,Int64,Int64,Int64,Int64,Int64,Int64,Pair,HfaF,Double,Double,Double,Double,Double,Double,Double,Double,HfaD);
typedef VarSig = Large Function(Int64,VarArgs<(Pair,Large,HfaF)>);
typedef EmptyVarSig = Pair Function(Pair,VarArgs<()>);
typedef ManySig = Large Function(Handle,Handle,Handle,Handle,Handle,Handle,Handle,Handle,Handle,Handle,Large,Pair);

late DynamicLibrary lib;
late int Function() gcNow, nativeCalls;
late int Function() nativeBacking;
late int Function() guardReturnAddress;
late int Function(int) markInputRead;
late Pointer<Int64> alias;
late Pointer<Envelope> nativeEnvelope;
late Pair retained;
late Large retainedLarge;
var fail = false;
var expectMissingScope = false;
var checked = 0;
void equal(Object? expected, Object? actual) { if (expected != actual) throw StateError('$expected != $actual'); }
void check(int value) { equal(1,value); ++checked; }
void collect() { gcNow(); }
Pair pairStatic(Pair value) { collect(); value.a += 100; return value; }
HfaF hfaStatic(HfaF value) { collect(); return value; }
final registrations = <NativeCallable>[];
final specs = <List<int>>[];
void remember(NativeCallable callback, {int flags=0,int indirect=0}) {
  registrations.add(callback); specs.add([callback.nativeFunction.address,1,flags,indirect]);
}
late NativeCallable<PairSig> pair, badPair, nullPair, wrongPair, retainPair;
late NativeCallable<HfaFSig> hfaF;
late NativeCallable<HfaDSig> hfaD;
late NativeCallable<LargeSig> largeInput, largeHeap, largeView, largeNative, largeFail, selfLarge, guardLarge, guardLargeFail;
late NativeCallable<WordSig> word;
late NativeCallable<NestedSig> nested;
late NativeCallable<NodeSig> node;
late NativeCallable<SpillSig> spills;
late NativeCallable<VarSig> variadic;
late NativeCallable<EmptyVarSig> empty;
late NativeCallable<ManySig> many;
late NativeCallable<HandleSig> identityHandle, throwingHandle, allocatedHandle, selfHandle;
late NativeCallable<HandleResultSig> resultHandle;
late NativeCallable<HandleInputSig> inputHandle;
late NativeCallable<HandleVarSig> handleVariadic;

@pragma('vm:entry-point')
List<List<int>> prepare(String path) {
  lib=DynamicLibrary.open(path);
  gcNow=lib.lookupFunction<IntPtr Function(),int Function()>('CallbackGcNow');
  nativeCalls=lib.lookupFunction<IntPtr Function(),int Function()>('AggregateCallCount');
  markInputRead=lib.lookupFunction<Int64 Function(Int64),int Function(int)>('MarkAggregateInputRead');
  alias=Pointer<Int64>.fromAddress(lib.lookupFunction<IntPtr Function(),int Function()>('CallbackAliasAddress')());
  nativeBacking=lib.lookupFunction<IntPtr Function(),int Function()>('ResetCallbackNativeBacking');
  guardReturnAddress=lib.lookupFunction<IntPtr Function(),int Function()>('CallbackGuardReturnAddress');
  nativeEnvelope=Pointer<Envelope>.fromAddress(nativeBacking());
  final capture=<int>[100];
  pair=NativeCallable<PairSig>.isolateLocal((Pair value) { collect(); value.a+=capture[0]; return value; }); remember(pair);
  badPair=NativeCallable<PairSig>.isolateLocal((Pair value) { collect(); throw StateError('${capture[0]} Pair'); }); remember(badPair);
  // These dynamic returns deliberately exercise Dart return-type failures;
  // no fabricated unchecked return object bypasses the language type checks.
  Pair nullPairTarget(Pair value) { collect(); if(capture[0]!=100) throw StateError('capture'); return (null as dynamic); }
  Pair wrongPairTarget(Pair value) { collect(); if(capture[0]!=100) throw StateError('capture'); return (Struct.create<Large>() as dynamic); }
  nullPair=NativeCallable<PairSig>.isolateLocal(nullPairTarget); remember(nullPair);
  wrongPair=NativeCallable<PairSig>.isolateLocal(wrongPairTarget); remember(wrongPair);
  retainPair=NativeCallable<PairSig>.isolateLocal((Pair value) { retained=value; collect(); value.a+=capture[0]; return value; }); remember(retainPair);
  hfaF=NativeCallable<HfaFSig>.isolateLocal((HfaF value) { collect(); if(fail) throw StateError('${capture[0]} HFA'); return value; }); remember(hfaF);
  hfaD=NativeCallable<HfaDSig>.isolateLocal((HfaD value) { collect(); if(fail) throw StateError('${capture[0]} HFA'); return value; }); remember(hfaD);
  largeInput=NativeCallable<LargeSig>.isolateLocal((Large value) { collect(); value.a+=capture[0]; value.b+=200; value.c+=300; return value; }); remember(largeInput,indirect:24);
  largeHeap=NativeCallable<LargeSig>.isolateLocal((Large value) { collect(); return Struct.create<Large>()..a=capture[0]+11..b=222..c=333; }); remember(largeHeap,indirect:24);
  largeView=NativeCallable<LargeSig>.isolateLocal((Large value) {
    final storage=Uint8List(80); final view=Uint8List.sublistView(storage,16,64);
    final result=Struct.create<Large>(view,8)..a=capture[0]+11..b=222..c=333;
    collect(); equal(0,storage[23]); equal(0,storage[48]); return result;
  }); remember(largeView,indirect:24);
  largeNative=NativeCallable<LargeSig>.isolateLocal((Large value) { collect(); if(capture[0]!=100) throw StateError('capture'); return nativeEnvelope.ref.payload; }); remember(largeNative,indirect:24);
  largeFail=NativeCallable<LargeSig>.isolateLocal((Large value) { equal(11,value.a); equal(22,value.b); equal(33,value.c); collect(); throw StateError('${capture[0]} indirect'); }); remember(largeFail,indirect:24);
  selfLarge=NativeCallable<LargeSig>.isolateLocal((Large value) {
    selfLarge.close(); collect(); check(checkPair(pair.nativeFunction,0));
    value.a+=capture[0]; value.b+=200; value.c+=300; collect(); return value;
  }); remember(selfLarge,indirect:24);
  guardLarge=NativeCallable<LargeSig>.isolateLocal((Large value) { retainedLarge=value; equal(11,value.a); equal(22,value.b); equal(33,value.c); equal(1,markInputRead(value.a+value.b+value.c)); collect(); if(capture[0]!=100) throw StateError('capture'); return value; }); remember(guardLarge,indirect:24);
  guardLargeFail=NativeCallable<LargeSig>.isolateLocal((Large value) { equal(11,value.a); equal(22,value.b); equal(33,value.c); equal(1,markInputRead(value.a+value.b+value.c)); collect(); throw StateError('${capture[0]} aliased input read before failure'); }); remember(guardLargeFail,indirect:24);
  word=NativeCallable<WordSig>.isolateLocal((Word value) { collect(); if(fail) throw StateError('${capture[0]} Union'); value.bits+=1; return value; }); remember(word);
  nested=NativeCallable<NestedSig>.isolateLocal((Nested value) { collect(); if(fail) throw StateError('${capture[0]} Nested'); value.inner.a+=1;value.inner.b+=2;value.items[0]+=10;value.items[1]+=20;value.items[2]+=30;return value; }); remember(nested,indirect:40);
  node=NativeCallable<NodeSig>.isolateLocal((Node value) { equal(alias.address,value.pointer.address);collect();value.pointer.value+=5;value.value+=capture[0]-99;return value; }); remember(node);
  spills=NativeCallable<SpillSig>.isolateLocal((int a,int b,int c,int d,int e,int f,int g,int h,Pair p,HfaF hf,double d0,double d1,double d2,double d3,double d4,double d5,double d6,double d7,HfaD hd) {
    collect(); equal(100,capture[0]);equal(7.5,hf.a+hf.b+hf.c);
    return Struct.create<Pair>()..a=a+b+c+d+e+f+g+h+p.a+p.b+(d0+d1+d2+d3+d4+d5+d6+d7).toInt()..b=(hd.a+hd.b+hd.c+hd.d).toInt();
  }); remember(spills);
  variadic=NativeCallable<VarSig>.isolateLocal((int fixed,Pair p,Large v,HfaF hf) {collect();equal(100,capture[0]);return Struct.create<Large>()..a=fixed+p.a+v.a..b=p.b+v.b..c=v.c+(hf.a+hf.b+hf.c).toInt();});remember(variadic,indirect:24);
  empty=NativeCallable<EmptyVarSig>.isolateLocal((Pair p) {collect();p.a+=capture[0];return p;});remember(empty);
  many=NativeCallable<ManySig>.isolateLocal((Object? a,Object? b,Object? c,Object? d,Object? e,Object? f,Object? g,Object? h,Object? i,Object? j,Large v,Pair p) {
    collect();equal(100,capture[0]);equal(null,b);equal(null,f);equal(true,c);equal(true,g);
    equal(true,identical(a,d)&&identical(a,e)&&identical(a,h)&&identical(a,i)&&identical(a,j));
    v.a+=p.a;v.b+=p.b;v.c+=9;return v;
  });remember(many,indirect:24);
  identityHandle=NativeCallable<HandleSig>.isolateLocal((Object? v) { collect();equal(100,capture[0]);return v;});remember(identityHandle,flags:1);
  throwingHandle=NativeCallable<HandleSig>.isolateLocal((Object? v) {collect();equal(100,capture[0]);throw v!;});remember(throwingHandle,flags:1);
  allocatedHandle=NativeCallable<HandleSig>.isolateLocal((Object? v) {collect();return <int>[42,capture[0]];});remember(allocatedHandle,flags:1);
  selfHandle=NativeCallable<HandleSig>.isolateLocal((Object? v) {selfHandle.close();collect();return <int>[42,capture[0]];});remember(selfHandle,flags:1);
  resultHandle=NativeCallable<HandleResultSig>.isolateLocal(() {if(expectMissingScope)print('FORBIDDEN_HANDLE_TARGET_WITHOUT_SCOPE');collect();return capture[0]-17;});remember(resultHandle,flags:1);
  inputHandle=NativeCallable<HandleInputSig>.isolateLocal((Object? v) {collect();equal(100,capture[0]);return v as int;},exceptionalReturn:-1);remember(inputHandle);
  handleVariadic=NativeCallable<HandleVarSig>.isolateLocal((int fixed,Object? token,Pair p,Large v) {
    collect();equal(100,capture[0]);equal(7,fixed);equal(11,p.a);equal(22,p.b);equal(31,v.a);equal(32,v.b);equal(33,v.c);return token;
  });remember(handleVariadic,flags:1);
  prepareOdds(capture);
  prepareGuardedReturns(capture);
  specs.add([Pointer.fromFunction<PairSig>(pairStatic).address,0,0,0]);
  specs.add([Pointer.fromFunction<HfaFSig>(hfaStatic).address,0,0,0]);
  return specs;
}

typedef Odd3Sig = Odd3 Function(Odd3);
late NativeCallable<Odd3Sig> odd3;

typedef Odd5Sig = Odd5 Function(Odd5);
late NativeCallable<Odd5Sig> odd5;

typedef Odd7Sig = Odd7 Function(Odd7);
late NativeCallable<Odd7Sig> odd7;

typedef Odd9Sig = Odd9 Function(Odd9);
late NativeCallable<Odd9Sig> odd9;

typedef Odd17Sig = Odd17 Function(Odd17);
late NativeCallable<Odd17Sig> odd17;
late NativeCallable<Odd17Sig> guardOdd, guardOddFail;
void prepareOdds(List<int> capture) {
  odd3=NativeCallable<Odd3Sig>.isolateLocal((Odd3 value) {collect();equal(100,capture[0]);if(fail)throw StateError('Odd3');return value;});remember(odd3,indirect:0);
  odd5=NativeCallable<Odd5Sig>.isolateLocal((Odd5 value) {collect();equal(100,capture[0]);if(fail)throw StateError('Odd5');return value;});remember(odd5,indirect:0);
  odd7=NativeCallable<Odd7Sig>.isolateLocal((Odd7 value) {collect();equal(100,capture[0]);if(fail)throw StateError('Odd7');return value;});remember(odd7,indirect:0);
  odd9=NativeCallable<Odd9Sig>.isolateLocal((Odd9 value) {collect();equal(100,capture[0]);if(fail)throw StateError('Odd9');return value;});remember(odd9,indirect:0);
  odd17=NativeCallable<Odd17Sig>.isolateLocal((Odd17 value) {collect();equal(100,capture[0]);if(fail)throw StateError('Odd17');return value;});remember(odd17,indirect:17);
  guardOdd=NativeCallable<Odd17Sig>.isolateLocal((Odd17 value) {equal(7,value.tag);equal(101,value.a);equal(202,value.b);equal(1,markInputRead(value.tag+value.a+value.b));collect();equal(100,capture[0]);return value;});remember(guardOdd,indirect:17);
  guardOddFail=NativeCallable<Odd17Sig>.isolateLocal((Odd17 value) {equal(7,value.tag);equal(101,value.a);equal(202,value.b);equal(1,markInputRead(value.tag+value.a+value.b));collect();throw StateError('${capture[0]} Odd17 alias');});remember(guardOddFail,indirect:17);
}

int checkPair(Pointer<NativeFunction<PairSig>> cb,int zero) => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<PairSig>>,Int64),int Function(Pointer<NativeFunction<PairSig>>,int)>('CheckPair')(cb,zero);

int checkHfaF(Pointer<NativeFunction<HfaFSig>> cb,int zero) => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<HfaFSig>>,Int64),int Function(Pointer<NativeFunction<HfaFSig>>,int)>('CheckHfaF')(cb,zero);

int checkHfaD(Pointer<NativeFunction<HfaDSig>> cb,int zero) => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<HfaDSig>>,Int64),int Function(Pointer<NativeFunction<HfaDSig>>,int)>('CheckHfaD')(cb,zero);

int checkLarge(Pointer<NativeFunction<LargeSig>> cb,int zero) => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<LargeSig>>,Int64),int Function(Pointer<NativeFunction<LargeSig>>,int)>('CheckLarge')(cb,zero);

int checkWord(Pointer<NativeFunction<WordSig>> cb,int zero) => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<WordSig>>,Int64),int Function(Pointer<NativeFunction<WordSig>>,int)>('CheckWord')(cb,zero);

int checkNested(Pointer<NativeFunction<NestedSig>> cb,int zero) => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<NestedSig>>,Int64),int Function(Pointer<NativeFunction<NestedSig>>,int)>('CheckNested')(cb,zero);

int checkOdd3(Pointer<NativeFunction<Odd3Sig>> cb,int zero) => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<Odd3Sig>>,Int64),int Function(Pointer<NativeFunction<Odd3Sig>>,int)>('CheckOdd3')(cb,zero);

int checkOdd5(Pointer<NativeFunction<Odd5Sig>> cb,int zero) => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<Odd5Sig>>,Int64),int Function(Pointer<NativeFunction<Odd5Sig>>,int)>('CheckOdd5')(cb,zero);

int checkOdd7(Pointer<NativeFunction<Odd7Sig>> cb,int zero) => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<Odd7Sig>>,Int64),int Function(Pointer<NativeFunction<Odd7Sig>>,int)>('CheckOdd7')(cb,zero);

int checkOdd9(Pointer<NativeFunction<Odd9Sig>> cb,int zero) => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<Odd9Sig>>,Int64),int Function(Pointer<NativeFunction<Odd9Sig>>,int)>('CheckOdd9')(cb,zero);

int checkOdd17(Pointer<NativeFunction<Odd17Sig>> cb,int zero) => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<Odd17Sig>>,Int64),int Function(Pointer<NativeFunction<Odd17Sig>>,int)>('CheckOdd17')(cb,zero);

int checkRetainedPair(Pointer<NativeFunction<PairSig>> cb) => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<PairSig>>),int Function(Pointer<NativeFunction<PairSig>>)>('CheckRetainedPair')(cb);

int checkNode(Pointer<NativeFunction<NodeSig>> cb) => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<NodeSig>>),int Function(Pointer<NativeFunction<NodeSig>>)>('CheckNode')(cb);

int checkSpills(Pointer<NativeFunction<SpillSig>> cb) => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<SpillSig>>),int Function(Pointer<NativeFunction<SpillSig>>)>('CheckSpills')(cb);

int checkCompoundVarArgs(Pointer<NativeFunction<VarSig>> cb) => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<VarSig>>),int Function(Pointer<NativeFunction<VarSig>>)>('CheckCompoundVarArgs')(cb);

int checkCompoundEmptyVarArgs(Pointer<NativeFunction<EmptyVarSig>> cb) => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<EmptyVarSig>>),int Function(Pointer<NativeFunction<EmptyVarSig>>)>('CheckCompoundEmptyVarArgs')(cb);

int guarded(Pointer<Void> cb,int size,int zero,int alias) => lib.lookupFunction<Int64 Function(Pointer<Void>,Int64,Int64,Int64),int Function(Pointer<Void>,int,int,int)>('CheckGuardedIndirect')(cb,size,zero,alias);
int overlap(Pointer<Void> cb) => lib.lookupFunction<Int64 Function(Pointer<Void>),int Function(Pointer<Void>)>('CheckOverlap')(cb);
int manyHandles(Pointer<NativeFunction<ManySig>> cb,Object? token) => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<ManySig>>,Handle),int Function(Pointer<NativeFunction<ManySig>>,Object?)>('CheckManyHandles')(cb,token);

typedef GuardedOdd3Sig = Odd3 Function();
late NativeCallable<GuardedOdd3Sig> guardedOdd3;
int checkGuardedReturnOdd3() => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<GuardedOdd3Sig>>),int Function(Pointer<NativeFunction<GuardedOdd3Sig>>)>('CheckGuardedReturnOdd3')(guardedOdd3.nativeFunction);
typedef GuardedOdd5Sig = Odd5 Function();
late NativeCallable<GuardedOdd5Sig> guardedOdd5;
int checkGuardedReturnOdd5() => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<GuardedOdd5Sig>>),int Function(Pointer<NativeFunction<GuardedOdd5Sig>>)>('CheckGuardedReturnOdd5')(guardedOdd5.nativeFunction);
typedef GuardedOdd7Sig = Odd7 Function();
late NativeCallable<GuardedOdd7Sig> guardedOdd7;
int checkGuardedReturnOdd7() => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<GuardedOdd7Sig>>),int Function(Pointer<NativeFunction<GuardedOdd7Sig>>)>('CheckGuardedReturnOdd7')(guardedOdd7.nativeFunction);
typedef GuardedOdd9Sig = Odd9 Function();
late NativeCallable<GuardedOdd9Sig> guardedOdd9;
int checkGuardedReturnOdd9() => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<GuardedOdd9Sig>>),int Function(Pointer<NativeFunction<GuardedOdd9Sig>>)>('CheckGuardedReturnOdd9')(guardedOdd9.nativeFunction);
typedef GuardedHfaFSig = HfaF Function();
late NativeCallable<GuardedHfaFSig> guardedHfaF;
int checkGuardedReturnHfaF() => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<GuardedHfaFSig>>),int Function(Pointer<NativeFunction<GuardedHfaFSig>>)>('CheckGuardedReturnHfaF')(guardedHfaF.nativeFunction);
typedef GuardedHfaDSig = HfaD Function();
late NativeCallable<GuardedHfaDSig> guardedHfaD;
int checkGuardedReturnHfaD() => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<GuardedHfaDSig>>),int Function(Pointer<NativeFunction<GuardedHfaDSig>>)>('CheckGuardedReturnHfaD')(guardedHfaD.nativeFunction);
void prepareGuardedReturns(List<int> capture) {
  guardedOdd3=NativeCallable<GuardedOdd3Sig>.isolateLocal(() {collect();equal(100,capture[0]);return Pointer<Odd3>.fromAddress(guardReturnAddress()).ref;});remember(guardedOdd3);
  guardedOdd5=NativeCallable<GuardedOdd5Sig>.isolateLocal(() {collect();equal(100,capture[0]);return Pointer<Odd5>.fromAddress(guardReturnAddress()).ref;});remember(guardedOdd5);
  guardedOdd7=NativeCallable<GuardedOdd7Sig>.isolateLocal(() {collect();equal(100,capture[0]);return Pointer<Odd7>.fromAddress(guardReturnAddress()).ref;});remember(guardedOdd7);
  guardedOdd9=NativeCallable<GuardedOdd9Sig>.isolateLocal(() {collect();equal(100,capture[0]);return Pointer<Odd9>.fromAddress(guardReturnAddress()).ref;});remember(guardedOdd9);
  guardedHfaF=NativeCallable<GuardedHfaFSig>.isolateLocal(() {collect();equal(100,capture[0]);return Pointer<HfaF>.fromAddress(guardReturnAddress()).ref;});remember(guardedHfaF);
  guardedHfaD=NativeCallable<GuardedHfaDSig>.isolateLocal(() {collect();equal(100,capture[0]);return Pointer<HfaD>.fromAddress(guardReturnAddress()).ref;});remember(guardedHfaD);
}
@pragma('vm:entry-point')
int runAggregates() {
  final before=nativeCalls(), expectedBefore=checked;
  check(checkPair(Pointer.fromAddress(specs[specs.length-2][0]),0));
  check(checkHfaF(Pointer.fromAddress(specs.last[0]),0));
  check(checkPair(pair.nativeFunction,0));check(checkPair(badPair.nativeFunction,1));
  check(checkPair(nullPair.nativeFunction,1));check(checkPair(wrongPair.nativeFunction,1));
  check(checkRetainedPair(retainPair.nativeFunction));collect();equal(111,retained.a);equal(22,retained.b);
  check(checkNode(node.nativeFunction));
  check(checkHfaF(hfaF.nativeFunction,0));check(checkHfaD(hfaD.nativeFunction,0));
  check(checkLarge(largeInput.nativeFunction,0));check(checkLarge(largeHeap.nativeFunction,0));
  check(checkLarge(largeView.nativeFunction,0));nativeBacking();check(checkLarge(largeNative.nativeFunction,0));
  check(checkLarge(largeFail.nativeFunction,1));
  check(checkWord(word.nativeFunction,0));check(checkNested(nested.nativeFunction,0));
  check(checkSpills(spills.nativeFunction));check(checkCompoundVarArgs(variadic.nativeFunction));
  check(checkCompoundEmptyVarArgs(empty.nativeFunction));check(manyHandles(many.nativeFunction,<int>[42]));
  check(checkLarge(selfLarge.nativeFunction,0));
  var closed=false;try {selfLarge.nativeFunction;} on StateError {closed=true;}equal(true,closed);
  for(final alias in [0,1]) {
    check(guarded(guardLarge.nativeFunction.cast(),24,0,alias));
    check(guarded(guardLargeFail.nativeFunction.cast(),24,1,alias));
    check(guarded(guardOdd.nativeFunction.cast(),17,0,alias));
    check(guarded(guardOddFail.nativeFunction.cast(),17,1,alias));
  }
  collect();equal(11,retainedLarge.a);equal(22,retainedLarge.b);equal(33,retainedLarge.c);
  check(overlap(largeNative.nativeFunction.cast()));
  equal(3,sizeOf<Odd3>());check(checkOdd3(odd3.nativeFunction,0));
  equal(5,sizeOf<Odd5>());check(checkOdd5(odd5.nativeFunction,0));
  equal(7,sizeOf<Odd7>());check(checkOdd7(odd7.nativeFunction,0));
  equal(9,sizeOf<Odd9>());check(checkOdd9(odd9.nativeFunction,0));
  equal(17,sizeOf<Odd17>());check(checkOdd17(odd17.nativeFunction,0));
  check(checkGuardedReturnOdd3());
  check(checkGuardedReturnOdd5());
  check(checkGuardedReturnOdd7());
  check(checkGuardedReturnOdd9());
  check(checkGuardedReturnHfaF());
  check(checkGuardedReturnHfaD());
  fail=true;
  try {
    check(checkHfaF(hfaF.nativeFunction,1));check(checkHfaD(hfaD.nativeFunction,1));
    check(checkWord(word.nativeFunction,1));check(checkNested(nested.nativeFunction,1));
    check(checkOdd3(odd3.nativeFunction,1));
    check(checkOdd5(odd5.nativeFunction,1));
    check(checkOdd7(odd7.nativeFunction,1));
    check(checkOdd9(odd9.nativeFunction,1));
    check(checkOdd17(odd17.nativeFunction,1));
  } finally {fail=false;}
  equal(before+checked-expectedBefore,nativeCalls());
  print('PRODUCT callback aggregate layouts/copy/aliases/offsets/guard pages/zero fallbacks passed');
  return checked-expectedBefore;
}

Object? handleRoundtrip(Pointer<NativeFunction<HandleSig>> cb,Object? token,int mode) => lib.lookupFunction<Handle Function(Pointer<NativeFunction<HandleSig>>,Handle,Int64),Object? Function(Pointer<NativeFunction<HandleSig>>,Object?,int)>('HandleCallbackRoundtrip')(cb,token,mode);
Object? handleResult(Pointer<NativeFunction<HandleSig>> cb,Object? token) => lib.lookupFunction<Handle Function(Pointer<NativeFunction<HandleSig>>,Handle),Object? Function(Pointer<NativeFunction<HandleSig>>,Object?)>('HandleCallbackResult')(cb,token);
int handleError(Pointer<NativeFunction<HandleSig>> cb,Object? token) => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<HandleSig>>,Handle),int Function(Pointer<NativeFunction<HandleSig>>,Object?)>('HandleCallbackInspectError')(cb,token);
void propagate(Pointer<NativeFunction<HandleSig>> cb,Object? token) => lib.lookupFunction<Void Function(Pointer<NativeFunction<HandleSig>>,Handle),void Function(Pointer<NativeFunction<HandleSig>>,Object?)>('HandleCallbackPropagate')(cb,token);
int explicitScope(Pointer<NativeFunction<HandleResultSig>> cb) => lib.lookupFunction<Int64 Function(Pointer<NativeFunction<HandleResultSig>>),int Function(Pointer<NativeFunction<HandleResultSig>>)>('HandleCallbackExplicitScope')(cb);

Object? handleVarArgs(Pointer<NativeFunction<HandleVarSig>> cb,Object? token) => lib.lookupFunction<Handle Function(Pointer<NativeFunction<HandleVarSig>>,Handle),Object? Function(Pointer<NativeFunction<HandleVarSig>>,Object?)>('HandleCallbackVarArgs')(cb,token);

@pragma('vm:entry-point')
int runHandleScopes() {
  final before=nativeCalls();
  final token=<Object?>[42,'Handle token'];
  late NativeCallable<HandleSig> recursion;
  final capture=<int>[17];
  recursion=NativeCallable<HandleSig>.isolateLocal((Object? value) {
    collect();equal(17,capture[0]);final n=value as int;
    return n==0?0:handleResult(recursion.nativeFunction,n-1);
  });
  try {
    for(var i=0;i<20;++i) {
      equal(true,identical(token,handleRoundtrip(identityHandle.nativeFunction,token,0)));
      equal(null,handleRoundtrip(identityHandle.nativeFunction,token,1));
      equal(true,handleRoundtrip(identityHandle.nativeFunction,token,2));
      equal(true,identical(token,handleRoundtrip(identityHandle.nativeFunction,token,3)));
      equal(1,handleError(throwingHandle.nativeFunction,token));
      var caught=false;try {propagate(throwingHandle.nativeFunction,token);}catch(error,trace){equal(true,identical(token,error));equal(true,trace.toString().isNotEmpty);caught=true;}equal(true,caught);
      equal(83,explicitScope(resultHandle.nativeFunction));
      final allocated=handleResult(allocatedHandle.nativeFunction,token) as List<int>;
      equal(42,allocated[0]);equal(100,allocated[1]);
      equal(0,handleResult(recursion.nativeFunction,4));
      equal(true,identical(token,handleVarArgs(handleVariadic.nativeFunction,token)));
    }
  } finally {recursion.close();}
  // Per iteration: 4 identity/null/read-only/persistent + error inspection +
  // propagation + explicit scope + allocated result + 5 recursive + 1 VarArgs.
  equal(before+20*14,nativeCalls());
  return 20;
}
@pragma('vm:entry-point')
void runHandleSelfClose() {
  final result=handleResult(selfHandle.nativeFunction,<int>[11]) as List<int>;
  equal(42,result[0]);equal(100,result[1]);
  var closed=false;try {selfHandle.nativeFunction;}on StateError{closed=true;}equal(true,closed);
}
@pragma('vm:entry-point')
List<int> directPointers() => [inputHandle.nativeFunction.address,resultHandle.nativeFunction.address];
@pragma('vm:entry-point')
int missingResultPointer() {expectMissingScope=true;return resultHandle.nativeFunction.address;}
@pragma('vm:entry-point')
void close() {for(final callback in registrations) callback.close();registrations.clear();collect();}
void main() {}
