#include <span>

int main() {
  int raw[] = {1, 2, 3, 4};
  std::span<int> view(raw);
  int sum = 0;
  for (int value : view)
    sum += value;
  std::span<int> mid = view.subspan(1, 2);
  return view.size() == 4 && sum == 10 && mid.front() == 2 && mid.size() == 2
             ? 0
             : 1;
}
