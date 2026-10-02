#include <list>

int main() {
  std::list<int> values{1, 2, 3};
  values.push_front(0);
  values.pop_back();
  return values.front() == 0 && values.back() == 2 && values.size() == 3 ? 0
                                                                          : 1;
}
