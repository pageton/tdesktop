/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "mtproto/core_types.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <rpl/rpl.h>

#include <optional>
#include <vector>

namespace Dev::Rpc {

extern const char kOptionRpcInspector[];

enum class Status : uchar {
	Pending,
	Succeeded,
	Failed,
	Cancelled,
};

// A single captured RPC. The request and response bodies are kept in the
// raw TL wire form (boxed body primes) and are decoded to JSON lazily, on
// demand, so recording stays cheap and only one representation is kept.
struct Event {
	uint64 id = 0;
	mtpRequestId requestId = 0;
	QString method;
	QByteArray request;
	QByteArray response;
	int errorCode = 0;
	QString errorType;
	QString errorDescription;
	MTP::ShiftedDcId dcId = 0;
	mtpMsgId msgId = 0;
	uint32 seqNo = 0;
	mtpRequestId afterRequestId = 0;
	bool needsLayer = false;
	int attempts = 0;
	crl::time started = 0;
	qint64 startedAt = 0;
	crl::time finished = 0;
	int requestSize = 0;
	int responseSize = 0;
	Status status = Status::Pending;
};

// Whether the interception seams should capture anything at all: the
// experimental toggle is on and recording was not paused in the UI.
[[nodiscard]] bool Active();

// In-memory recording state, controlled from the Inspector UI.
void SetRecording(bool recording);
[[nodiscard]] bool Recording();

// Capture seams, called unconditionally from the shared MTP layer points.
// Every one of them returns immediately unless Active().
void Start(
	mtpRequestId requestId,
	MTP::ShiftedDcId dcId,
	mtpRequestId afterRequestId,
	const void *body,
	int bytes,
	crl::time started);
// May be called from a connection thread.
void Sent(mtpRequestId requestId, mtpMsgId msgId, uint32 seqNo);
void Finish(
	mtpRequestId requestId,
	const void *body,
	int bytes,
	crl::time finished);
void Fail(
	mtpRequestId requestId,
	int code,
	const QString &type,
	const QString &description,
	crl::time finished);
void Cancel(mtpRequestId requestId);

[[nodiscard]] std::vector<Event> Snapshot();
[[nodiscard]] std::optional<Event> Lookup(uint64 id);
void Clear();

// Fires an event id from the main thread whenever an event is appended
// or its status changes, so the Inspector UI can refresh that row.
[[nodiscard]] rpl::producer<uint64> Updates();

[[nodiscard]] QJsonObject DecodeBoxed(const QByteArray &primes);

} // namespace Dev::Rpc
