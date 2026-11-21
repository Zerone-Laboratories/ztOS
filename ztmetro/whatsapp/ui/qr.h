// SPDX-License-Identifier: GPL-2.0-or-later
// A QR code for one string: byte mode, error correction M, the best of the eight
// masks. Own code so the app needs no extra package on the image.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace qr {

struct Code {
	int n = 0;
	std::vector<uint8_t> cells, fixed;
	bool at(int x, int y) const { return cells[size_t(y) * n + x]; }
};

namespace detail {
constexpr int ecc_per_block[41] = {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26,
	26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28};
constexpr int blocks[41] = {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16,
	17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49};

inline int raw_modules(int v) {
	int r = (16 * v + 128) * v + 64;
	if (v >= 2) {
		int a = v / 7 + 2;
		r -= (25 * a - 10) * a - 55;
		if (v >= 7) r -= 36;
	}
	return r;
}
inline int data_codewords(int v) { return raw_modules(v) / 8 - ecc_per_block[v] * blocks[v]; }

inline uint8_t gf_mul(uint8_t a, uint8_t b) {
	int z = 0;
	for (int i = 7; i >= 0; i--) {
		z = (z << 1) ^ ((z >> 7) * 0x11d);
		z ^= ((b >> i) & 1) * a;
	}
	return uint8_t(z);
}

inline std::vector<uint8_t> rs_divisor(int degree) {
	std::vector<uint8_t> d(degree, 0);
	d[degree - 1] = 1;
	uint8_t root = 1;
	for (int i = 0; i < degree; i++) {
		for (int j = 0; j < degree; j++) {
			d[j] = gf_mul(d[j], root);
			if (j + 1 < degree) d[j] ^= d[j + 1];
		}
		root = gf_mul(root, 2);
	}
	return d;
}

inline std::vector<uint8_t> rs_remainder(const uint8_t *data, int len, const std::vector<uint8_t> &div) {
	std::vector<uint8_t> r(div.size(), 0);
	for (int i = 0; i < len; i++) {
		uint8_t f = data[i] ^ r[0];
		r.erase(r.begin());
		r.push_back(0);
		for (size_t j = 0; j < r.size(); j++) r[j] ^= gf_mul(div[j], f);
	}
	return r;
}

inline std::vector<int> align_positions(int v) {
	if (v == 1) return {};
	int count = v / 7 + 2, step = v == 32 ? 26 : (v * 4 + count * 2 + 1) / (count * 2 - 2) * 2;
	std::vector<int> out{6};
	for (int i = 0, pos = v * 4 + 10; i < count - 1; i++, pos -= step) out.insert(out.begin() + 1, pos);
	return out;
}

inline bool mask_bit(int m, int x, int y) {
	switch (m) {
	case 0: return (x + y) % 2 == 0;
	case 1: return y % 2 == 0;
	case 2: return x % 3 == 0;
	case 3: return (x + y) % 3 == 0;
	case 4: return (x / 3 + y / 2) % 2 == 0;
	case 5: return x * y % 2 + x * y % 3 == 0;
	case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
	default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
	}
}

inline void set_fn(Code &c, int x, int y, bool on) {
	c.cells[size_t(y) * c.n + x] = on;
	c.fixed[size_t(y) * c.n + x] = 1;
}

inline void format_bits(Code &c, int mask) {
	int data = (0 << 3) | mask;	// M is 00
	int rem = data;
	for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
	int bits = ((data << 10) | rem) ^ 0x5412;
	auto bit = [&](int i) { return ((bits >> i) & 1) != 0; };
	for (int i = 0; i <= 5; i++) set_fn(c, 8, i, bit(i));
	set_fn(c, 8, 7, bit(6));
	set_fn(c, 8, 8, bit(7));
	set_fn(c, 7, 8, bit(8));
	for (int i = 9; i < 15; i++) set_fn(c, 14 - i, 8, bit(i));
	for (int i = 0; i < 8; i++) set_fn(c, c.n - 1 - i, 8, bit(i));
	for (int i = 8; i < 15; i++) set_fn(c, 8, c.n - 15 + i, bit(i));
	set_fn(c, 8, c.n - 8, true);
}

inline void function_patterns(Code &c, int v) {
	for (int i = 0; i < c.n; i++) {
		set_fn(c, 6, i, i % 2 == 0);
		set_fn(c, i, 6, i % 2 == 0);
	}
	auto finder = [&](int cx, int cy) {
		for (int dy = -4; dy <= 4; dy++) for (int dx = -4; dx <= 4; dx++) {
			int d = std::max(std::abs(dx), std::abs(dy)), x = cx + dx, y = cy + dy;
			if (x >= 0 && x < c.n && y >= 0 && y < c.n) set_fn(c, x, y, d != 2 && d != 4);
		}
	};
	finder(3, 3);
	finder(c.n - 4, 3);
	finder(3, c.n - 4);
	auto pos = align_positions(v);
	int k = int(pos.size());
	for (int i = 0; i < k; i++) for (int j = 0; j < k; j++) {
		if ((i == 0 && j == 0) || (i == 0 && j == k - 1) || (i == k - 1 && j == 0)) continue;
		for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++)
			set_fn(c, pos[i] + dx, pos[j] + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
	}
	format_bits(c, 0);
	if (v >= 7) {
		int rem = v;
		for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1f25);
		long bits = long(v) << 12 | rem;
		for (int i = 0; i < 18; i++) {
			bool on = (bits >> i) & 1;
			int a = c.n - 11 + i % 3, b = i / 3;
			set_fn(c, a, b, on);
			set_fn(c, b, a, on);
		}
	}
}

inline long penalty(const Code &c) {
	long score = 0;
	int n = c.n;
	auto line = [&](auto get) {
		int run = 0;
		bool last = false;
		unsigned hist = 0;
		for (int i = 0; i < n; i++) {
			bool b = get(i);
			if (i && b == last) {
				run++;
				if (run == 5) score += 3;
				else if (run > 5) score++;
			} else {
				run = 1;
				last = b;
			}
			hist = ((hist << 1) | b) & 0x7ff;
			if (i >= 10 && (hist == 0x05d || hist == 0x5d0)) score += 40;
		}
	};
	for (int y = 0; y < n; y++) line([&](int x) { return c.at(x, y); });
	for (int x = 0; x < n; x++) line([&](int y) { return c.at(x, y); });
	long dark = 0;
	for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
		dark += c.at(x, y);
		if (x + 1 < n && y + 1 < n && c.at(x, y) == c.at(x + 1, y) && c.at(x, y) == c.at(x, y + 1) &&
				c.at(x, y) == c.at(x + 1, y + 1)) score += 3;
	}
	long total = long(n) * n;
	score += (std::abs(dark * 20 - total * 10) + total - 1) / total * 10 - 10 > 0 ?
		((std::abs(dark * 20 - total * 10) + total - 1) / total - 1) * 10 : 0;
	return score;
}
}	// namespace detail

// n == 0: longer than a version 40 code holds
inline Code encode(std::string_view text) {
	using namespace detail;
	int v = 1;
	for (;; v++) {
		if (v > 40) return {};
		int count_bits = v < 10 ? 8 : 16;
		if (4 + count_bits + int(text.size()) * 8 <= data_codewords(v) * 8) break;
	}
	int count_bits = v < 10 ? 8 : 16, capacity = data_codewords(v);
	std::vector<uint8_t> data(capacity, 0);
	int at = 0;
	auto put = [&](unsigned value, int bits) {
		for (int i = bits - 1; i >= 0; i--, at++)
			if ((value >> i) & 1) data[at >> 3] |= uint8_t(1 << (7 - (at & 7)));
	};
	put(4, 4);
	put(unsigned(text.size()), count_bits);
	for (unsigned char ch : text) put(ch, 8);
	at += std::min(4, capacity * 8 - at);
	at = (at + 7) & ~7;
	for (uint8_t pad = 0xec; at < capacity * 8; pad ^= 0xec ^ 0x11) put(pad, 8);

	int nb = blocks[v], ecc = ecc_per_block[v], raw = raw_modules(v) / 8;
	int short_blocks = nb - raw % nb, short_len = raw / nb;
	auto div = rs_divisor(ecc);
	std::vector<std::vector<uint8_t>> parts;
	for (int i = 0, k = 0; i < nb; i++) {
		int dlen = short_len - ecc + (i < short_blocks ? 0 : 1);
		std::vector<uint8_t> b(data.begin() + k, data.begin() + k + dlen);
		k += dlen;
		auto rem = rs_remainder(b.data(), dlen, div);
		if (i < short_blocks) b.push_back(0);
		b.insert(b.end(), rem.begin(), rem.end());
		parts.push_back(std::move(b));
	}
	std::vector<uint8_t> all;
	for (size_t i = 0; i < parts[0].size(); i++) for (int j = 0; j < nb; j++)
		if (int(i) != short_len - ecc || j >= short_blocks) all.push_back(parts[j][i]);

	Code c;
	c.n = v * 4 + 17;
	c.cells.assign(size_t(c.n) * c.n, 0);
	c.fixed.assign(size_t(c.n) * c.n, 0);
	function_patterns(c, v);
	size_t bit = 0;
	for (int right = c.n - 1; right >= 1; right -= 2) {
		if (right == 6) right = 5;
		for (int vert = 0; vert < c.n; vert++) for (int j = 0; j < 2; j++) {
			int x = right - j, y = ((right + 1) & 2) == 0 ? c.n - 1 - vert : vert;
			size_t ix = size_t(y) * c.n + x;
			if (!c.fixed[ix] && bit < all.size() * 8) {
				c.cells[ix] = (all[bit >> 3] >> (7 - (bit & 7))) & 1;
				bit++;
			}
		}
	}
	auto apply = [&](int m) {
		for (int y = 0; y < c.n; y++) for (int x = 0; x < c.n; x++) {
			size_t ix = size_t(y) * c.n + x;
			if (!c.fixed[ix] && mask_bit(m, x, y)) c.cells[ix] ^= 1;
		}
	};
	int best = 0;
	long best_score = -1;
	for (int m = 0; m < 8; m++) {
		apply(m);
		format_bits(c, m);
		long s = penalty(c);
		if (best_score < 0 || s < best_score) { best_score = s; best = m; }
		apply(m);
	}
	apply(best);
	format_bits(c, best);
	return c;
}

}	// namespace qr
