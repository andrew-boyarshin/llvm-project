#include <limits>

int main() {
  if (std::numeric_limits<int>::max() < 1000)
    return 1;
  if (!std::numeric_limits<double>::is_iec559)
    return 2;
  unsigned digits = std::numeric_limits<unsigned>::digits;
  if (digits != 32 && digits != 64)
    return 3;
  bool sign = static_cast<char>(-1) < 0;
  return std::numeric_limits<char>::is_signed == sign ? 0 : 4;
}
