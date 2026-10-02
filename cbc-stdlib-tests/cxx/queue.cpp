#include <queue>
#include <stack>

int main() {
  std::priority_queue<int> heap;
  heap.push(2);
  heap.push(5);
  heap.push(1);
  if (heap.top() != 5 || heap.size() != 3)
    return 1;
  heap.pop();
  std::stack<int> stack;
  stack.push(4);
  stack.push(7);
  return heap.top() == 2 && stack.top() == 7 && stack.size() == 2 ? 0 : 2;
}
