/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "api/api_common.h"
#include "data/data_types.h"

namespace Main {
class Session;
} // namespace Main

namespace Api {

[[nodiscard]] bool NeedsForwardCopy(not_null<HistoryItem*> item);

void ForwardCopyItems(
	not_null<Main::Session*> session,
	HistoryItemsList items,
	SendAction action,
	bool withCaptions,
	FnMut<void()> done);

} // namespace Api
