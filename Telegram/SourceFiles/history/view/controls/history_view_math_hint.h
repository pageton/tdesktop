/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "base/unique_qptr.h"

#include <rpl/rpl.h>

namespace Ui {
class InputField;
class RoundButton;
} // namespace Ui

namespace HistoryView::Controls {

class MathHint final {
public:
	MathHint(
		not_null<Ui::InputField*> field,
		Fn<void(QString)> accepted);

private:
	void update();
	void updateGeometry();
	void accept();

	const not_null<Ui::InputField*> _field;
	const Fn<void(QString)> _accept;
	base::unique_qptr<Ui::RoundButton> _button;
	QString _expression;
	QString _result;
	rpl::lifetime _lifetime;

};

} // namespace HistoryView::Controls
