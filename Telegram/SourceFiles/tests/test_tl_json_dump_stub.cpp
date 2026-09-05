/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/details/mtproto_dump_to_text.h"

namespace MTP::details {

// The td_scheme object library references this from the generated
// scheme-dump_to_text.cpp, the real implementation lives in the app
// target. The JSON test links the generated objects but never dumps.
bool DumpToTextCore(
		DumpToTextBuffer &to,
		const mtpPrime *&from,
		const mtpPrime *end,
		mtpTypeId cons,
		uint32 level,
		mtpPrime vcons) {
	return false;
}

} // namespace MTP::details
