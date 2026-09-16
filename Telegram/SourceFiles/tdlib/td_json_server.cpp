/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tdlib/td_json_server.h"

#include "base/basic_types.h"
#include "export/export_controller.h"
#include "export/export_settings.h"
#include "export/output/export_output_abstract.h"
#include "main/main_domain.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/core_types.h"
#include "mtproto/details/mtproto_serialized_request.h"
#include "mtproto/mtproto_response.h"
#include "mtproto/mtproto_config.h"
#include "lang/lang_keys.h"

// Native send-file path (mirrors the desktop GUI uploader) used by the
// "sendFile" control command, so socket clients can upload through the same
// Storage::Uploader the GUI uses instead of TDLib's FileUploader.
#include "apiwrap.h"
#include "api/api_common.h"
#include "data/data_session.h"
#include "data/data_peer_id.h"
#include "history/history.h"
#include "storage/storage_media_prepare.h"
#include "storage/localimageloader.h"
#include "ui/chat/attach/attach_prepare.h"
#include "styles/style_boxes.h"

#include <td/telegram/td_json_client.h>
#include <td/telegram/telegram_api.h>
#include <td/telegram/telegram_api.hpp>
#include <td/telegram/telegram_api_json.h>
#include <td/tl/tl_json.h>
#include <td/utils/JsonBuilder.h>
#include <td/utils/buffer.h>
#include <td/utils/tl_storers.h>
#include <td/utils/tl_parsers.h>

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <array>

namespace TdBridge {
namespace {

constexpr auto kTdLibZeroChannelId = qint64(-1000000000000LL);
constexpr auto kTdLibZeroSecretChatId = qint64(-2000000000000LL);

Export::Settings ParseExportSettings(const QJsonObject &settingsJson) {
	auto result = Export::Settings();

	result.path = settingsJson.value("path").toString();

	const auto formatStr = settingsJson.value("format").toString(u"json"_q);
	if (formatStr == u"html"_q) {
		result.format = Export::Output::Format::Html;
	} else if (formatStr == u"json"_q) {
		result.format = Export::Output::Format::Json;
	} else if (formatStr == u"html_and_json"_q) {
		result.format = Export::Output::Format::HtmlAndJson;
	} else {
		result.format = Export::Output::Format::Json;
	}

	const auto typesArray = settingsJson.value("types").toArray();
	if (!typesArray.isEmpty()) {
		result.types = Export::Settings::Types();
		for (const auto &val : typesArray) {
			const auto t = val.toString();
			if (t == u"personal_info"_q) {
				result.types |= Export::Settings::Type::PersonalInfo;
			} else if (t == u"userpics"_q) {
				result.types |= Export::Settings::Type::Userpics;
			} else if (t == u"contacts"_q) {
				result.types |= Export::Settings::Type::Contacts;
			} else if (t == u"sessions"_q) {
				result.types |= Export::Settings::Type::Sessions;
			} else if (t == u"other_data"_q) {
				result.types |= Export::Settings::Type::OtherData;
			} else if (t == u"personal_chats"_q) {
				result.types |= Export::Settings::Type::PersonalChats;
			} else if (t == u"bot_chats"_q) {
				result.types |= Export::Settings::Type::BotChats;
			} else if (t == u"private_groups"_q) {
				result.types |= Export::Settings::Type::PrivateGroups;
			} else if (t == u"public_groups"_q) {
				result.types |= Export::Settings::Type::PublicGroups;
			} else if (t == u"private_channels"_q) {
				result.types |= Export::Settings::Type::PrivateChannels;
			} else if (t == u"public_channels"_q) {
				result.types |= Export::Settings::Type::PublicChannels;
			} else if (t == u"stories"_q) {
				result.types |= Export::Settings::Type::Stories;
			} else if (t == u"profile_music"_q) {
				result.types |= Export::Settings::Type::ProfileMusic;
			}
		}
	}

	const auto mediaObj = settingsJson.value("media").toObject();
	if (!mediaObj.isEmpty()) {
		const auto mediaTypesArray = mediaObj.value("types").toArray();
		if (!mediaTypesArray.isEmpty()) {
			result.media.types = Export::MediaSettings::Types();
			for (const auto &val : mediaTypesArray) {
				const auto t = val.toString();
				if (t == u"photo"_q) {
					result.media.types |= Export::MediaSettings::Type::Photo;
				} else if (t == u"video"_q) {
					result.media.types |= Export::MediaSettings::Type::Video;
				} else if (t == u"voice"_q) {
					result.media.types |= Export::MediaSettings::Type::VoiceMessage;
				} else if (t == u"video_message"_q) {
					result.media.types |= Export::MediaSettings::Type::VideoMessage;
				} else if (t == u"sticker"_q) {
					result.media.types |= Export::MediaSettings::Type::Sticker;
				} else if (t == u"gif"_q) {
					result.media.types |= Export::MediaSettings::Type::GIF;
				} else if (t == u"file"_q) {
					result.media.types |= Export::MediaSettings::Type::File;
				}
			}
		}
		if (mediaObj.contains("size_limit")) {
			result.media.sizeLimit = int64(mediaObj.value("size_limit").toDouble());
		}
	}

	if (settingsJson.contains("from_date")) {
		result.singlePeerFrom = TimeId(settingsJson.value("from_date").toInt());
	}
	if (settingsJson.contains("till_date")) {
		result.singlePeerTill = TimeId(settingsJson.value("till_date").toInt());
	}

	return result;
}

Export::Environment PrepareEnvironment(Main::Session *session) {
	auto result = Export::Environment();
	if (session) {
		result.internalLinksDomain = session->serverConfig().internalLinksDomain;
	} else {
		result.internalLinksDomain = u"https://t.me/"_q;
	}
	result.aboutTelegram = tr::lng_export_about_telegram(tr::now).toUtf8();
	result.aboutContacts = tr::lng_export_about_contacts(tr::now).toUtf8();
	result.aboutFrequent = tr::lng_export_about_frequent(tr::now).toUtf8();
	result.aboutSessions = tr::lng_export_about_sessions(tr::now).toUtf8();
	result.aboutWebSessions = tr::lng_export_about_web_sessions(tr::now).toUtf8();
	result.aboutChats = tr::lng_export_about_chats(tr::now).toUtf8();
	result.aboutLeftChats = tr::lng_export_about_left_chats(tr::now).toUtf8();
	return result;
}

QString StepToString(Export::ProcessingState::Step step) {
	using Step = Export::ProcessingState::Step;
	switch (step) {
	case Step::Initializing: return u"Initializing"_q;
	case Step::DialogsList: return u"DialogsList"_q;
	case Step::PersonalInfo: return u"PersonalInfo"_q;
	case Step::Userpics: return u"Userpics"_q;
	case Step::Stories: return u"Stories"_q;
	case Step::ProfileMusic: return u"ProfileMusic"_q;
	case Step::Contacts: return u"Contacts"_q;
	case Step::Sessions: return u"Sessions"_q;
	case Step::OtherData: return u"OtherData"_q;
	case Step::Dialogs: return u"Dialogs"_q;
	case Step::Topic: return u"Topic"_q;
	}
	return u"Unknown"_q;
}

QString EntityTypeToString(Export::ProcessingState::EntityType type) {
	using Type = Export::ProcessingState::EntityType;
	switch (type) {
	case Type::Chat: return u"Chat"_q;
	case Type::SavedMessages: return u"SavedMessages"_q;
	case Type::RepliesMessages: return u"RepliesMessages"_q;
	case Type::VerifyCodes: return u"VerifyCodes"_q;
	case Type::Topic: return u"Topic"_q;
	case Type::Other: return u"Other"_q;
	}
	return u"Unknown"_q;
}

QJsonObject ExportStateToJson(
		int accountIndex,
		const Export::State &state,
		const QJsonValue &extra) {
	auto payload = QJsonObject{
		{ "command", "exportProgress" },
		{ "account", accountIndex },
	};

	v::match(state, [&](const Export::ProcessingState &s) {
		payload["state"] = "processing";
		payload["step"] = StepToString(s.step);
		payload["entity_type"] = EntityTypeToString(s.entityType);
		if (!s.entityName.isEmpty()) {
			payload["entity_name"] = s.entityName;
		}
		payload["entity_index"] = s.entityIndex;
		payload["entity_count"] = s.entityCount;
		payload["item_index"] = s.itemIndex;
		payload["item_count"] = s.itemCount;
		if (s.bytesCount > 0) {
			payload["bytes_loaded"] = qint64(s.bytesLoaded);
			payload["bytes_count"] = qint64(s.bytesCount);
			if (!s.bytesName.isEmpty()) {
				payload["bytes_name"] = s.bytesName;
			}
		}
	}, [&](const Export::FinishedState &s) {
		payload["state"] = "finished";
		payload["path"] = s.path;
		payload["files_count"] = s.filesCount;
		payload["bytes_count"] = qint64(s.bytesCount);
	}, [&](const Export::ApiErrorState &s) {
		payload["state"] = "error";
		payload["error_code"] = s.data.code();
		payload["message"] = s.data.type();
		if (!s.data.description().isEmpty()) {
			payload["description"] = s.data.description();
		}
	}, [&](const Export::OutputErrorState &s) {
		payload["state"] = "error";
		payload["message"] = u"Output error"_q;
		payload["path"] = s.path;
	}, [&](const Export::CancelledState &) {
		payload["state"] = "cancelled";
	}, [&](const Export::PasswordCheckState &) {
		payload["state"] = "password_check";
	}, [&](v::null_t) {
	});

	if (!extra.isUndefined()) {
		payload["@extra"] = extra;
	}

	return QJsonObject{
		{ "type", "tdesktop" },
		{ "payload", payload },
	};
}

// Map a TDLib-style chat_id to a tdesktop PeerId, matching TDLib's DialogId
// encoding: users are positive; basic groups are -group_id; channels and
// supergroups are (ZERO_CHANNEL_ID - channel_id). Secret chats are not
// supported. Returns PeerId(0) for an out-of-range (secret-chat) id.
[[nodiscard]] PeerId PeerIdFromTdLibChatId(qint64 chatId) {
	if (chatId > 0) {
		return peerFromUser(UserId(chatId));
	} else if (chatId > kTdLibZeroChannelId) {
		return peerFromChat(ChatId(-chatId));
	} else if (chatId > kTdLibZeroSecretChatId) {
		return peerFromChannel(ChannelId(kTdLibZeroChannelId - chatId));
	}
	return PeerId(0);
}

[[nodiscard]] qint64 TdLibChatIdFromPeerId(PeerId peerId) {
	if (peerIsUser(peerId)) {
		return qint64(peerToUser(peerId).bare);
	} else if (peerIsChat(peerId)) {
		return -qint64(peerToChat(peerId).bare);
	} else if (peerIsChannel(peerId)) {
		return kTdLibZeroChannelId - qint64(peerToChannel(peerId).bare);
	}
	return 0;
}

[[nodiscard]] bool ReadJsonInteger(
		const QJsonValue &value,
		qint64 &result) {
	auto ok = false;
	const auto parsed = value.toVariant().toLongLong(&ok);
	if (!ok) {
		return false;
	}
	result = parsed;
	return true;
}

[[nodiscard]] bool IsChatIdField(const QString &key) {
	return key != u"secret_chat_id"_q
		&& (key == u"chat_id"_q || key.endsWith(u"_chat_id"_q));
}

[[nodiscard]] bool IsChatIdsField(const QString &key) {
	return key != u"secret_chat_ids"_q
		&& (key == u"chat_ids"_q || key.endsWith(u"_chat_ids"_q));
}

[[nodiscard]] bool IsChatScopedRequestType(const QString &type) {
	static const auto types = QStringView(
		u"addChatMember\naddChatMembers\n"
		u"addChatToList\naddChatWelcomeMessage\naddChecklistTasks\n"
		u"addFileToDownloads\naddLocalMessage\naddMessageReaction\naddOffer\n"
		u"addPendingPaidMessageReaction\naddPollOption\naddRecentlyFoundChat\n"
		u"addStoryAlbumStories\napproveSuggestedPost\nbanChatMember\n"
		u"boostChat\ncanPostStory\ncheckChatUsername\n"
		u"clickAnimatedEmojiMessage\nclickChatSponsoredMessage\ncloseChat\n"
		u"closeStory\ncommitPendingPaidMessageReactions\n"
		u"createChatInviteLink\n"
		u"createChatSubscriptionInviteLink\ncreateCommunity\n"
		u"createForumTopic\ncreateStoryAlbum\ncreateVideoChat\n"
		u"declineGroupCallInvitation\ndeclineSuggestedPost\n"
		u"deleteAllChatWelcomeMessages\n"
		u"deleteAllRecentMessageReactionsFromSender\n"
		u"deleteAllRevokedChatInviteLinks\ndeleteChat\ndeleteChatBackground\n"
		u"deleteChatHistory\ndeleteChatMessagesByDate\n"
		u"deleteChatMessagesBySender\ndeleteChatReplyMarkup\n"
		u"deleteChatWelcomeMessage\ndeleteDirectMessagesChatTopicHistory\n"
		u"deleteDirectMessagesChatTopicMessagesByDate\n"
		u"deleteEphemeralMessage\ndeleteForumTopic\n"
		u"deleteMessageEphemeralContent\ndeleteMessageReactionsFromSender\n"
		u"deleteMessages\ndeletePollOption\ndeleteRevokedChatInviteLink\n"
		u"deleteStory\ndeleteStoryAlbum\neditBusinessMessageCaption\n"
		u"editBusinessMessageChecklist\neditBusinessMessageLiveLocation\n"
		u"editBusinessMessageMedia\neditBusinessMessageReplyMarkup\n"
		u"editBusinessMessageText\neditBusinessStory\n"
		u"editChatInviteLink\n"
		u"editChatSubscriptionInviteLink\neditChatWelcomeMessage\n"
		u"editEphemeralMessage\neditEphemeralMessageCaption\neditForumTopic\n"
		u"editMessageCaption\neditMessageChecklist\neditMessageLiveLocation\n"
		u"editMessageMedia\neditMessageReplyMarkup\n"
		u"editMessageSchedulingState\neditMessageText\neditStory\n"
		u"editStoryCover\nforwardMessages\ngetAllStickerEmojis\n"
		u"getCallbackQueryAnswer\ngetCallbackQueryMessage\ngetChat\n"
		u"getChatActiveStories\ngetChatAdministrators\n"
		u"getChatArchivedStories\ngetChatAvailableMessageSenders\n"
		u"getChatAvailablePaidMessageReactionSenders\ngetChatBoostLink\n"
		u"getChatBoostStatus\ngetChatBoosts\ngetChatEventLog\ngetChatHistory\n"
		u"getChatInviteLink\ngetChatInviteLinkCounts\n"
		u"getChatInviteLinkMembers\ngetChatInviteLinks\ngetChatJoinRequests\n"
		u"getChatListsToAddChat\ngetChatMember\ngetChatMessageByDate\n"
		u"getChatMessageCalendar\ngetChatMessageCount\n"
		u"getChatMessagePosition\ngetChatOwnerAfterLeaving\n"
		u"getChatPinnedMessage\ngetChatPostedToChatPageStories\n"
		u"getChatRevenueStatistics\ngetChatRevenueTransactions\n"
		u"getChatRevenueWithdrawalUrl\ngetChatScheduledMessages\n"
		u"getChatSimilarChatCount\ngetChatSimilarChats\n"
		u"getChatSparseMessagePositions\ngetChatSponsoredMessages\n"
		u"getChatStatistics\ngetChatStoryAlbums\ngetChatStoryInteractions\n"
		u"getDirectMessagesChatTopic\ngetDirectMessagesChatTopicHistory\n"
		u"getDirectMessagesChatTopicMessageByDate\n"
		u"getDirectMessagesChatTopicRevenue\ngetForumTopic\n"
		u"getForumTopicHistory\ngetForumTopicLink\ngetForumTopics\n"
		u"getFullRichMessage\ngetGameHighScores\ngetGiveawayInfo\n"
		u"getInlineQueryResults\ngetLiveStoryRtmpUrl\n"
		u"getLoginUrl\ngetLoginUrlInfo\ngetMainWebApp\ngetMapThumbnailFile\n"
		u"getMessage\ngetMessageAddedReactions\ngetMessageAuthor\n"
		u"getMessageAvailableReactions\ngetMessageEmbeddingCode\n"
		u"getMessageImportConfirmationText\ngetMessageLink\n"
		u"getMessageLocally\ngetMessageProperties\ngetMessagePublicForwards\n"
		u"getMessageReadDate\ngetMessageStatistics\ngetMessageThread\n"
		u"getMessageThreadHistory\ngetMessageViewers\ngetMessages\n"
		u"getPaymentReceipt\ngetPollOptionProperties\ngetPollVoteStatistics\n"
		u"getPollVoters\ngetPremiumGiveawayPaymentOptions\ngetRepliedMessage\n"
		u"getStatisticalGraph\ngetStickers\ngetStory\ngetStoryAlbumStories\n"
		u"getStoryPublicForwards\ngetStoryStatistics\ngetUserChatBoosts\n"
		u"getVideoChatAvailableParticipants\ngetVideoChatRtmpUrl\n"
		u"getVideoMessageAdvertisements\ngetWebAppLinkUrl\nimportMessages\n"
		u"joinChat\nleaveChat\nloadChatWelcomeMessages\n"
		u"loadDirectMessagesChatTopics\nmarkChecklistTasksAsDone\nopenChat\n"
		u"openChatSimilarChat\nopenMessageContent\nopenStory\nopenWebApp\n"
		u"pinChatMessage\npostStory\n"
		u"processChatHasProtectedContentDisableRequest\n"
		u"processChatJoinRequest\nprocessChatJoinRequests\n"
		u"rateSpeechRecognition\nreadAllChatMentions\nreadAllChatPollVotes\n"
		u"readAllChatReactions\nreadAllDirectMessagesChatTopicReactions\n"
		u"readAllForumTopicMentions\nreadAllForumTopicPollVotes\n"
		u"readAllForumTopicReactions\nreadBusinessMessage\nrecognizeSpeech\n"
		u"removeBusinessConnectedBotFromChat\nremoveChatActionBar\n"
		u"removeMessageReaction\nremovePendingPaidMessageReactions\n"
		u"removeRecentlyFoundChat\nremoveStoryAlbumStories\nremoveTopChat\n"
		u"reorderStoryAlbumStories\nreorderStoryAlbums\n"
		u"replaceLiveStoryRtmpUrl\nreplacePrimaryChatInviteLink\n"
		u"replaceVideoChatRtmpUrl\nreportChat\nreportChatPhoto\n"
		u"reportChatSponsoredMessage\nreportMessageReactions\nreportStory\n"
		u"resendMessages\nrevokeChatInviteLink\nsaveApplicationLogEvent\n"
		u"searchChatMembers\nsearchChatMessages\n"
		u"searchChatRecentLocationMessages\nsearchPublicStoriesByTag\n"
		u"searchSecretMessages\nsendBotStartMessage\nsendBusinessMessage\n"
		u"sendBusinessMessageAlbum\nsendChatAction\nsendEphemeralMessage\n"
		u"sendInlineQueryResultMessage\nsendMessage\nsendMessageAlbum\n"
		u"sendMessageViewMetrics\nsendQuickReplyShortcutMessages\n"
		u"sendRichMessageDraft\nsendTextMessageDraft\n"
		u"setBusinessMessageIsPinned\nsetChatAccentColor\n"
		u"setChatActiveStoriesList\nsetChatAffiliateProgram\n"
		u"setChatAvailableReactions\nsetChatBackground\nsetChatClientData\n"
		u"setChatDescription\nsetChatDirectMessagesGroup\n"
		u"setChatDiscussionGroup\nsetChatDraftMessage\nsetChatEmojiStatus\n"
		u"setChatLocation\nsetChatMemberStatus\nsetChatMemberTag\n"
		u"setChatMessageAutoDeleteTime\nsetChatMessageSender\n"
		u"setChatNotificationSettings\nsetChatPaidMessageStarCount\n"
		u"setChatPermissions\nsetChatPhoto\nsetChatPinnedStories\n"
		u"setChatProfileAccentColor\nsetChatSlowModeDelay\nsetChatTheme\n"
		u"setChatTitle\nsetDirectMessagesChatTopicIsMarkedAsUnread\n"
		u"setForumTopicNotificationSettings\nsetGameScore\n"
		u"setMessageFactCheck\nsetMessageReactions\n"
		u"setPaidMessageReactionType\nsetPersonalChat\n"
		u"setPinnedForumTopics\nsetPollAnswer\nsetStoryAlbumName\n"
		u"setStoryReaction\nsetVideoChatDefaultParticipant\nshareChatWithBot\n"
		u"startLiveStory\nstopBusinessPoll\nstopPendingMessage\nstopPoll\n"
		u"summarizeMessage\ntoggleBusinessConnectedBotChatIsPaused\n"
		u"toggleChatDefaultDisableNotification\ntoggleChatGiftNotifications\n"
		u"toggleChatHasProtectedContent\ntoggleChatIsMarkedAsUnread\n"
		u"toggleChatIsPinned\ntoggleChatIsTranslatable\n"
		u"toggleChatViewAsTopics\n"
		u"toggleDirectMessagesChatTopicCanSendUnpaidMessages\n"
		u"toggleForumTopicIsClosed\ntoggleForumTopicIsPinned\n"
		u"toggleGeneralForumTopicIsHidden\ntoggleStoryIsPostedToChatPage\n"
		u"transferChatOwnership\ntranslateMessageRichMessage\n"
		u"translateMessageText\nunpinAllChatMessages\n"
		u"unpinAllDirectMessagesChatTopicMessages\n"
		u"unpinAllForumTopicMessages\nunpinChatMessage\n"
		u"upgradeBasicGroupChatToSupergroupChat\nviewMessages\n"
	).split(u'\n', Qt::SkipEmptyParts);
	return ranges::contains(types, type);
}

} // namespace

ControlServer::ControlServer(
		const QString &socketPath,
		const QStringList &allowedAccountSpecs,
		QObject *parent)
: QObject(parent)
, _socketPath(socketPath)
, _allowedAccountSpecs(allowedAccountSpecs)
, _chatFilteringEnabled(qEnvironmentVariableIsSet(
	"TDESKTOP_SOCKET_CHATS")) {
	if (_chatFilteringEnabled) {
		const auto allowedChatSpecs = qEnvironmentVariable(
			"TDESKTOP_SOCKET_CHATS").split(',', Qt::SkipEmptyParts);
		for (const auto &rawSpec : allowedChatSpecs) {
			const auto separator = rawSpec.indexOf(':');
			const auto accountSpec = rawSpec.left(separator).trimmed();
			const auto chatSpec = rawSpec.mid(separator + 1).trimmed();
			if (separator <= 0 || accountSpec.isEmpty() || chatSpec.isEmpty()) {
				base::LogWriteMain(
					u"Control Server: Ignoring invalid chat access spec '%1'"_q
						.arg(rawSpec));
				continue;
			}
			if (chatSpec == u"*"_q) {
				_allowedChatSpecs.push_back({ accountSpec, 0 });
				continue;
			}
			auto ok = false;
			const auto chatId = chatSpec.toLongLong(&ok);
			if (!ok || !chatId) {
				base::LogWriteMain(
					u"Control Server: Ignoring invalid chat access spec '%1'"_q
						.arg(rawSpec));
				continue;
			}
			_allowedChatSpecs.push_back({ accountSpec, chatId });
		}
	}

	connect(&_server, &QLocalServer::newConnection,
		this, &ControlServer::onNewConnection);

	_pollTimer.setInterval(50);
	connect(&_pollTimer, &QTimer::timeout,
		this, &ControlServer::pollTdLib);
}

ControlServer::~ControlServer() {
	stop();
}

bool ControlServer::start() {
	// Remove stale socket file.
	QFile::remove(_socketPath);

	if (!_server.listen(_socketPath)) {
		return false;
	}
	_pollTimer.start();
	return true;
}

void ControlServer::stop() {
	_pollTimer.stop();

	_activeExports.clear();

	for (auto &[socket, info] : _clients) {
		socket->disconnectFromServer();
	}
	_clients.clear();
	_accounts.clear();
	_clientIdToAccount.clear();
	_allowedFileIds.clear();
	_allowedUserIds.clear();
	_server.close();

	QFile::remove(_socketPath);
}

void ControlServer::setDomain(not_null<Main::Domain*> domain) {
	_domain = domain;
}

bool ControlServer::accountSpecMatches(
		int accountIndex,
		const QString &spec) const {
	const auto trimmed = spec.trimmed();
	if (trimmed == u"*"_q) {
		return true;
	}
	auto ok = false;
	const auto index = trimmed.toInt(&ok);
	if (ok && index == accountIndex) {
		return true;
	}
	const auto it = _accounts.find(accountIndex);
	return it != _accounts.end()
		&& !it->second.info.username.isEmpty()
		&& it->second.info.username.compare(
			trimmed,
			Qt::CaseInsensitive) == 0;
}

bool ControlServer::isAccountAllowed(int accountIndex) const {
	for (const auto &spec : _allowedAccountSpecs) {
		if (accountSpecMatches(accountIndex, spec)) {
			return true;
		}
	}
	return false;
}

bool ControlServer::isChatRestricted(int accountIndex) const {
	if (!_chatFilteringEnabled) {
		return false;
	}
	for (const auto &spec : _allowedChatSpecs) {
		if (!spec.chatId
			&& accountSpecMatches(accountIndex, spec.accountSpec)) {
			return false;
		}
	}
	return true;
}

bool ControlServer::isChatAllowed(
		int accountIndex,
		qint64 chatId) const {
	if (!isChatRestricted(accountIndex)) {
		return true;
	}
	for (const auto &spec : _allowedChatSpecs) {
		if (spec.chatId == chatId
			&& accountSpecMatches(accountIndex, spec.accountSpec)) {
			return true;
		}
	}
	return false;
}

ControlServer::ChatAccess ControlServer::chatAccessForJson(
		int accountIndex,
		const QJsonValue &value) const {
	if (value.isArray()) {
		auto result = ChatAccess::None;
		for (const auto &entry : value.toArray()) {
			const auto nested = chatAccessForJson(accountIndex, entry);
			if (nested == ChatAccess::Denied) {
				return nested;
			} else if (nested == ChatAccess::Allowed) {
				result = nested;
			}
		}
		return result;
	} else if (!value.isObject()) {
		return ChatAccess::None;
	}

	const auto object = value.toObject();
	auto result = ChatAccess::None;
	const auto checkChatId = [&](const QJsonValue &candidate) {
		auto chatId = qint64();
		if (!ReadJsonInteger(candidate, chatId) || !chatId) {
			return ChatAccess::None;
		}
		return isChatAllowed(accountIndex, chatId)
			? ChatAccess::Allowed
			: ChatAccess::Denied;
	};
	const auto merge = [&](ChatAccess nested) {
		if (nested == ChatAccess::Denied) {
			return false;
		} else if (nested == ChatAccess::Allowed) {
			result = nested;
		}
		return true;
	};

	const auto type = object.value("@type").toString();
	if (type == u"chat"_q
		&& !merge(checkChatId(object.value("id")))) {
		return ChatAccess::Denied;
	}
	if (type == u"basicGroup"_q || type == u"supergroup"_q) {
		auto groupId = qint64();
		if (ReadJsonInteger(object.value("id"), groupId) && groupId > 0) {
			const auto chatId = (type == u"basicGroup"_q)
				? -groupId
				: (kTdLibZeroChannelId - groupId);
			if (!merge(isChatAllowed(accountIndex, chatId)
					? ChatAccess::Allowed
					: ChatAccess::Denied)) {
				return ChatAccess::Denied;
			}
		}
	}
	const auto idIsChat = ranges::contains(std::array{
		u"basicGroup"_q,
		u"chat"_q,
		u"supergroup"_q,
	}, type);
	for (auto i = object.begin(), end = object.end(); i != end; ++i) {
		if (i.key() == u"@extra"_q) {
			continue;
		}
		if (IsChatIdField(i.key())) {
			if (!merge(checkChatId(i.value()))) {
				return ChatAccess::Denied;
			}
			continue;
		} else if (IsChatIdsField(i.key())) {
			for (const auto &chatId : i.value().toArray()) {
				if (!merge(checkChatId(chatId))) {
					return ChatAccess::Denied;
				}
			}
			continue;
		} else if (i.key() == u"id"_q && idIsChat) {
			continue;
		}
		if (!merge(chatAccessForJson(accountIndex, i.value()))) {
			return ChatAccess::Denied;
		}
	}
	return result;
}

bool ControlServer::isFileAllowed(int accountIndex, int fileId) const {
	const auto it = _allowedFileIds.find(accountIndex);
	return it != _allowedFileIds.end() && it->second.contains(fileId);
}

bool ControlServer::isUserAllowed(
		int accountIndex,
		qint64 userId) const {
	if (isChatAllowed(accountIndex, userId)) {
		return true;
	}
	const auto it = _allowedUserIds.find(accountIndex);
	return it != _allowedUserIds.end() && it->second.contains(userId);
}

bool ControlServer::isTdLibRequestAllowed(
		int accountIndex,
		const QJsonObject &payload) const {
	if (!isChatRestricted(accountIndex)) {
		return true;
	}
	const auto type = payload.value("@type").toString();
	if (chatAccessForJson(accountIndex, payload) == ChatAccess::Denied) {
		return false;
	}
	static const auto untargetedTypes = std::array{
		u"getAuthorizationState"_q,
		u"getChats"_q,
		u"loadChats"_q,
		u"searchChats"_q,
		u"searchChatsOnServer"_q,
		u"searchPublicChat"_q,
		u"searchPublicChats"_q,
	};
	if (ranges::contains(untargetedTypes, type)) {
		return true;
	}

	static const auto fileTypes = std::array{
		u"addFileToDownloads"_q,
		u"cancelDownloadFile"_q,
		u"deleteFile"_q,
		u"downloadFile"_q,
		u"getFile"_q,
		u"removeFileFromDownloads"_q,
	};
	for (const auto &allowed : fileTypes) {
		if (type == allowed) {
			return isFileAllowed(
				accountIndex,
				payload.value("file_id").toInt());
		}
	}

	static const auto userTypes = std::array{
		u"getUser"_q,
		u"getUserFullInfo"_q,
	};
	for (const auto &allowed : userTypes) {
		if (type == allowed) {
			auto userId = qint64();
			return ReadJsonInteger(payload.value("user_id"), userId)
				&& isUserAllowed(accountIndex, userId);
		}
	}

	static const auto basicGroupTypes = std::array{
		u"getBasicGroup"_q,
		u"getBasicGroupFullInfo"_q,
	};
	if (ranges::contains(basicGroupTypes, type)) {
		auto groupId = qint64();
		return ReadJsonInteger(payload.value("basic_group_id"), groupId)
			&& isChatAllowed(accountIndex, -groupId);
	}
	static const auto supergroupTypes = std::array{
		u"getSupergroup"_q,
		u"getSupergroupFullInfo"_q,
	};
	if (ranges::contains(supergroupTypes, type)) {
		auto groupId = qint64();
		return ReadJsonInteger(payload.value("supergroup_id"), groupId)
			&& isChatAllowed(
				accountIndex,
				kTdLibZeroChannelId - groupId);
	}
	if (!IsChatScopedRequestType(type)) {
		return false;
	}
	static const auto storyTypes = std::array{
		u"closeStory"_q,
		u"createStoryAlbum"_q,
		u"deleteStory"_q,
		u"editBusinessStory"_q,
		u"editStory"_q,
		u"editStoryCover"_q,
		u"getChatStoryInteractions"_q,
		u"getStory"_q,
		u"getStoryPublicForwards"_q,
		u"openStory"_q,
		u"reportStory"_q,
		u"searchPublicStoriesByTag"_q,
		u"setStoryReaction"_q,
		u"toggleStoryIsPostedToChatPage"_q,
	};
	const auto chatIdField = ranges::contains(storyTypes, type)
		? u"story_poster_chat_id"_q
		: (type == u"getPremiumGiveawayPaymentOptions"_q)
		? u"boosted_chat_id"_q
		: (type == u"shareChatWithBot"_q)
		? u"shared_chat_id"_q
		: u"chat_id"_q;
	auto chatId = qint64();
	return ReadJsonInteger(payload.value(chatIdField), chatId)
		&& chatId
		&& isChatAllowed(accountIndex, chatId);
}

void ControlServer::rememberAllowedFiles(
		int accountIndex,
		const QJsonValue &value) {
	if (value.isArray()) {
		for (const auto &entry : value.toArray()) {
			rememberAllowedFiles(accountIndex, entry);
		}
		return;
	} else if (!value.isObject()) {
		return;
	}

	const auto object = value.toObject();
	if (object.value("@type").toString() == u"file"_q) {
		const auto fileId = object.value("id").toInt();
		if (fileId > 0) {
			_allowedFileIds[accountIndex].emplace(fileId);
		}
	}
	for (auto i = object.begin(), end = object.end(); i != end; ++i) {
		if (i.key() == u"@extra"_q) {
			continue;
		}
		rememberAllowedFiles(accountIndex, i.value());
	}
}

void ControlServer::rememberAllowedUsers(
		int accountIndex,
		const QJsonValue &value) {
	if (value.isArray()) {
		for (const auto &entry : value.toArray()) {
			rememberAllowedUsers(accountIndex, entry);
		}
		return;
	} else if (!value.isObject()) {
		return;
	}

	const auto object = value.toObject();
	for (auto i = object.begin(), end = object.end(); i != end; ++i) {
		if (i.key() == u"@extra"_q) {
			continue;
		}
		if (i.key() == u"user_id"_q || i.key().endsWith(u"_user_id"_q)) {
			auto userId = qint64();
			if (ReadJsonInteger(i.value(), userId) && userId > 0) {
				_allowedUserIds[accountIndex].emplace(userId);
			}
		} else if (i.key() == u"user_ids"_q
			|| i.key().endsWith(u"_user_ids"_q)) {
			for (const auto &entry : i.value().toArray()) {
				auto userId = qint64();
				if (ReadJsonInteger(entry, userId) && userId > 0) {
					_allowedUserIds[accountIndex].emplace(userId);
				}
			}
		}
		rememberAllowedUsers(accountIndex, i.value());
	}
}

bool ControlServer::filterTdLibPayload(
		int accountIndex,
		QJsonObject &payload) {
	if (!isChatRestricted(accountIndex)) {
		return true;
	}

	const auto type = payload.value("@type").toString();
	if (type == u"chats"_q) {
		auto filtered = QJsonArray();
		for (const auto &value : payload.value("chat_ids").toArray()) {
			auto chatId = qint64();
			if (ReadJsonInteger(value, chatId)
				&& isChatAllowed(accountIndex, chatId)) {
				filtered.append(value);
			}
		}
		payload["chat_ids"] = filtered;
		payload["total_count"] = filtered.size();
	}

	const auto access = chatAccessForJson(accountIndex, payload);
	if (access == ChatAccess::Denied) {
		const auto extra = payload.value("@extra");
		if (extra.isUndefined()) {
			return false;
		}
		payload = QJsonObject{
			{ "@type", "error" },
			{ "code", 403 },
			{ "message", "Chat not allowed" },
			{ "@extra", extra },
		};
		return true;
	}

	if (type == u"file"_q) {
		if (!isFileAllowed(accountIndex, payload.value("id").toInt())) {
			return false;
		}
	} else if (type == u"updateFile"_q) {
		const auto fileId = payload.value("file").toObject().value("id").toInt();
		if (!isFileAllowed(accountIndex, fileId)) {
			return false;
		}
	} else if (type == u"updateUser"_q) {
		auto userId = qint64();
		if (!ReadJsonInteger(
				payload.value("user").toObject().value("id"),
				userId)
			|| !isUserAllowed(accountIndex, userId)) {
			return false;
		}
	} else if (type == u"updateUserFullInfo"_q) {
		auto userId = qint64();
		if (!ReadJsonInteger(payload.value("user_id"), userId)
			|| !isUserAllowed(accountIndex, userId)) {
			return false;
		}
	} else if (type.startsWith(u"update"_q)
		&& access == ChatAccess::None) {
		return false;
	}

	const auto allowedUnscopedUpdate = ranges::contains(std::array{
		u"updateFile"_q,
		u"updateUser"_q,
		u"updateUserFullInfo"_q,
	}, type);
	if (access == ChatAccess::None
		&& !payload.contains("@extra")
		&& !allowedUnscopedUpdate
		&& type != u"ok"_q
		&& type != u"error"_q
		&& type != u"chats"_q) {
		return false;
	}

	rememberAllowedFiles(accountIndex, payload);
	rememberAllowedUsers(accountIndex, payload);
	return true;
}

void ControlServer::recomputeDefaultAccount() {
	for (const auto &[accountIndex, entry] : _accounts) {
		if (isAccountAllowed(accountIndex)) {
			_defaultAccount = accountIndex;
			return;
		}
	}
	_defaultAccount = 0;
}

void ControlServer::addAccountClient(
		int accountIndex,
		int tdlibClientId) {
	addAccountClient(accountIndex, tdlibClientId, AccountInfo());
}

void ControlServer::addAccountClient(
		int accountIndex,
		int tdlibClientId,
		const AccountInfo &info) {
	_accounts[accountIndex] = AccountEntry{ tdlibClientId, info };
	_clientIdToAccount[tdlibClientId] = accountIndex;
	recomputeDefaultAccount();
}

void ControlServer::updateAccountInfo(
		int accountIndex,
		const AccountInfo &info) {
	auto it = _accounts.find(accountIndex);
	if (it != _accounts.end()) {
		it->second.info = info;
		recomputeDefaultAccount();
	}
}

void ControlServer::removeAccountClient(int accountIndex) {
	auto it = _accounts.find(accountIndex);
	if (it != _accounts.end()) {
		_clientIdToAccount.erase(it->second.clientId);
		_accounts.erase(it);
	}
	_activeExports.erase(accountIndex);
	_allowedFileIds.erase(accountIndex);
	_allowedUserIds.erase(accountIndex);
	recomputeDefaultAccount();
}

void ControlServer::onNewConnection() {
	while (auto *socket = _server.nextPendingConnection()) {
		_clients[socket];  // create empty SocketInfo

		connect(socket, &QLocalSocket::readyRead,
			this, [this, socket] { onClientReadyRead(socket); });
		connect(socket, &QLocalSocket::disconnected,
			this, [this, socket] { onClientDisconnected(socket); });
	}
}

void ControlServer::onClientReadyRead(QLocalSocket *socket) {
	auto it = _clients.find(socket);
	if (it == _clients.end()) {
		return;
	}
	auto &info = it->second;

	info.readBuffer.append(socket->readAll());

	while (true) {
		const auto nlPos = info.readBuffer.indexOf('\n');
		if (nlPos < 0) {
			break;
		}
		auto line = info.readBuffer.left(nlPos).trimmed();
		info.readBuffer.remove(0, nlPos + 1);

		if (!line.isEmpty()) {
			processLine(socket, line);
		}
	}
}

void ControlServer::processLine(
		QLocalSocket *socket,
		const QByteArray &line) {
	auto doc = QJsonDocument::fromJson(line);
	if (doc.isNull() || !doc.isObject()) {
		sendJson(socket, QJsonObject{
			{ "type", "error" },
			{ "code", 400 },
			{ "message", "Invalid JSON" },
		});
		return;
	}

	auto obj = doc.object();
	const auto type = obj.value("type").toString();

	// Resolve account index and enforce access control.
	// For tdlib/mtp the account is at the top level; for tdesktop
	// export commands it's inside the payload. Account-less tdesktop
	// commands (ping, listAccounts) skip this check.
	auto accountIndex = _defaultAccount;
	auto needsAccountCheck = false;

	if (type == u"tdlib"_q || type == u"mtp"_q) {
		needsAccountCheck = true;
		if (obj.contains("account")) {
			accountIndex = obj.value("account").toInt(_defaultAccount);
		} else {
			obj["account"] = _defaultAccount;
		}
	} else if (type == u"tdesktop"_q) {
		const auto payload = obj.value("payload").toObject();
		const auto command = payload.value("command").toString();
		if (command == u"export"_q
			|| command == u"cancelExport"_q
			|| command == u"sendFile"_q) {
			needsAccountCheck = true;
			accountIndex = payload.value("account").toInt(_defaultAccount);
		}
	}

	const auto sendForbidden = [&](const QString &message) {
		auto errorPayload = QJsonObject{
			{ "@type", "error" },
			{ "code", 403 },
			{ "message", message },
		};
		const auto requestPayload = obj.value("payload").toObject();
		if (type == u"tdesktop"_q) {
			const auto extra = requestPayload.value("@extra");
			if (!extra.isUndefined()) {
				errorPayload["@extra"] = extra;
			}
			sendJson(socket, QJsonObject{
				{ "type", "tdesktop" },
				{ "payload", errorPayload },
			});
		} else {
			if (type == u"tdlib"_q) {
				const auto extra = requestPayload.value("@extra");
				if (!extra.isUndefined()) {
					errorPayload["@extra"] = extra;
				}
			}
			auto response = QJsonObject{
				{ "type", type },
				{ "account", accountIndex },
				{ "payload", errorPayload },
			};
			if (type == u"mtp"_q && obj.contains("@extra")) {
				response["@extra"] = obj.value("@extra");
			}
			sendJson(socket, response);
		}
	};

	if (needsAccountCheck && !isAccountAllowed(accountIndex)) {
		sendForbidden(u"Account %1 not allowed"_q.arg(accountIndex));
		return;
	}
	if (type == u"tdlib"_q
		&& !isTdLibRequestAllowed(
			accountIndex,
			obj.value("payload").toObject())) {
		sendForbidden(u"Chat access denied for account %1"_q.arg(
			accountIndex));
		return;
	}
	if (type == u"mtp"_q && isChatRestricted(accountIndex)) {
		sendForbidden(u"Raw MTP is disabled for chat-restricted account %1"_q
			.arg(accountIndex));
		return;
	}
	if (type == u"tdesktop"_q && isChatRestricted(accountIndex)) {
		const auto command = obj.value(
			"payload").toObject().value("command").toString();
		if (command == u"export"_q || command == u"cancelExport"_q) {
			sendForbidden(u"Export is disabled for chat-restricted account %1"_q
				.arg(accountIndex));
			return;
		}
	}

	if (type == u"tdlib"_q) {
		handleTdLibRequest(socket, obj);
	} else if (type == u"tdesktop"_q) {
		handleControlRequest(socket, obj.value("payload").toObject());
	} else if (type == u"mtp"_q) {
		handleMtpRequest(socket, obj);
	} else {
		sendJson(socket, QJsonObject{
			{ "type", "error" },
			{ "code", 400 },
			{ "message",
				u"Unknown type: \"%1\". Expected \"tdlib\", \"tdesktop\", or \"mtp\"."_q
					.arg(type) },
		});
	}
}

void ControlServer::handleTdLibRequest(
		QLocalSocket *socket,
		const QJsonObject &obj) {
	const auto accountIndex = obj.value("account").toInt(0);
	const auto payload = obj.value("payload").toObject();

	auto it = _accounts.find(accountIndex);
	if (it == _accounts.end()) {
		sendJson(socket, QJsonObject{
			{ "type", "tdlib" },
			{ "account", accountIndex },
			{ "payload", QJsonObject{
				{ "@type", "error" },
				{ "code", 404 },
				{ "message",
					u"No TDLib client for account %1"_q.arg(accountIndex) },
			}},
		});
		return;
	}

	const auto json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
	td_send(it->second.clientId, json.constData());
}

void ControlServer::handleMtpRequest(
		QLocalSocket *socket,
		const QJsonObject &obj) {
	const auto accountIndex = obj.value("account").toInt(0);
	const auto payloadValue = obj.value("payload");
	const auto extra = obj.value("@extra");

	if (!payloadValue.isObject()) {
		sendJson(socket, QJsonObject{
			{ "type", "mtp" },
			{ "account", accountIndex },
			{ "payload", QJsonObject{
				{ "@type", "error" },
				{ "code", 400 },
				{ "message", "Missing or invalid 'payload' object" },
			}},
		});
		return;
	}

	if (!_domain) {
		auto response = QJsonObject{
			{ "type", "mtp" },
			{ "account", accountIndex },
			{ "payload", QJsonObject{
				{ "@type", "error" },
				{ "code", 500 },
				{ "message", "Domain not available" },
			}},
		};
		if (!extra.isUndefined()) {
			response["@extra"] = extra;
		}
		sendJson(socket, response);
		return;
	}

	// Find the account.
	Main::Account *account = nullptr;
	for (const auto &[idx, acc] : _domain->accounts()) {
		if (idx == accountIndex) {
			account = acc.get();
			break;
		}
	}
	if (!account || !account->sessionExists()) {
		auto response = QJsonObject{
			{ "type", "mtp" },
			{ "account", accountIndex },
			{ "payload", QJsonObject{
				{ "@type", "error" },
				{ "code", 404 },
				{ "message",
					u"No active session for account %1"_q
						.arg(accountIndex) },
			}},
		};
		if (!extra.isUndefined()) {
			response["@extra"] = extra;
		}
		sendJson(socket, response);
		return;
	}

	// Parse the JSON payload into a telegram_api::Function.
	auto payloadJson = QJsonDocument(
		payloadValue.toObject()).toJson(QJsonDocument::Compact);
	auto r_json_value = td::json_decode(
		td::MutableSlice(payloadJson.data(), payloadJson.size()));
	if (r_json_value.is_error()) {
		auto response = QJsonObject{
			{ "type", "mtp" },
			{ "account", accountIndex },
			{ "payload", QJsonObject{
				{ "@type", "error" },
				{ "code", 400 },
				{ "message", u"JSON decode error: %1"_q.arg(
					QString::fromStdString(
						r_json_value.error().message().str())) },
			}},
		};
		if (!extra.isUndefined()) {
			response["@extra"] = extra;
		}
		sendJson(socket, response);
		return;
	}

	td::telegram_api::object_ptr<td::telegram_api::Function> func;
	auto status = td::telegram_api::from_json(
		func, r_json_value.move_as_ok());
	if (status.is_error() || !func) {
		auto response = QJsonObject{
			{ "type", "mtp" },
			{ "account", accountIndex },
			{ "payload", QJsonObject{
				{ "@type", "error" },
				{ "code", 400 },
				{ "message", u"Failed to parse telegram_api function: %1"_q
					.arg(QString::fromStdString(
						status.is_error()
							? status.error().message().str()
							: "null result")) },
			}},
		};
		if (!extra.isUndefined()) {
			response["@extra"] = extra;
		}
		sendJson(socket, response);
		return;
	}

	// Serialize the Function to TL binary.
	td::TlStorerCalcLength calcLength;
	func->store(calcLength);
	const auto tlSize = calcLength.get_length();

	auto tlBuffer = std::vector<unsigned char>(tlSize);
	td::TlStorerUnsafe storer(tlBuffer.data());
	func->store(storer);

	// Build a SerializedRequest from the raw TL bytes.
	const auto bodySizeInInts = static_cast<uint32>((tlSize + 3) / 4);
	auto serialized = MTP::details::SerializedRequest::Prepare(
		bodySizeInInts);
	auto &buf = *serialized;

	const auto *srcInts = reinterpret_cast<const mtpPrime *>(
		tlBuffer.data());
	for (uint32 i = 0; i < bodySizeInInts; ++i) {
		if ((i + 1) * sizeof(mtpPrime) <= tlSize) {
			buf.push_back(srcInts[i]);
		} else {
			mtpPrime last = 0;
			memcpy(&last, tlBuffer.data() + i * sizeof(mtpPrime),
				tlSize - i * sizeof(mtpPrime));
			buf.push_back(last);
		}
	}
	serialized->needsLayer = true;

	const auto requestId = MTP::details::GetNextRequestId();
	serialized->requestId = requestId;

	// Prevent socket and extra from dangling in the callbacks
	// by capturing copies/weak references.
	auto socketPtr = QPointer<QLocalSocket>(socket);
	auto extraCopy = extra;

	auto done = [this, socketPtr, accountIndex, extraCopy](
			const MTP::Response &response) -> bool {
		if (!socketPtr) {
			return true;
		}

		const auto &reply = response.reply;
		if (reply.isEmpty()) {
			auto resp = QJsonObject{
				{ "type", "mtp" },
				{ "account", accountIndex },
				{ "payload", QJsonObject{
					{ "@type", "error" },
					{ "code", 500 },
					{ "message", "Empty response" },
				}},
			};
			if (!extraCopy.isUndefined()) {
				resp["@extra"] = extraCopy;
			}
			sendJson(socketPtr.data(), resp);
			return true;
		}

		// Parse the raw MTP response into a telegram_api::Object.
		const auto *data = reinterpret_cast<const char *>(
			reply.constData());
		const auto size = static_cast<size_t>(
			reply.size() * sizeof(mtpPrime));

		// Check for Bool responses (boolTrue/boolFalse) which are
		// not part of the Object hierarchy.
		constexpr int32_t kBoolFalse = 0xbc799737;
		constexpr int32_t kBoolTrue = 0x997275b5;
		if (size >= sizeof(int32_t)) {
			int32_t constructorId = 0;
			memcpy(&constructorId, data, sizeof(int32_t));
			if (constructorId == kBoolTrue
				|| constructorId == kBoolFalse) {
				auto resp = QJsonObject{
					{ "type", "mtp" },
					{ "account", accountIndex },
					{ "payload", constructorId == kBoolTrue },
				};
				if (!extraCopy.isUndefined()) {
					resp["@extra"] = extraCopy;
				}
				sendJson(socketPtr.data(), resp);
				return true;
			}
		}

		auto bufSlice = td::BufferSlice(td::Slice(data, size));
		td::TlBufferParser parser(&bufSlice);
		auto resultObj = td::telegram_api::Object::fetch(parser);
		parser.fetch_end();

		if (parser.get_error() || !resultObj) {
			auto resp = QJsonObject{
				{ "type", "mtp" },
				{ "account", accountIndex },
				{ "payload", QJsonObject{
					{ "@type", "error" },
					{ "code", 500 },
					{ "message", u"Failed to parse response: %1"_q.arg(
						parser.get_error()
							? QString::fromUtf8(parser.get_error())
							: u"null"_q) },
				}},
			};
			if (!extraCopy.isUndefined()) {
				resp["@extra"] = extraCopy;
			}
			sendJson(socketPtr.data(), resp);
			return true;
		}

		// Serialize the result object to JSON.
		auto jsonStr = td::json_encode<std::string>(
			td::ToJson(*resultObj));
		auto jsonDoc = QJsonDocument::fromJson(
			QByteArray::fromRawData(jsonStr.data(), jsonStr.size()));

		auto resp = QJsonObject{
			{ "type", "mtp" },
			{ "account", accountIndex },
			{ "payload", jsonDoc.object() },
		};
		if (!extraCopy.isUndefined()) {
			resp["@extra"] = extraCopy;
		}
		sendJson(socketPtr.data(), resp);
		return true;
	};

	auto fail = [this, socketPtr, accountIndex, extraCopy](
			const MTP::Error &error,
			const MTP::Response &) -> bool {
		if (!socketPtr) {
			return true;
		}
		auto resp = QJsonObject{
			{ "type", "mtp" },
			{ "account", accountIndex },
			{ "payload", QJsonObject{
				{ "@type", "error" },
				{ "code", error.code() },
				{ "message", error.type() },
			}},
		};
		if (!extraCopy.isUndefined()) {
			resp["@extra"] = extraCopy;
		}
		sendJson(socketPtr.data(), resp);
		return true;
	};

	account->mtp().sendSerialized(
		requestId,
		std::move(serialized),
		MTP::ResponseHandler{ std::move(done), std::move(fail) },
		0,  // shiftedDcId: 0 = main DC
		0,  // msCanWait
		0); // afterRequestId
}

void ControlServer::handleControlRequest(
		QLocalSocket *socket,
		const QJsonObject &payload) {
	const auto command = payload.value("command").toString();
	const auto extra = payload.value("@extra");

	if (command == u"ping"_q) {
		auto responsePayload = QJsonObject{
			{ "command", "pong" },
		};
		if (!extra.isUndefined()) {
			responsePayload["@extra"] = extra;
		}
		sendJson(socket, QJsonObject{
			{ "type", "tdesktop" },
			{ "payload", responsePayload },
		});
	} else if (command == u"listAccounts"_q) {
		auto accounts = QJsonArray();
		for (const auto &[accountIndex, entry] : _accounts) {
			if (!isAccountAllowed(accountIndex)) {
				continue;
			}
			auto obj = QJsonObject{
				{ "index", accountIndex },
			};
			const auto &info = entry.info;
			if (!info.firstName.isEmpty()) {
				obj["first_name"] = info.firstName;
			}
			if (!info.lastName.isEmpty()) {
				obj["last_name"] = info.lastName;
			}
			if (!info.username.isEmpty()) {
				obj["username"] = info.username;
			}
			if (!info.phone.isEmpty()) {
				obj["phone"] = info.phone;
			}
			if (info.userId) {
				obj["user_id"] = qint64(info.userId);
			}
			accounts.append(obj);
		}
		auto responsePayload = QJsonObject{
			{ "command", "listAccounts" },
			{ "accounts", accounts },
		};
		if (!extra.isUndefined()) {
			responsePayload["@extra"] = extra;
		}
		sendJson(socket, QJsonObject{
			{ "type", "tdesktop" },
			{ "payload", responsePayload },
		});
	} else if (command == u"export"_q) {
		handleExportCommand(socket, payload, extra);
	} else if (command == u"cancelExport"_q) {
		handleCancelExportCommand(socket, payload, extra);
	} else if (command == u"sendFile"_q) {
		handleSendFileCommand(socket, payload, extra);
	} else {
		auto responsePayload = QJsonObject{
			{ "error", u"Unknown command: \"%1\""_q.arg(command) },
		};
		if (!extra.isUndefined()) {
			responsePayload["@extra"] = extra;
		}
		sendJson(socket, QJsonObject{
			{ "type", "tdesktop" },
			{ "payload", responsePayload },
		});
	}
}

void ControlServer::handleExportCommand(
		QLocalSocket *socket,
		const QJsonObject &payload,
		const QJsonValue &extra) {
	if (!_domain) {
		auto responsePayload = QJsonObject{
			{ "command", "exportProgress" },
			{ "state", "error" },
			{ "message", "Domain not available" },
		};
		if (!extra.isUndefined()) {
			responsePayload["@extra"] = extra;
		}
		sendJson(socket, QJsonObject{
			{ "type", "tdesktop" },
			{ "payload", responsePayload },
		});
		return;
	}

	const auto accountIndex = payload.value("account").toInt(0);

	if (_activeExports.contains(accountIndex)) {
		auto responsePayload = QJsonObject{
			{ "command", "exportProgress" },
			{ "account", accountIndex },
			{ "state", "error" },
			{ "message", "Export already running for this account" },
		};
		if (!extra.isUndefined()) {
			responsePayload["@extra"] = extra;
		}
		sendJson(socket, QJsonObject{
			{ "type", "tdesktop" },
			{ "payload", responsePayload },
		});
		return;
	}

	// Find the account by index.
	Main::Account *account = nullptr;
	for (const auto &[idx, acc] : _domain->accounts()) {
		if (idx == accountIndex) {
			account = acc.get();
			break;
		}
	}
	if (!account) {
		auto responsePayload = QJsonObject{
			{ "command", "exportProgress" },
			{ "account", accountIndex },
			{ "state", "error" },
			{ "message", u"Account %1 not found"_q.arg(accountIndex) },
		};
		if (!extra.isUndefined()) {
			responsePayload["@extra"] = extra;
		}
		sendJson(socket, QJsonObject{
			{ "type", "tdesktop" },
			{ "payload", responsePayload },
		});
		return;
	}

	if (!account->sessionExists()) {
		auto responsePayload = QJsonObject{
			{ "command", "exportProgress" },
			{ "account", accountIndex },
			{ "state", "error" },
			{ "message", u"Account %1 has no active session"_q.arg(accountIndex) },
		};
		if (!extra.isUndefined()) {
			responsePayload["@extra"] = extra;
		}
		sendJson(socket, QJsonObject{
			{ "type", "tdesktop" },
			{ "payload", responsePayload },
		});
		return;
	}

	const auto settingsJson = payload.value("settings").toObject();
	if (settingsJson.value("path").toString().isEmpty()) {
		auto responsePayload = QJsonObject{
			{ "command", "exportProgress" },
			{ "account", accountIndex },
			{ "state", "error" },
			{ "message", "Missing required field: settings.path" },
		};
		if (!extra.isUndefined()) {
			responsePayload["@extra"] = extra;
		}
		sendJson(socket, QJsonObject{
			{ "type", "tdesktop" },
			{ "payload", responsePayload },
		});
		return;
	}

	auto settings = ParseExportSettings(settingsJson);

	auto &activeExport = _activeExports[accountIndex];
	activeExport.extra = extra;
	activeExport.controller = std::make_unique<Export::Controller>(
		&account->mtp(),
		MTP_inputPeerEmpty());

	auto *session = account->maybeSession();
	auto environment = PrepareEnvironment(session);

	activeExport.controller->state(
	) | rpl::on_next([this, accountIndex](Export::State state) {
		auto it = _activeExports.find(accountIndex);
		if (it == _activeExports.end()) {
			return;
		}
		const auto &activeExtra = it->second.extra;
		const auto isTerminal = v::is<Export::FinishedState>(state)
			|| v::is<Export::ApiErrorState>(state)
			|| v::is<Export::OutputErrorState>(state)
			|| v::is<Export::CancelledState>(state);

		broadcastJson(ExportStateToJson(accountIndex, state, activeExtra));

		if (isTerminal) {
			_activeExports.erase(accountIndex);
		}
	}, activeExport.lifetime);

	activeExport.controller->startExport(settings, environment);

	// Send acknowledgment.
	auto responsePayload = QJsonObject{
		{ "command", "exportStarted" },
		{ "account", accountIndex },
	};
	if (!extra.isUndefined()) {
		responsePayload["@extra"] = extra;
	}
	sendJson(socket, QJsonObject{
		{ "type", "tdesktop" },
		{ "payload", responsePayload },
	});
}

void ControlServer::handleCancelExportCommand(
		QLocalSocket *socket,
		const QJsonObject &payload,
		const QJsonValue &extra) {
	const auto accountIndex = payload.value("account").toInt(0);

	auto it = _activeExports.find(accountIndex);
	if (it == _activeExports.end()) {
		auto responsePayload = QJsonObject{
			{ "command", "exportProgress" },
			{ "account", accountIndex },
			{ "state", "error" },
			{ "message", u"No active export for account %1"_q.arg(accountIndex) },
		};
		if (!extra.isUndefined()) {
			responsePayload["@extra"] = extra;
		}
		sendJson(socket, QJsonObject{
			{ "type", "tdesktop" },
			{ "payload", responsePayload },
		});
		return;
	}

	it->second.controller->cancelExportFast();

	auto responsePayload = QJsonObject{
		{ "command", "exportCancelled" },
		{ "account", accountIndex },
	};
	if (!extra.isUndefined()) {
		responsePayload["@extra"] = extra;
	}
	sendJson(socket, QJsonObject{
		{ "type", "tdesktop" },
		{ "payload", responsePayload },
	});
}

void ControlServer::handleSendFileCommand(
		QLocalSocket *socket,
		const QJsonObject &payload,
		const QJsonValue &extra) {
	const auto accountIndex = payload.value("account").toInt(_defaultAccount);
	const auto respond = [&](QJsonObject responsePayload) {
		responsePayload["command"] = u"sendFile"_q;
		responsePayload["account"] = accountIndex;
		if (!extra.isUndefined()) {
			responsePayload["@extra"] = extra;
		}
		sendJson(socket, QJsonObject{
			{ "type", "tdesktop" },
			{ "payload", responsePayload },
		});
	};
	const auto fail = [&](int code, const QString &message) {
		respond(QJsonObject{
			{ "state", "error" },
			{ "code", code },
			{ "message", message },
		});
	};

	if (!_domain) {
		return fail(500, u"Domain not available"_q);
	}

	const auto path = payload.value("path").toString();
	if (path.isEmpty()) {
		return fail(400, u"Missing required field: path"_q);
	}
	if (!QFile::exists(path)) {
		return fail(400, u"File not found: %1"_q.arg(path));
	}

	// Resolve account -> active session.
	Main::Account *account = nullptr;
	for (const auto &[idx, acc] : _domain->accounts()) {
		if (idx == accountIndex) {
			account = acc.get();
			break;
		}
	}
	if (!account || !account->sessionExists()) {
		return fail(404,
			u"No active session for account %1"_q.arg(accountIndex));
	}
	const auto session = &account->session();

	// Resolve the target peer: explicit tdesktop peer_id, or a TDLib chat_id.
	auto peerId = PeerId(0);
	auto chatId = qint64();
	if (payload.contains("peer_id")) {
		peerId = PeerId(uint64(
			payload.value("peer_id").toVariant().toLongLong()));
		chatId = TdLibChatIdFromPeerId(peerId);
	} else if (payload.contains("chat_id")) {
		chatId = qint64(payload.value(
			"chat_id").toVariant().toLongLong());
		peerId = PeerIdFromTdLibChatId(chatId);
	}
	if (!peerId.value) {
		return fail(400,
			u"Missing or unsupported target (chat_id or peer_id)"_q);
	}
	if (!isChatAllowed(accountIndex, chatId)) {
		return fail(403, u"Chat %1 not allowed"_q.arg(chatId));
	}

	const auto peer = session->data().peerLoaded(peerId);
	if (!peer) {
		return fail(404,
			u"Peer %1 is not loaded for this account"_q.arg(peerId.value));
	}
	const auto history = session->data().history(peer);

	// Build the prepared list exactly the way the GUI does, then hand it to
	// the native ApiWrap::sendFiles path (Storage::Uploader drives the upload).
	auto list = Storage::PrepareMediaList(
		QStringList(path),
		st::sendMediaPreviewSize,
		session->premium());
	if (list.files.empty()) {
		return fail(400, u"Failed to prepare file for sending"_q);
	}
	const auto caption = payload.value("caption").toString();
	if (!caption.isEmpty()) {
		list.files.back().caption.text = caption;
	}
	const auto type = payload.value("as_photo").toBool(false)
		? SendMediaType::Photo
		: SendMediaType::File;

	session->api().sendFiles(
		std::move(list),
		type,
		nullptr,
		Api::SendAction(history));

	respond(QJsonObject{
		{ "state", "queued" },
		{ "peer_id", qint64(peerId.value) },
	});
}

void ControlServer::sendJson(
		QLocalSocket *socket,
		const QJsonObject &obj) {
	if (socket->state() != QLocalSocket::ConnectedState) {
		return;
	}
	socket->write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
	socket->write("\n");
}

void ControlServer::broadcastJson(const QJsonObject &obj) {
	for (auto &[socket, info] : _clients) {
		sendJson(socket, obj);
	}
}

void ControlServer::onClientDisconnected(QLocalSocket *socket) {
	_clients.erase(socket);
	socket->deleteLater();
}

void ControlServer::pollTdLib() {
	while (true) {
		const char *result = td_receive(0);
		if (!result) {
			break;
		}

		// Parse to extract @client_id.
		auto doc = QJsonDocument::fromJson(QByteArray(result));
		if (doc.isNull()) {
			continue;
		}

		auto obj = doc.object();
		const auto clientId = obj.value("@client_id").toInt();

		// Look up account index for this client ID.
		auto accountIt = _clientIdToAccount.find(clientId);
		if (accountIt == _clientIdToAccount.end()) {
			continue;
		}
		const auto accountIndex = accountIt->second;

		// One-shot chat-list warm-up: the first time this TDLib client
		// reaches authorizationStateReady, send loadChats so the dialog
		// folder structure is allocated before any client (e.g.
		// searchChats) touches it. Without this, an out-of-order
		// chat-list-touching request can hit a fatal LOG_CHECK in
		// TDLib's MessagesManager::set_dialog_order on a null folder.
		if (obj.value("@type").toString() == u"updateAuthorizationState"_q) {
			const auto state = obj.value(
				"authorization_state").toObject();
			if (state.value("@type").toString()
					== u"authorizationStateReady"_q) {
				auto entryIt = _accounts.find(accountIndex);
				if (entryIt != _accounts.end()
						&& !entryIt->second.warmed) {
					entryIt->second.warmed = true;
					const auto warmRequest = QJsonDocument(QJsonObject{
						{ "@type", "loadChats" },
						{ "chat_list", QJsonObject{
							{ "@type", "chatListMain" } } },
						{ "limit", 100 },
					}).toJson(QJsonDocument::Compact);
					td_send(clientId, warmRequest.constData());
				}
			}
		}

		// Remove @client_id, wrap in envelope with "type":"tdlib".
		obj.remove("@client_id");
		if (!filterTdLibPayload(accountIndex, obj)) {
			continue;
		}

		auto envelope = QJsonObject{
			{ "type", "tdlib" },
			{ "account", accountIndex },
			{ "payload", obj },
		};

		// Broadcast to all connected sockets.
		broadcastJson(envelope);
	}
}

} // namespace TdBridge
