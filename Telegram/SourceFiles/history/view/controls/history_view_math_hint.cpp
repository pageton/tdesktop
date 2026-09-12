/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/controls/history_view_math_hint.h"

#include "chat_helpers/math_expression.h"
#include "ui/qt_object_factory.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "styles/style_history_view_math_hint.h"

#include <cmath>

namespace HistoryView::Controls {
namespace {

[[nodiscard]] QString FormatMathResult(double value) {
	if (std::abs(value) < 1e-15) {
		value = 0.;
	}
	return QString::number(value, 'g', 12);
}

} // namespace

MathHint::MathHint(
	not_null<Ui::InputField*> field,
	Fn<void(QString)> accept)
: _field(field)
, _accept(std::move(accept))
, _button(base::unique_qptr<Ui::RoundButton>(
	Ui::CreateChild<Ui::RoundButton>(
		field,
		rpl::single(QString()),
		st::historyMathHint))) {
	_button->hide();
	_button->setClickedCallback([=] {
		if (!_result.isEmpty()) {
			_accept(u"%1=%2"_q.arg(_expression, _result));
		}
	});
	_button->widthValue(
	) | rpl::on_next([=] {
		updateGeometry();
	}, _button->lifetime());
	_field->sizeValue(
	) | rpl::on_next([=] {
		updateGeometry();
	}, _button->lifetime());
	_field->changes(
	) | rpl::on_next([=] {
		update();
	}, _button->lifetime());
}

void MathHint::update() {
	const auto &textWithTags = _field->getTextWithTags();
	auto expression = QString();
	auto result = std::optional<double>();
	if (textWithTags.tags.empty()) {
		expression = textWithTags.text.trimmed();
		while (!expression.isEmpty() && expression.back() == u'=') {
			expression.chop(1);
			expression = expression.trimmed();
		}
		const auto text = expression.toUtf8();
		result = ChatHelpers::EvaluateMathExpression(
			std::string_view(text.data(), text.size()));
	}
	if (!result) {
		_button->hide();
		_expression.clear();
		_result.clear();
		return;
	}
	_expression = expression;
	_result = FormatMathResult(*result);
	_button->setText(rpl::single(u"= %1"_q.arg(_result)));
	_button->show();
	updateGeometry();
}

void MathHint::updateGeometry() {
	if (_button->isHidden()) {
		return;
	}
	_button->moveToLeft(
		_field->width() - _button->width() - st::historyMathHintSkip,
		_field->height() - _button->height() - st::historyMathHintSkip);
}

} // namespace HistoryView::Controls
