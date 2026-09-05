/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "scheme.h"
#include "scheme-tl_json.h"

#include <QtCore/QJsonObject>

namespace MTP::details {

// Complete recursive JSON representation of a raw TL object, written to
// its wire form and decoded back: every constructor keeps its identity
// in the "_" key, every field that exists on the wire is present
// (flag-disabled optionals are absent), nothing is simplified or
// replaced by UI-level data.
template <typename Object>
[[nodiscard]] QJsonObject TlObjectToJson(const Object &object) {
	auto primes = mtpBuffer();
	object.write(primes);
	auto result = QJsonObject();
	const auto data = primes.constData();
	auto from = data;
	if (!primes.isEmpty()
		&& TlJsonDecodeBoxed(result, from, from + primes.size())) {
		return result;
	}
	result.insert(u"_"_q, u"error"_q);
	return result;
}

[[nodiscard]] QJsonObject TlMessageToJson(const MTPMessage &message);

} // namespace MTP::details
