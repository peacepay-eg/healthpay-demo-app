#include "meeza.h"
#include <vector>
#include <cstdio>

namespace meeza {

std::string crc16(const std::string &s) {
  unsigned c = 0xFFFF;
  for (unsigned char ch : s) {
    c ^= (unsigned)ch << 8;
    for (int b = 0; b < 8; b++) c = (c & 0x8000) ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF;
  }
  char buf[5];
  snprintf(buf, sizeof buf, "%04X", c);
  return buf;
}

typedef std::vector<std::pair<std::string, std::string>> Tlv;

static bool isDigit2(const std::string &s, size_t i) {
  return i + 2 <= s.size() && s[i] >= '0' && s[i] <= '9' && s[i + 1] >= '0' && s[i + 1] <= '9';
}

static bool read(const std::string &s, Tlv &out) {
  size_t i = 0;
  while (i + 4 <= s.size()) {
    if (!isDigit2(s, i) || !isDigit2(s, i + 2)) return false;
    std::string tag = s.substr(i, 2);
    size_t len = (size_t)((s[i + 2] - '0') * 10 + (s[i + 3] - '0'));
    if (i + 4 + len > s.size()) return false;
    out.push_back({tag, s.substr(i + 4, len)});
    i += 4 + len;
  }
  return i == s.size();
}

static std::string tlv(const std::string &tag, const std::string &v) {
  char len[3];
  snprintf(len, sizeof len, "%02u", (unsigned)v.size());
  return tag + len + v;
}

static std::string upper(std::string s) {
  for (auto &ch : s) if (ch >= 'a' && ch <= 'f') ch = ch - 'a' + 'A';
  return s;
}

bool valid(const std::string &base) {
  if (base.size() < 12 || base.compare(0, 6, "000201") != 0) return false;
  return crc16(base.substr(0, base.size() - 4)) == upper(base.substr(base.size() - 4));
}

static std::string termLabel(const std::string &t) {
  std::string o;
  for (char c : t) {
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) o += c;
    if (o.size() >= 25) break;
  }
  return o.empty() ? "MAIN" : o;
}

std::string saleQr(const std::string &base, long amountEgp, long bill, const std::string &terminal) {
  if (!valid(base)) return "";
  Tlv tags;
  if (!read(base, tags)) return "";
  char amt[24];
  snprintf(amt, sizeof amt, "%ld.00", amountEgp);
  char billS[24];
  snprintf(billS, sizeof billS, "%ld", bill);
  std::string extra, out;
  for (auto &t : tags) {
    if (t.first == "62") {
      Tlv sub;
      if (read(t.second, sub))
        for (auto &x : sub)
          if (x.first != "01" && x.first != "05" && x.first != "07") extra += tlv(x.first, x.second);
    }
  }
  for (auto &t : tags) {
    const std::string &tag = t.first;
    if (tag == "01") out += tlv("01", "12");
    else if (tag == "54" || tag == "62" || tag == "63") continue;
    else if (tag == "58") out += tlv("54", amt) + tlv("58", t.second);
    else out += tlv(tag, t.second);
  }
  out += tlv("62", extra + tlv("01", billS) + tlv("05", billS) + (terminal.empty() ? std::string() : tlv("07", termLabel(terminal))));
  out += "6304";
  return out + crc16(out);
}

}  // namespace meeza
