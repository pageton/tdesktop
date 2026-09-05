/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Dev::Rpc {

// Opens the RPC Inspector box over the passed session window. Does
// nothing unless the RPC Inspector experimental option is enabled.
void ShowRpcInspector(not_null<Window::SessionController*> controller);

} // namespace Dev::Rpc
