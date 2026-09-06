/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QString>
#include <vector>

namespace Dev::Rpc {

struct ConstructorField {
	QString name;
	QString type;
};

struct ConstructorMeta {
	QString name;
	QString type; // The type after '=', without the trailing ';'.
	std::vector<ConstructorField> fields;

	friend inline bool operator<(
			const ConstructorMeta &a,
			const ConstructorMeta &b) {
		return a.name < b.name;
	}
};

// All constructors of the embedded api scheme, parsed lazily on first
// call, sorted by name.
[[nodiscard]] const std::vector<ConstructorMeta> &Constructors();

// Look up a constructor by its exact TL name ("messages.sendMessage").
[[nodiscard]] const ConstructorMeta *FindConstructor(const QString &name);

// All constructors declaring `= type`, in declaration order (the first
// one is the canonical constructor of the type).
[[nodiscard]] std::vector<const ConstructorMeta*> ConstructorsOfType(
	const QString &type);

} // namespace Dev::Rpc
