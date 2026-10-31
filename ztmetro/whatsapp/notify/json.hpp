// SPDX-License-Identifier: GPL-2.0-or-later
// Just enough JSON for one IPC line: objects, arrays, strings, numbers, booleans.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zwn::json {

struct Value {
	enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
	bool boolean = false;
	double number = 0;
	std::string string;
	std::vector<Value> array;
	std::map<std::string, Value, std::less<>> object;

	const Value *find(std::string_view key) const;
	std::string str(std::string_view key) const;
	bool flag(std::string_view key) const;
	int64_t integer(std::string_view key, int64_t otherwise = 0) const;
};

// nullopt for anything malformed, too deep, or followed by more than white space.
std::optional<Value> parse(std::string_view text);

}
