#include "words.h"

namespace words {

// 1..99 each have their own clip ("five and twenty" as one recording), so
// the most common amounts sound natural.
static void two(long n, std::vector<std::string> &o) { o.push_back("n" + std::to_string(n)); }

static void three(long n, std::vector<std::string> &o) {  // 1..999
  long h = n / 100, r = n % 100;
  if (h) o.push_back("h" + std::to_string(h * 100));
  if (r) { if (h) o.push_back("wa"); two(r, o); }
}

std::vector<std::string> amount(long n) {
  std::vector<std::string> o;
  if (n <= 0 || n > 999999) return o;
  long k = n / 1000, r = n % 1000;
  if (k == 1) o.push_back("k1");
  else if (k == 2) o.push_back("k2");
  else if (k >= 3 && k <= 10) { three(k, o); o.push_back("kpl"); }
  else if (k > 10) { three(k, o); o.push_back("kacc"); }
  if (r) { if (k) o.push_back("wa"); three(r, o); }
  return o;
}

std::vector<std::string> received(long n, const std::string &from3) {
  std::vector<std::string> o{"ding", "recv"};
  auto a = amount(n);
  o.insert(o.end(), a.begin(), a.end());
  o.push_back("egp");
  if (from3.size() == 3) {
    o.push_back("from");
    for (char c : from3) {
      if (c == '0') o.push_back("d0");
      else if (c >= '1' && c <= '9') o.push_back(std::string("n") + c);
    }
  }
  return o;
}

}  // namespace words
