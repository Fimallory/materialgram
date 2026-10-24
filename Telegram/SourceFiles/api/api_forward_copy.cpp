/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_forward_copy.h"

#include "api/api_sending.h"
#include "apiwrap.h"
#include "base/base_file_utilities.h"
#include "base/random.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_histories.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "storage/localimageloader.h"
#include "storage/storage_account.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/text/text_utilities.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

namespace Api {
namespace {

constexpr auto kWaitCheckInterval = crl::time(500);

[[nodiscard]] TextWithTags CaptionFor(
		not_null<HistoryItem*> item,
		bool withCaptions) {
	if (!withCaptions) {
		return {};
	}
	const auto &text = item->originalText();
	auto result = TextWithTags();
	result.text = text.text;
	result.tags = TextUtilities::ConvertEntitiesToTextTags(text.entities);
	return result;
}

class ForwardCopyer final
	: public std::enable_shared_from_this<ForwardCopyer> {
public:
	ForwardCopyer(
		not_null<Main::Session*> session,
		SendAction action,
		bool withCaptions,
		FnMut<void()> done);

	void add(not_null<HistoryItem*> item);
	void start();

private:
	enum class Step {
		Continue,
		Wait,
	};

	Step processItem();
	void processNext();
	void checkWaiting();
	void sendCurrentPhoto();
	void sendDocument(
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document,
		const QString &path);
	void sendSticker(
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document);
	void sendText(not_null<HistoryItem*> item);
	void clearWaiting();
	void finish();

	[[nodiscard]] QString tempPath(const QString &name) const;
	[[nodiscard]] bool fileComplete(
		const QString &path,
		int64 expectedSize) const;
	void cleanupOldTempFiles() const;

	const not_null<Main::Session*> _session;
	SendAction _action;
	const bool _withCaptions = false;
	FnMut<void()> _done;
	HistoryItemsList _items;
	int _index = 0;
	bool _processing = false;

	std::shared_ptr<Data::PhotoMedia> _photoView;
	PhotoData *_photo = nullptr;
	DocumentData *_document = nullptr;
	HistoryItem *_waitingItem = nullptr;
	QString _documentPath;
	base::Timer _waitTimer;
	rpl::lifetime _lifetime;

};

ForwardCopyer::ForwardCopyer(
	not_null<Main::Session*> session,
	SendAction action,
	bool withCaptions,
	FnMut<void()> done)
: _session(session)
, _action(action)
, _withCaptions(withCaptions)
, _done(std::move(done)) {
	_waitTimer.setCallback([=] {
		checkWaiting();
	});
}

void ForwardCopyer::add(not_null<HistoryItem*> item) {
	_items.push_back(item);
}

void ForwardCopyer::start() {
	cleanupOldTempFiles();
	const auto self = shared_from_this();
	_session->downloaderTaskFinished(
	) | rpl::on_next([self] {
		self->checkWaiting();
	}, _lifetime);

	processNext();
}

void ForwardCopyer::processNext() {
	if (_processing) {
		return;
	}
	_processing = true;
	while (_index < int(_items.size())) {
		if (processItem() == Step::Wait) {
			_processing = false;
			return;
		}
	}
	_processing = false;
	finish();
}

ForwardCopyer::Step ForwardCopyer::processItem() {
	const auto item = _items[_index];
	if (!item->isRegular()) {
		++_index;
		return Step::Continue;
	}
	const auto media = item->media();
	const auto photo = media ? media->photo() : nullptr;
	const auto document = media ? media->document() : nullptr;
	if (photo) {
		const auto view = photo->createMediaView();
		view->wanted(Data::PhotoSize::Large, Data::FileOrigin(item->fullId()));
		if (!view->loaded()) {
			if (!photo->loading(Data::PhotoSize::Large)) {
				++_index;
				return Step::Continue;
			}
			_photoView = view;
			_photo = photo;
			_waitingItem = item;
			_waitTimer.callEach(kWaitCheckInterval);
			return Step::Wait;
		}
		_photoView = view;
		_photo = photo;
		_waitingItem = item;
		sendCurrentPhoto();
		clearWaiting();
		++_index;
	} else if (document && document->sticker()) {
		sendSticker(item, document);
		++_index;
	} else if (document) {
		const auto existing = document->filepath(true);
		if (fileComplete(existing, document->size)) {
			sendDocument(item, document, existing);
			++_index;
		} else {
			const auto name = base::FileNameFromUserString(
				document->filename());
			const auto path = tempPath(name.isEmpty()
				? u"file"_q
				: name);
			_document = document;
			_documentPath = path;
			_waitingItem = item;
			document->save(Data::FileOrigin(item->fullId()), path);
			if (_document != document) {
				return Step::Continue;
			} else if (document->loading()) {
				_waitTimer.callEach(kWaitCheckInterval);
				return Step::Wait;
			}
			clearWaiting();
			if (fileComplete(path, document->size)) {
				sendDocument(item, document, path);
			}
			++_index;
		}
	} else if (!item->originalText().text.isEmpty()) {
		sendText(item);
		++_index;
	} else {
		++_index;
	}
	return Step::Continue;
}

void ForwardCopyer::checkWaiting() {
	auto advanced = false;
	if (_photo) {
		if (_photoView->loaded()) {
			sendCurrentPhoto();
			clearWaiting();
			++_index;
			advanced = true;
		} else if (!_photo->loading(Data::PhotoSize::Large)) {
			clearWaiting();
			++_index;
			advanced = true;
		}
	} else if (_document) {
		const auto document = _document;
		if (document->loading()) {
			return;
		}
		const auto item = _waitingItem;
		const auto path = _documentPath;
		clearWaiting();
		if (item && fileComplete(path, document->size)) {
			sendDocument(item, document, path);
		}
		++_index;
		advanced = true;
	}
	if (advanced && !_processing) {
		const auto self = shared_from_this();
		crl::on_main([self] {
			self->processNext();
		});
	}
}

void ForwardCopyer::sendCurrentPhoto() {
	const auto item = _waitingItem;
	if (!item || !_photoView) {
		return;
	}
	const auto video = !_photoView->videoContent(
		Data::PhotoSize::Large).isEmpty();
	const auto path = tempPath(video ? u"photo.mp4"_q : u"photo.jpg"_q);
	if (!_photoView->saveToFile(path)) {
		return;
	}
	auto file = Ui::PreparedFile(path);
	file.displayName = video ? u"photo.mp4"_q : u"photo.jpg"_q;
	file.caption = CaptionFor(item, _withCaptions);
	file.sendLargePhotos = true;

	auto list = Ui::PreparedList();
	list.files.push_back(std::move(file));
	_session->api().sendFiles(
		std::move(list),
		SendMediaType::Photo,
		nullptr,
		_action);
}

void ForwardCopyer::sendDocument(
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document,
		const QString &path) {
	auto file = Ui::PreparedFile(path);
	file.displayName = document->filename();
	file.caption = CaptionFor(item, _withCaptions);

	auto list = Ui::PreparedList();
	list.files.push_back(std::move(file));
	_session->api().sendFiles(
		std::move(list),
		SendMediaType::File,
		nullptr,
		_action);
}

void ForwardCopyer::sendSticker(
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document) {
	auto message = MessageToSend(_action);
	message.textWithTags = CaptionFor(item, _withCaptions);
	message.action.clearDraft = false;
	SendExistingDocument(std::move(message), document);
}

void ForwardCopyer::sendText(not_null<HistoryItem*> item) {
	auto message = MessageToSend(_action);
	message.textWithTags = CaptionFor(item, true);
	message.action.clearDraft = false;
	message.action.generateLocal = true;
	_session->api().sendMessage(std::move(message));
}

void ForwardCopyer::clearWaiting() {
	_waitTimer.cancel();
	_photoView = nullptr;
	_photo = nullptr;
	_document = nullptr;
	_documentPath = QString();
	_waitingItem = nullptr;
}

void ForwardCopyer::finish() {
	auto done = std::move(_done);
	_waitTimer.cancel();
	_lifetime.destroy();
	if (done) {
		done();
	}
}

QString ForwardCopyer::tempPath(const QString &name) const {
	auto dir = _session->local().tempDirectory();
	if (!dir.isEmpty() && !dir.endsWith('/')) {
		dir += '/';
	}
	QDir().mkpath(dir);
	return dir + u"forward_%1_%2"_q
		.arg(base::RandomValue<uint64>())
		.arg(name);
}

bool ForwardCopyer::fileComplete(
		const QString &path,
		int64 expectedSize) const {
	if (path.isEmpty()) {
		return false;
	}
	const auto info = QFileInfo(path);
	return info.exists()
		&& info.size() > 0
		&& (!expectedSize || info.size() == expectedSize);
}

void ForwardCopyer::cleanupOldTempFiles() const {
	const auto dir = _session->local().tempDirectory();
	if (dir.isEmpty()) {
		return;
	}
	const auto folder = QDir(dir);
	const auto list = folder.entryInfoList(
		{ u"forward_*"_q },
		QDir::Files | QDir::NoDotAndDotDot);
	const auto now = QDateTime::currentDateTime();
	for (const auto &info : list) {
		if (info.lastModified().addSecs(24 * 60 * 60) < now) {
			QFile::remove(info.absoluteFilePath());
		}
	}
}

} // namespace

bool NeedsForwardCopy(not_null<HistoryItem*> item) {
	if (!item->isRegular()) {
		return false;
	} else if (item->history()->peer->allowsForwarding()
		&& !item->forbidsForward()) {
		return false;
	}
	const auto media = item->media();
	return !media || media->allowsForward();
}

void ForwardCopyItems(
		not_null<Main::Session*> session,
		HistoryItemsList items,
		SendAction action,
		bool withCaptions,
		FnMut<void()> done) {
	auto copyer = std::make_shared<ForwardCopyer>(
		session,
		action,
		withCaptions,
		std::move(done));
	for (const auto &item : items) {
		copyer->add(item);
	}
	copyer->start();
}

} // namespace Api
