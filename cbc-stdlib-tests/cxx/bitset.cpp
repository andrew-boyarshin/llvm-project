#include <bitset>

int main() {
  std::bitset<8> bits("10110001");
  if (bits.count() != 4)
    return 3;
  bits.reset();
  bits.set(0);
  bits.set(1);
  bits.set(3);
  if (!bits.test(0) || bits.test(2))
    return 1;
  bits.flip(0);
  bits.reset(1);
  return !bits.test(0) && !bits.test(1) && bits.test(3) && bits.to_ulong() == 8
             ? 0
             : 2;
}
