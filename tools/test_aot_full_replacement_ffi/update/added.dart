// Every declaration and signature in this library is absent from N and B.
import 'dart:ffi';

@AbiSpecificIntegerMapping({Abi.macosArm64: Int16(), Abi.linuxArm64: Int16()})
final class PluginInt extends AbiSpecificInteger {
  const PluginInt();
}

final class PluginWord extends Union {
  @Uint64()
  external int bits;
  @Double()
  external double floating;
}

final class PluginHfa extends Struct {
  @Float()
  external double a;
  @Float()
  external double b;
  @Float()
  external double c;
}

@Packed(1)
final class PluginPacked extends Struct {
  @Uint8()
  external int tag;
  @Uint64()
  external int value;
}

final class PluginNested extends Struct {
  external PluginHfa hfa;
  external PluginWord word;
}

@Native<Int64 Function(Int64)>(symbol: 'FullAddedValue')
external int addedValue(int value);

int addedProbe() => addedValue(14);
