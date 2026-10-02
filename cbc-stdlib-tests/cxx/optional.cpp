#include <optional>

int main() {
  std::optional<int> a;
  if (a)
    return 1;
  a = 5;
  std::optional<int> b = a;
  a.reset();
  return !a && b && *b == 5 ? 0 : 2;
}
