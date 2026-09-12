/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_helpers/math_expression.h"

#include <cmath>
#include <cstdlib>
#include <string>

namespace ChatHelpers {
namespace {

constexpr auto kMaxExpressionLength = size_t(256);
constexpr auto kMaxDepth = 32;

[[nodiscard]] bool IsDigit(char ch) {
	return (ch >= '0') && (ch <= '9');
}

[[nodiscard]] bool IsLetter(char ch) {
	return (ch >= 'a') && (ch <= 'z');
}

[[nodiscard]] double FunctionValue(
		std::string_view name,
		double argument,
		bool *found) {
	*found = true;
	if (name == "sqrt") {
		return std::sqrt(argument);
	} else if (name == "cbrt") {
		return std::cbrt(argument);
	} else if (name == "abs") {
		return std::abs(argument);
	} else if (name == "exp") {
		return std::exp(argument);
	} else if (name == "ln") {
		return std::log(argument);
	} else if (name == "log") {
		return std::log10(argument);
	} else if (name == "log2") {
		return std::log2(argument);
	} else if (name == "sin") {
		return std::sin(argument);
	} else if (name == "cos") {
		return std::cos(argument);
	} else if (name == "tan") {
		return std::tan(argument);
	} else if (name == "asin") {
		return std::asin(argument);
	} else if (name == "acos") {
		return std::acos(argument);
	} else if (name == "atan") {
		return std::atan(argument);
	} else if (name == "sinh") {
		return std::sinh(argument);
	} else if (name == "cosh") {
		return std::cosh(argument);
	} else if (name == "tanh") {
		return std::tanh(argument);
	} else if (name == "floor") {
		return std::floor(argument);
	} else if (name == "ceil") {
		return std::ceil(argument);
	} else if (name == "round") {
		return std::round(argument);
	}
	*found = false;
	return 0.;
}

class Parser final {
public:
	explicit Parser(std::string_view text)
	: _text(text) {
	}

	[[nodiscard]] std::optional<double> parse() {
		skipSpaces();
		const auto result = parseExpression(0);
		if (!result) {
			return std::nullopt;
		}
		skipSpaces();
		if (_position != _text.size()) {
			return std::nullopt;
		}
		if (!_sawOperator && !_sawFunction) {
			return std::nullopt;
		}
		if (!std::isfinite(*result)) {
			return std::nullopt;
		}
		return result;
	}

private:
	[[nodiscard]] bool atEnd() const {
		return _position >= _text.size();
	}

	[[nodiscard]] char current() const {
		return atEnd() ? char() : _text[_position];
	}

	void skipSpaces() {
		while (!atEnd() && (current() == ' ' || current() == '\t')) {
			++_position;
		}
	}

	[[nodiscard]] std::optional<double> parseExpression(int depth) {
		if (depth > kMaxDepth) {
			return std::nullopt;
		}
		auto value = parseTerm(depth);
		if (!value) {
			return std::nullopt;
		}
		while (true) {
			skipSpaces();
			const auto op = current();
			if (op != '+' && op != '-') {
				break;
			}
			++_position;
			_sawOperator = true;
			const auto right = parseTerm(depth);
			if (!right) {
				return std::nullopt;
			}
			*value = (op == '+') ? (*value + *right) : (*value - *right);
			if (!std::isfinite(*value)) {
				return std::nullopt;
			}
		}
		return value;
	}

	[[nodiscard]] std::optional<double> parseTerm(int depth) {
		auto value = parseUnary(depth);
		if (!value) {
			return std::nullopt;
		}
		while (true) {
			skipSpaces();
			const auto op = current();
			if (op != '*' && op != '/' && op != '%') {
				break;
			}
			++_position;
			_sawOperator = true;
			const auto right = parseUnary(depth);
			if (!right) {
				return std::nullopt;
			}
			switch (op) {
			case '*': *value *= *right; break;
			case '/': *value /= *right; break;
			case '%': *value = std::fmod(*value, *right); break;
			}
			if (!std::isfinite(*value)) {
				return std::nullopt;
			}
		}
		return value;
	}

	[[nodiscard]] std::optional<double> parseUnary(int depth) {
		if (depth > kMaxDepth) {
			return std::nullopt;
		}
		skipSpaces();
		const auto op = current();
		if (op == '+' || op == '-') {
			++_position;
			const auto value = parseUnary(depth + 1);
			if (!value) {
				return std::nullopt;
			}
			return (op == '-') ? std::optional<double>(-*value) : value;
		}
		return parsePower(depth);
	}

	[[nodiscard]] std::optional<double> parsePower(int depth) {
		const auto base = parsePrimary(depth);
		if (!base) {
			return std::nullopt;
		}
		skipSpaces();
		if (current() != '^') {
			return base;
		}
		++_position;
		_sawOperator = true;
		const auto exponent = parseUnary(depth + 1);
		if (!exponent) {
			return std::nullopt;
		}
		const auto result = std::pow(*base, *exponent);
		if (!std::isfinite(result)) {
			return std::nullopt;
		}
		return result;
	}

	[[nodiscard]] std::optional<double> parsePrimary(int depth) {
		if (depth > kMaxDepth) {
			return std::nullopt;
		}
		skipSpaces();
		const auto ch = current();
		if (ch == '(') {
			++_position;
			const auto value = parseExpression(depth + 1);
			if (!value) {
				return std::nullopt;
			}
			skipSpaces();
			if (current() != ')') {
				return std::nullopt;
			}
			++_position;
			return value;
		} else if (IsDigit(ch) || ch == '.') {
			return parseNumber();
		} else if (IsLetter(ch)) {
			return parseIdentifier(depth);
		}
		return std::nullopt;
	}

	[[nodiscard]] std::optional<double> parseNumber() {
		const auto start = _position;
		auto digits = 0;
		while (!atEnd() && IsDigit(current())) {
			++_position;
			++digits;
		}
		if (!atEnd() && current() == '.') {
			++_position;
			while (!atEnd() && IsDigit(current())) {
				++_position;
				++digits;
			}
		}
		if (!digits) {
			return std::nullopt;
		}
		if (!atEnd() && (current() == 'e' || current() == 'E')) {
			const auto saved = _position;
			++_position;
			if (!atEnd() && (current() == '+' || current() == '-')) {
				++_position;
			}
			auto exponentDigits = 0;
			while (!atEnd() && IsDigit(current())) {
				++_position;
				++exponentDigits;
			}
			if (!exponentDigits) {
				_position = saved;
			}
		}
		const auto buffer = std::string(
			_text.substr(start, _position - start));
		char *end = nullptr;
		const auto value = std::strtod(buffer.c_str(), &end);
		if (end != buffer.c_str() + buffer.size()) {
			return std::nullopt;
		}
		if (!std::isfinite(value)) {
			return std::nullopt;
		}
		return value;
	}

	[[nodiscard]] std::optional<double> parseIdentifier(int depth) {
		const auto start = _position;
		while (!atEnd() && IsLetter(current())) {
			++_position;
		}
		const auto name = _text.substr(start, _position - start);
		if (name == "pi") {
			return 3.14159265358979323846;
		} else if (name == "e") {
			return 2.71828182845904523536;
		} else if (name == "tau") {
			return 6.28318530717958647692;
		}
		skipSpaces();
		if (current() != '(') {
			return std::nullopt;
		}
		++_position;
		const auto argument = parseExpression(depth + 1);
		if (!argument) {
			return std::nullopt;
		}
		skipSpaces();
		if (current() != ')') {
			return std::nullopt;
		}
		++_position;
		auto found = false;
		const auto result = FunctionValue(name, *argument, &found);
		if (!found) {
			return std::nullopt;
		}
		_sawFunction = true;
		if (!std::isfinite(result)) {
			return std::nullopt;
		}
		return result;
	}

	const std::string_view _text;
	size_t _position = 0;
	bool _sawOperator = false;
	bool _sawFunction = false;

};

} // namespace

std::optional<double> EvaluateMathExpression(std::string_view expression) {
	if (expression.empty() || expression.size() > kMaxExpressionLength) {
		return std::nullopt;
	}
	return Parser(expression).parse();
}

} // namespace ChatHelpers
