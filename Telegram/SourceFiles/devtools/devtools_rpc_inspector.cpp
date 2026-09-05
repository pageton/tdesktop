/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "devtools/devtools_rpc_inspector.h"

#include "base/options.h"
#include "devtools/devtools_rpc_log.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mtproto/mtproto_dc_options.h"
#include "mtproto/mtproto_response.h"
#include "scheme.h"
#include "scheme-tl_json.h"
#include "ui/layers/box_content.h"
#include "ui/boxes/confirm_box.h"
#include "ui/painter.h"
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/abstract_button.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/number_input.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/selecting_scroll.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_devtools.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtGui/QGuiApplication>

#include <array>

namespace Dev::Rpc {
namespace {

constexpr auto kMaxInlineJson = 256 * 1024;

struct Query {
	QString text;
	std::optional<int> dc;
	std::optional<bool> errors;
	std::optional<int> minDuration;
	std::optional<int> maxDuration;
};

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
		} else {
			freeText.push_back(token);
		}
	}
	result.text = freeText.join(u' ');
	return result;
}

[[nodiscard]] bool Matches(const Event &event, const Query &query) {
	if (!query.text.isEmpty()
		&& !event.method.contains(query.text, Qt::CaseInsensitive)
		&& !event.errorType.contains(query.text, Qt::CaseInsensitive)) {
		return false;
	} else if (query.dc && query.dc != MTP::BareDcId(event.dcId)) {
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
	EventRow(QWidget *parent, Event data)
	: Ui::AbstractButton(parent)
	, _data(std::move(data)) {
		resize(parent->width(), st::rpcInspectorRowHeight);
	}

	void updateData(Event data) {
		_data = std::move(data);
		update();
	}

	[[nodiscard]] uint64 eventId() const {
		return _data.id;
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		if (isOver() || isDown()) {
			p.fillRect(rect(), st::windowBgOver);
		}
		const auto padding = st::rpcInspectorRowPadding;
		const auto left = padding.left();
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

};

class HistoryList final : public Ui::RpWidget {
public:
	HistoryList(QWidget *parent, Fn<void(uint64)> openDetails)
	: Ui::RpWidget(parent)
	, _openDetails(std::move(openDetails)) {
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

	void clearAll() {
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
				Recording()
					? u"No RPC calls captured yet."_q
					: u"Recording is off, enable it to capture calls."_q);
		}

		int resizeGetHeight(int newWidth) override {
			RelayoutRows(newWidth);
			return std::max(
				int(_rows.size()) * st::rpcInspectorRowHeight,
				st::rpcInspectorListHeight);
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
			const auto row = Ui::CreateChild<EventRow>(this, event);
			row->setClickedCallback([=] {
				_openDetails(event.id);
			});
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
	std::vector<EventRow*> _rows;
	Fn<void(uint64)> _openDetails;

};

class HistoryPane final : public Ui::RpWidget {
public:
	HistoryPane(QWidget *parent, Fn<void(uint64)> openDetails)
	: Ui::RpWidget(parent) {
		_search = Ui::CreateChild<Ui::InputField>(
			this,
			st::defaultInputField,
			rpl::single(u"Filter: text, dc:4, error, ok, >500ms, <1s"_q));
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
			std::move(openDetails)));

		_search->changes(
		) | rpl::on_next([=] {
			_list->applyQuery(_search->getLastText());
		}, lifetime());

		_recording->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			SetRecording(checked);
		}, lifetime());

		_clear->setClickedCallback([=] {
			Clear();
			_list->clearAll();
		});

		_search->show();
		_recording->show();
		_clear->show();
		_scroll->show();
	}

	void eventUpdated(uint64 id) {
		if (_list->eventUpdated(id)) {
			_scroll->scrollToY(0);
		}
	}

protected:
	int resizeGetHeight(int newWidth) override {
		if (isHidden()) {
			return 0;
		}
		const auto padding = st::rpcInspectorRowPadding;
		const auto top = padding.top();
		const auto fieldHeight = st::defaultInputField.heightMin;
		const auto rowHeight = std::max({
			fieldHeight,
			_recording->height(),
			_clear->height(),
		}) + 2 * top;
		const auto controlsWidth = _recording->width()
			+ _clear->width()
			+ 3 * padding.left();

		_search->setGeometry(
			padding.left(),
			top,
			newWidth - padding.left() - padding.right() - controlsWidth,
			fieldHeight);
		_recording->moveToLeft(
			newWidth - padding.right() - _clear->width()
				- padding.left() - _recording->width(),
			top + (rowHeight - 2 * top - _recording->height()) / 2);
		_clear->moveToLeft(
			newWidth - padding.right() - _clear->width(),
			top + (rowHeight - 2 * top - _clear->height()) / 2);

		const auto listTop = top + rowHeight + padding.top();
		_scroll->setGeometry(
			0,
			listTop,
			newWidth,
			st::rpcInspectorListHeight);
		_list->resizeToWidth(newWidth - st::boxScroll.width);
		_scroll->updateBars();

		resize(newWidth, listTop + st::rpcInspectorListHeight + padding.bottom());
		return height();
	}

private:
	Ui::InputField *_search = nullptr;
	Ui::Checkbox *_recording = nullptr;
	Ui::RoundButton *_clear = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	HistoryList *_list = nullptr;

};

class InvokePane final : public Ui::RpWidget {
public:
	InvokePane(
		QWidget *parent,
		not_null<Window::SessionController*> window,
		Fn<void()> showHistory)
	: Ui::RpWidget(parent)
	, _window(window)
	, _showHistory(std::move(showHistory)) {
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

		_label->show();
		_layer->show();
		_request->show();
		_auto->show();
		_dc->show();
		_invoke->show();
		_status->show();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		if (isHidden()) {
			return 0;
		}
		const auto padding = st::rpcInspectorRowPadding;
		auto y = padding.top();
		_label->moveToLeft(padding.left(), y);
		_layer->moveToRight(padding.right(), y);
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
		_invoke->moveToRight(
			padding.right(),
			y + (controlsHeight - _invoke->height()) / 2);
		y += controlsHeight + padding.top();

		_status->moveToLeft(padding.left(), y);
		y += _status->height() + padding.bottom();

		resize(newWidth, y);
		return y;
	}

private:
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
			_window->show(Ui::MakeConfirmBox(Ui::ConfirmBoxArgs{
				.text = u"This RPC may modify Telegram state.\n\n"
					"Are you sure you want to invoke %1?"_q.arg(name),
				.confirmed = [=] { send(primes, name); },
				.confirmText = u"Invoke"_q,
			}));
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
						u"Invoked %1 (request %2, layer %3), see History."_q.arg(
							name,
							QString::number(requestId),
							QString::number(MTP::details::kCurrentLayer)),
						false);
					_showHistory();
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
	}

	void showStatus(const QString &text, bool isError) {
		_status->setText(text);
		_status->setTextColorOverride(
			(isError ? st::boxTextFgError : st::boxTextFgGood)->c);
	}

	not_null<Window::SessionController*> _window;
	Fn<void()> _showHistory;
	Ui::FlatLabel *_label = nullptr;
	Ui::FlatLabel *_layer = nullptr;
	Ui::InputField *_request = nullptr;
	Ui::Checkbox *_auto = nullptr;
	Ui::InputField *_dc = nullptr;
	Ui::RoundButton *_invoke = nullptr;
	Ui::FlatLabel *_status = nullptr;

};

[[nodiscard]] QString StatusText(Status status) {
	switch (status) {
	case Status::Succeeded: return u"Success"_q;
	case Status::Failed: return u"Failed"_q;
	case Status::Cancelled: return u"Cancelled"_q;
	default: return u"Pending"_q;
	}
}

class RpcDetailsBox final : public Ui::BoxContent {
public:
	RpcDetailsBox(QWidget*, Event event)
	: _event(std::move(event)) {
	}

protected:
	void prepare() override {
		setTitle(_event.method);
		setDimensions(st::boxWideWidth, st::boxMaxListHeight);
		_container = setInnerWidget(
			object_ptr<Ui::VerticalLayout>(this));

		addOverview();
		addTextSection(
			u"REQUEST"_q,
			JsonText(DecodeBoxed(_event.request)));
		if (_event.status == Status::Succeeded && !_event.response.isEmpty()) {
			addTextSection(
				u"RESPONSE"_q,
				JsonText(DecodeBoxed(_event.response)));
		} else if (_event.status == Status::Failed) {
			auto error = QString();
			if (!_event.errorType.isEmpty()) {
				error = u"Error: %1 (%2)\n"_q.arg(
					_event.errorType,
					QString::number(_event.errorCode));
			}
			if (!_event.errorDescription.isEmpty()) {
				error += u"Description: %1\n"_q.arg(_event.errorDescription);
			}
			if (!error.isEmpty()) {
				addTextSection(u"ERROR"_q, error);
			}
		}
		if (_event.needsLayer) {
			const auto container = _container.data();
			container->add(
				object_ptr<Ui::FlatLabel>(
					container,
					u"Wrapped in invokeWithLayer and initConnection"
					" by the transport layer when the connection"
					" is not initialized yet."_q,
					st::defaultFlatLabel),
				st::boxRowPadding);
		}
		addCopyButtons();
	}

private:
	void addOverview() {
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
		addRow(u"Status"_q, StatusText(_event.status));
		addRow(u"Started"_q, _event.startedAt
			? QDateTime::fromMSecsSinceEpoch(_event.startedAt)
				.toLocalTime()
				.toString(u"HH:mm:ss"_q)
			: u"\u2013"_q);
		addRow(u"DC"_q, QString::number(MTP::BareDcId(_event.dcId)));
		addRow(u"Layer"_q, QString::number(MTP::details::kCurrentLayer));
		addRow(u"Duration"_q, _event.finished
			? FormatDuration(_event.finished - _event.started)
			: u"pending"_q);
		if (_event.msgId) {
			addRow(u"Msg ID"_q, QString::number(_event.msgId));
			addRow(u"Seq No"_q, QString::number(_event.seqNo));
		}
		addRow(u"Request Size"_q, FormatSize(_event.requestSize));
		if (_event.responseSize) {
			addRow(u"Response Size"_q, FormatSize(_event.responseSize));
		}
		if (_event.attempts > 1) {
			addRow(u"Attempts"_q, QString::number(_event.attempts));
		}
		if (_event.afterRequestId) {
			addRow(u"After"_q, u"request %1 (invokeAfterMsg)"_q.arg(
				_event.afterRequestId));
		}
		addRow(u"Request ID"_q, QString::number(_event.requestId));
		addRow(u"Event ID"_q, QString::number(_event.id));

		const auto container = _container.data();
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				rpl::single(std::move(text)),
				st::rpcInspectorDetailsOverview),
			st::boxRowPadding);
	}

	void addTextSection(const QString &title, const QString &text) {
		const auto container = _container.data();
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				rpl::single(title),
				st::rpcInspectorDetailsTitle),
			st::boxRowPadding);
		const auto clipped = (text.size() > kMaxInlineJson)
			? text.left(kMaxInlineJson)
				+ u"\n\n\u2026 (%1 bytes total, use the copy"
				" buttons for the full text)"_q.arg(text.size())
			: text;
		const auto label = container->add(
			object_ptr<Ui::FlatLabel>(container, st::rpcInspectorDetailsJson),
			st::boxRowPadding);
		label->setText(clipped);
		label->setSelectable(true);
		Ui::SetupSelectingScroll(label, [=](int pixels) {
			scrollToY(scrollTop() + pixels);
		});
	}

	void addCopyButtons() {
		addButton(rpl::single(u"Copy Request"_q), [=] {
			CopyText(JsonText(DecodeBoxed(_event.request)));
		});
		if (!_event.response.isEmpty()) {
			addButton(rpl::single(u"Copy Response"_q), [=] {
				CopyText(JsonText(DecodeBoxed(_event.response)));
			});
		}
		if (_event.status == Status::Failed && !_event.errorType.isEmpty()) {
			addButton(rpl::single(u"Copy Error"_q), [=] {
				CopyText(_event.errorType + u" ("_q
					+ QString::number(_event.errorCode) + u")"_q
					+ (_event.errorDescription.isEmpty()
						? QString()
						: u": "_q + _event.errorDescription));
			});
		}
		addButton(rpl::single(u"Copy JSON"_q), [=] {
			auto json = QJsonObject();
			json.insert(u"id"_q, double(_event.id));
			json.insert(u"request_id"_q, double(_event.requestId));
			json.insert(u"method"_q, _event.method);
			json.insert(u"dc"_q, double(MTP::BareDcId(_event.dcId)));
			if (_event.msgId) {
				json.insert(u"msg_id"_q, QString::number(_event.msgId));
				json.insert(u"seq_no"_q, double(_event.seqNo));
			}
			json.insert(u"duration_ms"_q, double(
				_event.finished ? _event.finished - _event.started : 0));
			json.insert(u"request_size"_q, double(_event.requestSize));
			json.insert(u"response_size"_q, double(_event.responseSize));
			json.insert(u"status"_q, StatusText(_event.status).toLower());
			json.insert(
				u"request"_q,
				DecodeBoxed(_event.request));
			if (!_event.response.isEmpty()) {
				json.insert(
					u"response"_q,
					DecodeBoxed(_event.response));
			}
			if (_event.status == Status::Failed) {
				auto error = QJsonObject();
				error.insert(u"code"_q, double(_event.errorCode));
				error.insert(u"type"_q, _event.errorType);
				error.insert(u"description"_q, _event.errorDescription);
				json.insert(u"error", error);
			}
			CopyText(JsonText(json));
		});
		addButton(tr::lng_box_ok(), [=] { closeBox(); });
	}

	Event _event;
	QPointer<Ui::VerticalLayout> _container;

};

class RpcInspectorBox final : public Ui::BoxContent {
public:
	RpcInspectorBox(QWidget*, not_null<Window::SessionController*> window)
	: _window(window) {
	}

protected:
	void prepare() override {
		setTitle(u"RPC Inspector"_q);

		const auto content = setInnerWidget(
			object_ptr<Ui::VerticalLayout>(this)).data();

		const auto tabs = content->add(
			object_ptr<Ui::RpWidget>(content));
		tabs->resize(st::boxWideWidth, st::defaultTabsSlider.height);
		tabs->show();
		_historyTab = Ui::CreateChild<TabButton>(tabs, u"History"_q);
		_invokeTab = Ui::CreateChild<TabButton>(tabs, u"Invoke"_q);
		const auto tabLeft = st::rpcInspectorRowPadding.left();
		_historyTab->moveToLeft(tabLeft, 0);
		_invokeTab->moveToLeft(tabLeft + _historyTab->width() + tabLeft, 0);
		_historyTab->show();
		_invokeTab->show();

		_history = content->add(
			object_ptr<HistoryPane>(
				content,
				[=](uint64 id) { showDetails(id); }));
		_history->show();
		_invoke = content->add(
			object_ptr<InvokePane>(
				content,
				_window,
				[=] { showHistoryTab(content); }));
		_invoke->hide();

		_historyTab->setClickedCallback([=] {
			showHistoryTab(content);
		});
		_invokeTab->setClickedCallback([=] {
			if (_invoke->isHidden()) {
				_invoke->show();
				_history->hide();
				_invokeTab->setActive(true);
				_historyTab->setActive(false);
				content->resizeToWidth(content->width());
				setDimensions(st::boxWideWidth, content->height());
			}
		});

		std::move(
			Updates()
		) | rpl::on_next([=](uint64 id) {
			_history->eventUpdated(id);
		}, lifetime());

		_historyTab->setActive(true);
		_invokeTab->setActive(false);
		content->resizeToWidth(st::boxWideWidth);
		setDimensions(st::boxWideWidth, content->height());
	}

private:
	void showHistoryTab(not_null<Ui::VerticalLayout*> content) {
		if (_history->isHidden()) {
			_history->show();
			_invoke->hide();
			_historyTab->setActive(true);
			_invokeTab->setActive(false);
			content->resizeToWidth(content->width());
			setDimensions(st::boxWideWidth, content->height());
		}
	}

	void showDetails(uint64 id) {
		if (const auto event = Lookup(id)) {
			_window->show(Box<RpcDetailsBox>(*event));
		}
	}

	not_null<Window::SessionController*> _window;
	TabButton *_historyTab = nullptr;
	TabButton *_invokeTab = nullptr;
	HistoryPane *_history = nullptr;
	InvokePane *_invoke = nullptr;

};

} // namespace

void ShowRpcInspector(not_null<Window::SessionController*> controller) {
	base::options::lookup<bool>(kOptionRpcInspector).set(true);
	controller->show(Box<RpcInspectorBox>(controller));
}

} // namespace Dev::Rpc
