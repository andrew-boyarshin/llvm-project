#include <bit>

int main() {
  if (std::countl_zero(0ul) != 64)
    return 1;
  if (std::countl_zero(1ul) != 63)
    return 2;
  return 0;
}
