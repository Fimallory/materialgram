/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/facade.h"

#include "storage/localstorage.h"
#include "core/application.h"
#include "main/main_account.h"

#include <atomic>

namespace MTP {
namespace details {
namespace {

int PauseLevel = 0;
rpl::event_stream<> Unpaused;
std::atomic<bool> NetworkSendBlocked = false;

} // namespace

bool paused() {
	return PauseLevel > 0;
}

void pause() {
	++PauseLevel;
}

void unpause() {
	--PauseLevel;
	if (!PauseLevel) {
		Unpaused.fire({});
	}
}

rpl::producer<> unpaused() {
	return Unpaused.events();
}

void setNetworkSendBlocked(bool blocked) {
	NetworkSendBlocked.store(blocked, std::memory_order_release);
}

bool networkSendBlocked() {
	return NetworkSendBlocked.load(std::memory_order_acquire);
}

} // namespace details
} // namespace MTP
