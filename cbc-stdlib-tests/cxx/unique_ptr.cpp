#include <memory>

int main() {
  int n = 0;
  struct S {
    int *p;
    explicit S(int *p) : p(p) {}
    ~S() { *p = 1; }
  };
  {
    std::unique_ptr<S> p(new S(&n));
    if (!p)
      return 1;
  }
  auto q = std::make_unique<int>(4);
  return n == 1 && q && *q == 4 ? 0 : 2;
}
