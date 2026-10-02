int main() {
  int *p = new int(5);
  if (*p != 5)
    return 1;
  delete p;
  int *a = new int[4]{1, 2, 3, 4};
  int sum = a[0] + a[3];
  delete[] a;
  return sum == 5 ? 0 : 2;
}
