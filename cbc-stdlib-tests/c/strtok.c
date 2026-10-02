#include <string.h>

int main(void) {
  char buf[] = "a,b,c";
  char *tok = strtok(buf, ",");
  if (!tok || tok[0] != 'a' || tok[1] != 0)
    return 1;
  tok = strtok(0, ",");
  if (!tok || tok[0] != 'b')
    return 2;
  tok = strtok(0, ",");
  if (!tok || tok[0] != 'c')
    return 3;
  if (strtok(0, ",") != 0)
    return 4;
  return 0;
}
