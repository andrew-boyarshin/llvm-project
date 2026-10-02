#include <deque>

int main() {
  std::deque<int> values;
  values.push_back(2);
  values.push_front(1);
  values.push_back(3);
  return values[0] == 1 && values[1] == 2 && values[2] == 3 &&
                 values.size() == 3
             ? 0
             : 1;
}
