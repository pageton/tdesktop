/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/controls/history_view_math_hint.h"

#include "base/event_filter.h"
#include "chat_helpers/math_expression.h"
#include "ui/qt_object_factory.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "styles/style_history_view_math_hint.h"

#include <QtGui/QKeyEvent>
#include <QtWidgets/QTextEdit>

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
	Fn<void(QString)> accepted)
: _field(field)
, _accept(std::move(accepted))
, _button(base::unique_qptr<Ui::RoundButton>(
	Ui::CreateChild<Ui::RoundButton>(
		field,
		rpl::single(QString()),
		st::historyMathHint))) {
	_button->hide();
	_button->setClickedCallback([=] { accept(); });
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
	base::install_event_filter(
		not_null<QObject*>(_field->rawTextEdit().get()),
		[=](not_null<QEvent*> event) {
			if (event->type() == QEvent::KeyPress) {
				const auto key = static_cast<QKeyEvent*>(event.get());
				if (key->key() == Qt::Key_Tab && !_result.isEmpty()) {
					accept();
					return base::EventFilterResult::Cancel;
				}
			}
			return base::EventFilterResult::Continue;
		},
		_lifetime);
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

void MathHint::accept() {
	if (!_result.isEmpty()) {
		_accept(u"%1=%2"_q.arg(_expression, _result));
	}
}

} // namespace HistoryView::Controls
