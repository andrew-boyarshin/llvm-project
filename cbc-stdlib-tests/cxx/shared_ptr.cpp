#include <memory>

int main() {
  auto a = std::make_shared<int>(9);
  std::shared_ptr<int> b = a;
  a.reset();
  if (!b || *b != 9 || b.use_count() != 1)
    return 1;
  b.reset();
  return 0;
}
