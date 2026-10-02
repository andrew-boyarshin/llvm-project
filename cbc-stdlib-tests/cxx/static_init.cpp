static int flag = 0;

struct Marker {
  Marker() { flag = 1; }
};

static Marker marker;

int main() { return flag == 1 ? 0 : 1; }
