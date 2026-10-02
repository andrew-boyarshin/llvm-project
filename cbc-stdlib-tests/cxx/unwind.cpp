struct Guard {
  int *hits;
  ~Guard() { *hits += 1; }
};

int main() {
  int hits = 0;
  try {
    Guard guard{&hits};
    throw 1;
  } catch (int value) {
    return hits == 1 && value == 1 ? 0 : 1;
  }
  return 2;
}
