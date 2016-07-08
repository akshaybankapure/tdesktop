/*
This file is part of Telegram Desktop,
the official desktop version of Telegram messaging app, see https://telegram.org

Telegram Desktop is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

It is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

In addition, as a special exception, the copyright holders give permission
to link the code of portions of this program with the OpenSSL library.

Full license: https://github.com/telegramdesktop/tdesktop/blob/master/LICENSE
Copyright (c) 2014-2016 John Preston, https://desktop.telegram.org
*/
#include "stdafx.h"
#include "lang.h"

#include "stickersetbox.h"
#include "mainwidget.h"
#include "mainwindow.h"
#include "settingswidget.h"
#include "boxes/confirmbox.h"
#include "apiwrap.h"
#include "localstorage.h"
#include "dialogs/dialogs_layout.h"

StickerSetInner::StickerSetInner(const MTPInputStickerSet &set) : TWidget()
, _input(set) {
	connect(App::wnd(), SIGNAL(imageLoaded()), this, SLOT(update()));
	switch (set.type()) {
	case mtpc_inputStickerSetID: _setId = set.c_inputStickerSetID().vid.v; _setAccess = set.c_inputStickerSetID().vaccess_hash.v; break;
	case mtpc_inputStickerSetShortName: _setShortName = qs(set.c_inputStickerSetShortName().vshort_name); break;
	}
	MTP::send(MTPmessages_GetStickerSet(_input), rpcDone(&StickerSetInner::gotSet), rpcFail(&StickerSetInner::failedSet));
	App::main()->updateStickers();

	_previewTimer.setSingleShot(true);
	connect(&_previewTimer, SIGNAL(timeout()), this, SLOT(onPreview()));
}

void StickerSetInner::gotSet(const MTPmessages_StickerSet &set) {
	_pack.clear();
	_emoji.clear();
	if (set.type() == mtpc_messages_stickerSet) {
		auto &d(set.c_messages_stickerSet());
		auto &v(d.vdocuments.c_vector().v);
		_pack.reserve(v.size());
		for (int i = 0, l = v.size(); i < l; ++i) {
			auto doc = App::feedDocument(v.at(i));
			if (!doc || !doc->sticker()) continue;

			_pack.push_back(doc);
		}
		auto &packs(d.vpacks.c_vector().v);
		for (int i = 0, l = packs.size(); i < l; ++i) {
			if (packs.at(i).type() != mtpc_stickerPack) continue;
			auto &pack(packs.at(i).c_stickerPack());
			if (auto e = emojiGetNoColor(emojiFromText(qs(pack.vemoticon)))) {
				auto &stickers(pack.vdocuments.c_vector().v);
				StickerPack p;
				p.reserve(stickers.size());
				for (int32 j = 0, c = stickers.size(); j < c; ++j) {
					DocumentData *doc = App::document(stickers.at(j).v);
					if (!doc || !doc->sticker()) continue;

					p.push_back(doc);
				}
				_emoji.insert(e, p);
			}
		}
		if (d.vset.type() == mtpc_stickerSet) {
			auto &s(d.vset.c_stickerSet());
			_setTitle = stickerSetTitle(s);
			_title = st::boxTitleFont->elided(_setTitle, width() - st::boxTitlePosition.x() - st::boxTitleHeight);
			_setShortName = qs(s.vshort_name);
			_setId = s.vid.v;
			_setAccess = s.vaccess_hash.v;
			_setCount = s.vcount.v;
			_setHash = s.vhash.v;
			_setFlags = s.vflags.v;
			auto &sets = Global::RefStickerSets();
			auto it = sets.find(_setId);
			if (it != sets.cend()) {
				auto clientFlags = it->flags & (MTPDstickerSet_ClientFlag::f_featured | MTPDstickerSet_ClientFlag::f_not_loaded | MTPDstickerSet_ClientFlag::f_unread);
				_setFlags |= clientFlags;
				it->flags = _setFlags;
				it->stickers = _pack;
				it->emoji = _emoji;
			}
		}
	}

	if (_pack.isEmpty()) {
		Ui::showLayer(new InformBox(lang(lng_stickers_not_found)));
	} else {
		int32 rows = _pack.size() / StickerPanPerRow + ((_pack.size() % StickerPanPerRow) ? 1 : 0);
		resize(st::stickersPadding.left() + StickerPanPerRow * st::stickersSize.width(), st::stickersPadding.top() + rows * st::stickersSize.height() + st::stickersPadding.bottom());
	}
	_loaded = true;

	emit updateButtons();
}

bool StickerSetInner::failedSet(const RPCError &error) {
	if (MTP::isDefaultHandledError(error)) return false;

	_loaded = true;

	Ui::showLayer(new InformBox(lang(lng_stickers_not_found)));

	return true;
}

void StickerSetInner::installDone(const MTPBool &result) {
	auto &sets = Global::RefStickerSets();

	_setFlags &= ~MTPDstickerSet::Flag::f_disabled;
	_setFlags |= MTPDstickerSet::Flag::f_installed;
	auto it = sets.find(_setId);
	if (it == sets.cend()) {
		it = sets.insert(_setId, Stickers::Set(_setId, _setAccess, _setTitle, _setShortName, _setCount, _setHash, _setFlags));
	} else {
		it->flags = _setFlags;
	}
	it->stickers = _pack;
	it->emoji = _emoji;

	auto &order = Global::RefStickerSetsOrder();
	int insertAtIndex = 0, currentIndex = order.indexOf(_setId);
	if (currentIndex != insertAtIndex) {
		if (currentIndex > 0) {
			order.removeAt(currentIndex);
		}
		order.insert(insertAtIndex, _setId);
	}

	auto custom = sets.find(Stickers::CustomSetId);
	if (custom != sets.cend()) {
		for_const (auto sticker, _pack) {
			int removeIndex = custom->stickers.indexOf(sticker);
			if (removeIndex >= 0) custom->stickers.removeAt(removeIndex);
		}
		if (custom->stickers.isEmpty()) {
			sets.erase(custom);
		}
	}
	Local::writeStickers();
	emit App::main()->stickersUpdated();
	emit installed(_setId);
}

bool StickerSetInner::installFailed(const RPCError &error) {
	if (MTP::isDefaultHandledError(error)) return false;

	if (error.type() == qstr("STICKERSETS_TOO_MUCH")) {
		Ui::showLayer(new InformBox(lang(lng_stickers_too_many_packs)));
	} else {
		Ui::showLayer(new InformBox(lang(lng_stickers_not_found)));
	}

	return true;
}

void StickerSetInner::mousePressEvent(QMouseEvent *e) {
	int32 index = stickerFromGlobalPos(e->globalPos());
	if (index >= 0 && index < _pack.size()) {
		_previewTimer.start(QApplication::startDragTime());
	}
}

void StickerSetInner::mouseMoveEvent(QMouseEvent *e) {
	if (_previewShown >= 0) {
		int32 index = stickerFromGlobalPos(e->globalPos());
		if (index >= 0 && index < _pack.size() && index != _previewShown) {
			_previewShown = index;
			Ui::showMediaPreview(_pack.at(_previewShown));
		}
	}
}

void StickerSetInner::mouseReleaseEvent(QMouseEvent *e) {
	_previewTimer.stop();
}

void StickerSetInner::onPreview() {
	int32 index = stickerFromGlobalPos(QCursor::pos());
	if (index >= 0 && index < _pack.size()) {
		_previewShown = index;
		Ui::showMediaPreview(_pack.at(_previewShown));
	}
}

int32 StickerSetInner::stickerFromGlobalPos(const QPoint &p) const {
	QPoint l(mapFromGlobal(p));
	if (rtl()) l.setX(width() - l.x());
	int32 row = (l.y() >= st::stickersPadding.top()) ? qFloor((l.y() - st::stickersPadding.top()) / st::stickersSize.height()) : -1;
	int32 col = (l.x() >= st::stickersPadding.left()) ? qFloor((l.x() - st::stickersPadding.left()) / st::stickersSize.width()) : -1;
	if (row >= 0 && col >= 0 && col < StickerPanPerRow) {
		int32 result = row * StickerPanPerRow + col;
		return (result < _pack.size()) ? result : -1;
	}
	return -1;
}

void StickerSetInner::paintEvent(QPaintEvent *e) {
	QRect r(e->rect());
	Painter p(this);

	if (_pack.isEmpty()) return;

	int32 rows = _pack.size() / StickerPanPerRow + ((_pack.size() % StickerPanPerRow) ? 1 : 0);
	int32 from = qFloor(e->rect().top() / st::stickersSize.height()), to = qFloor(e->rect().bottom() / st::stickersSize.height()) + 1;

	for (int32 i = from; i < to; ++i) {
		for (int32 j = 0; j < StickerPanPerRow; ++j) {
			int32 index = i * StickerPanPerRow + j;
			if (index >= _pack.size()) break;

			DocumentData *doc = _pack.at(index);
			QPoint pos(st::stickersPadding.left() + j * st::stickersSize.width(), st::stickersPadding.top() + i * st::stickersSize.height());

			bool goodThumb = !doc->thumb->isNull() && ((doc->thumb->width() >= 128) || (doc->thumb->height() >= 128));
			if (goodThumb) {
				doc->thumb->load();
			} else {
				if (doc->status == FileReady) {
					doc->automaticLoad(0);
				}
				if (doc->sticker()->img->isNull() && doc->loaded(DocumentData::FilePathResolveChecked)) {
					doc->sticker()->img = doc->data().isEmpty() ? ImagePtr(doc->filepath()) : ImagePtr(doc->data());
				}
			}

			float64 coef = qMin((st::stickersSize.width() - st::buttonRadius * 2) / float64(doc->dimensions.width()), (st::stickersSize.height() - st::buttonRadius * 2) / float64(doc->dimensions.height()));
			if (coef > 1) coef = 1;
			int32 w = qRound(coef * doc->dimensions.width()), h = qRound(coef * doc->dimensions.height());
			if (w < 1) w = 1;
			if (h < 1) h = 1;
			QPoint ppos = pos + QPoint((st::stickersSize.width() - w) / 2, (st::stickersSize.height() - h) / 2);
			if (goodThumb) {
				p.drawPixmapLeft(ppos, width(), doc->thumb->pix(w, h));
			} else if (!doc->sticker()->img->isNull()) {
				p.drawPixmapLeft(ppos, width(), doc->sticker()->img->pix(w, h));
			}
		}
	}
}

void StickerSetInner::setScrollBottom(int32 bottom) {
	if (bottom == _bottom) return;

	_bottom = bottom;
}

bool StickerSetInner::loaded() const {
	return _loaded && !_pack.isEmpty();
}

int32 StickerSetInner::notInstalled() const {
	if (!_loaded) return 0;
	auto it = Global::StickerSets().constFind(_setId);
	if (it == Global::StickerSets().cend() || !(it->flags & MTPDstickerSet::Flag::f_installed) || (it->flags & MTPDstickerSet::Flag::f_disabled)) return _pack.size();
	return 0;
}

bool StickerSetInner::official() const {
	return _loaded && _setShortName.isEmpty();
}

QString StickerSetInner::title() const {
	return _loaded ? (_pack.isEmpty() ? lang(lng_attach_failed) : _title) : lang(lng_contacts_loading);
}

QString StickerSetInner::shortName() const {
	return _setShortName;
}

void StickerSetInner::install() {
	if (_installRequest) return;
	_installRequest = MTP::send(MTPmessages_InstallStickerSet(_input, MTP_bool(false)), rpcDone(&StickerSetInner::installDone), rpcFail(&StickerSetInner::installFailed));
}

StickerSetInner::~StickerSetInner() {
}

StickerSetBox::StickerSetBox(const MTPInputStickerSet &set) : ScrollableBox(st::stickersScroll)
, _inner(set)
, _shadow(this)
, _add(this, lang(lng_stickers_add_pack), st::defaultBoxButton)
, _share(this, lang(lng_stickers_share_pack), st::defaultBoxButton)
, _cancel(this, lang(lng_cancel), st::cancelBoxButton)
, _done(this, lang(lng_about_done), st::defaultBoxButton) {
	setMaxHeight(st::stickersMaxHeight);
	connect(App::main(), SIGNAL(stickersUpdated()), this, SLOT(onStickersUpdated()));

	init(&_inner, st::boxButtonPadding.bottom() + _cancel.height() + st::boxButtonPadding.top());

	connect(&_add, SIGNAL(clicked()), this, SLOT(onAddStickers()));
	connect(&_share, SIGNAL(clicked()), this, SLOT(onShareStickers()));
	connect(&_cancel, SIGNAL(clicked()), this, SLOT(onClose()));
	connect(&_done, SIGNAL(clicked()), this, SLOT(onClose()));

	connect(&_inner, SIGNAL(updateButtons()), this, SLOT(onUpdateButtons()));
	connect(&_scroll, SIGNAL(scrolled()), this, SLOT(onScroll()));

	connect(&_inner, SIGNAL(installed(uint64)), this, SLOT(onInstalled(uint64)));

	onStickersUpdated();

	onScroll();

	prepare();
}

void StickerSetBox::onInstalled(uint64 setId) {
	emit installed(setId);
	onClose();
}

void StickerSetBox::onStickersUpdated() {
	showAll();
}

void StickerSetBox::onAddStickers() {
	_inner.install();
}

void StickerSetBox::onShareStickers() {
	QString url = qsl("https://telegram.me/addstickers/") + _inner.shortName();
	QApplication::clipboard()->setText(url);
	Ui::showLayer(new InformBox(lang(lng_stickers_copied)));
}

void StickerSetBox::onUpdateButtons() {
	if (!_cancel.isHidden() || !_done.isHidden()) {
		showAll();
	}
}

void StickerSetBox::onScroll() {
	_inner.setScrollBottom(_scroll.scrollTop() + _scroll.height());
}

void StickerSetBox::hideAll() {
	ScrollableBox::hideAll();
	_shadow.hide();
	_cancel.hide();
	_add.hide();
	_share.hide();
	_done.hide();
}

void StickerSetBox::showAll() {
	ScrollableBox::showAll();
	int32 cnt = _inner.notInstalled();
	if (_inner.loaded()) {
		_shadow.show();
		if (_inner.notInstalled()) {
			_add.show();
			_cancel.show();
			_share.hide();
			_done.hide();
		} else if (_inner.official()) {
			_add.hide();
			_share.hide();
			_cancel.hide();
			_done.show();
		} else {
			_share.show();
			_cancel.show();
			_add.hide();
			_done.hide();
		}
	} else {
		_shadow.hide();
		_add.hide();
		_share.hide();
		_cancel.show();
		_done.hide();
	}
	resizeEvent(0);
	update();
}

void StickerSetBox::paintEvent(QPaintEvent *e) {
	Painter p(this);
	if (paint(p)) return;

	paintTitle(p, _inner.title());
}

void StickerSetBox::resizeEvent(QResizeEvent *e) {
	ScrollableBox::resizeEvent(e);
	_inner.resize(width(), _inner.height());
	_shadow.setGeometry(0, height() - st::boxButtonPadding.bottom() - _cancel.height() - st::boxButtonPadding.top() - st::lineWidth, width(), st::lineWidth);
	_add.moveToRight(st::boxButtonPadding.right(), height() - st::boxButtonPadding.bottom() - _add.height());
	_share.moveToRight(st::boxButtonPadding.right(), _add.y());
	_done.moveToRight(st::boxButtonPadding.right(), _add.y());
	if (_add.isHidden() && _share.isHidden()) {
		_cancel.moveToRight(st::boxButtonPadding.right(), _add.y());
	} else if (_add.isHidden()) {
		_cancel.moveToRight(st::boxButtonPadding.right() + _share.width() + st::boxButtonPadding.left(), _add.y());
	} else {
		_cancel.moveToRight(st::boxButtonPadding.right() + _add.width() + st::boxButtonPadding.left(), _add.y());
	}
}

namespace internal {

StickersInner::StickersInner(StickersBox::Section section) : TWidget()
, _section(section)
, _rowHeight(st::contactsPadding.top() + st::contactsPhotoSize + st::contactsPadding.bottom())
, _a_shifting(animation(this, &StickersInner::step_shifting))
, _itemsTop(st::membersPadding.top())
, _removeWidth(st::normalFont->width(lang(lng_stickers_remove)))
, _returnWidth(st::normalFont->width(lang(lng_stickers_return)))
, _restoreWidth(st::normalFont->width(lang(lng_stickers_restore)))
, _addText(lang(lng_stickers_add).toUpper())
, _addWidth(st::defaultActiveButton.font->width(_addText))
, _aboveShadow(st::boxShadow) {
	connect(App::wnd(), SIGNAL(imageLoaded()), this, SLOT(update()));
	setMouseTracking(true);
}

void StickersInner::paintFeaturedButton(Painter &p) const {
	if (!_featuredHeight) return;

	if (_selected == -1) {
		p.fillRect(0, st::membersPadding.top(), width(), _featuredHeight, st::contactsBgOver);
	}
	p.setFont(st::stickersFeaturedFont);
	p.setPen(st::stickersFeaturedPen);
	p.drawTextLeft(st::stickersFeaturedPosition.x(), st::membersPadding.top() + st::stickersFeaturedPosition.y(), width(), lang(lng_stickers_featured));

	if (auto unread = Global::FeaturedStickerSetsUnreadCount()) {
		Dialogs::Layout::UnreadBadgeStyle unreadSt;
		unreadSt.sizeId = Dialogs::Layout::UnreadBadgeInStickersBox;
		unreadSt.size = st::stickersFeaturedBadgeSize;
		int unreadRight = width() - (st::contactsPadding.right() + st::contactsCheckPosition.x());
		if (rtl()) unreadRight = width() - unreadRight;
		int unreadTop = st::membersPadding.top() + (_featuredHeight - st::stickersFeaturedBadgeSize) / 2;
		Dialogs::Layout::paintUnreadCount(p, QString::number(unread), unreadRight, unreadTop, unreadSt);
	}
}

void StickersInner::paintEvent(QPaintEvent *e) {
	QRect r(e->rect());
	Painter p(this);

	_a_shifting.step();

	p.fillRect(r, st::white);
	p.setClipRect(r);

	paintFeaturedButton(p);

	if (_rows.isEmpty()) {
		p.setFont(st::noContactsFont);
		p.setPen(st::noContactsColor);
		p.drawText(QRect(0, _featuredHeight, width(), st::noContactsHeight), lang(lng_contacts_loading), style::al_center);
	} else {
		p.translate(0, _itemsTop);

		int32 yFrom = r.y() - _itemsTop, yTo = r.y() + r.height() - _itemsTop;
		int32 from = floorclamp(yFrom - _rowHeight, _rowHeight, 0, _rows.size());
		int32 to = ceilclamp(yTo + _rowHeight, _rowHeight, 0, _rows.size());
		p.translate(0, from * _rowHeight);
		for (int32 i = from; i < to; ++i) {
			if (i != _above) {
				paintRow(p, i);
			}
			p.translate(0, _rowHeight);
		}
		if (from <= _above && _above < to) {
			p.translate(0, (_above - to) * _rowHeight);
			paintRow(p, _above);
		}
	}
}

void StickersInner::paintRow(Painter &p, int32 index) {
	const StickerSetRow *s(_rows.at(index));

	int32 xadd = 0, yadd = s->yadd.current();
	if (xadd || yadd) p.translate(xadd, yadd);

	if (_section == Section::Installed) {
		bool removeSel = (index == _actionSel && (_actionDown < 0 || index == _actionDown));
		bool removeDown = removeSel && (index == _actionDown);

		p.setFont(removeSel ? st::linkOverFont : st::linkFont);
		if (removeDown) {
			p.setPen(st::btnDefLink.downColor);
		} else {
			p.setPen(st::btnDefLink.color);
		}
		int32 remWidth = s->disabled ? (s->official ? _restoreWidth : _returnWidth) : _removeWidth;
		QString remText = lang(s->disabled ? (s->official ? lng_stickers_restore : lng_stickers_return) : lng_stickers_remove);
		p.drawTextRight(st::contactsPadding.right() + st::contactsCheckPosition.x(), st::contactsPadding.top() + (st::contactsPhotoSize - st::normalFont->height) / 2, width(), remText, remWidth);

		if (index == _above) {
			float64 current = _aboveShadowFadeOpacity.current();
			if (_started >= 0) {
				float64 o = aboveShadowOpacity();
				if (o > current) {
					_aboveShadowFadeOpacity = anim::fvalue(o, o);
					current = o;
				}
			}
			p.setOpacity(current);
			QRect row(myrtlrect(_aboveShadow.getDimensions(st::boxShadowShift).left(), st::contactsPadding.top() / 2, width() - (st::contactsPadding.left() / 2) - _scrollbar - _aboveShadow.getDimensions(st::boxShadowShift).right(), _rowHeight - ((st::contactsPadding.top() + st::contactsPadding.bottom()) / 2)));
			_aboveShadow.paint(p, row, st::boxShadowShift);
			p.fillRect(row, st::white);
			p.setOpacity(1);
		}
	} else if (s->installed) {
		int addw = _addWidth - st::defaultActiveButton.width;
		int checkx = width() - (st::contactsPadding.right() + st::contactsCheckPosition.x() + (addw + st::stickersFeaturedInstalled.width()) / 2);
		int checky = st::contactsPadding.top() + (st::contactsPhotoSize - st::stickersFeaturedInstalled.height()) / 2;
		st::stickersFeaturedInstalled.paint(p, QPoint(checkx, checky), width());
	} else {
		int addw = _addWidth - st::defaultActiveButton.width;
		int addx = width() - st::contactsPadding.right() - st::contactsCheckPosition.x() - addw;
		int addy = st::contactsPadding.top() + (st::contactsPhotoSize - st::defaultActiveButton.height) / 2;
		QRect add(myrtlrect(addx, addy, addw, st::defaultActiveButton.height));

		App::roundRect(p, add, st::defaultActiveButton.textBgOver, ImageRoundRadius::Small);
		p.setFont(st::defaultActiveButton.font);
		p.setPen(st::defaultActiveButton.textFg);
		p.drawTextLeft(addx - st::defaultActiveButton.width / 2, addy + st::defaultActiveButton.textTop, width(), _addText, _addWidth);
	}

	if (s->disabled) p.setOpacity(st::stickersRowDisabledOpacity);
	if (s->sticker) {
		s->sticker->thumb->load();
		QPixmap pix(s->sticker->thumb->pix(s->pixw, s->pixh));
		p.drawPixmapLeft(st::contactsPadding.left() + (st::contactsPhotoSize - s->pixw) / 2, st::contactsPadding.top() + (st::contactsPhotoSize - s->pixh) / 2, width(), pix);
	}

	int namex = st::contactsPadding.left() + st::contactsPhotoSize + st::contactsPadding.left();
	int namey = st::contactsPadding.top() + st::contactsNameTop;
	int statusx = namex;
	int statusy = st::contactsPadding.top() + st::contactsStatusTop;

	if (s->unread) {
		p.setPen(Qt::NoPen);
		p.setBrush(st::stickersFeaturedUnreadBg);

		p.setRenderHint(QPainter::HighQualityAntialiasing, true);
		p.drawEllipse(rtlrect(namex, namey + st::stickersFeaturedUnreadTop, st::stickersFeaturedUnreadSize, st::stickersFeaturedUnreadSize, width()));
		p.setRenderHint(QPainter::HighQualityAntialiasing, false);
		namex += st::stickersFeaturedUnreadSize + st::stickersFeaturedUnreadSkip;
	}
	p.setFont(st::contactsNameFont);
	p.setPen(st::black);
	p.drawTextLeft(namex, namey, width(), s->title);

	p.setFont(st::contactsStatusFont);
	p.setPen(st::contactsStatusFg);
	p.drawTextLeft(statusx, statusy, width(), lng_stickers_count(lt_count, s->count));

	p.setOpacity(1);
	if (xadd || yadd) p.translate(-xadd, -yadd);
}

void StickersInner::mousePressEvent(QMouseEvent *e) {
	if (_saving) return;
	if (_dragging >= 0) mouseReleaseEvent(e);
	_mouse = e->globalPos();
	onUpdateSelected();

	_pressed = _selected;
	if (_actionSel >= 0) {
		_actionDown = _actionSel;
		update(0, _itemsTop + _actionSel * _rowHeight, width(), _rowHeight);
	} else if (_selected >= 0 && _section == Section::Installed) {
		_above = _dragging = _started = _selected;
		_dragStart = mapFromGlobal(_mouse);
	}
}

void StickersInner::mouseMoveEvent(QMouseEvent *e) {
	if (_saving) return;
	_mouse = e->globalPos();
	onUpdateSelected();
}

void StickersInner::onUpdateSelected() {
	if (_saving) return;
	QPoint local(mapFromGlobal(_mouse));
	if (_dragging >= 0) {
		int32 shift = 0;
		uint64 ms = getms();
		if (_dragStart.y() > local.y() && _dragging > 0) {
			shift = -floorclamp(_dragStart.y() - local.y() + (_rowHeight / 2), _rowHeight, 0, _dragging);
			for (int32 from = _dragging, to = _dragging + shift; from > to; --from) {
				qSwap(_rows[from], _rows[from - 1]);
				_rows.at(from)->yadd = anim::ivalue(_rows.at(from)->yadd.current() - _rowHeight, 0);
				_animStartTimes[from] = ms;
			}
		} else if (_dragStart.y() < local.y() && _dragging + 1 < _rows.size()) {
			shift = floorclamp(local.y() - _dragStart.y() + (_rowHeight / 2), _rowHeight, 0, _rows.size() - _dragging - 1);
			for (int32 from = _dragging, to = _dragging + shift; from < to; ++from) {
				qSwap(_rows[from], _rows[from + 1]);
				_rows.at(from)->yadd = anim::ivalue(_rows.at(from)->yadd.current() + _rowHeight, 0);
				_animStartTimes[from] = ms;
			}
		}
		if (shift) {
			_dragging += shift;
			_above = _dragging;
			_dragStart.setY(_dragStart.y() + shift * _rowHeight);
			if (!_a_shifting.animating()) {
				_a_shifting.start();
			}
		}
		_rows.at(_dragging)->yadd = anim::ivalue(local.y() - _dragStart.y(), local.y() - _dragStart.y());
		_animStartTimes[_dragging] = 0;
		_a_shifting.step(getms(), true);

		emit checkDraggingScroll(local.y());
	} else {
		bool in = rect().marginsRemoved(QMargins(0, _itemsTop, 0, st::membersPadding.bottom())).contains(local);
		int selected = -2;
		int actionSel = -1;
		if (in) {
			selected = floorclamp(local.y() - _itemsTop, _rowHeight, 0, _rows.size() - 1);

			if (_section == Section::Installed) {
				int remw = _rows.at(selected)->disabled ? (_rows.at(selected)->official ? _restoreWidth : _returnWidth) : _removeWidth;
				QRect rem(myrtlrect(width() - st::contactsPadding.right() - st::contactsCheckPosition.x() - remw, st::contactsPadding.top() + (st::contactsPhotoSize - st::normalFont->height) / 2, remw, st::normalFont->height));
				actionSel = rem.contains(local.x(), local.y() - _itemsTop - selected * _rowHeight) ? selected : -1;
			} else if (_rows.at(selected)->installed) {
				actionSel = -1;
			} else {
				int addw = _addWidth - st::defaultActiveButton.width;
				int addx = width() - st::contactsPadding.right() - st::contactsCheckPosition.x() - addw;
				int addy = st::contactsPadding.top() + (st::contactsPhotoSize - st::defaultActiveButton.height) / 2;
				QRect add(myrtlrect(addx, addy, addw, st::defaultActiveButton.height));
				actionSel = add.contains(local.x(), local.y() - _itemsTop - selected * _rowHeight) ? selected : -1;
			}
		} else if (_featuredHeight && QRect(0, st::membersPadding.top(), width(), _featuredHeight).contains(local)) {
			selected = -1;
		} else {
			selected = -2;
		}
		if (_selected != selected) {
			if ((_selected == -1) != (selected == -1)) {
				update();
			}
			if (_section == Section::Featured && ((_selected >= 0 || _pressed >= 0) != (selected >= 0 || _pressed >= 0))) {
				setCursor((selected >= 0 || _pressed >= 0) ? style::cur_pointer : style::cur_default);
			}
			_selected = selected;
		}
		setActionSel(actionSel);
		emit noDraggingScroll();
	}
}

float64 StickersInner::aboveShadowOpacity() const {
	if (_above < 0) return 0;

	int32 dx = 0;
	int32 dy = qAbs(_above * _rowHeight + _rows.at(_above)->yadd.current() - _started * _rowHeight);
	return qMin((dx + dy)  * 2. / _rowHeight, 1.);
}

void StickersInner::mouseReleaseEvent(QMouseEvent *e) {
	auto pressed = _pressed;
	_pressed = -2;

	if (_section == Section::Featured && _selected < 0 && pressed >= 0) {
		setCursor(style::cur_default);
	}

	if (_saving) return;

	_mouse = e->globalPos();
	onUpdateSelected();
	if (_actionDown == _actionSel && _actionSel >= 0) {
		if (_section == Section::Installed) {
			_rows[_actionDown]->disabled = !_rows[_actionDown]->disabled;
		} else {
			installSet(_rows[_actionDown]->id);
		}
	} else if (_dragging >= 0) {
		QPoint local(mapFromGlobal(_mouse));
		_rows[_dragging]->yadd.start(0);
		_aboveShadowFadeStart = _animStartTimes[_dragging] = getms();
		_aboveShadowFadeOpacity = anim::fvalue(aboveShadowOpacity(), 0);
		if (!_a_shifting.animating()) {
			_a_shifting.start();
		}

		_dragging = _started = -1;
	} else if (pressed == _selected) {
		if (_selected == -1) {
			_selected = -2;
			Ui::showLayer(new StickersBox(Section::Featured), KeepOtherLayers);
		} else if (_selected >= 0 && _section == Section::Featured) {
			auto &sets = Global::RefStickerSets();
			auto it = sets.find(_rows.at(pressed)->id);
			if (it != sets.cend()) {
				_selected = -2;
				Ui::showLayer(new StickerSetBox(Stickers::inputSetId(*it)), KeepOtherLayers);
			}
		}
	}
	if (_actionDown >= 0) {
		update(0, _itemsTop + _actionDown * _rowHeight, width(), _rowHeight);
		_actionDown = -1;
	}
}

void StickersInner::leaveEvent(QEvent *e) {
	_mouse = QPoint(-1, -1);
	onUpdateSelected();
}

void StickersInner::installSet(uint64 setId) {
	auto &sets = Global::RefStickerSets();
	auto it = sets.find(setId);
	if (it == sets.cend()) {
		rebuild();
		return;
	}

	MTP::send(MTPmessages_InstallStickerSet(Stickers::inputSetId(*it), MTP_boolFalse()));

	it->flags &= ~(MTPDstickerSet::Flag::f_disabled | MTPDstickerSet_ClientFlag::f_unread);
	it->flags |= MTPDstickerSet::Flag::f_installed;

	auto &order = Global::RefStickerSetsOrder();
	int insertAtIndex = 0, currentIndex = order.indexOf(setId);
	if (currentIndex != insertAtIndex) {
		if (currentIndex > 0) {
			order.removeAt(currentIndex);
		}
		order.insert(insertAtIndex, setId);
	}

	auto custom = sets.find(Stickers::CustomSetId);
	if (custom != sets.cend()) {
		for_const (auto sticker, it->stickers) {
			int removeIndex = custom->stickers.indexOf(sticker);
			if (removeIndex >= 0) custom->stickers.removeAt(removeIndex);
		}
		if (custom->stickers.isEmpty()) {
			sets.erase(custom);
		}
	}
	Local::writeStickers();
	emit App::main()->stickersUpdated();
}

void StickersInner::step_shifting(uint64 ms, bool timer) {
	bool animating = false;
	int32 updateMin = -1, updateMax = 0;
	for (int32 i = 0, l = _animStartTimes.size(); i < l; ++i) {
		uint64 start = _animStartTimes.at(i);
		if (start) {
			if (updateMin < 0) updateMin = i;
			updateMax = i;
			if (start + st::stickersRowDuration > ms && ms >= start) {
				_rows.at(i)->yadd.update(float64(ms - start) / st::stickersRowDuration, anim::sineInOut);
				animating = true;
			} else {
				_rows.at(i)->yadd.finish();
				_animStartTimes[i] = 0;
			}
		}
	}
	if (_aboveShadowFadeStart) {
		if (updateMin < 0 || updateMin > _above) updateMin = _above;
		if (updateMax < _above) updateMin = _above;
		if (_aboveShadowFadeStart + st::stickersRowDuration > ms && ms > _aboveShadowFadeStart) {
			_aboveShadowFadeOpacity.update(float64(ms - _aboveShadowFadeStart) / st::stickersRowDuration, anim::sineInOut);
			animating = true;
		} else {
			_aboveShadowFadeOpacity.finish();
			_aboveShadowFadeStart = 0;
		}
	}
	if (timer) {
		if (_dragging >= 0) {
			if (updateMin < 0 || updateMin > _dragging) updateMin = _dragging;
			if (updateMax < _dragging) updateMax = _dragging;
		}
		if (updateMin >= 0) {
			update(0, _itemsTop + _rowHeight * (updateMin - 1), width(), _rowHeight * (updateMax - updateMin + 3));
		}
	}
	if (!animating) {
		_above = _dragging;
		_a_shifting.stop();
	}
}

void StickersInner::clear() {
	for (int32 i = 0, l = _rows.size(); i < l; ++i) {
		delete _rows.at(i);
	}
	_rows.clear();
	_animStartTimes.clear();
	_aboveShadowFadeStart = 0;
	_aboveShadowFadeOpacity = anim::fvalue(0, 0);
	_a_shifting.stop();
_above = _dragging = _started = -1;
_selected = -2;
_pressed = -2;
_actionDown = -1;
setActionSel(-1);
update();
}

void StickersInner::setActionSel(int32 actionSel) {
	if (actionSel != _actionSel) {
		if (_actionSel >= 0) update(0, _itemsTop + _actionSel * _rowHeight, width(), _rowHeight);
		_actionSel = actionSel;
		if (_actionSel >= 0) update(0, _itemsTop + _actionSel * _rowHeight, width(), _rowHeight);
		if (_section == Section::Installed) {
			setCursor((_actionSel >= 0 && (_actionDown < 0 || _actionDown == _actionSel)) ? style::cur_pointer : style::cur_default);
		}
	}
}

void StickersInner::rebuild() {
	QList<StickerSetRow*> rows, rowsDisabled;

	_itemsTop = st::membersPadding.top();
	_featuredHeight = 0;
	if (_section == Section::Installed && !Global::FeaturedStickerSetsOrder().isEmpty()) {
		_featuredHeight = st::stickersFeaturedHeight;
		_itemsTop += _featuredHeight + st::membersPadding.top();
	}

	int namex = st::contactsPadding.left() + st::contactsPhotoSize + st::contactsPadding.left();
	int namew = st::boxWideWidth - namex - st::contactsPadding.right() - st::contactsCheckPosition.x();
	if (_section == Section::Installed) {
		namew -= qMax(qMax(_returnWidth, _removeWidth), _restoreWidth);
	} else {
		namew -= _addWidth - st::defaultActiveButton.width;
	}

	clear();
	auto &order = (_section == Section::Installed) ? Global::StickerSetsOrder() : Global::FeaturedStickerSetsOrder();
	_animStartTimes.reserve(order.size());

	auto &sets = Global::StickerSets();
	for_const (auto setId, order) {
		auto it = sets.constFind(setId);
		if (it == sets.cend()) {
			continue;
		}

		bool installed = (it->flags & MTPDstickerSet::Flag::f_installed);
		bool disabled = (_section == Section::Installed) && (it->flags & MTPDstickerSet::Flag::f_disabled);
		bool official = (it->flags & MTPDstickerSet::Flag::f_official);
		bool unread = (_section == Section::Featured) && _unreadSets.contains(it->id);
		if (!unread && _section == Section::Featured && (it->flags & MTPDstickerSet_ClientFlag::f_unread)) {
			unread = true;
			_unreadSets.insert(it->id);
		}

		DocumentData *sticker = it->stickers.isEmpty() ? 0 : it->stickers.at(0);
		int32 pixw = 0, pixh = 0;
		if (sticker) {
			pixw = sticker->thumb->width();
			pixh = sticker->thumb->height();
			if (pixw > st::contactsPhotoSize) {
				if (pixw > pixh) {
					pixh = (pixh * st::contactsPhotoSize) / pixw;
					pixw = st::contactsPhotoSize;
				} else {
					pixw = (pixw * st::contactsPhotoSize) / pixh;
					pixh = st::contactsPhotoSize;
				}
			} else if (pixh > st::contactsPhotoSize) {
				pixw = (pixw * st::contactsPhotoSize) / pixh;
				pixh = st::contactsPhotoSize;
			}
		}
		QString title = it->title;
		int32 titleWidth = st::contactsNameFont->width(title);
		if (titleWidth > namew) {
			title = st::contactsNameFont->elided(title, namew);
		}
		(disabled ? rowsDisabled : rows).push_back(new StickerSetRow(it->id, sticker, it->stickers.size(), title, installed, official, unread, disabled, pixw, pixh));
		_animStartTimes.push_back(0);
		if (it->stickers.isEmpty() || (it->flags & MTPDstickerSet_ClientFlag::f_not_loaded)) {
			App::api()->scheduleStickerSetRequest(it->id, it->access);
		}
	}
	App::api()->requestStickerSets();
	_rows = rows + rowsDisabled;
	resize(width(), _itemsTop + _rows.size() * _rowHeight + st::membersPadding.bottom());

	if (_section == Section::Featured && Global::FeaturedStickerSetsUnreadCount()) {
		Global::SetFeaturedStickerSetsUnreadCount(0);
		for (auto &set : Global::RefStickerSets()) {
			set.flags &= ~MTPDstickerSet_ClientFlag::f_unread;
		}
		MTP::send(MTPmessages_ReadFeaturedStickers(), rpcDone(&StickersInner::readFeaturedDone), rpcFail(&StickersInner::readFeaturedFail));
	}
}

void StickersInner::readFeaturedDone(const MTPBool &result) {
	Local::writeStickers();
	emit App::main()->stickersUpdated();
}

bool StickersInner::readFeaturedFail(const RPCError &error) {
	if (MTP::isDefaultHandledError(error)) return false;

	int unreadCount = 0;
	for_const (auto &set, Global::StickerSets()) {
		if (!(set.flags & MTPDstickerSet::Flag::f_installed)) {
			if (set.flags & MTPDstickerSet_ClientFlag::f_unread) {
				++unreadCount;
			}
		}
	}
	Global::SetFeaturedStickerSetsUnreadCount(unreadCount);
	return true;
}

QVector<uint64> StickersInner::getOrder() const {
	QVector<uint64> result;
	result.reserve(_rows.size());
	for (int32 i = 0, l = _rows.size(); i < l; ++i) {
		if (_rows.at(i)->disabled) {
			auto it = Global::StickerSets().constFind(_rows.at(i)->id);
			if (it == Global::StickerSets().cend() || !(it->flags & MTPDstickerSet::Flag::f_official)) {
				continue;
			}
		}
		result.push_back(_rows.at(i)->id);
	}
	return result;
}

QVector<uint64> StickersInner::getDisabledSets() const {
	QVector<uint64> result;
	result.reserve(_rows.size());
	for (int32 i = 0, l = _rows.size(); i < l; ++i) {
		if (_rows.at(i)->disabled) {
			result.push_back(_rows.at(i)->id);
		}
	}
	return result;
}

void StickersInner::setVisibleScrollbar(int32 width) {
	_scrollbar = width;
}

StickersInner::~StickersInner() {
	clear();
}

} // namespace internal

StickersBox::StickersBox(Section section) : ItemListBox(st::boxScroll)
, _section(section)
, _inner(section)
, _reorderRequest(0)
, _topShadow(this, st::contactsAboutShadow)
, _scrollDelta(0)
, _aboutWidth(st::boxWideWidth - st::contactsPadding.left() - st::contactsPadding.left())
, _about(st::boxTextFont, lang(lng_stickers_reorder), _defaultOptions, _aboutWidth) {

	int bottomSkip = st::boxPadding.bottom();
	if (_section == Section::Installed) {
		_aboutHeight = st::stickersReorderPadding.top() + _about.countHeight(_aboutWidth) + st::stickersReorderPadding.bottom();

		_save = new BoxButton(this, lang(lng_settings_save), st::defaultBoxButton);
		connect(_save, SIGNAL(clicked()), this, SLOT(onSave()));

		_cancel = new BoxButton(this, lang(lng_cancel), st::cancelBoxButton);
		connect(_cancel, SIGNAL(clicked()), this, SLOT(onClose()));

		_bottomShadow = new ScrollableBoxShadow(this);
		bottomSkip = st::boxButtonPadding.top() + _save->height() + st::boxButtonPadding.bottom();
	}
	ItemListBox::init(_inner, bottomSkip, st::boxTitleHeight + _aboutHeight);
	setMaxHeight(snap(countHeight(), int32(st::sessionsHeight), int32(st::boxMaxListHeight)));

	connect(App::main(), SIGNAL(stickersUpdated()), this, SLOT(onStickersUpdated()));
	App::main()->updateStickers();

	connect(_inner, SIGNAL(checkDraggingScroll(int)), this, SLOT(onCheckDraggingScroll(int)));
	connect(_inner, SIGNAL(noDraggingScroll()), this, SLOT(onNoDraggingScroll()));
	connect(&_scrollTimer, SIGNAL(timeout()), this, SLOT(onScrollTimer()));
	_scrollTimer.setSingleShot(false);

	onStickersUpdated();

	prepare();
}

int32 StickersBox::countHeight() const {
	int bottomSkip = st::boxPadding.bottom();
	if (_section == Section::Installed) {
		bottomSkip = st::boxButtonPadding.top() + _save->height() + st::boxButtonPadding.bottom();
	}
	return st::boxTitleHeight + _aboutHeight + _inner->height() + bottomSkip;
}

void StickersBox::disenableDone(const MTPBool & result, mtpRequestId req) {
	_disenableRequests.remove(req);
	if (_disenableRequests.isEmpty()) {
		saveOrder();
	}
}

bool StickersBox::disenableFail(const RPCError &error, mtpRequestId req) {
	if (MTP::isDefaultHandledError(error)) return false;
	_disenableRequests.remove(req);
	if (_disenableRequests.isEmpty()) {
		saveOrder();
	}
	return true;
}

void StickersBox::saveOrder() {
	auto order = _inner->getOrder();
	if (order.size() > 1) {
		QVector<MTPlong> mtpOrder;
		mtpOrder.reserve(order.size());
		for (int32 i = 0, l = order.size(); i < l; ++i) {
			mtpOrder.push_back(MTP_long(order.at(i)));
		}
		_reorderRequest = MTP::send(MTPmessages_ReorderStickerSets(MTP_vector<MTPlong>(mtpOrder)), rpcDone(&StickersBox::reorderDone), rpcFail(&StickersBox::reorderFail));
	} else {
		reorderDone(MTP_boolTrue());
	}
}

void StickersBox::reorderDone(const MTPBool &result) {
	_reorderRequest = 0;
	onClose();
}

bool StickersBox::reorderFail(const RPCError &result) {
	if (MTP::isDefaultHandledError(result)) return false;
	_reorderRequest = 0;
	Global::SetLastStickersUpdate(0);
	App::main()->updateStickers();
	onClose();
	return true;
}

void StickersBox::paintEvent(QPaintEvent *e) {
	Painter p(this);
	if (paint(p)) return;

	paintTitle(p, lang(lng_stickers_packs));
	p.translate(0, st::boxTitleHeight);

	if (_aboutHeight > 0) {
		p.fillRect(0, 0, width(), _aboutHeight, st::contactsAboutBg);
		p.setPen(st::stickersReorderFg);
		_about.draw(p, st::contactsPadding.left(), st::stickersReorderPadding.top(), _aboutWidth, style::al_center);
	}
}

void StickersBox::closePressed() {
	if (!_disenableRequests.isEmpty()) {
		for (QMap<mtpRequestId, NullType>::const_iterator i = _disenableRequests.cbegin(), e = _disenableRequests.cend(); i != e; ++i) {
			MTP::cancel(i.key());
		}
		_disenableRequests.clear();
		Global::SetLastStickersUpdate(0);
		App::main()->updateStickers();
	} else if (_reorderRequest) {
		MTP::cancel(_reorderRequest);
		_reorderRequest = 0;
		Global::SetLastStickersUpdate(0);
		App::main()->updateStickers();
	}
}

void StickersBox::resizeEvent(QResizeEvent *e) {
	ItemListBox::resizeEvent(e);
	_inner->resize(width(), _inner->height());
	_topShadow.setGeometry(0, st::boxTitleHeight + _aboutHeight, width(), st::lineWidth);
	_inner->setVisibleScrollbar((_scroll.scrollTopMax() > 0) ? (st::boxScroll.width - st::boxScroll.deltax) : 0);
	if (_save) {
		_save->moveToRight(st::boxButtonPadding.right(), height() - st::boxButtonPadding.bottom() - _save->height());
		_cancel->moveToRight(st::boxButtonPadding.right() + _save->width() + st::boxButtonPadding.left(), _save->y());
		_bottomShadow->setGeometry(0, height() - st::boxButtonPadding.bottom() - _save->height() - st::boxButtonPadding.top() - st::lineWidth, width(), st::lineWidth);
	}
}

void StickersBox::onStickersUpdated() {
	_inner->rebuild();
	setMaxHeight(snap(countHeight(), int32(st::sessionsHeight), int32(st::boxMaxListHeight)));
	_inner->setVisibleScrollbar((_scroll.scrollTopMax() > 0) ? (st::boxScroll.width - st::boxScroll.deltax) : 0);
}

void StickersBox::onCheckDraggingScroll(int localY) {
	if (localY < _scroll.scrollTop()) {
		_scrollDelta = localY - _scroll.scrollTop();
	} else if (localY >= _scroll.scrollTop() + _scroll.height()) {
		_scrollDelta = localY - _scroll.scrollTop() - _scroll.height() + 1;
	} else {
		_scrollDelta = 0;
	}
	if (_scrollDelta) {
		_scrollTimer.start(15);
	} else {
		_scrollTimer.stop();
	}
}

void StickersBox::onNoDraggingScroll() {
	_scrollTimer.stop();
}

void StickersBox::onScrollTimer() {
	int32 d = (_scrollDelta > 0) ? qMin(_scrollDelta * 3 / 20 + 1, int32(MaxScrollSpeed)) : qMax(_scrollDelta * 3 / 20 - 1, -int32(MaxScrollSpeed));
	_scroll.scrollToY(_scroll.scrollTop() + d);
}

void StickersBox::onSave() {
	if (!_inner->savingStart()) {
		return;
	}

	bool writeRecent = false;
	RecentStickerPack &recent(cGetRecentStickers());
	auto &sets = Global::RefStickerSets();

	QVector<uint64> reorder = _inner->getOrder(), disabled = _inner->getDisabledSets();
	for (int32 i = 0, l = disabled.size(); i < l; ++i) {
		auto it = sets.find(disabled.at(i));
		if (it != sets.cend()) {
			for (RecentStickerPack::iterator i = recent.begin(); i != recent.cend();) {
				if (it->stickers.indexOf(i->first) >= 0) {
					i = recent.erase(i);
					writeRecent = true;
				} else {
					++i;
				}
			}
			if (!(it->flags & MTPDstickerSet::Flag::f_disabled)) {
				MTPInputStickerSet setId = (it->id && it->access) ? MTP_inputStickerSetID(MTP_long(it->id), MTP_long(it->access)) : MTP_inputStickerSetShortName(MTP_string(it->shortName));
				if (it->flags & MTPDstickerSet::Flag::f_official) {
					_disenableRequests.insert(MTP::send(MTPmessages_InstallStickerSet(setId, MTP_boolTrue()), rpcDone(&StickersBox::disenableDone), rpcFail(&StickersBox::disenableFail), 0, 5), NullType());
					it->flags |= MTPDstickerSet::Flag::f_disabled;
				} else {
					_disenableRequests.insert(MTP::send(MTPmessages_UninstallStickerSet(setId), rpcDone(&StickersBox::disenableDone), rpcFail(&StickersBox::disenableFail), 0, 5), NullType());
					int removeIndex = Global::StickerSetsOrder().indexOf(it->id);
					if (removeIndex >= 0) Global::RefStickerSetsOrder().removeAt(removeIndex);
					if (!(it->flags & MTPDstickerSet_ClientFlag::f_featured)) {
						sets.erase(it);
					} else {
						it->flags &= ~(MTPDstickerSet::Flag::f_installed | MTPDstickerSet::Flag::f_disabled);
					}
				}
			}
		}
	}

	// Clear all installed flags, set only for sets from order.
	for (auto &set : sets) {
		set.flags &= ~MTPDstickerSet::Flag::f_installed;
	}

	auto &order(Global::RefStickerSetsOrder());
	order.clear();
	for (int i = 0, l = reorder.size(); i < l; ++i) {
		auto it = sets.find(reorder.at(i));
		if (it != sets.cend()) {
			if ((it->flags & MTPDstickerSet::Flag::f_disabled) && !disabled.contains(it->id)) {
				MTPInputStickerSet setId = (it->id && it->access) ? MTP_inputStickerSetID(MTP_long(it->id), MTP_long(it->access)) : MTP_inputStickerSetShortName(MTP_string(it->shortName));
				_disenableRequests.insert(MTP::send(MTPmessages_InstallStickerSet(setId, MTP_boolFalse()), rpcDone(&StickersBox::disenableDone), rpcFail(&StickersBox::disenableFail), 0, 5), NullType());
				it->flags &= ~MTPDstickerSet::Flag::f_disabled;
			}
			order.push_back(reorder.at(i));
			it->flags |= MTPDstickerSet::Flag::f_installed;
		}
	}
	for (auto it = sets.begin(); it != sets.cend();) {
		if (it->id == Stickers::CustomSetId
			|| it->id == Stickers::RecentSetId
			|| (it->flags & MTPDstickerSet_ClientFlag::f_featured)
			|| (it->flags & MTPDstickerSet::Flag::f_installed)) {
			++it;
		} else {
			it = sets.erase(it);
		}
	}

	Local::writeStickers();
	if (writeRecent) Local::writeUserSettings();
	emit App::main()->stickersUpdated();

	if (_disenableRequests.isEmpty()) {
		saveOrder();
	} else {
		MTP::sendAnything();
	}
}

void StickersBox::hideAll() {
	_topShadow.hide();
	if (_save) {
		_save->hide();
		_cancel->hide();
		_bottomShadow->hide();
	}
	ItemListBox::hideAll();
}

void StickersBox::showAll() {
	_topShadow.show();
	if (_save) {
		_save->show();
		_cancel->show();
		_bottomShadow->show();
	}
	ItemListBox::showAll();
}

int32 stickerPacksCount(bool includeDisabledOfficial) {
	int32 result = 0;
	auto &order = Global::StickerSetsOrder();
	auto &sets = Global::StickerSets();
	for (int i = 0, l = order.size(); i < l; ++i) {
		auto it = sets.constFind(order.at(i));
		if (it != sets.cend()) {
			if (!(it->flags & MTPDstickerSet::Flag::f_disabled) || ((it->flags & MTPDstickerSet::Flag::f_official) && includeDisabledOfficial)) {
				++result;
			}
		}
	}
	return result;
}
