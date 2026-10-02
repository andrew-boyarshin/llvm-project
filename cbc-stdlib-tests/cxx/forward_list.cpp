#include <forward_list>

int main() {
  std::forward_list<int> values{1, 2, 3};
  values.push_front(0);
  auto it = values.begin();
  if (*it != 0)
    return 1;
  ++it;
  return *it == 1 && values.front() == 0 ? 0 : 2;
}
