// SPDX-License-Identifier: GPL-2.0-or-later
#include "json.hpp"

#include <charconv>

namespace zwn::json {
namespace {

constexpr int MAX_DEPTH = 32;

struct Parser {
	std::string_view s;
	size_t at = 0;

	void space()
	{
		while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\r' || s[at] == '\n'))
			at++;
	}

	bool literal(std::string_view word)
	{
		if (s.substr(at, word.size()) != word)
			return false;
		at += word.size();
		return true;
	}

	static void utf8(std::string &out, uint32_t c)
	{
		if (c < 0x80) {
			out += char(c);
		} else if (c < 0x800) {
			out += char(0xc0 | c >> 6);
			out += char(0x80 | (c & 0x3f));
		} else if (c < 0x10000) {
			out += char(0xe0 | c >> 12);
			out += char(0x80 | ((c >> 6) & 0x3f));
			out += char(0x80 | (c & 0x3f));
		} else {
			out += char(0xf0 | c >> 18);
			out += char(0x80 | ((c >> 12) & 0x3f));
			out += char(0x80 | ((c >> 6) & 0x3f));
			out += char(0x80 | (c & 0x3f));
		}
	}

	bool hex4(uint32_t &out)
	{
		if (at + 4 > s.size())
			return false;
		unsigned v = 0;
		auto r = std::from_chars(s.data() + at, s.data() + at + 4, v, 16);
		if (r.ec != std::errc{} || r.ptr != s.data() + at + 4)
			return false;
		at += 4;
		out = v;
		return true;
	}

	bool string(std::string &out)
	{
		if (at >= s.size() || s[at] != '"')
			return false;
		at++;
		while (at < s.size()) {
			char c = s[at++];
			if (c == '"')
				return true;
			if (c != '\\') {
				out += c;
				continue;
			}
			if (at >= s.size())
				return false;
			c = s[at++];
			switch (c) {
			case 'n': out += '\n'; break;
			case 't': out += '\t'; break;
			case 'r': out += '\r'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case '"': case '\\': case '/': out += c; break;
			case 'u': {
				uint32_t a;
				if (!hex4(a))
					return false;
				if (a >= 0xd800 && a < 0xdc00 && literal("\\u")) {
					uint32_t b;
					if (!hex4(b) || b < 0xdc00 || b > 0xdfff)
						return false;
					a = 0x10000 + ((a - 0xd800) << 10) + (b - 0xdc00);
				} else if (a >= 0xd800 && a < 0xe000) {
					a = 0xfffd;	// a lone surrogate
				}
				utf8(out, a);
				break;
			}
			default:
				return false;
			}
		}
		return false;
	}

	bool value(Value &v, int depth)
	{
		if (depth > MAX_DEPTH)
			return false;
		space();
		if (at >= s.size())
			return false;
		char c = s[at];
		if (c == '{') {
			v.type = Value::Type::Object;
			at++;
			space();
			if (at < s.size() && s[at] == '}') {
				at++;
				return true;
			}
			for (;;) {
				std::string key;
				space();
				if (!string(key))
					return false;
				space();
				if (at >= s.size() || s[at++] != ':')
					return false;
				Value item;
				if (!value(item, depth + 1))
					return false;
				v.object.insert_or_assign(std::move(key), std::move(item));
				space();
				if (at >= s.size())
					return false;
				c = s[at++];
				if (c == '}')
					return true;
				if (c != ',')
					return false;
			}
		}
		if (c == '[') {
			v.type = Value::Type::Array;
			at++;
			space();
			if (at < s.size() && s[at] == ']') {
				at++;
				return true;
			}
			for (;;) {
				Value item;
				if (!value(item, depth + 1))
					return false;
				v.array.push_back(std::move(item));
				space();
				if (at >= s.size())
					return false;
				c = s[at++];
				if (c == ']')
					return true;
				if (c != ',')
					return false;
			}
		}
		if (c == '"') {
			v.type = Value::Type::String;
			return string(v.string);
		}
		if (literal("true")) {
			v.type = Value::Type::Bool;
			v.boolean = true;
			return true;
		}
		if (literal("false")) {
			v.type = Value::Type::Bool;
			return true;
		}
		if (literal("null"))
			return true;
		auto r = std::from_chars(s.data() + at, s.data() + s.size(), v.number);
		if (r.ec != std::errc{})
			return false;
		at = size_t(r.ptr - s.data());
		v.type = Value::Type::Number;
		return true;
	}
};

}

const Value *Value::find(std::string_view key) const
{
	auto it = object.find(key);
	return it == object.end() ? nullptr : &it->second;
}

std::string Value::str(std::string_view key) const
{
	const Value *v = find(key);
	return v && v->type == Type::String ? v->string : std::string();
}

bool Value::flag(std::string_view key) const
{
	const Value *v = find(key);
	return v && v->type == Type::Bool && v->boolean;
}

int64_t Value::integer(std::string_view key, int64_t otherwise) const
{
	const Value *v = find(key);
	return v && v->type == Type::Number ? int64_t(v->number) : otherwise;
}

std::optional<Value> parse(std::string_view text)
{
	Parser p{text};
	Value v;
	if (!p.value(v, 0))
		return std::nullopt;
	p.space();
	if (p.at != text.size())
		return std::nullopt;
	return v;
}

}
