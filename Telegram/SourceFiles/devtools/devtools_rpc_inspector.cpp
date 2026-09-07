/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "devtools/devtools_rpc_inspector.h"

#include "base/options.h"
#include "base/unique_qptr.h"
#include "devtools/devtools_rpc_log.h"
#include "devtools/devtools_rpc_scheme.h"
#include "main/main_session.h"
#include "mtproto/mtproto_response.h"
#include "platform/platform_specific.h"
#include "scheme.h"
#include "scheme-tl_json.h"
#include "ui/boxes/confirm_box.h"
#include "ui/painter.h"
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/abstract_button.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/selecting_scroll.h"
#include "ui/widgets/separate_panel.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_devtools.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QRegularExpression>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtGui/QTextBlock>
#include <QtWidgets/QTextEdit>

#include <algorithm>
#include <array>

namespace Dev::Rpc {
namespace {

constexpr auto kMaxInlineJson = 256 * 1024;

struct Query {
	QString text;
	std::vector<qint64> ids;
	std::optional<int> dc;
	std::optional<bool> errors;
	std::optional<int> minDuration;
	std::optional<int> maxDuration;

	[[nodiscard]] bool empty() const {
		return text.isEmpty()
			&& ids.empty()
			&& !dc.has_value()
			&& !errors.has_value()
			&& !minDuration.has_value()
			&& !maxDuration.has_value();
	}
};

// Parse a bare id token: plain numbers as-is, "-100"-prefixed channel
// ids as the bare channel id (the -100 form never appears in TL bodies).
[[nodiscard]] std::optional<qint64> ParseIdToken(const QString &token) {
	auto digits = token;
	if (digits.startsWith(u"-100")) {
		digits = digits.mid(4);
	} else if (digits.startsWith(u'-')) {
		digits = digits.mid(1);
	}
	if (digits.isEmpty()
		|| digits.size() > 19
		|| !ranges::all_of(digits, [](QChar c) { return c.isDigit(); })) {
		return std::nullopt;
	}
	return digits.toLongLong();
}

[[nodiscard]] Query ParseQuery(const QString &text) {
	auto result = Query();
	auto freeText = QStringList();
	for (const auto &token : text.simplified().split(' ', Qt::SkipEmptyParts)) {
		if (token == u"error"_q) {
			result.errors = true;
		} else if (token == u"ok"_q) {
			result.errors = false;
		} else if (token.startsWith(u"dc:"_q)) {
			result.dc = token.mid(3).toInt();
		} else if (token.startsWith(u'>') || token.startsWith(u'<')) {
			const auto bounded = (token[0] == u'>');
			auto number = token.mid(1);
			auto ms = false;
			if (number.endsWith(u"ms"_q)) {
				number.chop(2);
				ms = true;
			} else if (number.endsWith(u"s"_q)) {
				number.chop(1);
			}
			const auto value = number.toInt();
			if (value > 0) {
				const auto millis = ms ? value : value * 1000;
				(bounded ? result.minDuration : result.maxDuration) = millis;
			}
		} else if (const auto id = ParseIdToken(token)) {
			result.ids.push_back(*id);
		} else {
			freeText.push_back(token);
		}
	}
	result.text = freeText.join(u' ');
	return result;
}

// Search the raw TL wire for the id as an int32 prime, or as the low/high
// prime pair of an int64. This finds the id in any field of the request
// or response without decoding them.
[[nodiscard]] bool BodyContainsId(const QByteArray &body, qint64 id) {
	if (id == 0 || body.size() < int(sizeof(quint32))) {
		return false;
	}
	const auto primes = reinterpret_cast<const quint32*>(body.constData());
	const auto count = int(body.size() / sizeof(quint32));
	const auto lo = quint32(quint64(id) & 0xFFFFFFFFULL);
	const auto hi = quint32(quint64(id) >> 32);
	for (auto i = 0; i != count; ++i) {
		if (primes[i] != lo) {
			continue;
		} else if (hi == 0) {
			return true;
		} else if (i + 1 < count && primes[i + 1] == hi) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool Matches(const Event &event, const Query &query) {
	if (!query.text.isEmpty()
		&& !event.method.contains(query.text, Qt::CaseInsensitive)
		&& !event.errorType.contains(query.text, Qt::CaseInsensitive)) {
		return false;
	}
	for (const auto id : query.ids) {
		if (!BodyContainsId(event.request, id)
			&& !BodyContainsId(event.response, id)) {
			return false;
		}
	}
	if (query.dc && query.dc != MTP::BareDcId(event.dcId)) {
		return false;
	} else if (query.errors && event.status != Status::Failed) {
		return false;
	}
	const auto duration = (event.finished && event.started)
		? (event.finished - event.started)
		: 0;
	if (query.minDuration && duration < *query.minDuration) {
		return false;
	} else if (query.maxDuration && duration > *query.maxDuration) {
		return false;
	}
	return true;
}

[[nodiscard]] QString FormatSize(int bytes) {
	if (bytes >= 1024 * 1024) {
		return u"%1 MB"_q.arg(QString::number(
			bytes / double(1024 * 1024),
			'f',
			1));
	} else if (bytes >= 1024) {
		return u"%1 KB"_q.arg(QString::number(bytes / 1024.0, 'f', 0));
	}
	return u"%1 B"_q.arg(bytes);
}

[[nodiscard]] QString FormatDuration(crl::time ms) {
	if (ms >= 10000) {
		return u"%1 s"_q.arg(QString::number(ms / 1000.0, 'f', 0));
	} else if (ms >= 1000) {
		return u"%1 s"_q.arg(QString::number(ms / 1000.0, 'f', 1));
	}
	return u"%1 ms"_q.arg(ms);
}

[[nodiscard]] QString JsonText(const QJsonObject &object) {
	return QString::fromUtf8(
		QJsonDocument(object).toJson(QJsonDocument::Indented));
}

void CopyText(const QString &text) {
	QGuiApplication::clipboard()->setText(text);
}

[[nodiscard]] bool IsReadOnlyMethod(const QString &name) {
	constexpr auto kReadOnlyPrefixes = std::array<const char*, 10>{
		"get",
		"search",
		"load",
		"check",
		"lookup",
		"resolve",
		"export",
		"is",
		"has",
		"test",
	};
	const auto dot = name.indexOf('.');
	const auto bare = (dot >= 0) ? name.mid(dot + 1) : name;
	return ranges::any_of(kReadOnlyPrefixes, [&](const char *prefix) {
		return bare.startsWith(QLatin1String(prefix));
	});
}

[[nodiscard]] uint64 FindEventId(mtpRequestId requestId) {
	for (const auto &event : Snapshot()) {
		if (event.requestId == requestId) {
			return event.id;
		}
	}
	return 0;
}

[[nodiscard]] QString StatusText(Status status) {
	switch (status) {
	case Status::Succeeded: return u"Success"_q;
	case Status::Failed: return u"Failed"_q;
	case Status::Cancelled: return u"Cancelled"_q;
	default: return u"Pending"_q;
	}
}

class TextButton final : public Ui::AbstractButton {
public:
	TextButton(QWidget *parent, const QString &text)
	: Ui::AbstractButton(parent)
	, _text(text) {
		resize(
			st::semiboldFont->width(_text) + 2 * st::rpcInspectorRowPadding.left(),
			st::semiboldFont->height);
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		p.setFont(st::semiboldFont);
		p.setPen(isOver()
			? st::windowActiveTextFg
			: st::windowSubTextFg);
		p.drawText(
			QPoint(st::rpcInspectorRowPadding.left(), st::semiboldFont->ascent),
			_text);
	}

private:
	QString _text;

};

class TabButton final : public Ui::AbstractButton {
public:
	TabButton(QWidget *parent, const QString &text)
	: Ui::AbstractButton(parent)
	, _text(text) {
		resize(
			st::semiboldFont->width(text) + 2 * st::rpcInspectorRowPadding.left(),
			st::defaultTabsSlider.height);
	}

	void setActive(bool active) {
		_active = active;
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		p.setFont(_active ? st::semiboldFont : st::boxTextFont);
		p.setPen(_active
			? st::windowActiveTextFg
			: (isOver() ? st::windowFg : st::windowSubTextFg));
		p.drawText(
			QPoint(st::rpcInspectorRowPadding.left(), st::semiboldFont->ascent),
			_text);
		if (_active) {
			p.fillRect(
				st::rpcInspectorRowPadding.left(),
				height() - st::lineWidth * 3,
				st::semiboldFont->width(_text),
				st::lineWidth * 3,
				st::windowActiveTextFg);
		}
	}

private:
	QString _text;
	bool _active = false;

};

class EventRow final : public Ui::AbstractButton {
public:
	EventRow(QWidget *parent, Event data, bool selected)
	: Ui::AbstractButton(parent)
	, _data(std::move(data))
	, _selected(selected) {
		resize(parent->width(), st::rpcInspectorRowHeight);
	}

	void updateData(Event data) {
		_data = std::move(data);
		update();
	}

	void setSelected(bool selected) {
		if (_selected != selected) {
			_selected = selected;
			update();
		}
	}

	[[nodiscard]] uint64 eventId() const {
		return _data.id;
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		if (_selected) {
			p.fillRect(rect(), st::windowBgOver);
			p.fillRect(
				0,
				0,
				st::lineWidth * 3,
				height(),
				st::windowActiveTextFg);
		} else if (isOver() || isDown()) {
			p.fillRect(rect(), st::windowBgOver);
		}
		const auto padding = st::rpcInspectorRowPadding;
		const auto left = padding.left() + st::lineWidth * 3;
		const auto top = padding.top();
		const auto textWidth = width()
			- left
			- padding.right()
			- st::rpcInspectorStatusSize * 2;

		p.setFont(st::boxTextFont);
		p.setPen(st::boxTextFg);
		p.drawText(
			left,
			top + st::boxTextFont->ascent,
			st::boxTextFont->elided(_data.method, textWidth));

		p.setFont(st::rpcInspectorMetaFont);
		p.setPen(st::windowSubTextFg);
		auto meta = u"%1 \u2022 DC %2 \u2022 L%3"_q.arg(
			_data.startedAt
				? QDateTime::fromMSecsSinceEpoch(_data.startedAt)
					.toLocalTime()
					.toString(u"HH:mm:ss"_q)
				: QString(),
			QString::number(MTP::BareDcId(_data.dcId)),
			QString::number(MTP::details::kCurrentLayer));
		if (_data.status == Status::Succeeded && _data.finished) {
			meta += u" \u2022 %1 \u2022 %2 \u2192 %3"_q.arg(
				FormatDuration(_data.finished - _data.started),
				FormatSize(_data.requestSize),
				FormatSize(_data.responseSize));
		} else if (_data.status == Status::Failed && !_data.errorType.isEmpty()) {
			meta += u" \u2022 %1"_q.arg(_data.errorType);
		} else if (_data.status == Status::Cancelled) {
			meta += u" \u2022 cancelled"_q;
		} else {
			meta += u" \u2022 pending \u2022 %1"_q.arg(
				FormatSize(_data.requestSize));
		}
		p.drawText(
			left,
			top + st::boxTextFont->height + st::rpcInspectorMetaFont->ascent,
			st::rpcInspectorMetaFont->elided(meta, textWidth));

		const auto size = st::rpcInspectorStatusSize;
		const auto cy = height() / 2;
		p.setPen(Qt::NoPen);
		switch (_data.status) {
		case Status::Succeeded:
			p.setBrush(st::boxTextFgGood);
			break;
		case Status::Failed:
			p.setBrush(st::boxTextFgError);
			break;
		default:
			p.setBrush(st::windowSubTextFg);
			break;
		}
		p.drawEllipse(
			width() - padding.right() - size,
			cy - size / 2,
			size,
			size);
	}

private:
	Event _data;
	bool _selected = false;

};

// The scrolling row container inside the log pane. Rows are newest first.
class HistoryList final : public Ui::RpWidget {
public:
	HistoryList(QWidget *parent, Fn<void(uint64)> openEvent)
	: Ui::RpWidget(parent)
	, _openEvent(std::move(openEvent)) {
		Rebuild();
	}

	void applyQuery(const QString &text) {
		_query = ParseQuery(text);
		Rebuild();
	}

	[[nodiscard]] bool eventUpdated(uint64 id) {
		for (const auto &row : _rows) {
			if (row->eventId() == id) {
				if (const auto event = Lookup(id)) {
					row->updateData(*event);
				}
				return false;
			}
		}
		for (const auto &event : Snapshot()) {
			if (event.id == id) {
				if (Matches(event, _query)) {
					Prepend(event);
					resizeToWidth(width());
					return true;
				}
				return false;
			}
		}
		return false;
	}

	void setSelected(uint64 id) {
		if (_selected == id) {
			return;
		}
		_selected = id;
		for (const auto &row : _rows) {
			row->setSelected(row->eventId() == id);
		}
	}

	void clearAll() {
		_selected = 0;
		Rebuild();
	}

protected:
	void paintEvent(QPaintEvent *) override {
		auto p = Painter(this);
		if (!_rows.empty()) {
			return;
		}
		const auto padding = st::rpcInspectorRowPadding;
		p.setFont(st::boxTextFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(
				padding.left(),
				0,
				width() - padding.left() - padding.right(),
				height()),
			Qt::AlignHCenter | Qt::AlignVCenter,
			!Recording()
				? u"Recording is off, enable it to capture calls."_q
				: (!_query.empty()
					? u"No RPC calls match the filter."_q
					: u"No RPC calls captured yet."_q));
	}

	int resizeGetHeight(int newWidth) override {
		RelayoutRows(newWidth);
		return std::max(
			int(_rows.size()) * st::rpcInspectorRowHeight,
			st::rpcInspectorRowHeight * 4);
	}

private:
	void Rebuild() {
		for (const auto &row : _rows) {
			delete row;
		}
		_rows.clear();
		for (const auto &event : Snapshot()) {
			if (Matches(event, _query)) {
				Prepend(event);
			}
		}
		resizeToWidth(width());
	}

	void Prepend(const Event &event) {
		const auto row = Ui::CreateChild<EventRow>(
			this,
			event,
			event.id == _selected);
		row->setClickedCallback([=] {
			setSelected(event.id);
			_openEvent(event.id);
		});
		row->show();
		_rows.insert(_rows.begin(), row);
	}

	void RelayoutRows(int width) {
		auto y = 0;
		for (const auto &row : _rows) {
			row->setGeometry(0, y, width, st::rpcInspectorRowHeight);
			y += st::rpcInspectorRowHeight;
		}
	}

	Query _query;
	uint64 _selected = 0;
	std::vector<EventRow*> _rows;
	Fn<void(uint64)> _openEvent;

};

// Left column of the inspector: toolbar over the live event log.
class EventLog final : public Ui::RpWidget {
public:
	EventLog(QWidget *parent)
	: Ui::RpWidget(parent) {
		_search = Ui::CreateChild<Ui::InputField>(
			this,
			st::defaultInputField,
			rpl::single(u"Filter: text, id, dc:4, error, ok, >500ms, <1s"_q));
		_recording = Ui::CreateChild<Ui::Checkbox>(
			this,
			u"Recording"_q,
			Recording(),
			st::defaultCheckbox);
		_clear = Ui::CreateChild<Ui::RoundButton>(
			this,
			rpl::single(u"Clear"_q),
			st::defaultBoxButton);
		_scroll = Ui::CreateChild<Ui::ScrollArea>(this, st::boxScroll);
		_list = _scroll->setOwnedWidget(object_ptr<HistoryList>(
			_scroll,
			[=](uint64 id) { _selections.fire_copy(id); }));

		_search->changes(
		) | rpl::on_next([=] {
			_list->applyQuery(_search->getLastText());
			Layout(height());
		}, lifetime());

		_recording->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			SetRecording(checked);
		}, lifetime());

		_clear->setClickedCallback([=] {
			Clear();
			_list->clearAll();
			_cleared.fire({});
		});

		_search->show();
		_recording->show();
		_clear->show();
		_scroll->show();
	}

	[[nodiscard]] rpl::producer<uint64> selections() const {
		return _selections.events();
	}

	[[nodiscard]] rpl::producer<> clears() const {
		return _cleared.events();
	}

	void select(uint64 id) {
		_list->setSelected(id);
	}

	void scrollToTop() {
		_scroll->scrollToY(0);
	}

	void eventUpdated(uint64 id) {
		if (_list->eventUpdated(id)) {
			_scroll->scrollToY(0);
		}
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		Layout(height());
	}

private:
	void Layout(int newHeight) {
		if (isHidden()) {
			return;
		}
		const auto padding = st::rpcInspectorRowPadding;
		const auto top = padding.top();
		const auto fieldHeight = st::defaultInputField.heightMin;
		auto y = top;
		_search->setGeometry(
			padding.left(),
			y,
			width() - padding.left() - padding.right(),
			fieldHeight);
		y += fieldHeight + top;
		const auto rowHeight = std::max(_recording->height(), _clear->height());
		_recording->moveToLeft(
			padding.left(),
			y + (rowHeight - _recording->height()) / 2);
		_clear->moveToRight(
			padding.right(),
			y + (rowHeight - _clear->height()) / 2);
		y += rowHeight + top;
		_scroll->setGeometry(0, y, width(), newHeight - y - padding.bottom());
		_list->resizeToWidth(width() - st::boxScroll.width);
		_scroll->updateBars();
	}

	Ui::InputField *_search = nullptr;
	Ui::Checkbox *_recording = nullptr;
	Ui::RoundButton *_clear = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	HistoryList *_list = nullptr;
	rpl::event_stream<uint64> _selections;
	rpl::event_stream<> _cleared;

};

// Non-focus-stealing completion list, shown near the text cursor. Lives
// as a child widget, so the keyboard stays in the request field; arrow
// keys are forwarded from it through Composer::eventFilter.
class CompletionPopup final : public Ui::RpWidget {
public:
	struct Item {
		QString text;
		bool field = false;
	};

	static constexpr auto kMaxRows = 8;

	CompletionPopup(QWidget *parent, Fn<void(Item)> accept)
	: Ui::RpWidget(parent)
	, _accept(std::move(accept)) {
		hide();
	}

	void showAt(QRect caret, std::vector<Item> items) {
		_items = std::move(items);
		_selected = 0;
		_first = 0;
		if (_items.empty()) {
			hide();
			return;
		}
		const auto parent = parentWidget();
		const auto shown = int(std::min(
			_items.size(),
			size_t(kMaxRows)));
		resize(
			std::min(
				st::rpcInspectorSuggestWidth,
				parent->width() - 2 * st::rpcInspectorRowPadding.left()),
			shown * st::rpcInspectorSuggestRow + 2 * st::lineWidth);
		const auto pad = st::rpcInspectorRowPadding.left();
		const auto x = std::clamp(
			caret.x(),
			pad,
			std::max(parent->width() - width() - pad, pad));
		const auto below = caret.bottom() + 2;
		const auto y = (below + height() > parent->height() - pad)
			? std::max(caret.y() - height() - 2, pad)
			: below;
		move(x, y);
		raise();
		show();
		update();
	}

	[[nodiscard]] bool active() const {
		return isVisible() && !_items.empty();
	}

	// Returns true if the key was consumed.
	[[nodiscard]] bool handleKey(int key) {
		if (!active()) {
			return false;
		}
		switch (key) {
		case Qt::Key_Up: moveSelection(-1); return true;
		case Qt::Key_Down: moveSelection(1); return true;
		case Qt::Key_PageUp: moveSelection(-kMaxRows); return true;
		case Qt::Key_PageDown: moveSelection(kMaxRows); return true;
		case Qt::Key_Enter:
		case Qt::Key_Return:
		case Qt::Key_Tab: acceptSelected(); return true;
		case Qt::Key_Escape: hide(); return true;
		default: return false;
		}
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		p.fillRect(rect(), st::windowBg);
		p.fillRect(0, 0, width(), st::lineWidth, st::shadowFg);
		p.fillRect(0, height() - st::lineWidth, width(), st::lineWidth, st::shadowFg);
		p.fillRect(0, 0, st::lineWidth, height(), st::shadowFg);
		p.fillRect(width() - st::lineWidth, 0, st::lineWidth, height(), st::shadowFg);
		const auto row = st::rpcInspectorSuggestRow;
		const auto pad = st::rpcInspectorRowPadding.left();
		const auto top = st::lineWidth;
		p.setFont(st::boxTextFont);
		for (auto i = _first; i <= lastShown(); ++i) {
			const auto y = top + (i - _first) * row;
			if (i == _selected) {
				p.fillRect(
					st::lineWidth,
					y,
					width() - 2 * st::lineWidth,
					row,
					st::windowBgOver);
			}
			p.setPen(i == _selected ? st::windowFg : st::boxTextFg);
			p.drawText(
				QPoint(pad, y + (row - st::boxTextFont->height) / 2
					+ st::boxTextFont->ascent),
				st::boxTextFont->elided(_items[i].text, width() - 2 * pad));
		}
	}

	void mousePressEvent(QMouseEvent *e) override {
		const auto row = _first
			+ (e->pos().y() - st::lineWidth) / st::rpcInspectorSuggestRow;
		if (row >= _first && row <= lastShown()) {
			_selected = row;
			acceptSelected();
		}
	}

	void wheelEvent(QWheelEvent *e) override {
		moveSelection(e->angleDelta().y() > 0 ? -3 : 3);
	}

private:
	[[nodiscard]] int lastShown() const {
		return std::min(int(_items.size()), _first + kMaxRows) - 1;
	}

	void moveSelection(int delta) {
		_selected = std::clamp(
			_selected + delta,
			0,
			int(_items.size()) - 1);
		if (_selected < _first) {
			_first = _selected;
		} else if (_selected > lastShown()) {
			_first = _selected - (kMaxRows - 1);
		}
		update();
	}

	void acceptSelected() {
		const auto item = _items[_selected];
		hide();
		_accept(item);
	}

	std::vector<Item> _items;
	int _selected = 0;
	int _first = 0;
	Fn<void(Item)> _accept;

};

[[nodiscard]] QJsonObject TemplateConstructor(
	const QString &constructorName,
	int depth);

// A conservative placeholder value for one TL field: strings and quoted
// 64-bit ids (access_hash is lossless as a string), empty vectors, and
// nested constructor skeletons up to two levels deep.
[[nodiscard]] QJsonValue TemplateValueFor(
		const QString &type,
		const QString &name,
		int depth) {
	if (type == u"string" || type == u"bytes") {
		return QJsonValue(QString());
	} else if (type == u"Bool") {
		return QJsonValue(false);
	} else if (type == u"int"
		|| type == u"long"
		|| type == u"int32"
		|| type == u"int53"
		|| type == u"int64"
		|| type == u"double") {
		return name.contains(u"access_hash")
			? QJsonValue(QString())
			: QJsonValue(0);
	} else if (type.startsWith(u"Vector<") || type.startsWith(u"vector<")) {
		return QJsonValue(QJsonArray());
	} else if (depth >= 2) {
		return QJsonValue(QString());
	}
	const auto candidates = ConstructorsOfType(type);
	if (candidates.empty()) {
		return QJsonValue(QString());
	}
	return TemplateConstructor(candidates.front()->name, depth + 1);
}

[[nodiscard]] QJsonObject TemplateConstructor(
		const QString &constructorName,
		int depth) {
	auto object = QJsonObject();
	object.insert(u"_"_q, constructorName);
	if (const auto meta = FindConstructor(constructorName)) {
		for (const auto &field : meta->fields) {
			if (field.type == u"true") {
				// Optional boolean flags must stay absent: their mere
				// presence flips the corresponding flag bit.
				continue;
			}
			object.insert(
				field.name,
				TemplateValueFor(field.type, field.name, depth));
		}
	}
	return object;
}

// Right column, top: TL JSON request composer with live status line.
class Composer final : public Ui::RpWidget {
public:
	Composer(
		QWidget *parent,
		not_null<Window::SessionController*> window,
		not_null<Ui::SeparatePanel*> panel,
		Fn<void(uint64)> openEvent)
	: Ui::RpWidget(parent)
	, _window(window)
	, _panel(panel)
	, _openEvent(std::move(openEvent)) {
		_label = Ui::CreateChild<Ui::FlatLabel>(
			this,
			u"Request (TL JSON)"_q,
			st::defaultFlatLabel);
		_layer = Ui::CreateChild<Ui::FlatLabel>(
			this,
			u"API layer %1"_q.arg(MTP::details::kCurrentLayer),
			st::defaultFlatLabel);
		_request = Ui::CreateChild<Ui::InputField>(
			this,
			st::rpcInspectorField,
			Ui::InputField::Mode::MultiLine);
		_request->setText(u"{\n  \"_\": \"help.getConfig\"\n}"_q);
		_auto = Ui::CreateChild<Ui::Checkbox>(
			this,
			u"Auto DC"_q,
			true,
			st::defaultCheckbox);
		_dc = Ui::CreateChild<Ui::InputField>(
			this,
			st::defaultInputField,
			rpl::single(u"DC id"_q));
		_invoke = Ui::CreateChild<Ui::RoundButton>(
			this,
			rpl::single(u"Invoke"_q),
			st::defaultActiveButton);
		_status = Ui::CreateChild<Ui::FlatLabel>(
			this,
			QString(),
			st::defaultFlatLabel);

		_auto->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			_dc->setEnabled(!checked);
		}, lifetime());

		_invoke->setClickedCallback([=] {
			invoke();
		});

		_popup = Ui::CreateChild<CompletionPopup>(
			this,
			[=](CompletionPopup::Item item) { acceptCompletion(item); });
		const auto edit = _request->rawTextEdit();
		edit->installEventFilter(this);
		QObject::connect(
			edit,
			&QTextEdit::cursorPositionChanged,
			[=] {
				crl::on_main(crl::guard(this, [=] { updateSuggestions(); }));
			});
		_request->changes(
		) | rpl::on_next([=] {
			crl::on_main(crl::guard(this, [=] { updateSuggestions(); }));
		}, lifetime());

		_label->show();
		_layer->show();
		_request->show();
		_auto->show();
		_dc->show();
		_invoke->show();
		_status->show();
	}

protected:
	bool eventFilter(QObject *obj, QEvent *e) override {
		if (_popup
			&& obj == _request->rawTextEdit()
			&& e->type() == QEvent::KeyPress) {
			const auto event = static_cast<QKeyEvent*>(e);
			if (_popup->handleKey(event->key())) {
				return true;
			} else if (handleEditorKey(event)) {
				return true;
			}
		}
		return Ui::RpWidget::eventFilter(obj, e);
	}

	int resizeGetHeight(int newWidth) override {
		const auto padding = st::rpcInspectorRowPadding;
		auto y = padding.top();
		_label->moveToLeft(padding.left(), y);
		_layer->moveToLeft(newWidth - padding.right() - _layer->width(), y);
		y += _label->height() + padding.top();
		_request->resizeToWidth(newWidth - padding.left() - padding.right());
		_request->moveToLeft(padding.left(), y);
		y += _request->height() + padding.top();

		const auto controlsHeight = std::max({
			_auto->height(),
			st::defaultInputField.heightMin,
			_invoke->height(),
		});
		_auto->moveToLeft(
			padding.left(),
			y + (controlsHeight - _auto->height()) / 2);
		_dc->resize(
			newWidth / 4,
			st::defaultInputField.heightMin);
		_dc->moveToLeft(
			padding.left() + _auto->width() + padding.left(),
			y + (controlsHeight - _dc->height()) / 2);
		_invoke->moveToLeft(
			newWidth - padding.right() - _invoke->width(),
			y + (controlsHeight - _invoke->height()) / 2);
		y += controlsHeight + padding.top();

		_status->moveToLeft(padding.left(), y);
		y += _status->height() + padding.bottom();

		resize(newWidth, y);
		return y;
	}

private:
	[[nodiscard]] static bool IsWordChar(QChar c) {
		return c.isLetterOrNumber() || c == u'.' || c == u'_';
	}

	[[nodiscard]] static std::pair<int, int> WordBounds(
			const QString &text,
			int position) {
		auto start = position;
		auto end = position;
		while (start > 0 && IsWordChar(text[start - 1])) {
			--start;
		}
		while (end < text.size() && IsWordChar(text[end])) {
			++end;
		}
		return { start, end };
	}

	void updateSuggestions() {
		const auto edit = _request->rawTextEdit();
		const auto text = edit->toPlainText();
		const auto position = edit->textCursor().position();
		const auto [start, end] = WordBounds(text, position);
		const auto word = text.mid(start, end - start);
		if (word.isEmpty()) {
			_popup->hide();
			return;
		}
		static const auto InCtorValue = QRegularExpression(
			u"\"_\"\\s*:\\s*\"[^\"]*$"_q);
		static const auto CtorName = QRegularExpression(
			u"\"_\"\\s*:\\s*\"([A-Za-z0-9_.]*)"_q);
		const auto inCtorValue = InCtorValue.match(
			text.left(position)).hasMatch();
		const auto matched = CtorName.match(text);
		const auto meta = matched.hasMatch()
			? FindConstructor(matched.captured(1))
			: nullptr;
		auto items = std::vector<CompletionPopup::Item>();
		if (!inCtorValue && meta) {
			for (const auto &field : meta->fields) {
				const auto &name = field.name;
				if (name.compare(word, Qt::CaseInsensitive) != 0
					&& name.startsWith(word, Qt::CaseInsensitive)) {
					items.push_back({ name, true });
				}
			}
		}
		if (items.empty()) {
			for (const auto &constructor : Constructors()) {
				if (!constructor.name.compare(word, Qt::CaseInsensitive)) {
					continue;
				}
				if (constructor.name.startsWith(word, Qt::CaseInsensitive)) {
					items.push_back({ constructor.name, false });
					if (items.size() >= 50) {
						break;
					}
				}
			}
		}
		if (items.empty()) {
			_popup->hide();
			return;
		}
		const auto caret = edit->cursorRect();
		const auto topLeft = edit->viewport()->mapToGlobal(caret.topLeft());
		_popup->showAt(
			QRect(mapFromGlobal(topLeft), QSize(1, caret.height())),
			std::move(items));
	}

	// Code-editor key behavior for the request field: Tab indents or
	// moves out of a pair, Enter auto-indents (and expands {} pairs),
	// brackets and quotes auto-close, type over, and pair-delete.
	[[nodiscard]] bool handleEditorKey(QKeyEvent *e) {
		const auto edit = _request->rawTextEdit();
		auto cursor = edit->textCursor();
		const auto text = edit->toPlainText();
		const auto pos = cursor.position();
		const auto before = (pos > 0) ? text[pos - 1] : QChar();
		const auto after = (pos < text.size()) ? text[pos] : QChar();
		const auto isPair = before == u'{' ? after == u'}'
			: before == u'[' ? after == u']'
			: before == u'"' && after == u'"';
		const auto modifiers = e->modifiers() & ~Qt::ShiftModifier;
		switch (e->key()) {
		case Qt::Key_Tab: {
			if (modifiers != 0) {
				return false;
			}
			cursor.insertText(u"  "_q);
			edit->setTextCursor(cursor);
			return true;
		}
		case Qt::Key_Backspace: {
			if (modifiers != 0 || cursor.hasSelection() || !isPair) {
				return false;
			}
			cursor.setPosition(pos - 1, QTextCursor::MoveAnchor);
			cursor.setPosition(pos + 1, QTextCursor::KeepAnchor);
			cursor.removeSelectedText();
			edit->setTextCursor(cursor);
			return true;
		}
		case Qt::Key_Enter:
		case Qt::Key_Return: {
			if (modifiers & (Qt::ControlModifier | Qt::MetaModifier)) {
				invoke();
				return true;
			}
			const auto line = cursor.block().text();
			auto indent = 0;
			while (indent < line.size() && line[indent] == u' ') {
				++indent;
			}
			const auto expands = (before == u'{' && after == u'}')
				|| (before == u'[' && after == u']');
			if (!expands) {
				cursor.insertText(u"\n"_q + line.left(indent));
				edit->setTextCursor(cursor);
				return true;
			}
			cursor.insertText(
				u"\n"_q + line.left(indent)
					+ u"  \n"_q + line.left(indent));
			cursor.setPosition(pos + 1 + indent + 2);
			edit->setTextCursor(cursor);
			return true;
		}
		default:
			break;
		}
		const auto ch = e->text();
		if (modifiers != 0 || ch.size() != 1) {
			return false;
		}
		const auto c = ch.front();
		const auto opener = (c == u'"' || c == u'{' || c == u'[');
		const auto closer = (c == u'}' || c == u']');
		if (opener) {
			const auto pairWith = (c == u'"') ? u'"' : (c == u'{') ? u'}' : u']';
			if (cursor.hasSelection()) {
				const auto selected = cursor.selectedText();
				cursor.insertText(c + selected + pairWith);
				cursor.movePosition(QTextCursor::PreviousCharacter);
				edit->setTextCursor(cursor);
				return true;
			} else if (c == u'"' && after == u'"') {
				cursor.movePosition(QTextCursor::NextCharacter);
				edit->setTextCursor(cursor);
				return true;
			}
			cursor.insertText(QString(c) + pairWith);
			cursor.movePosition(QTextCursor::PreviousCharacter);
			edit->setTextCursor(cursor);
			return true;
		} else if (closer && isPair) {
			cursor.movePosition(QTextCursor::NextCharacter);
			edit->setTextCursor(cursor);
			return true;
		}
		return false;
	}

	void acceptCompletion(CompletionPopup::Item item) {
		const auto edit = _request->rawTextEdit();
		const auto text = edit->toPlainText();
		const auto position = edit->textCursor().position();
		const auto [start, end] = WordBounds(text, position);
		if (!item.field) {
			// Accepting a constructor expands the whole document to a
			// full request template when the user is typing the top-level
			// "_" value, or is just typing the bare method name without
			// any "_" key in the document yet.
			static const auto CtorName = QRegularExpression(
				u"\"_\"\\s*:\\s*\"([A-Za-z0-9_.]*)"_q);
			const auto matched = CtorName.match(text);
			const auto inTopLevelValue = matched.hasMatch()
				&& position >= matched.capturedStart(1)
				&& position <= matched.capturedEnd(1);
			const auto meta = FindConstructor(item.text);
			if (meta && (inTopLevelValue || !matched.hasMatch())) {
				_request->setText(
					JsonText(TemplateConstructor(item.text, 0)));
				auto cursor = edit->textCursor();
				cursor.movePosition(QTextCursor::End);
				edit->setTextCursor(cursor);
				return;
			}
		}
		auto insert = item.text;
		if (item.field) {
			const auto insideString
				= (text.left(start).count(u'"') % 2) == 1;
			const auto quoteNext = (end < text.size() && text[end] == u'"');
			if (insideString && !quoteNext) {
				insert += u"\": "_q;
			}
		}
		auto cursor = edit->textCursor();
		cursor.setPosition(start, QTextCursor::MoveAnchor);
		cursor.setPosition(end, QTextCursor::KeepAnchor);
		cursor.insertText(insert);
		edit->setTextCursor(cursor);
	}

	void invoke() {
		auto error = QJsonParseError();
		const auto document = QJsonDocument::fromJson(
			_request->getLastText().toUtf8(),
			&error);
		if (error.error != QJsonParseError::NoError || !document.isObject()) {
			showStatus(
				u"JSON parse error: %1"_q.arg(error.errorString()),
				true);
			return;
		}
		auto primes = mtpBuffer();
		if (!MTP::details::TlJsonEncodeBoxed(document.object(), primes)) {
			showStatus(
				u"Could not encode the JSON as a TL request: check the"
				" constructor name, required fields and value formats."_q,
				true);
			return;
		}
		const auto name = MTP::details::TlJsonBoxedName(
			primes.data(),
			primes.data() + primes.size());
		if (IsReadOnlyMethod(name)) {
			send(primes, name);
		} else {
			_panel->showBox(
				Ui::MakeConfirmBox(Ui::ConfirmBoxArgs{
					.text = u"This RPC may modify Telegram state.\n\n"
						"Are you sure you want to invoke %1?"_q.arg(name),
					.confirmed = crl::guard(this, [=] { send(primes, name); }),
					.confirmText = u"Invoke"_q,
				}),
				Ui::LayerOption::KeepOther,
				anim::type::normal);
		}
	}

	void send(const mtpBuffer &primes, const QString &name) {
		auto serialized = MTP::details::SerializedRequest::Prepare(
			uint32(primes.size()));
		serialized->append(primes);
		const auto requestId = MTP::details::GetNextRequestId();
		const auto dcId = _auto->checked()
			? MTP::ShiftedDcId(0)
			: MTP::ShiftedDcId(_dc->getLastText().toInt());
		_window->session().mtp().sendSerialized(
			requestId,
			std::move(serialized),
			MTP::ResponseHandler{
				crl::guard(this, [=](const MTP::Response &) {
					showStatus(
						u"Invoked %1 (request %2, layer %3)."_q.arg(
							name,
							QString::number(requestId),
							QString::number(MTP::details::kCurrentLayer)),
						false);
					return true;
				}),
				crl::guard(this, [=](
						const MTP::Error &error,
						const MTP::Response &) {
					showStatus(
						u"Request %1 failed: %2 (%3)"_q.arg(
							QString::number(requestId),
							error.type(),
							QString::number(error.code())),
						true);
					return true;
				}),
			},
			dcId,
			0,
			0);
		// Start() has already run inside sendSerialized, so the event is
		// in the store: select it and watch its response land live.
		if (const auto id = FindEventId(requestId)) {
			_openEvent(id);
		}
	}

	void showStatus(const QString &text, bool isError) {
		_status->setText(text);
		_status->setTextColorOverride(
			(isError ? st::boxTextFgError : st::boxTextFgGood)->c);
	}

	not_null<Window::SessionController*> _window;
	not_null<Ui::SeparatePanel*> _panel;
	Fn<void(uint64)> _openEvent;
	CompletionPopup *_popup = nullptr;
	Ui::FlatLabel *_label = nullptr;
	Ui::FlatLabel *_layer = nullptr;
	Ui::InputField *_request = nullptr;
	Ui::Checkbox *_auto = nullptr;
	Ui::InputField *_dc = nullptr;
	Ui::RoundButton *_invoke = nullptr;
	Ui::FlatLabel *_status = nullptr;

};

// Right column, bottom: details of the selected event, refreshed live.
class DetailsPane final : public Ui::RpWidget {
public:
	DetailsPane(QWidget *parent)
	: Ui::RpWidget(parent) {
		_title = Ui::CreateChild<Ui::FlatLabel>(
			this,
			u"Details"_q,
			st::rpcInspectorDetailsTitle);
		_copyRequest = Ui::CreateChild<TextButton>(this, u"Copy Request"_q);
		_copyResponse = Ui::CreateChild<TextButton>(this, u"Copy Response"_q);
		_copyError = Ui::CreateChild<TextButton>(this, u"Copy Error"_q);
		_copyJson = Ui::CreateChild<TextButton>(this, u"Copy JSON"_q);
		_scroll = Ui::CreateChild<Ui::ScrollArea>(this, st::boxScroll);

		_copyRequest->setClickedCallback([=] {
			if (auto event = Lookup(_id)) {
				CopyText(JsonText(DecodeBoxed(event->request)));
			}
		});
		_copyResponse->setClickedCallback([=] {
			if (auto event = Lookup(_id)) {
				CopyText(JsonText(DecodeBoxed(event->response)));
			}
		});
		_copyError->setClickedCallback([=] {
			if (auto event = Lookup(_id)) {
				CopyText(event->errorType + u" ("_q
					+ QString::number(event->errorCode) + u")"_q
					+ (event->errorDescription.isEmpty()
						? QString()
						: u": "_q + event->errorDescription));
			}
		});
		_copyJson->setClickedCallback([=] {
			if (auto event = Lookup(_id)) {
				CopyText(JsonText(EventJson(*event)));
			}
		});

		_title->show();
		open(std::nullopt);
	}

	void open(std::optional<uint64> id) {
		_id = id.value_or(0);
		if (const auto event = Lookup(_id)) {
			_title->setText(event->method);
		} else {
			_title->setText(u"Details"_q);
		}
		refreshButtons();
		rebuild();
	}

	[[nodiscard]] bool showing(uint64 id) const {
		return _id == id;
	}

	void refresh() {
		refreshButtons();
		rebuild();
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		Layout();
	}

private:
	[[nodiscard]] QJsonObject EventJson(const Event &event) const {
		auto json = QJsonObject();
		json.insert(u"id"_q, double(event.id));
		json.insert(u"request_id"_q, double(event.requestId));
		json.insert(u"method"_q, event.method);
		json.insert(u"dc"_q, double(MTP::BareDcId(event.dcId)));
		if (event.msgId) {
			json.insert(u"msg_id"_q, QString::number(event.msgId));
			json.insert(u"seq_no"_q, double(event.seqNo));
		}
		json.insert(u"duration_ms"_q, double(
			event.finished ? event.finished - event.started : 0));
		json.insert(u"request_size"_q, double(event.requestSize));
		json.insert(u"response_size"_q, double(event.responseSize));
		json.insert(u"status"_q, StatusText(event.status).toLower());
		json.insert(u"request"_q, DecodeBoxed(event.request));
		if (!event.response.isEmpty()) {
			json.insert(u"response"_q, DecodeBoxed(event.response));
		}
		if (event.status == Status::Failed) {
			auto error = QJsonObject();
			error.insert(u"code"_q, double(event.errorCode));
			error.insert(u"type"_q, event.errorType);
			error.insert(u"description"_q, event.errorDescription);
			json.insert(u"error", error);
		}
		return json;
	}

	void refreshButtons() {
		const auto event = Lookup(_id);
		_copyRequest->setVisible(event.has_value());
		_copyResponse->setVisible(
			event && !event->response.isEmpty());
		_copyError->setVisible(
			event
			&& event->status == Status::Failed
			&& !event->errorType.isEmpty());
		_copyJson->setVisible(event.has_value());
	}

	void rebuild() {
		auto fresh = object_ptr<Ui::VerticalLayout>(_scroll);
		_content = fresh.data();
		if (const auto event = Lookup(_id)) {
			addOverview(_content, *event);
			addTextSection(
				_content,
				u"REQUEST"_q,
				JsonText(DecodeBoxed(event->request)));
			if (event->status == Status::Succeeded
				&& !event->response.isEmpty()) {
				addTextSection(
					_content,
					u"RESPONSE"_q,
					JsonText(DecodeBoxed(event->response)));
			} else if (event->status == Status::Failed) {
				auto error = QString();
				if (!event->errorType.isEmpty()) {
					error = u"Error: %1 (%2)\n"_q.arg(
						event->errorType,
						QString::number(event->errorCode));
				}
				if (!event->errorDescription.isEmpty()) {
					error += u"Description: %1\n"_q.arg(
						event->errorDescription);
				}
				if (!error.isEmpty()) {
					addTextSection(_content, u"ERROR"_q, error);
				}
			}
			if (event->needsLayer) {
				_content->add(
					object_ptr<Ui::FlatLabel>(
						_content,
						u"Wrapped in invokeWithLayer and initConnection"
						" by the transport layer when the connection"
						" is not initialized yet."_q,
						st::defaultFlatLabel),
					st::rpcInspectorRowPadding);
			}
		} else {
			const auto empty = _content->add(
				object_ptr<Ui::FlatLabel>(
					_content,
					Recording()
						? u"Select a request on the left to inspect it."_q
						: u"Recording is off, enable it to capture calls."_q,
					st::defaultFlatLabel),
				st::rpcInspectorRowPadding);
			empty->setTextColorOverride(st::windowSubTextFg->c);
		}
		_scroll->setOwnedWidget(std::move(fresh));
		Layout();
	}

	void addOverview(
			not_null<Ui::VerticalLayout*> container,
			const Event &event) {
		auto text = TextWithEntities();
		const auto addRow = [&](
				const QString &key,
				const QString &value) {
			if (!text.text.isEmpty()) {
				text.text += '\n';
			}
			text.entities.push_back(EntityInText(
				EntityType::Bold,
				text.text.size(),
				key.size()));
			text.text += key + u": "_q + value;
		};
		addRow(u"Status"_q, StatusText(event.status));
		addRow(u"Started"_q, event.startedAt
			? QDateTime::fromMSecsSinceEpoch(event.startedAt)
				.toLocalTime()
				.toString(u"HH:mm:ss"_q)
			: u"\u2013"_q);
		addRow(u"DC"_q, QString::number(MTP::BareDcId(event.dcId)));
		addRow(u"Layer"_q, QString::number(MTP::details::kCurrentLayer));
		addRow(u"Duration"_q, event.finished
			? FormatDuration(event.finished - event.started)
			: u"pending"_q);
		if (event.msgId) {
			addRow(u"Msg ID"_q, QString::number(event.msgId));
			addRow(u"Seq No"_q, QString::number(event.seqNo));
		}
		addRow(u"Request Size"_q, FormatSize(event.requestSize));
		if (event.responseSize) {
			addRow(u"Response Size"_q, FormatSize(event.responseSize));
		}
		if (event.attempts > 1) {
			addRow(u"Attempts"_q, QString::number(event.attempts));
		}
		if (event.afterRequestId) {
			addRow(u"After"_q, u"request %1 (invokeAfterMsg)"_q.arg(
				event.afterRequestId));
		}
		addRow(u"Request ID"_q, QString::number(event.requestId));
		addRow(u"Event ID"_q, QString::number(event.id));

		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				rpl::single(std::move(text)),
				st::rpcInspectorDetailsOverview),
			st::rpcInspectorRowPadding);
	}

	void addTextSection(
			not_null<Ui::VerticalLayout*> container,
			const QString &title,
			const QString &text) {
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				rpl::single(title),
				st::rpcInspectorDetailsTitle),
			st::rpcInspectorRowPadding);
		const auto clipped = (text.size() > kMaxInlineJson)
			? text.left(kMaxInlineJson)
				+ u"\n\n\u2026 (%1 bytes total, use the copy"
				" buttons for the full text)"_q.arg(text.size())
			: text;
		const auto label = container->add(
			object_ptr<Ui::FlatLabel>(container, st::rpcInspectorDetailsJson),
			st::rpcInspectorRowPadding);
		label->setText(clipped);
		label->setSelectable(true);
		Ui::SetupSelectingScroll(label, [=](int pixels) {
			_scroll->scrollToY(_scroll->scrollTop() + pixels);
		});
	}

	void Layout() {
		const auto padding = st::rpcInspectorRowPadding;
		const auto headerHeight = _title->height() + padding.top();
		auto x = width() - padding.right();
		const auto place = [&](not_null<TextButton*> button) {
			if (!button->isHidden()) {
				x -= button->width();
				button->moveToLeft(
					x,
					padding.top()
						+ (_title->height() - button->height()) / 2);
				x -= padding.left();
			}
		};
		place(_copyJson);
		place(_copyError);
		place(_copyResponse);
		place(_copyRequest);
		_title->setGeometry(
			padding.left(),
			padding.top(),
			std::max(x - padding.left(), 0),
			_title->height());
		const auto top = headerHeight + padding.top();
		_scroll->setGeometry(
			0,
			top,
			width(),
			std::max(height() - top - padding.bottom(), 0));
		if (_content) {
			_content->resizeToWidth(
				width() - padding.left() - padding.right() - st::boxScroll.width);
			_scroll->updateBars();
		}
	}

	Ui::FlatLabel *_title = nullptr;
	TextButton *_copyRequest = nullptr;
	TextButton *_copyResponse = nullptr;
	TextButton *_copyError = nullptr;
	TextButton *_copyJson = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	Ui::VerticalLayout *_content = nullptr;
	uint64 _id = 0;

};

// The "RPC" tab page: log on the left, composer over details on the
// right, both always visible, everything live.
class RpcPage final : public Ui::RpWidget {
public:
	RpcPage(
		QWidget *parent,
		not_null<Window::SessionController*> window,
		not_null<Ui::SeparatePanel*> panel)
	: Ui::RpWidget(parent) {
		_log = Ui::CreateChild<EventLog>(this);
		_details = Ui::CreateChild<DetailsPane>(this);
		_composer = Ui::CreateChild<Composer>(
			this,
			window,
			panel,
			[=](uint64 id) {
				_log->select(id);
				_log->scrollToTop();
				_details->open(id);
			});

		_log->selections(
		) | rpl::on_next([=](uint64 id) {
			_details->open(id);
		}, lifetime());

		_log->clears(
		) | rpl::on_next([=] {
			_details->open(std::nullopt);
		}, lifetime());

		Dev::Rpc::Updates(
		) | rpl::on_next([=](uint64 id) {
			_log->eventUpdated(id);
			if (_details->showing(id)) {
				_details->refresh();
			}
		}, lifetime());

		_log->show();
		_composer->show();
		_details->show();
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		const auto logWidth = std::min(
			st::rpcInspectorLogWidth,
			width() / 2);
		_log->setGeometry(0, 0, logWidth, height());
		const auto rightWidth = width() - logWidth;
		_composer->resizeToWidth(rightWidth);
		_composer->moveToLeft(logWidth, 0);
		_details->setGeometry(
			logWidth,
			_composer->height(),
			rightWidth,
			std::max(height() - _composer->height(), 0));
		update();
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto divider = st::lineWidth;
		const auto x = std::min(st::rpcInspectorLogWidth, width() / 2);
		p.fillRect(
			x,
			0,
			divider,
			height(),
			st::shadowFg);
		p.fillRect(
			x + divider,
			_composer->height(),
			width() - x - divider,
			divider,
			st::shadowFg);
	}

private:
	EventLog *_log = nullptr;
	Composer *_composer = nullptr;
	DetailsPane *_details = nullptr;

};

// The inner widget of the inspector mini-app: a tab strip over tool
// pages. New tools slot in with one addPage(title, page) call.
class InspectorInner final : public Ui::RpWidget {
public:
	InspectorInner(
		QWidget *parent,
		not_null<Window::SessionController*> window,
		not_null<Ui::SeparatePanel*> panel)
	: Ui::RpWidget(parent) {
		addPage(u"RPC"_q, base::make_unique_q<RpcPage>(
			this,
			window,
			panel));
		selectPage(0);
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		LayoutTabs();
		if (auto page = currentPage()) {
			page->setGeometry(
				0,
				st::defaultTabsSlider.height,
				width(),
				std::max(
					height() - st::defaultTabsSlider.height,
					0));
		}
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		p.fillRect(
			0,
			st::defaultTabsSlider.height - st::lineWidth,
			width(),
			st::lineWidth,
			st::shadowFg);
	}

private:
	void addPage(const QString &title, base::unique_qptr<Ui::RpWidget> page) {
		const auto raw = page.release();
		raw->hide();
		_pages.push_back(raw);
		const auto tab = Ui::CreateChild<TabButton>(this, title);
		const auto index = int(_tabs.size());
		tab->setClickedCallback([=] {
			selectPage(index);
		});
		tab->show();
		_tabs.push_back(tab);
		LayoutTabs();
	}

	void selectPage(int index) {
		_current = index;
		for (auto i = 0, count = int(_tabs.size()); i != count; ++i) {
			_tabs[i]->setActive(i == index);
			if (auto page = _pages[i]) {
				if (i == index) {
					page->show();
				} else {
					page->hide();
				}
			}
		}
		if (auto page = currentPage()) {
			page->setGeometry(
				0,
				st::defaultTabsSlider.height,
				width(),
				std::max(height() - st::defaultTabsSlider.height, 0));
		}
	}

	[[nodiscard]] Ui::RpWidget *currentPage() const {
		return (_current >= 0 && _current < int(_pages.size()))
			? _pages[_current]
			: nullptr;
	}

	void LayoutTabs() {
		auto x = st::rpcInspectorRowPadding.left();
		const auto y = 0;
		for (const auto tab : _tabs) {
			tab->setGeometry(x, y, tab->width(), st::defaultTabsSlider.height);
			x += tab->width();
		}
	}

	std::vector<TabButton*> _tabs;
	std::vector<Ui::RpWidget*> _pages;
	int _current = 0;

};

} // namespace

void ShowRpcInspector(not_null<Window::SessionController*> controller) {
	base::options::lookup<bool>(kOptionRpcInspector).set(true);

	// One panel per main window: it stays alive (hidden) after it is
	// closed, keeping filter and composer text, and is destroyed together
	// with the window so it never outlives the MTP session it invokes on.
	// Qt-parented instead of rpl::lifetime::make_state, which constructs
	// a fresh holder on every call.
	const auto window = controller->window().widget();
	auto panel = static_cast<Ui::SeparatePanel*>(
		window->findChild<QObject*>(u"RpcInspectorPanel"_q));
	if (!panel) {
		panel = new Ui::SeparatePanel(Ui::SeparatePanelArgs{
			.parent = window,
		});
		panel->setObjectName(u"RpcInspectorPanel"_q);
		panel->setTitle(rpl::single(u"RPC Inspector"_q));
		panel->setWindowTitle(u"RPC Inspector"_q);
		panel->setInnerSize(st::rpcInspectorWindowSize, true);
		panel->showInner(base::make_unique_q<InspectorInner>(
			panel,
			controller,
			panel));
		panel->closeRequests(
		) | rpl::on_next([=] {
			panel->hideGetDuration();
		}, panel->lifetime());
		Platform::SetWindowAppId(panel, u"org.telegram.org.inspector"_q);
	}

	// Open docked to the main window: centered under it, or above it when
	// there is no room left on the screen. moveToAnchorGeometry() clamps
	// the result into the available screen area on show.
	const auto screen = window->screen();
	const auto available = screen ? screen->availableGeometry() : QRect();
	const auto main = window->geometry();
	auto target = QRect(
		QPoint(
			main.center().x() - panel->width() / 2,
			main.y() + main.height() + st::rpcInspectorAnchorGap),
		panel->size());
	if (target.bottom() > available.bottom()) {
		target.moveBottom(main.y() - st::rpcInspectorAnchorGap);
	}
	target.moveLeft(main.center().x() - target.width() / 2);
	panel->setAnchorData(target, {});

	panel->showAndActivate();
}

} // namespace Dev::Rpc
