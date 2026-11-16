// Tip choices on a restaurant box. Plain C++ so it can be tested on a computer.
#pragma once

namespace tips {
// Three tip amounts in whole EGP for a bill: small bills get 1, 2, 5;
// from 100 EGP: 5%, 10% and 15%, rounded up to whole pounds.
void options(long bill, long out[3]);
}
