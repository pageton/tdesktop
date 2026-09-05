/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "devtools/devtools_rpc_log.h"

#include "base/flat_map.h"
#include "base/options.h"
#include "crl/crl.h"
#include "scheme-tl_json.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>

#include <atomic>
#include <deque>

namespace Dev::Rpc {
namespace {

// Bounded history: at most that many events and at most that many body
// bytes across all of them, the oldest events are evicted first.
constexpr auto kMaxEvents = 1000;
constexpr auto kMaxTotalBytes = int64(64) * 1024 * 1024;

class Store {
public:
	Store() {
		base::options::lookup<bool>(kOptionRpcInspector
		).changes(
		) | rpl::on_next([=] {
			RefreshGate();
		}, _lifetime);
		_recording = true;
		RefreshGate();
	}

	void RefreshGate() {
		_gate = base::options::value<bool>(kOptionRpcInspector) && _recording;
	}

	[[nodiscard]] bool gate() const {
		return _gate;
	}

	void SetRecording(bool recording) {
		_recording = recording;
		RefreshGate();
	}

	[[nodiscard]] bool recording() const {
		return _recording;
	}

	void Start(
			mtpRequestId requestId,
			MTP::ShiftedDcId dcId,
			mtpRequestId afterRequestId,
			const void *body,
			int bytes,
			crl::time started) {
		auto updated = std::vector<uint64>();
		{
			const auto guard = QMutexLocker(&_mutex);
			removeById(requestId);
			auto event = Event();
			event.id = _nextId++;
			event.requestId = requestId;
			event.dcId = dcId;
			event.afterRequestId = afterRequestId;
			event.started = started;
			event.startedAt = QDateTime::currentMSecsSinceEpoch();
			event.needsLayer = true;
			event.request.resize(bytes);
			if (bytes > 0) {
				memcpy(event.request.data(), body, bytes);
			}
			event.requestSize = bytes;
			const auto name = MTP::details::TlJsonBoxedName(
					reinterpret_cast<const mtpPrime *>(event.request.constData()),
					reinterpret_cast<const mtpPrime *>(event.request.constData())
						+ (bytes / int(sizeof(mtpPrime))));
			event.method = name.isEmpty() ? u"unknown"_q : name;
			const auto id = event.id;
			_ids.emplace(requestId, id);
			_bytes += event.request.size() + event.response.size();
			_events.push_back(std::move(event));
			evict();
			updated.push_back(id);
		}
		notify(std::move(updated));
	}

	void Sent(mtpRequestId requestId, mtpMsgId msgId, uint32 seqNo) {
		auto updated = std::vector<uint64>();
		{
			const auto guard = QMutexLocker(&_mutex);
			const auto id = takeEventId(requestId);
			if (!id) {
				return;
			}
			for (auto &event : _events) {
				if (event.id == *id) {
					event.msgId = msgId;
					event.seqNo = seqNo;
					updated.push_back(*id);
					break;
				}
			}
		}
		notify(std::move(updated));
	}

	void Finish(
			mtpRequestId requestId,
			const void *body,
			int bytes,
			crl::time finished) {
		auto updated = std::vector<uint64>();
		{
			const auto guard = QMutexLocker(&_mutex);
			const auto id = takeEventId(requestId);
			if (!id) {
				return;
			}
			for (auto &event : _events) {
				if (event.id == *id) {
					event.response.resize(bytes);
					if (bytes > 0) {
						memcpy(event.response.data(), body, bytes);
					}
					event.responseSize = bytes;
					event.finished = finished;
					event.status = Status::Succeeded;
					++event.attempts;
					_bytes += bytes;
					updated.push_back(*id);
					break;
				}
			}
		}
		notify(std::move(updated));
	}

	void Fail(
			mtpRequestId requestId,
			int code,
			const QString &type,
			const QString &description,
			crl::time finished) {
		auto updated = std::vector<uint64>();
		{
			const auto guard = QMutexLocker(&_mutex);
			const auto id = takeEventId(requestId);
			if (!id) {
				return;
			}
			for (auto &event : _events) {
				if (event.id == *id) {
					event.errorCode = code;
					event.errorType = type;
					event.errorDescription = description;
					event.finished = finished;
					event.status = Status::Failed;
					++event.attempts;
					updated.push_back(*id);
					break;
				}
			}
		}
		notify(std::move(updated));
	}

	void Cancel(mtpRequestId requestId) {
		auto updated = std::vector<uint64>();
		{
			const auto guard = QMutexLocker(&_mutex);
			const auto id = takeEventId(requestId);
			if (!id) {
				return;
			}
			for (auto &event : _events) {
				if (event.id == *id) {
					event.finished = crl::now();
					event.status = Status::Cancelled;
					updated.push_back(*id);
					break;
				}
			}
		}
		notify(std::move(updated));
	}

	[[nodiscard]] std::vector<Event> snapshot() const {
		const auto guard = QMutexLocker(&_mutex);
		return { _events.begin(), _events.end() };
	}

	[[nodiscard]] std::optional<Event> lookup(uint64 id) const {
		const auto guard = QMutexLocker(&_mutex);
		for (const auto &event : _events) {
			if (event.id == id) {
				return event;
			}
		}
		return std::nullopt;
	}

	void clear() {
		const auto guard = QMutexLocker(&_mutex);
		_events.clear();
		_ids.clear();
		_bytes = 0;
	}

	[[nodiscard]] rpl::producer<uint64> updates() const {
		return _updates.events();
	}

private:
	// Requires _mutex held.
	void removeById(mtpRequestId requestId) {
		const auto i = _ids.find(requestId);
		if (i == _ids.end()) {
			return;
		}
		const auto id = i->second;
		for (auto j = _events.begin(); j != _events.end(); ++j) {
			if (j->id == id) {
				_bytes -= j->request.size() + j->response.size();
				_events.erase(j);
				break;
			}
		}
		_ids.erase(i);
	}

	// Requires _mutex held.
	[[nodiscard]] std::optional<uint64> takeEventId(
			mtpRequestId requestId) const {
		const auto i = _ids.find(requestId);
		if (i == _ids.end()) {
			return std::nullopt;
		}
		return i->second;
	}

	// Requires _mutex held.
	void evict() {
		while (_events.size() > kMaxEvents
			|| (_bytes > kMaxTotalBytes && _events.size() > 1)) {
			const auto &oldest = _events.front();
			_bytes -= oldest.request.size() + oldest.response.size();
			_ids.remove(oldest.requestId);
			_events.pop_front();
		}
	}

	// Must be called without _mutex held: consumers are allowed to read
	// events back, and _updates consumers run Qt UI code that is only
	// safe on the main thread.
	void notify(std::vector<uint64> ids) {
		if (ids.empty()) {
			return;
		}
		crl::on_main([=, this] {
			for (const auto id : ids) {
				_updates.fire_copy(id);
			}
		});
	}

	mutable QMutex _mutex;
	std::deque<Event> _events;
	base::flat_map<mtpRequestId, uint64> _ids;
	uint64 _nextId = 1;
	int64 _bytes = 0;
	bool _recording = true;
	std::atomic<bool> _gate = false;
	rpl::event_stream<uint64> _updates;
	rpl::lifetime _lifetime;

};

Store &State() {
	static auto result = Store();
	return result;
}

} // namespace

bool Active() {
	return State().gate();
}

void SetRecording(bool recording) {
	State().SetRecording(recording);
}

bool Recording() {
	return State().recording();
}

void Start(
		mtpRequestId requestId,
		MTP::ShiftedDcId dcId,
		mtpRequestId afterRequestId,
		const void *body,
		int bytes,
		crl::time started) {
	if (!Active()) {
		return;
	}
	State().Start(requestId, dcId, afterRequestId, body, bytes, started);
}

void Sent(mtpRequestId requestId, mtpMsgId msgId, uint32 seqNo) {
	if (!Active()) {
		return;
	}
	State().Sent(requestId, msgId, seqNo);
}

void Finish(
		mtpRequestId requestId,
		const void *body,
		int bytes,
		crl::time finished) {
	if (!Active()) {
		return;
	}
	State().Finish(requestId, body, bytes, finished);
}

void Fail(
		mtpRequestId requestId,
		int code,
		const QString &type,
		const QString &description,
		crl::time finished) {
	if (!Active()) {
		return;
	}
	State().Fail(requestId, code, type, description, finished);
}

void Cancel(mtpRequestId requestId) {
	if (!Active()) {
		return;
	}
	State().Cancel(requestId);
}

std::vector<Event> Snapshot() {
	return State().snapshot();
}

std::optional<Event> Lookup(uint64 id) {
	return State().lookup(id);
}

void Clear() {
	State().clear();
}

rpl::producer<uint64> Updates() {
	return State().updates();
}

QJsonObject DecodeBoxed(const QByteArray &primes) {
	auto result = QJsonObject();
	const auto data = reinterpret_cast<const mtpPrime *>(primes.constData());
	auto from = data;
	if (!MTP::details::TlJsonDecodeBoxed(
		result,
		from,
		from + primes.size() / int(sizeof(mtpPrime)))) {
		result.insert(u"_"_q, u"error"_q);
	}
	return result;
}

const char kOptionRpcInspector[] = "rpc-inspector";

base::options::toggle OptionRpcInspector({
	.id = kOptionRpcInspector,
	.name = "RPC Inspector",
	.description = "Record MTProto RPC requests and responses in memory"
		" for the RPC Inspector developer tool (last 1000 events).",
	.defaultValue = true,
	.restartRequired = false,
});

} // namespace Dev::Rpc
