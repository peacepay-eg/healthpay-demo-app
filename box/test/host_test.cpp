// Runs on a computer: g++ -std=c++17 -I../src host_test.cpp ../src/meeza.cpp ../src/words.cpp ../src/tips.cpp
#include "meeza.h"
#include "words.h"
#include "tips.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
static int fails = 0, passes = 0;
static void check(bool ok, const std::string &what) { if (ok) passes++; else { fails++; printf("FAIL %s\n", what.c_str()); } }
static std::string join(const std::vector<std::string> &v) { std::string s; for (auto &x : v) s += (s.empty() ? "" : " ") + x; return s; }
int main(int argc, char **argv) {
  // vectors.txt lines: base|amount|bill|terminal|expected  (made by the server's own meezaQr_)
  std::ifstream f(argc > 1 ? argv[1] : "vectors.txt");
  std::string line; int n = 0;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    std::stringstream ss(line); std::string base, amt, bill, term, want;
    std::getline(ss, base, '|'); std::getline(ss, amt, '|'); std::getline(ss, bill, '|'); std::getline(ss, term, '|'); std::getline(ss, want, '|');
    std::string got = meeza::saleQr(base, std::stol(amt), std::stol(bill), term);
    check(got == want, "QR " + amt + " " + bill + "\n  got  " + got + "\n  want " + want); n++;
  }
  check(n >= 3, "QR vectors read");
  check(meeza::saleQr("hello", 5, 1001).empty(), "bad base gives empty");
  check(join(words::amount(5)) == "n5", "5");
  check(join(words::tripLeft(1, true)) == "left n1 notpaid", "trip 1 not paid");
  check(join(words::tripLeft(5, false)) == "left n5", "trip 5 left");
  check(join(words::tripLeft(0, true)) == "allpaid", "trip all paid");
  check(join(words::amount(19)) == "n19", "19");
  check(join(words::amount(20)) == "n20", "20");
  check(join(words::amount(25)) == "n25", "25");
  check(join(words::amount(100)) == "h100", "100");
  check(join(words::amount(250)) == "h200 wa n50", "250");
  check(join(words::amount(1000)) == "k1", "1000");
  check(join(words::amount(2005)) == "k2 wa n5", "2005");
  check(join(words::amount(3500)) == "n3 kpl wa h500", "3500");
  check(join(words::amount(15000)) == "n15 kacc", "15000");
  check(join(words::amount(999999)) == "h900 wa n99 kacc wa h900 wa n99", "999999");
  check(words::amount(0).empty() && words::amount(1000000).empty(), "out of range");
  check(join(words::received(50, "620")) == "ding recv n50 egp from n6 n2 d0", "received with digits");
  check(join(words::received(7, "")) == "ding recv n7 egp", "received, no digits");
  long t[3];
  tips::options(3, t); check(t[0] == 1 && t[1] == 2 && t[2] == 5, "tips small bill");
  tips::options(99, t); check(t[0] == 1 && t[2] == 5, "tips 99");
  tips::options(100, t); check(t[0] == 5 && t[1] == 10 && t[2] == 15, "tips 100");
  tips::options(123, t); check(t[0] == 7 && t[1] == 13 && t[2] == 19, "tips round up");
  printf("%d passed, %d failed\n", passes, fails);
  return fails ? 1 : 0;
}
