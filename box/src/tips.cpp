#include "tips.h"

namespace tips {
void options(long bill, long out[3]) {
  if (bill < 100) { out[0] = 1; out[1] = 2; out[2] = 5; return; }
  const long pct[3] = {5, 10, 15};
  for (int i = 0; i < 3; i++) out[i] = (bill * pct[i] + 99) / 100;
}
}
