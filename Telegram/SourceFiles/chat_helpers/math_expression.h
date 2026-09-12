/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <optional>
#include <string_view>

namespace ChatHelpers {

// Evaluates a single arithmetic expression, like "2+2" or "sqrt(16)/2".
// Returns nullopt if the whole string isn't a valid expression, if it has
// no actual operation (a lone number is not interesting), or if the result
// is not a finite number.
[[nodiscard]] std::optional<double> EvaluateMathExpression(
	std::string_view expression);

} // namespace ChatHelpers
