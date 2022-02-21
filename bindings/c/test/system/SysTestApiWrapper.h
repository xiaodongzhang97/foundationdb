/*
 * SysTestApiWrapper.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2021 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#ifndef SYS_TEST_API_WRAPPER_H
#define SYS_TEST_API_WRAPPER_H

#include <string_view>
#include <optional>

#define FDB_API_VERSION 710
#include "bindings/c/foundationdb/fdb_c.h"

namespace FDBSystemTester {

// Wrapper parent class to manage memory of an FDBFuture pointer. Cleans up
// FDBFuture when this instance goes out of scope.
class Future {
public:
	Future() : future_(nullptr) {}
	Future(FDBFuture* f) : future_(f) {}
	virtual ~Future();

	Future& operator=(Future&& other) {
		future_ = other.future_;
		other.future_ = nullptr;
		return *this;
	}

	FDBFuture* fdbFuture() { return future_; };

	fdb_error_t getError();
	void reset();

protected:
	FDBFuture* future_;
};

class ValueFuture : public Future {
public:
	ValueFuture() = default;
	ValueFuture(FDBFuture* f) : Future(f) {}
	std::optional<std::string_view> getValue();
};

class EmptyFuture : public Future {
public:
	EmptyFuture() = default;
	EmptyFuture(FDBFuture* f) : Future(f) {}
};

class Transaction {
public:
	// Given an FDBDatabase, initializes a new transaction.
	Transaction(FDBTransaction* tx);
	~Transaction();

	ValueFuture get(std::string_view key, fdb_bool_t snapshot);
	void set(std::string_view key, std::string_view value);
	EmptyFuture commit();
	EmptyFuture onError(fdb_error_t err);

private:
	FDBTransaction* tx_;
};

} // namespace FDBSystemTester

#endif