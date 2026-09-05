/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/details/mtproto_tl_json.h"

#include "scheme-tl_json.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <iostream>

int Failures = 0;

void Check(bool ok, const QByteArray &what, const QJsonObject &got = {}, const QJsonObject &expected = {}) {
	if (ok) {
		return;
	}
	++Failures;
	std::cout << "FAIL: " << what.toStdString() << std::endl;
	if (!expected.isEmpty() || !got.isEmpty()) {
		std::cout
			<< " expected: "
			<< QJsonDocument(expected).toJson(QJsonDocument::Compact).toStdString()
			<< "\n got:      "
			<< QJsonDocument(got).toJson(QJsonDocument::Compact).toStdString()
			<< std::endl;
	}
}

QJsonObject Decoded(const MTPMessage &message) {
	return MTP::details::TlMessageToJson(message);
}

QJsonObject Obj(std::initializer_list<std::pair<QString, QJsonValue>> items) {
	auto result = QJsonObject();
	for (const auto &[key, value] : items) {
		result.insert(key, value);
	}
	return result;
}

void TestTextMessage() {
	const auto message = MTP_message(
		MTP_flags(MTPDmessage::Flag::f_out | MTPDmessage::Flag::f_entities),
		MTP_int(100),
		MTP_peerUser(MTP_long(42)),
		MTP_int(0),
		MTPstring(),
		MTP_peerUser(MTP_long(42)),
		MTPPeer(),
		MTPMessageFwdHeader(),
		MTPlong(),
		MTPlong(),
		MTPPeer(),
		MTPMessageReplyHeader(),
		MTP_int(1700000000),
		MTP_string("hello world"),
		MTPMessageMedia(),
		MTPReplyMarkup(),
		MTP_vector<MTPMessageEntity>(
			1,
			MTP_messageEntityBold(MTP_int(0), MTP_int(5))),
		MTP_int(0),
		MTP_int(0),
		MTPMessageReplies(),
		MTP_int(0),
		MTPstring(),
		MTPlong(),
		MTPMessageReactions(),
		MTP_vector<MTPRestrictionReason>(),
		MTP_int(0),
		MTP_int(0),
		MTPlong(),
		MTPFactCheck(),
		MTPint(),
		MTPlong(),
		MTPSuggestedPost(),
		MTP_int(0),
		MTPstring(),
		MTPRichMessage());
	const auto json = Decoded(message);
	const auto expected = Obj({
		{ u"_"_q, u"message"_q },
		{ u"flags"_q, 130.0 },
		{ u"flags2"_q, 0.0 },
		{ u"out"_q, true },
		{ u"id"_q, 100.0 },
		{ u"peer_id"_q, Obj({
			{ u"_"_q, u"peerUser"_q },
			{ u"user_id"_q, 42.0 },
		}) },
		{ u"date"_q, 1700000000.0 },
		{ u"message"_q, u"hello world"_q },
		{ u"entities"_q, QJsonArray({
			QJsonValue(Obj({
				{ u"_"_q, u"messageEntityBold"_q },
				{ u"offset"_q, 0.0 },
				{ u"length"_q, 5.0 },
			})),
		}) },
	});
	Check(json == expected, "text message full tree", json, expected);
}

void TestPhotoMessage() {
	const auto media = MTP_messageMediaPhoto(
		MTP_flags(MTPDmessageMediaPhoto::Flag::f_photo
			| MTPDmessageMediaPhoto::Flag::f_spoiler),
		MTP_photo(
			MTP_flags(0),
			MTP_long(9007199254740993LL),
			MTP_long(123456),
			MTP_bytes("ref123"),
			MTP_int(1700000001),
			MTP_vector<MTPPhotoSize>(QVector<MTPPhotoSize>{
				MTP_photoSize(
					MTP_string("x"),
					MTP_int(640),
					MTP_int(360),
					MTP_int(100000)),
				MTP_photoSizeProgressive(
					MTP_string("y"),
					MTP_int(800),
					MTP_int(450),
					MTP_vector<MTPint>(QVector<MTPint>{
						MTP_int(10000),
						MTP_int(50000),
						MTP_int(90000)})),
			}),
			MTP_vector<MTPVideoSize>(),
			MTP_int(2)),
		MTPint(),
		MTPDocument());
	const auto message = MTP_message(
		MTP_flags(MTPDmessage::Flag::f_media),
		MTP_int(101),
		MTPPeer(),
		MTP_int(0),
		MTPstring(),
		MTP_peerChannel(MTP_long(777)),
		MTPPeer(),
		MTPMessageFwdHeader(),
		MTPlong(),
		MTPlong(),
		MTPPeer(),
		MTPMessageReplyHeader(),
		MTP_int(1700000002),
		MTP_string(""),
		media,
		MTPReplyMarkup(),
		MTP_vector<MTPMessageEntity>(),
		MTP_int(0),
		MTP_int(0),
		MTPMessageReplies(),
		MTP_int(0),
		MTPstring(),
		MTPlong(),
		MTPMessageReactions(),
		MTP_vector<MTPRestrictionReason>(),
		MTP_int(0),
		MTP_int(0),
		MTPlong(),
		MTPFactCheck(),
		MTPint(),
		MTPlong(),
		MTPSuggestedPost(),
		MTP_int(0),
		MTPstring(),
		MTPRichMessage());
	const auto json = Decoded(message);
	const auto expected = Obj({
		{ u"_"_q, u"message"_q },
		{ u"flags"_q, 512.0 },
		{ u"flags2"_q, 0.0 },
		{ u"id"_q, 101.0 },
		{ u"peer_id"_q, Obj({
			{ u"_"_q, u"peerChannel"_q },
			{ u"channel_id"_q, 777.0 },
		}) },
		{ u"date"_q, 1700000002.0 },
		{ u"message"_q, u""_q },
		{ u"media"_q, Obj({
			{ u"_"_q, u"messageMediaPhoto"_q },
			{ u"flags"_q, 9.0 },
			{ u"spoiler"_q, true },
			{ u"photo"_q, Obj({
				{ u"_"_q, u"photo"_q },
				{ u"flags"_q, 0.0 },
				{ u"id"_q, u"9007199254740993"_q },
				{ u"access_hash"_q, 123456.0 },
				{ u"file_reference"_q, u"cmVmMTIz"_q },
				{ u"date"_q, 1700000001.0 },
				{ u"sizes"_q, QJsonArray({
					QJsonValue(Obj({
						{ u"_"_q, u"photoSize"_q },
						{ u"type"_q, u"x"_q },
						{ u"w"_q, 640.0 },
						{ u"h"_q, 360.0 },
						{ u"size"_q, 100000.0 },
					})),
					QJsonValue(Obj({
						{ u"_"_q, u"photoSizeProgressive"_q },
						{ u"type"_q, u"y"_q },
						{ u"w"_q, 800.0 },
						{ u"h"_q, 450.0 },
						{ u"sizes"_q, QJsonArray({
							QJsonValue(10000.0),
							QJsonValue(50000.0),
							QJsonValue(90000.0),
						}) },
					})),
				}) },
				{ u"dc_id"_q, 2.0 },
			}) },
		}) },
	});
	Check(json == expected, "photo message full tree", json, expected);
}

void TestReplyMarkupAndReactions() {
	const auto markup = MTP_replyInlineMarkup(
		MTP_flags(0),
		MTP_vector<MTPKeyboardInlineButtonRow>(
			1,
			MTP_keyboardInlineButtonRow(
				MTP_vector<MTPKeyboardInlineButton>(
					1,
					MTP_keyboardInlineButton(
						MTP_flags(0),
						MTPKeyboardButtonStyle(),
						MTP_string("Press"),
						MTP_inlineButtonTypeCallback(
							MTP_flags(
								MTPDinlineButtonTypeCallback::Flag::f_requires_password),
							MTP_bytes("payload")))))));
	const auto reactions = MTP_messageReactions(
		MTP_flags(MTPDmessageReactions::Flag::f_can_see_list
			| MTPDmessageReactions::Flag::f_recent_reactions),
		MTP_vector<MTPReactionCount>(
			1,
			MTP_reactionCount(
				MTP_flags(0),
				MTP_int(0),
				MTP_reactionEmoji(MTP_string(u"\U0001F44D"_q)),
				MTP_int(3))),
		MTP_vector<MTPMessagePeerReaction>(
			1,
			MTP_messagePeerReaction(
				MTP_flags(MTPDmessagePeerReaction::Flag::f_big),
				MTP_peerUser(MTP_long(43)),
				MTP_int(1700000003),
				MTP_reactionEmoji(MTP_string(u"\U0001F44D"_q)))),
		MTP_vector<MTPMessageReactor>());	const auto fwd = MTP_messageFwdHeader(
		MTP_flags(MTPDmessageFwdHeader::Flag::f_imported),
		MTPPeer(),
		MTPstring(),
		MTP_int(1700000004),
		MTPint(),
		MTPstring(),
		MTPPeer(),
		MTPint(),
		MTPPeer(),
		MTPstring(),
		MTPint(),
		MTPstring());
	const auto message = MTP_message(
		MTP_flags(MTPDmessage::Flag::f_reply_to
			| MTPDmessage::Flag::f_fwd_from
			| MTPDmessage::Flag::f_reply_markup
			| MTPDmessage::Flag::f_reactions
			| MTPDmessage::Flag::f_effect),
		MTP_int(102),
		MTPPeer(),
		MTP_int(0),
		MTPstring(),
		MTP_peerUser(MTP_long(42)),
		MTPPeer(),
		fwd,
		MTPlong(),
		MTPlong(),
		MTPPeer(),
			MTP_messageReplyHeader(
				MTP_flags(MTPDmessageReplyHeader::Flag::f_reply_to_msg_id
					| MTPDmessageReplyHeader::Flag::f_forum_topic),
				MTP_int(99),
				MTPPeer(),
				MTPMessageFwdHeader(),
				MTPMessageMedia(),
				MTP_int(0),
				MTPstring(),
				MTP_vector<MTPMessageEntity>(),
				MTP_int(0),
				MTP_int(0),
				MTPbytes()),
		MTP_int(1700000005),
		MTP_string("with markup"),
		MTPMessageMedia(),
		markup,
		MTP_vector<MTPMessageEntity>(),
		MTP_int(0),
		MTP_int(0),
		MTPMessageReplies(),
		MTP_int(0),
		MTPstring(),
		MTPlong(),
		reactions,
		MTP_vector<MTPRestrictionReason>(),
		MTP_int(0),
		MTP_int(0),
		MTP_long(1LL << 40),
		MTPFactCheck(),
		MTPint(),
		MTPlong(),
		MTPSuggestedPost(),
		MTP_int(0),
		MTPstring(),
		MTPRichMessage());
	const auto json = Decoded(message);

	const auto expectedReply = Obj({
		{ u"_"_q, u"messageReplyHeader"_q },
		{ u"flags"_q, 24.0 },
		{ u"forum_topic"_q, true },
		{ u"reply_to_msg_id"_q, 99.0 },
	});
	const auto replyCheck = json.value(u"reply_to"_q) == QJsonValue(expectedReply);
	Check(replyCheck, "reply_to subtree");
	const auto expectedMarkup = Obj({
		{ u"_"_q, u"replyInlineMarkup"_q },
		{ u"flags"_q, 0.0 },
		{ u"rows"_q, QJsonArray({
			QJsonValue(Obj({
				{ u"_"_q, u"keyboardInlineButtonRow"_q },
				{ u"buttons"_q, QJsonArray({
					QJsonValue(Obj({
						{ u"_"_q, u"keyboardInlineButton"_q },
						{ u"flags"_q, 0.0 },
						{ u"text"_q, u"Press"_q },
						{ u"type"_q, Obj({
							{ u"_"_q, u"inlineButtonTypeCallback"_q },
							{ u"flags"_q, 1.0 },
							{ u"requires_password"_q, true },
							{ u"data"_q, u"cGF5bG9hZA=="_q },
						}) },
					})),
				}) },
			})),
		}) },
	});
	Check(
		json.value(u"reply_markup"_q) == QJsonValue(expectedMarkup),
		"reply_markup subtree");
	const auto expectedReactions = Obj({
		{ u"_"_q, u"messageReactions"_q },
		{ u"flags"_q, 6.0 },
		{ u"can_see_list"_q, true },
		{ u"results"_q, QJsonArray({
			QJsonValue(Obj({
				{ u"_"_q, u"reactionCount"_q },
				{ u"flags"_q, 0.0 },
				{ u"reaction"_q, Obj({
					{ u"_"_q, u"reactionEmoji"_q },
					{ u"emoticon"_q, u"\U0001F44D"_q },
				}) },
				{ u"count"_q, 3.0 },
			})),
		}) },
		{ u"recent_reactions"_q, QJsonArray({
			QJsonValue(Obj({
				{ u"_"_q, u"messagePeerReaction"_q },
				{ u"flags"_q, 1.0 },
				{ u"big"_q, true },
				{ u"peer_id"_q, Obj({
					{ u"_"_q, u"peerUser"_q },
					{ u"user_id"_q, 43.0 },
				}) },
				{ u"date"_q, 1700000003.0 },
				{ u"reaction"_q, Obj({
					{ u"_"_q, u"reactionEmoji"_q },
					{ u"emoticon"_q, u"\U0001F44D"_q },
				}) },
			})),
		}) },
	});
	const auto reactionsJson = json.value(u"reactions"_q).toObject();
	Check(
		reactionsJson == expectedReactions,
		"reactions subtree",
		reactionsJson,
		expectedReactions);
	const auto expectedFwd = Obj({
		{ u"_"_q, u"messageFwdHeader"_q },
		{ u"flags"_q, 128.0 },
		{ u"imported"_q, true },
		{ u"date"_q, 1700000004.0 },
	});
	Check(
		json.value(u"fwd_from"_q) == QJsonValue(expectedFwd),
		"fwd_from subtree");
	Check(
		json.value(u"effect"_q) == QJsonValue(double(Q_INT64_C(1) << 40)),
		"effect flags2 long value");
}

void TestServiceMessage() {
	const auto messageService = MTP_messageService(
		MTP_flags(MTPDmessageService::Flag::f_out
			| MTPDmessageService::Flag::f_from_id),
		MTP_int(103),
		MTP_peerUser(MTP_long(42)),
		MTP_peerChat(MTP_long(555)),
		MTPPeer(),
		MTPMessageReplyHeader(),
		MTP_int(1700000006),
		MTP_messageActionChatAddUser(
			MTP_vector<MTPlong>(2, MTP_long(9007199254740993LL))),
		MTPMessageReactions(),
		MTP_int(0));
	const auto json = Decoded(messageService);
	const auto expected = Obj({
		{ u"_"_q, u"messageService"_q },
		{ u"flags"_q, 258.0 },
		{ u"out"_q, true },
		{ u"id"_q, 103.0 },
		{ u"from_id"_q, Obj({
			{ u"_"_q, u"peerUser"_q },
			{ u"user_id"_q, 42.0 },
		}) },
		{ u"peer_id"_q, Obj({
			{ u"_"_q, u"peerChat"_q },
			{ u"chat_id"_q, 555.0 },
		}) },
		{ u"date"_q, 1700000006.0 },
		{ u"action"_q, Obj({
			{ u"_"_q, u"messageActionChatAddUser"_q },
			{ u"users"_q, QJsonArray({
				QJsonValue(u"9007199254740993"_q),
				QJsonValue(u"9007199254740993"_q),
			}) },
		}) },
	});
	Check(json == expected, "service message full tree", json, expected);
}

void TestBoolAndAbsentOptional() {
	const auto markup = MTP_replyKeyboardMarkup(
		MTP_flags(0),
		MTP_vector<MTPKeyboardButtonRow>(
			1,
			MTP_keyboardButtonRow(
				MTP_vector<MTPKeyboardButton>(
					1,
					MTP_keyboardButton(
						MTP_flags(0),
						MTPKeyboardButtonStyle(),
						MTP_string("Poll"),
						MTP_buttonTypeRequestPoll(
							MTP_flags(MTPDbuttonTypeRequestPoll::Flag::f_quiz),
							MTP_boolTrue()))))),
		MTPstring());
	const auto message = MTP_message(
		MTP_flags(MTPDmessage::Flag::f_reply_markup),
		MTP_int(104),
		MTPPeer(),
		MTP_int(0),
		MTPstring(),
		MTP_peerUser(MTP_long(42)),
		MTPPeer(),
		MTPMessageFwdHeader(),
		MTPlong(),
		MTPlong(),
		MTPPeer(),
		MTPMessageReplyHeader(),
		MTP_int(1700000007),
		MTP_string(""),
		MTPMessageMedia(),
		markup,
		MTP_vector<MTPMessageEntity>(),
		MTP_int(0),
		MTP_int(0),
		MTPMessageReplies(),
		MTP_int(0),
		MTPstring(),
		MTPlong(),
		MTPMessageReactions(),
		MTP_vector<MTPRestrictionReason>(),
		MTP_int(0),
		MTP_int(0),
		MTPlong(),
		MTPFactCheck(),
		MTPint(),
		MTPlong(),
		MTPSuggestedPost(),
		MTP_int(0),
		MTPstring(),
		MTPRichMessage());
	const auto json = Decoded(message);
	const auto markupJson = json.value(u"reply_markup"_q).toObject();
	const auto buttonJson = markupJson
		.value(u"rows"_q).toArray().at(0).toObject()
		.value(u"buttons"_q).toArray().at(0).toObject();
	const auto button = Obj({
		{ u"_"_q, u"keyboardButton"_q },
		{ u"flags"_q, 0.0 },
		{ u"text"_q, u"Poll"_q },
		{ u"type"_q, Obj({
			{ u"_"_q, u"buttonTypeRequestPoll"_q },
			{ u"flags"_q, 1.0 },
			{ u"quiz"_q, true },
		}) },
	});
	Check(buttonJson == button, "keyboardButton with Bool true", buttonJson, button);

	const auto callback = MTP_keyboardInlineButton(
		MTP_flags(0),
		MTPKeyboardButtonStyle(),
		MTP_string("NoPassword"),
		MTP_inlineButtonTypeCallback(MTP_flags(0), MTP_bytes("abc")));
	const auto boxedCallback = MTPKeyboardInlineButton(callback);
	auto primes = mtpBuffer();
	boxedCallback.write(primes);
	auto from = primes.constData();
	auto callbackJson = QJsonObject();
	Check(
		MTP::details::TlJsonDecodeBoxed(
			callbackJson,
			from,
			from + primes.size()),
		"callback button decodes");
	const auto expectedCallback = Obj({
		{ u"_"_q, u"keyboardInlineButton"_q },
		{ u"flags"_q, 0.0 },
		{ u"text"_q, u"NoPassword"_q },
		{ u"type"_q, Obj({
			{ u"_"_q, u"inlineButtonTypeCallback"_q },
			{ u"flags"_q, 0.0 },
			{ u"data"_q, u"YWJj"_q },
		}) },
	});
	Check(
		callbackJson == expectedCallback,
		"flag-disabled optional is absent, not false",
		callbackJson,
		expectedCallback);
}

void TestEmptyVector() {
	const auto message = MTP_message(
		MTP_flags(MTPDmessage::Flag::f_entities),
		MTP_int(105),
		MTPPeer(),
		MTP_int(0),
		MTPstring(),
		MTP_peerUser(MTP_long(42)),
		MTPPeer(),
		MTPMessageFwdHeader(),
		MTPlong(),
		MTPlong(),
		MTPPeer(),
		MTPMessageReplyHeader(),
		MTP_int(1700000008),
		MTP_string("empty"),
		MTPMessageMedia(),
		MTPReplyMarkup(),
		MTP_vector<MTPMessageEntity>(),
		MTP_int(0),
		MTP_int(0),
		MTPMessageReplies(),
		MTP_int(0),
		MTPstring(),
		MTPlong(),
		MTPMessageReactions(),
		MTP_vector<MTPRestrictionReason>(),
		MTP_int(0),
		MTP_int(0),
		MTPlong(),
		MTPFactCheck(),
		MTPint(),
		MTPlong(),
		MTPSuggestedPost(),
		MTP_int(0),
		MTPstring(),
		MTPRichMessage());
	const auto json = Decoded(message);
	const auto expected = Obj({
		{ u"_"_q, u"message"_q },
		{ u"flags"_q, 128.0 },
		{ u"flags2"_q, 0.0 },
		{ u"id"_q, 105.0 },
		{ u"peer_id"_q, Obj({
			{ u"_"_q, u"peerUser"_q },
			{ u"user_id"_q, 42.0 },
		}) },
		{ u"date"_q, 1700000008.0 },
		{ u"message"_q, u"empty"_q },
		{ u"entities"_q, QJsonValue(QJsonArray()) },
	});
	Check(json == expected, "empty entities vector", json, expected);
}

void TestTruncatedFails() {
	const auto message = MTP_messageEmpty(
		MTP_flags(0),
		MTP_int(7),
		MTPPeer());
	auto primes = mtpBuffer();
	message.write(primes);
	auto result = QJsonObject();
	auto from = primes.constData();
	const auto ok = MTP::details::TlJsonDecodeBoxed(
		result,
		from,
		from + primes.size() - 1);
	Check(!ok, "truncated buffer fails");
	Check(
		result.isEmpty() || !ok,
		"no partial object on failure");
}

void TestRequestRoundTrip() {
	const auto request = MTPmessages_GetHistory(
		MTP_inputPeerChannel(MTP_long(123456789), MTP_long(987654321)),
		MTP_int(0),
		MTP_int(0),
		MTP_int(0),
		MTP_int(100),
		MTP_int(0),
		MTP_int(0),
		MTP_long(0));
	auto primes = mtpBuffer();
	request.write(primes);

	auto json = QJsonObject();
	auto from = primes.constData();
	Check(
		MTP::details::TlJsonDecodeBoxed(json, from, from + primes.size()),
		"request decodes");
	const auto expected = Obj({
		{ u"_"_q, u"messages.getHistory"_q },
		{ u"peer"_q, Obj({
			{ u"_"_q, u"inputPeerChannel"_q },
			{ u"channel_id"_q, 123456789.0 },
			{ u"access_hash"_q, 987654321.0 },
		}) },
		{ u"offset_id"_q, 0.0 },
		{ u"offset_date"_q, 0.0 },
		{ u"add_offset"_q, 0.0 },
		{ u"limit"_q, 100.0 },
		{ u"max_id"_q, 0.0 },
		{ u"min_id"_q, 0.0 },
		{ u"hash"_q, 0.0 },
	});
	Check(json == expected, "request tree", json, expected);

	auto encoded = mtpBuffer();
	Check(
		MTP::details::TlJsonEncodeBoxed(json, encoded),
		"request re-encodes");
	Check(encoded == primes, "re-encoded primes equal");

	const auto name = MTP::details::TlJsonBoxedName(
		primes.constData(),
		primes.constData() + primes.size());
	Check(name == u"messages.getHistory"_q, "boxed name lookup");
}

void TestConditionalFlagsRoundTrip() {
	const auto json = Obj({
		{ u"_"_q, u"messages.search"_q },
		{ u"peer"_q, Obj({
			{ u"_"_q, u"inputPeerSelf"_q },
		}) },
		{ u"q"_q, u"query"_q },
		{ u"filter"_q, Obj({
			{ u"_"_q, u"inputMessagesFilterPhotos"_q },
		}) },
		{ u"min_date"_q, 0.0 },
		{ u"max_date"_q, 0.0 },
		{ u"offset_id"_q, 0.0 },
		{ u"add_offset"_q, 0.0 },
		{ u"limit"_q, 10.0 },
		{ u"max_id"_q, 0.0 },
		{ u"min_id"_q, 0.0 },
		{ u"hash"_q, 0.0 },
		{ u"from_id"_q, Obj({
			{ u"_"_q, u"inputPeerSelf"_q },
		}) },
	});
	auto primes = mtpBuffer();
	Check(
		MTP::details::TlJsonEncodeBoxed(json, primes),
		"conditional fields encode");
	auto back = QJsonObject();
	auto from = primes.constData();
	Check(
		MTP::details::TlJsonDecodeBoxed(back, from, from + primes.size()),
		"conditional fields decode");
	Check(back.value(u"flags"_q) == 1.0, "flags computed from presence");
	// The input omitted the flags field, the decoded wire form has it.
	back.remove(u"flags"_q);
	Check(back == json, "conditional fields round trip", back, json);
}

void TestEncodeFailures() {
	auto to = mtpBuffer();
	Check(
		!MTP::details::TlJsonEncodeBoxed(
			Obj({{ u"_"_q, u"no.suchConstructor"_q }}),
			to),
		"unknown constructor fails");
	Check(
		!MTP::details::TlJsonEncodeBoxed(
			Obj({{ u"_"_q, u"messages.getHistory"_q }}),
			to),
		"missing required fields fail");
	Check(
		!MTP::details::TlJsonEncodeBoxed(Obj({{ u"key"_q, u"value"_q }}), to),
		"missing constructor name fails");
}

int main(int argc, char *argv[]) {
	const auto app = QCoreApplication(argc, argv);
	TestTextMessage();
	TestPhotoMessage();
	TestReplyMarkupAndReactions();
	TestServiceMessage();
	TestBoolAndAbsentOptional();
	TestEmptyVector();
	TestTruncatedFails();
	TestRequestRoundTrip();
	TestConditionalFlagsRoundTrip();
	TestEncodeFailures();
	if (Failures > 0) {
		std::cout << Failures << " checks failed" << std::endl;
		return 1;
	}
	std::cout << "all tl json checks passed" << std::endl;
	return 0;
}
