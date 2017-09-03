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
Copyright (c) 2014-2017 John Preston, https://desktop.telegram.org
*/
#pragma once

#include "rpl/lifetime.h"
#include <mutex>

namespace rpl {

struct no_value {
};

struct no_error {
	no_error() = delete;
};

template <typename Value, typename Error>
class consumer {
public:
	template <
		typename OnNext,
		typename OnError,
		typename OnDone,
		typename = decltype(std::declval<OnNext>()(std::declval<Value>())),
		typename = decltype(std::declval<OnError>()(std::declval<Error>())),
		typename = decltype(std::declval<OnDone>()())>
	consumer(
		OnNext &&next,
		OnError &&error,
		OnDone &&done);

	bool putNext(Value value) const;
	void putError(Error error) const;
	void putDone() const;

	void setLifetime(lifetime &&lifetime) const;
	void terminate() const;

private:
	class abstract_consumer_instance;

	template <typename OnNext, typename OnError, typename OnDone>
	class consumer_instance;

	template <typename OnNext, typename OnError, typename OnDone>
	std::shared_ptr<abstract_consumer_instance> ConstructInstance(
		OnNext &&next,
		OnError &&error,
		OnDone &&done);

	std::shared_ptr<abstract_consumer_instance> _instance;

};


template <typename Value, typename Error>
class consumer<Value, Error>::abstract_consumer_instance {
public:
	virtual bool putNext(Value value) = 0;
	virtual void putError(Error error) = 0;
	virtual void putDone() = 0;

	void setLifetime(lifetime &&lifetime);
	void terminate();

protected:
	lifetime _lifetime;
	bool _terminated = false;
	std::mutex _mutex;

};

template <typename Value, typename Error>
template <typename OnNext, typename OnError, typename OnDone>
class consumer<Value, Error>::consumer_instance
	: public consumer<Value, Error>::abstract_consumer_instance {
public:
	template <typename OnNextImpl, typename OnErrorImpl, typename OnDoneImpl>
	consumer_instance(
		OnNextImpl &&next,
		OnErrorImpl &&error,
		OnDoneImpl &&done)
		: _next(std::forward<OnNextImpl>(next))
		, _error(std::forward<OnErrorImpl>(error))
		, _done(std::forward<OnDoneImpl>(done)) {
	}

	bool putNext(Value value) override;
	void putError(Error error) override;
	void putDone() override;

private:
	OnNext _next;
	OnError _error;
	OnDone _done;

};

template <typename Value, typename Error>
template <typename OnNext, typename OnError, typename OnDone>
std::shared_ptr<typename consumer<Value, Error>::abstract_consumer_instance>
consumer<Value, Error>::ConstructInstance(
		OnNext &&next,
		OnError &&error,
		OnDone &&done) {
	return std::make_shared<consumer_instance<
		std::decay_t<OnNext>,
		std::decay_t<OnError>,
		std::decay_t<OnDone>>>(
			std::forward<OnNext>(next),
			std::forward<OnError>(error),
			std::forward<OnDone>(done));
}

template <typename Value, typename Error>
template <
	typename OnNext,
	typename OnError,
	typename OnDone,
	typename,
	typename,
	typename>
consumer<Value, Error>::consumer(
	OnNext &&next,
	OnError &&error,
	OnDone &&done) : _instance(ConstructInstance(
		std::forward<OnNext>(next),
		std::forward<OnError>(error),
		std::forward<OnDone>(done))) {
}

template <typename Value, typename Error>
bool consumer<Value, Error>::putNext(Value value) const {
	return _instance->putNext(std::move(value));
}

template <typename Value, typename Error>
void consumer<Value, Error>::putError(Error error) const {
	return _instance->putError(std::move(error));
}

template <typename Value, typename Error>
void consumer<Value, Error>::putDone() const {
	return _instance->putDone();
}

template <typename Value, typename Error>
void consumer<Value, Error>::setLifetime(lifetime &&lifetime) const {
	return _instance->setLifetime(std::move(lifetime));
}

template <typename Value, typename Error>
void consumer<Value, Error>::terminate() const {
	return _instance->terminate();
}

template <typename Value, typename Error>
void consumer<Value, Error>::abstract_consumer_instance::setLifetime(
		lifetime &&lifetime) {
	std::unique_lock<std::mutex> lock(_mutex);
	if (_terminated) {
		lock.unlock();

		lifetime.destroy();
	} else {
		_lifetime = std::move(lifetime);
	}
}

template <typename Value, typename Error>
void consumer<Value, Error>::abstract_consumer_instance::terminate() {
	std::unique_lock<std::mutex> lock(_mutex);
	if (!_terminated) {
		_terminated = true;
		auto handler = std::exchange(_lifetime, lifetime());
		lock.unlock();

		handler.destroy();
	}
}

template <typename Value, typename Error>
template <typename OnNext, typename OnError, typename OnDone>
bool consumer<Value, Error>::consumer_instance<OnNext, OnError, OnDone>::putNext(
		Value value) {
	std::unique_lock<std::mutex> lock(this->_mutex);
	if (this->_terminated) {
		return false;
	}
	auto handler = this->_next;
	lock.unlock();

	handler(std::move(value));
	return true;
}

template <typename Value, typename Error>
template <typename OnNext, typename OnError, typename OnDone>
void consumer<Value, Error>::consumer_instance<OnNext, OnError, OnDone>::putError(
		Error error) {
	std::unique_lock<std::mutex> lock(this->_mutex);
	if (!this->_terminated) {
		auto handler = std::move(this->_error);
		lock.unlock();

		handler(std::move(error));
		this->terminate();
	}
}

template <typename Value, typename Error>
template <typename OnNext, typename OnError, typename OnDone>
void consumer<Value, Error>::consumer_instance<OnNext, OnError, OnDone>::putDone() {
	std::unique_lock<std::mutex> lock(this->_mutex);
	if (!this->_terminated) {
		auto handler = std::move(this->_done);
		lock.unlock();

		handler();
		this->terminate();
	}
}

} // namespace rpl