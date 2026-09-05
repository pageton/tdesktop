/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/details/mtproto_tl_json.h"

namespace MTP::details {

QJsonObject TlMessageToJson(const MTPMessage &message) {
	return TlObjectToJson(message);
}

} // namespace MTP::details
