@pragma('vm:external-name', 'FullOldNative')
external int forbiddenOldNative(int value);
void main() {
  forbiddenOldNative(1);
}
