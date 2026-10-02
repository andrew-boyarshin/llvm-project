#include <typeinfo>

struct Base {
  virtual ~Base() = default;
};
struct Derived : Base {
  int x = 7;
};

int main() {
  Base *p = new Derived;
  auto *d = dynamic_cast<Derived *>(p);
  int ok = d && d->x == 7 && typeid(*p) == typeid(Derived);
  Base *b = new Base;
  ok = ok && dynamic_cast<Derived *>(b) == nullptr;
  delete p;
  delete b;
  return ok ? 0 : 1;
}
