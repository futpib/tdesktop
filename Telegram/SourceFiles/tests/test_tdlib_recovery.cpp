/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tdlib/tdlib_updates.h"

#include <td/telegram/td_api.h>
#include <td/telegram/logevent/LogEvent.h>
#include <td/db/BinlogKeyValue.h>
#include <QtCore/QTemporaryDir>

#include <chrono>
#include <cstring>
#include <ctime>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace {

namespace Api = td::telegram_api;
namespace Json = td::td_api;
using Clock = std::chrono::steady_clock;

void Require(bool condition, const char *message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << std::endl;
		throw std::runtime_error(message);
	}
}

struct Wire {
	std::string data;

	template <typename T>
	void add(const T &value) {
		data += td::serialize(value);
	}

	void vector(int count) {
		add(std::int32_t(0x1cb5c415));
		add(count);
	}

	void user(std::int64_t id, bool self) {
		add(Api::user::ID);
		add(int(3 | (self ? (1 << 10) : 0)));
		add(0);
		add(id);
		add(std::int64_t(123));
		add(std::string(self ? "Receiver" : "Sender"));
	}

	void state(int pts, int date, int seq) {
		add(Api::updates_state::ID);
		add(pts);
		add(0);
		add(date);
		add(seq);
		add(0);
	}
};

struct Harness {
	std::mutex mutex;
	std::deque<td::ExternalQuery> queries;
	td::ClientManager manager;
	int client = 0;
	int date = int(std::time(nullptr));
	int differences = 0;
	int lastPts = 0;
	int delivered = 0;
	bool ready = false;
	bool closed = false;
	bool stateSent = false;
	bool missing = false;
	bool hold = false;
	std::uint64_t held = 0;

	void complete(std::uint64_t id, Wire reply) {
		auto result = td::ExternalQueryResult();
		result.is_ok = true;
		result.ok_data = std::move(reply.data);
		td::complete_external_query(client, id, std::move(result));
	}

	void difference(std::uint64_t id) {
		auto reply = Wire();
		if (missing) {
			reply.add(Api::updates_difference::ID);
			reply.vector(1);
			reply.add(Api::message::ID);
			reply.add(1 << 8);
			reply.add(0);
			reply.add(42);
			reply.add(Api::peerUser::ID);
			reply.add(std::int64_t(2));
			reply.add(Api::peerUser::ID);
			reply.add(std::int64_t(2));
			reply.add(date);
			reply.add(std::string("sent while disconnected"));
			reply.vector(0);
			reply.vector(0);
			reply.vector(0);
			reply.vector(2);
			reply.user(1, true);
			reply.user(2, false);
			reply.state(101, date, 2);
			missing = false;
		} else {
			reply.add(Api::updates_differenceEmpty::ID);
			reply.add(date);
			reply.add(lastPts == 101 ? 2 : 1);
		}
		complete(id, std::move(reply));
	}

	void step() {
		auto pending = std::deque<td::ExternalQuery>();
		{
			const auto lock = std::lock_guard(mutex);
			pending.swap(queries);
		}
		for (const auto &query : pending) {
			Require(!query.gzip && query.query.size() >= 4, "invalid query");
			auto ctor = std::int32_t(0);
			std::memcpy(&ctor, query.query.data(), sizeof(ctor));
			auto reply = Wire();
			if (ctor == Api::users_getUsers::ID) {
				reply.vector(1);
				reply.user(1, true);
			} else if (ctor == Api::updates_getState::ID) {
				reply.state(100, date, 1);
				stateSent = true;
			} else if (ctor == Api::messages_getDialogs::ID) {
				reply.add(Api::messages_dialogs::ID);
				for (auto i = 0; i != 4; ++i) {
					reply.vector(0);
				}
			} else if (ctor == Api::updates_getDifference::ID) {
				Require(query.query.size() >= 12, "truncated getDifference");
				std::memcpy(&lastPts, query.query.data() + 8, sizeof(lastPts));
				++differences;
				if (hold) {
					held = query.id;
				} else {
					difference(query.id);
				}
				continue;
			} else {
				auto result = td::ExternalQueryResult();
				result.error_code = 400;
				result.error_message = "TEST_UNSUPPORTED";
				td::complete_external_query(client, query.id, std::move(result));
				continue;
			}
			complete(query.id, std::move(reply));
		}
		auto response = manager.receive(0.01);
		if (!response.object) {
			return;
		}
		const auto type = response.object->get_id();
		if (type == Json::error::ID && response.request_id) {
			const auto error = static_cast<Json::error*>(response.object.get());
			throw std::runtime_error(error->message_);
		} else if (type == Json::updateAuthorizationState::ID) {
			const auto state = static_cast<Json::updateAuthorizationState*>(
				response.object.get());
			ready = state->authorization_state_->get_id()
				== Json::authorizationStateReady::ID;
			closed = state->authorization_state_->get_id()
				== Json::authorizationStateClosed::ID;
		} else if (type == Json::updateNewMessage::ID) {
			const auto update = static_cast<Json::updateNewMessage*>(
				response.object.get());
			Require(update->message_->chat_id_ == 2, "wrong recovered chat");
			Require(!update->message_->is_outgoing_, "recovered message became outgoing");
			Require(update->message_->id_ == (std::int64_t(42) << 20), "wrong message");
			Require(update->message_->content_->get_id() == Json::messageText::ID,
				"wrong recovered content type");
			const auto text = static_cast<Json::messageText*>(
				update->message_->content_.get());
			Require(text->text_->text_ == "sent while disconnected", "wrong content");
			++delivered;
		}
	}

	template <typename Predicate>
	void until(Predicate predicate) {
		const auto deadline = Clock::now() + std::chrono::seconds(10);
		while (!predicate()) {
			if (Clock::now() >= deadline) {
				std::cerr << "ready=" << ready << " state=" << stateSent
					<< " differences=" << differences << " pts=" << lastPts
					<< " delivered=" << delivered << '\n';
				throw std::runtime_error("TDLib recovery timed out");
			}
			step();
		}
	}

	void drain() {
		const auto deadline = Clock::now() + std::chrono::milliseconds(300);
		while (Clock::now() < deadline) {
			step();
		}
	}

};

} // namespace

int main() {
	auto directory = QTemporaryDir();
	try {
		Require(directory.isValid(), "temporary directory unavailable");
		td::ClientManager::execute(Json::make_object<Json::setLogVerbosityLevel>(0));
		{
			auto database = td::BinlogKeyValue<td::Binlog>();
			database.init(directory.path().toStdString() + "/td.binlog",
				td::DbKey::empty(), -1,
				int(td::LogEvent::HandlerType::BinlogPmcMagic)).ensure();
			database.set("auth", "ok");
			database.set("my_id", "1");
			database.close();
		}
		auto test = Harness();
		td::set_external_dispatch([&](int client, td::ExternalQuery query) {
			const auto lock = std::lock_guard(test.mutex);
			test.queries.push_back(std::move(query));
		});
		test.client = test.manager.create_client_id();
		auto parameters = Json::make_object<Json::setTdlibParameters>();
		parameters->database_directory_ = directory.path().toStdString();
		parameters->api_id_ = 1;
		parameters->api_hash_ = "test";
		parameters->system_language_code_ = "en";
		parameters->device_model_ = "recovery-test";
		parameters->application_version_ = "1";
		test.manager.send(test.client, 1, std::move(parameters));
		test.until([&] { return test.ready && test.stateSent; });
		test.drain();
		const auto baseline = test.differences;
		test.missing = true;
		test.drain();
		Require(test.delivered == 0 && test.differences == baseline,
			"negative control recovered without a signal");
		test.hold = true;
		TdBridge::RequestDifference(test.client);
		test.until([&] { return test.held != 0; });
		Require(test.lastPts == 100, "recovery lost the pre-outage cursor");
		TdBridge::RequestDifference(test.client);
		test.drain();
		Require(test.differences == baseline + 1, "concurrent recovery was not coalesced");
		test.hold = false;
		test.difference(test.held);
		test.until([&] { return test.delivered == 1; });
		test.drain();
		TdBridge::RequestDifference(test.client);
		test.until([&] { return test.differences == baseline + 2; });
		test.drain();
		Require(test.lastPts == 101 && test.delivered == 1,
			"repeated recovery lost the cursor or duplicated delivery");
		test.manager.send(test.client, 2, Json::make_object<Json::close>());
		test.until([&] { return test.closed; });
		td::set_external_dispatch({});
		std::cout << "PASS: missed message recovered once; cursor and coalescing preserved\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
