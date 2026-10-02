int main() {
  try {
    throw 1;
  } catch (int x) {
    return x == 1 ? 0 : 1;
  }
}
