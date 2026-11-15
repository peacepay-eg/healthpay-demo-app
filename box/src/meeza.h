// Meeza (EMV) QR text for one sale, built on the box itself.
// Same rules as the server's meezaQr_(): the merchant's static QR becomes a
// one-sale QR with the amount (tag 54) and the sale number as bill number
// (tag 62, sub-tag 01). Plain C++ so it can be tested on a computer.
#pragma once
#include <string>

namespace meeza {
// CRC16-CCITT (start 0xFFFF), 4 upper-case hex digits.
std::string crc16(const std::string &s);
// True if the text looks like a valid EMV QR (starts 000201, checksum right).
bool valid(const std::string &base);
// One-sale QR text, or "" if the base QR is not valid.
std::string saleQr(const std::string &base, long amountEgp, long bill);
}
