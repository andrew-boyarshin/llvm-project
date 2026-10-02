struct Base {
  virtual ~Base() = default;
  int code = 0;
};

struct Derived : Base {};

int main() {
  try {
    Derived error;
    error.code = 4;
    throw error;
  } catch (const Base &error) {
    return error.code == 4 ? 0 : 1;
  } catch (...) {
    return 2;
  }
  return 3;
}
