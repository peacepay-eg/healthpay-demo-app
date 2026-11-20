// Turns an amount into the list of voice clips to play, in Arabic order:
// 25 -> "five and twenty". Plain C++ so it can be tested on a computer.
#pragma once
#include <string>
#include <vector>

namespace words {
// Clips for a whole amount 1..999999 (names without folder or ".wav").
std::vector<std::string> amount(long n);
// "Received <amount> pounds", plus "from a number ending in d d d" when given.
std::vector<std::string> received(long n, const std::string &from3);
// Trip mode: "<n> left" after a payment; with notPaid, "<n> left, not paid";
// "everyone paid" when none is left.
std::vector<std::string> tripLeft(long left, bool notPaid);
// Seat stickers: "seat 3 and seat 4, not paid".
std::vector<std::string> seatsNotPaid(const std::vector<long> &seats);
}
