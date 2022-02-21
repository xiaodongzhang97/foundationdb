/*
 * SysTestTransactionExecutor.cpp
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

#include "SysTestTransactionExecutor.h"
#include <iostream>
#include <cassert>

namespace FDBSystemTester {

namespace {

void fdb_check(fdb_error_t e) {
	if (e) {
		std::cerr << fdb_get_error(e) << std::endl;
		std::abort();
	}
}

} // namespace

class TransactionContext : public ITransactionContext {
public:
	TransactionContext(FDBTransaction* tx,
	                   ITransactionActor* txActor,
	                   TTaskFct cont,
	                   const TransactionExecutorOptions& options,
	                   IScheduler* scheduler)
	  : options(options), fdbTx(tx), txActor(txActor), contAfterDone(cont), scheduler(scheduler), finalError(0) {}

	Transaction* tx() override { return &fdbTx; }
	void continueAfter(Future& f, TTaskFct cont) override { doContinueAfter(f, cont); }
	void commit() override {
		currFuture = fdbTx.commit();
		doContinueAfter(currFuture, [this]() { done(); });
	}
	void done() override {
		TTaskFct cont = contAfterDone;
		delete this;
		cont();
	}
	std::string_view dbKey(std::string_view key) override {
		std::string keyWithPrefix(options.prefix);
		keyWithPrefix.append(key);
		return key;
	}

private:
	void doContinueAfter(Future& f, TTaskFct cont) {
		if (options.blockOnFutures) {
			blockingContinueAfter(f, cont);
		} else {
			asyncContinueAfter(f, cont);
		}
	}

	void blockingContinueAfter(Future& f, TTaskFct cont) {
		Future* fptr = &f;
		scheduler->schedule([this, fptr, cont]() {
			fdb_check(fdb_future_block_until_ready(fptr->fdbFuture()));
			fdb_error_t err = fptr->getError();
			if (err) {
				currFuture = fdbTx.onError(err);
				fdb_check(fdb_future_block_until_ready(currFuture.fdbFuture()));
				handleOnErrorResult();
			} else {
				cont();
			}
		});
	}

	void asyncContinueAfter(Future& f, TTaskFct cont) {
		currCont = cont;
		fdb_check(fdb_future_set_callback(f.fdbFuture(), futureReadyCallback, this));
	}

	static void futureReadyCallback(FDBFuture* f, void* param) {
		TransactionContext* txCtx = (TransactionContext*)param;
		txCtx->onFutureReady(f);
	}

	void onFutureReady(FDBFuture* f) {
		fdb_error_t err = fdb_future_get_error(f);
		if (err) {
			currFuture = tx()->onError(err);
			fdb_check(fdb_future_set_callback(currFuture.fdbFuture(), onErrorReadyCallback, this));
		} else {
			scheduler->schedule(currCont);
		}
	}

	static void onErrorReadyCallback(FDBFuture* f, void* param) {
		TransactionContext* txCtx = (TransactionContext*)param;
		txCtx->onErrorReady(f);
	}

	void onErrorReady(FDBFuture* f) {
		scheduler->schedule([this]() { handleOnErrorResult(); });
	}

	void handleOnErrorResult() {
		fdb_error_t err = currFuture.getError();
		if (err) {
			finalError = err;
			done();
		} else {
			txActor->reset();
			txActor->start();
		}
	}

	const TransactionExecutorOptions& options;
	Transaction fdbTx;
	ITransactionActor* txActor;
	TTaskFct currCont;
	TTaskFct contAfterDone;
	IScheduler* scheduler;
	fdb_error_t finalError;
	EmptyFuture currFuture;
};

class TransactionExecutor : public ITransactionExecutor {
public:
	TransactionExecutor() : db(nullptr), scheduler(nullptr) {}

	~TransactionExecutor() { release(); }

	void init(IScheduler* scheduler, const char* clusterFile, const TransactionExecutorOptions& options) override {
		this->scheduler = scheduler;
		this->options = options;
		fdb_check(fdb_create_database(clusterFile, &db));
	}

	void execute(ITransactionActor* txActor, TTaskFct cont) override {
		FDBTransaction* tx;
		fdb_check(fdb_database_create_transaction(db, &tx));
		TransactionContext* ctx = new TransactionContext(tx, txActor, cont, options, scheduler);
		txActor->init(ctx);
		txActor->start();
	}

	void release() override { fdb_database_destroy(db); }

private:
	FDBDatabase* db;
	TransactionExecutorOptions options;
	IScheduler* scheduler;
};

ITransactionExecutor* createTransactionExecutor() {
	return new TransactionExecutor();
}

} // namespace FDBSystemTester