/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "devtools/devtools_rpc_scheme.h"

#include <QtCore/QFile>

#include <algorithm>

namespace Dev::Rpc {
namespace {

[[nodiscard]] std::vector<ConstructorMeta> ParseApiScheme() {
	auto result = std::vector<ConstructorMeta>();
	auto file = QFile(u":/gui/api.tl"_q);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return result;
	}
	const auto text = QString::fromUtf8(file.readAll());
	for (auto line : text.split(u'\n')) {
		line = line.trimmed();
		if (line.isEmpty() || line.startsWith(u"//")) {
			continue;
		}
		const auto hash = line.indexOf(u'#');
		const auto eq = line.indexOf(u'=');
		if (hash <= 0 || eq < hash) {
			continue;
		}
		auto meta = ConstructorMeta();
		meta.name = line.left(hash).trimmed();
		auto resultType = line.mid(eq + 1).trimmed();
		if (resultType.endsWith(u';')) {
			resultType.chop(1);
		}
		meta.type = resultType.trimmed();
		const auto body = line.mid(hash + 1, eq - hash - 1);
		for (auto token : body.split(u' ')) {
			token = token.trimmed();
			const auto colon = token.indexOf(u':');
			if (colon <= 0) {
				continue;
			}
			const auto type = token.mid(colon + 1);
			if (type == u"#") {
				// The flags field is computed by the JSON encoder.
				continue;
			}
			meta.fields.push_back({ token.left(colon), type });
		}
		result.push_back(std::move(meta));
	}
	std::sort(result.begin(), result.end());
	return result;
}

} // namespace

const std::vector<ConstructorMeta> &Constructors() {
	static const auto value = ParseApiScheme();
	return value;
}

const ConstructorMeta *FindConstructor(const QString &name) {
	const auto &all = Constructors();
	const auto i = std::lower_bound(
		all.begin(),
		all.end(),
		name,
		[](const ConstructorMeta &a, const QString &n) {
			return a.name < n;
		});
	return (i != all.end() && i->name == name) ? &*i : nullptr;
}

std::vector<const ConstructorMeta*> ConstructorsOfType(
		const QString &type) {
	auto result = std::vector<const ConstructorMeta*>();
	for (const auto &constructor : Constructors()) {
		if (constructor.type == type) {
			result.push_back(&constructor);
		}
	}
	return result;
}

} // namespace Dev::Rpc
