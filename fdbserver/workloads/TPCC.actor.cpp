/*
 * TPCC.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2020 Apple Inc. and the FoundationDB project authors
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

#include "flow/actorcompiler.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbserver/workloads/TPCCWorkload.h"

#include <fdbclient/ReadYourWrites.h>
#include "flow/actorcompiler.h" // has to be last include

using namespace TPCCWorkload;

namespace {

struct TPCCMetrics {
	static constexpr int latenciesStored = 1000;

	uint64_t successfulStockLevelTransactions, failedStockLevelTransactions, successfulDeliveryTransactions,
	    failedDeliveryTransactions, successfulOrderStatusTransactions, failedOrderStatusTransactions,
	    successfulPaymentTransactions, failedPaymentTransactions, successfulNewOrderTransactions,
	    failedNewOrderTransactions;
	double stockLevelResponseTime, deliveryResponseTime, orderStatusResponseTime, paymentResponseTime,
	    newOrderResponseTime;
	std::vector<double> stockLevelLatencies, deliveryLatencies, orderStatusLatencies, paymentLatencies,
	    newOrderLatencies;
	TPCCMetrics()
	  : successfulStockLevelTransactions(0), successfulDeliveryTransactions(0), successfulOrderStatusTransactions(0),
	    successfulPaymentTransactions(0), successfulNewOrderTransactions(0), failedStockLevelTransactions(0),
	    failedDeliveryTransactions(0), failedOrderStatusTransactions(0), failedPaymentTransactions(0),
	    failedNewOrderTransactions(0), stockLevelResponseTime(0.0), deliveryResponseTime(0.0),
	    orderStatusResponseTime(0.0), paymentResponseTime(0.0), newOrderResponseTime(0.0),
	    stockLevelLatencies(latenciesStored, 0.0), deliveryLatencies(latenciesStored, 0.0),
	    orderStatusLatencies(latenciesStored, 0.0), paymentLatencies(latenciesStored, 0.0),
	    newOrderLatencies(latenciesStored, 0.0) {}

	void sort() {
		std::sort(stockLevelLatencies.begin(), stockLevelLatencies.end());
		std::sort(deliveryLatencies.begin(), deliveryLatencies.end());
		std::sort(orderStatusLatencies.begin(), orderStatusLatencies.end());
		std::sort(paymentLatencies.begin(), paymentLatencies.end());
		std::sort(newOrderLatencies.begin(), newOrderLatencies.end());
	}

	static double median(const std::vector<double>& latencies) {
		// assumes latencies is sorted
		return latencies[latencies.size() / 2];
	}

	static double percentile_90(const std::vector<double>& latencies) {
		// assumes latencies is sorted
		return latencies[(9 * latencies.size()) / 10];
	}

	static double percentile_99(const std::vector<double>& latencies) {
		// assumes latencies is sorted
		return latencies[(99 * latencies.size()) / 100];
	}

	static void updateMetrics(bool committed,
	                          uint64_t& successCounter,
	                          uint64_t& failedCounter,
	                          double txnStartTime,
	                          std::vector<double>& latencies,
	                          double& totalLatency,
	                          std::string txnType) {
		auto responseTime = g_network->now() - txnStartTime;
		if (committed) {
			totalLatency += responseTime;
			++successCounter;
			if (successCounter <= latenciesStored)
				latencies[successCounter - 1] = responseTime;
			else {
				auto index = deterministicRandom()->randomInt(0, successCounter);
				if (index < latenciesStored) {
					latencies[index] = responseTime;
				}
			}
		} else {
			++failedCounter;
		}
		TraceEvent("TransactionComplete")
		    .detail("TransactionType", txnType)
		    .detail("Latency", responseTime)
		    .detail("Begin", txnStartTime)
		    .detail("End", txnStartTime + responseTime)
		    .detail("Success", committed);
	}
};

struct TPCC : TestWorkload {
	static constexpr const char* DESCRIPTION = "TPCC";

	int warehousesNum;
	int clientProcessesUsed;
	int warehousesPerClientProcess;
	int clientsUsed;
	int clientsPerWarehouse;
	int expectedTransactionsPerMinute;
	int testDuration;
	int warmupTime;
	double startTime;
	int remoteProbability;

	GlobalState gState;
	TPCCMetrics metrics;

	TPCC(WorkloadContext const& ctx) : TestWorkload(ctx) {
		std::string workloadName = DESCRIPTION;
		warehousesNum = getOption(options, LiteralStringRef("warehousesNum"), 16);
		clientsUsed = getOption(options, LiteralStringRef("clientsUsed"), 16);
		clientProcessesUsed = getOption(options, LiteralStringRef("clientProcessesUsed"), 8);
		warehousesPerClientProcess = warehousesNum / clientProcessesUsed;
		clientsPerWarehouse = clientsUsed / warehousesNum;
		remoteProbability =  getOption(options, LiteralStringRef("remoteProbability"), 1);
		expectedTransactionsPerMinute = getOption(options, LiteralStringRef("expectedTransactionsPerMinute"), 1);
		testDuration = getOption(options, LiteralStringRef("testDuration"), 300);
		warmupTime = getOption(options, LiteralStringRef("warmupTime"), 60);
	}

	int NURand(int C, int A, int x, int y) {
		return (((deterministicRandom()->randomInt(0, A + 1) | deterministicRandom()->randomInt(x, y + 1)) + C) %
		        (y - x + 1)) +
		       x;
	}

	StringRef genCLast(Arena& arena, int x) {
		int l = x % 10;
		x /= 10;
		int m = x % 10;
		x /= 10;
		int f = x % 10;
		std::stringstream ss;
		ss << syllables[f] << syllables[m] << syllables[l];
		return StringRef(arena, ss.str());
	}

	// Should call in setup
	ACTOR static Future<Void> readGlobalState(TPCC* self, Database cx) {
		state ReadYourWritesTransaction tr(cx);
		loop {
			tr.reset();
			try {
				Optional<Value> val = wait(tr.get(self->gState.key()));
				if (val.present()) {
					BinaryReader reader(val.get(), IncludeVersion());
					serializer(reader, self->gState);
				} else {
					wait(delay(1.0));
				}
				return Void();
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}
	}

	virtual std::string description() override { return DESCRIPTION; }

	// Transactions

	ACTOR static Future<bool> newOrder(TPCC* self, Database cx, int w_id) {
		state int d_id = deterministicRandom()->randomInt(0, 10);
		state int c_id = self->NURand(self->gState.CRun, 1023, 1, 3000) - 1;
		state int ol_cnt = deterministicRandom()->randomInt(5, 16);
		state bool willRollback = deterministicRandom()->randomInt(1, 100) == 1;
		state ReadYourWritesTransaction tr(cx);
		try {
			state Warehouse warehouse;
			warehouse.w_id = w_id;
			Optional<Value> wValue = wait(tr.get(warehouse.key()));
			ASSERT(wValue.present());
			{
				BinaryReader r(wValue.get(), IncludeVersion());
				serializer(r, warehouse);
			}
			state District district;
			district.d_w_id = w_id;
			district.d_id = d_id;
			Optional<Value> dValue = wait(tr.get(district.key()));
			ASSERT(dValue.present());
			{
				BinaryReader r(dValue.get(), IncludeVersion());
				serializer(r, district);
			}
			state Customer customer;
			customer.c_id = c_id;
			customer.c_w_id = w_id;
			customer.c_d_id = d_id;
			Optional<Value> cValue = wait(tr.get(customer.key()));
			ASSERT(cValue.present());
			{
				BinaryReader r(cValue.get(), IncludeVersion());
				serializer(r, customer);
			}
			state Order order;
			order.o_entry_d = g_network->now();
			order.o_c_id = c_id;
			order.o_d_id = d_id;
			order.o_w_id = w_id;
			order.o_ol_cnt = ol_cnt;
			order.o_id = district.d_next_o_id;

			++district.d_next_o_id;
			{
				BinaryWriter w(IncludeVersion());
				serializer(w, district);
				tr.set(district.key(), w.toValue());
			}

			state NewOrder newOrder;
			newOrder.no_w_id = w_id;
			newOrder.no_d_id = d_id;
			newOrder.no_o_id = order.o_id;
			state int ol_id = 0;
			state bool allLocal = true;
			for (; ol_id < order.o_ol_cnt; ++ol_id) {
				if (ol_id + 1 == order.o_ol_cnt && willRollback) {
					// Simulated abort - order item not found
					return false;
				}
				state OrderLine orderLine;
				orderLine.ol_number = ol_id;
				orderLine.ol_w_id = w_id;
				orderLine.ol_d_id = d_id;
				orderLine.ol_supply_w_id = w_id;
				orderLine.ol_o_id = order.o_id;
				orderLine.ol_i_id = self->NURand(self->gState.CRun, 8191, 1, 100000) - 1;
				orderLine.ol_quantity = deterministicRandom()->randomInt(1, 11);
				if (deterministicRandom()->randomInt(0, 100) < self->remoteProbability) {
					orderLine.ol_supply_w_id = deterministicRandom()->randomInt(0, self->warehousesNum);
				}
				state Item item;
				item.i_id = orderLine.ol_i_id;
				orderLine.ol_i_id = item.i_id;
				Optional<Value> iValue = wait(tr.get(item.key()));
				ASSERT(iValue.present());
				{
					BinaryReader r(iValue.get(), IncludeVersion());
					serializer(r, item);
				}
				state Stock stock;
				stock.s_i_id = item.i_id;
				stock.s_w_id = orderLine.ol_supply_w_id;
				Optional<Value> sValue = wait(tr.get(stock.key()));
				ASSERT(sValue.present());
				{
					BinaryReader r(sValue.get(), IncludeVersion());
					serializer(r, stock);
				}
				if (stock.s_quantity - orderLine.ol_quantity >= 10) {
					stock.s_quantity -= orderLine.ol_quantity;
				} else {
					stock.s_quantity = (stock.s_quantity - orderLine.ol_quantity) + 91;
				}
				stock.s_ytd += orderLine.ol_quantity;
				stock.s_order_cnt += 1;
				if (orderLine.ol_supply_w_id != w_id) {
					stock.s_remote_cnt += 1;
					allLocal = false;
				}
				{
					BinaryWriter w(IncludeVersion());
					serializer(w, stock);
					tr.set(stock.key(), w.toValue());
				}
				orderLine.ol_amount = orderLine.ol_quantity * item.i_price;
				switch (orderLine.ol_d_id) {
				case 0:
					orderLine.ol_dist_info = stock.s_dist_01;
					break;
				case 1:
					orderLine.ol_dist_info = stock.s_dist_02;
					break;
				case 2:
					orderLine.ol_dist_info = stock.s_dist_03;
					break;
				case 3:
					orderLine.ol_dist_info = stock.s_dist_04;
					break;
				case 4:
					orderLine.ol_dist_info = stock.s_dist_05;
					break;
				case 5:
					orderLine.ol_dist_info = stock.s_dist_06;
					break;
				case 6:
					orderLine.ol_dist_info = stock.s_dist_07;
					break;
				case 7:
					orderLine.ol_dist_info = stock.s_dist_08;
					break;
				case 8:
					orderLine.ol_dist_info = stock.s_dist_09;
					break;
				case 9:
					orderLine.ol_dist_info = stock.s_dist_10;
					break;
				}
				{
					BinaryWriter w(IncludeVersion());
					serializer(w, orderLine);
					tr.set(orderLine.key(), w.toValue());
				}
			}
			order.o_all_local = allLocal;
			{
				BinaryWriter w(IncludeVersion());
				serializer(w, order);
				tr.set(order.key(), w.toValue());
			}
			{
				BinaryWriter w(IncludeVersion());
				serializer(w, newOrder);
				tr.set(newOrder.key(), w.toValue());
			}
			wait(tr.commit());
		} catch (Error& e) {
			return false;
		}
		return true;
	}

	ACTOR static Future<Customer> getRandomCustomer(TPCC* self, ReadYourWritesTransaction* tr, int w_id, int d_id) {
		state Customer result;
		result.c_w_id = w_id;
		result.c_d_id = d_id;
		if (deterministicRandom()->randomInt(0, 100) >= 85) {
			result.c_d_id = deterministicRandom()->randomInt(0, 10);
			result.c_w_id = deterministicRandom()->randomInt(0, self->warehousesNum);
		}
		if (deterministicRandom()->randomInt(0, 100) < 60) {
			// select through last name
			result.c_last = self->genCLast(result.arena, self->NURand(self->gState.CRun, 1023, 1, 3000) - 1);
			auto s = result.indexLastKey(1);
			auto begin = new (result.arena) uint8_t[s.size() + 1];
			auto end = new (result.arena) uint8_t[s.size() + 1];
			memcpy(begin, s.begin(), s.size());
			memcpy(end, s.begin(), s.size());
			begin[s.size()] = '/';
			end[s.size()] = '0';
			state Standalone<RangeResultRef> range =
			    wait(tr->getRange(KeyRangeRef(StringRef(begin, s.size() + 1), StringRef(end, s.size() + 1)), 1000));
			ASSERT(range.size() > 0);

			state std::vector<Customer> customers;
			state int i = 0;
			for (; i < range.size(); ++i) {
				Optional<Value> cValue = wait(tr->get(range[i].value));
				ASSERT(cValue.present());
				BinaryReader r(cValue.get(), IncludeVersion());
				state Customer customer;
				serializer(r, customer);
				customers.push_back(customer);
			}

			// Sort customers by first name and choose median
			std::sort(customers.begin(), customers.end(), [](const Customer& cus1, const Customer& cus2) {
				const std::string cus1Name = cus1.c_first.toString();
				const std::string cus2Name = cus2.c_first.toString();
				return (cus1Name.compare(cus2Name) < 0);
			});
			result = customers[customers.size() / 2];
		} else {
			// select through random id
			result.c_id = self->NURand(self->gState.CRun, 1023, 1, 3000) - 1;
			Optional<Value> val = wait(tr->get(result.key()));
			ASSERT(val.present());
			BinaryReader r(val.get(), IncludeVersion());
			serializer(r, result);
		}
		return result;
	}

	ACTOR static Future<bool> payment(TPCC* self, Database cx, int w_id) {
		state ReadYourWritesTransaction tr(cx);
		state int d_id = deterministicRandom()->randomInt(0, 10);
		state History history;
		state Warehouse warehouse;
		state District district;
		history.h_amount = deterministicRandom()->random01() * 4999.0 + 1.0;
		history.h_date = g_network->now();
		try {
			// get the customer
			state Customer customer = wait(getRandomCustomer(self, &tr, w_id, d_id));
			warehouse.w_id = w_id;
			Optional<Value> wValue = wait(tr.get(warehouse.key()));
			ASSERT(wValue.present());
			{
				BinaryReader r(wValue.get(), IncludeVersion());
				serializer(r, warehouse);
			}
			warehouse.w_ytd += history.h_amount;
			{
				BinaryWriter w(IncludeVersion());
				serializer(w, warehouse);
				tr.set(warehouse.key(), w.toValue());
			}
			district.d_w_id = w_id;
			district.d_id = d_id;
			Optional<Value> dValue = wait(tr.get(district.key()));
			ASSERT(dValue.present());
			{
				BinaryReader r(dValue.get(), IncludeVersion());
				serializer(r, district);
			}
			district.d_ytd += history.h_amount;
			customer.c_balance -= history.h_amount;
			customer.c_ytd_payment += history.h_amount;
			customer.c_payment_cnt += 1;
			if (customer.c_credit == LiteralStringRef("BC")) {
				// we must update c_data
				std::stringstream ss;
				ss << customer.c_id << "," << customer.c_d_id << "," << customer.c_w_id << "," << district.d_id << ","
				   << w_id << history.h_amount << ";";
				auto s = ss.str();
				auto len = std::min(int(s.size()) + customer.c_data.size(), 500);
				auto data = new (customer.arena) uint8_t[len];
				std::copy(s.begin(), s.end(), reinterpret_cast<char*>(data));
				std::copy(customer.c_data.begin(), customer.c_data.begin() + len - s.size(), data);
				customer.c_data = StringRef(data, len);
			}
			{
				BinaryWriter w(IncludeVersion());
				serializer(w, customer);
				tr.set(customer.key(), w.toValue());
			}
			std::stringstream ss;
			ss << warehouse.w_name.toString() << "    " << district.d_name.toString();
			history.h_data = StringRef(history.arena, ss.str());
			history.h_c_id = customer.c_id;
			history.h_c_d_id = customer.c_d_id;
			history.h_c_w_id = customer.c_w_id;
			history.h_d_id = d_id;
			history.h_w_id = w_id;
			{
				BinaryWriter w(IncludeVersion());
				serializer(w, history);
				UID k = deterministicRandom()->randomUniqueID();
				BinaryWriter kW(Unversioned());
				serializer(kW, k);
				auto key = kW.toValue().withPrefix(LiteralStringRef("History/"));
				tr.set(key, w.toValue());
			}
			wait(tr.commit());
		} catch (Error& e) {
			return false;
		}
		return true;
	}

	ACTOR static Future<bool> orderStatus(TPCC* self, Database cx, int w_id) {
		state ReadYourWritesTransaction tr(cx);
		state int d_id = deterministicRandom()->randomInt(0, 10);
		state int i;
		state Order order;
		state std::vector<OrderLine> orderLines;
		try {
			state Customer customer = wait(getRandomCustomer(self, &tr, w_id, d_id));
			order.o_w_id = customer.c_w_id;
			order.o_d_id = customer.c_d_id;
			order.o_c_id = customer.c_id;
			Standalone<RangeResultRef> range = wait(tr.getRange(order.keyRange(1), 1, false, true));
			ASSERT(range.size() > 0);
			{
				BinaryReader r(range[0].value, IncludeVersion());
				serializer(r, order);
			}
			for (i = 0; i < order.o_ol_cnt; ++i) {
				OrderLine orderLine;
				orderLine.ol_w_id = order.o_w_id;
				orderLine.ol_d_id = order.o_d_id;
				orderLine.ol_o_id = order.o_id;
				orderLine.ol_number = i;
				Optional<Value> olValue = wait(tr.get(orderLine.key()));
				ASSERT(olValue.present());
				BinaryReader r(olValue.get(), IncludeVersion());
				OrderLine ol;
				serializer(r, ol);
				orderLines.push_back(ol);
			}
		} catch (Error& e) {
			return false;
		}
		return true;
	}

	ACTOR static Future<bool> delivery(TPCC* self, Database cx, int w_id) {
		state ReadYourWritesTransaction tr(cx);
		state int carrier_id = deterministicRandom()->randomInt(0, 10);
		state int d_id;
		state NewOrder newOrder;
		state Order order;
		state double sumAmount = 0.0;
		state Customer customer;
		state int i;
		try {
			for (d_id = 0; d_id < 10; ++d_id) {
				newOrder.no_w_id = w_id;
				newOrder.no_d_id = d_id;
				Standalone<RangeResultRef> range = wait(tr.getRange(newOrder.keyRange(1), 1));
				if (range.size() > 0) {
					{
						BinaryReader r(range[0].value, IncludeVersion());
						serializer(r, newOrder);
					}
					tr.clear(newOrder.key());
					order.o_w_id = w_id;
					order.o_d_id = d_id;
					order.o_id = newOrder.no_o_id;
					Optional<Value> oValue = wait(tr.get(order.key()));
					ASSERT(oValue.present());
					{
						BinaryReader r(oValue.get(), IncludeVersion());
						serializer(r, order);
					}
					order.o_carrier_id = carrier_id;
					{
						BinaryWriter w(IncludeVersion());
						serializer(w, order);
						tr.set(order.key(), w.toValue());
					}
					for (i = 0; i < order.o_ol_cnt; ++i) {
						state OrderLine orderLine;
						orderLine.ol_w_id = order.o_w_id;
						orderLine.ol_d_id = order.o_d_id;
						orderLine.ol_o_id = order.o_id;
						orderLine.ol_number = i;
						Optional<Value> olV = wait(tr.get(orderLine.key()));
						ASSERT(olV.present());
						BinaryReader r(olV.get(), IncludeVersion());
						serializer(r, orderLine);
						orderLine.ol_delivery_d = g_network->now();
						sumAmount += orderLine.ol_amount;
					}
					customer.c_w_id = w_id;
					customer.c_d_id = d_id;
					customer.c_id = order.o_c_id;
					Optional<Value> cV = wait(tr.get(customer.key()));
					ASSERT(cV.present());
					{
						BinaryReader r(cV.get(), IncludeVersion());
						serializer(r, customer);
					}
					customer.c_balance += sumAmount;
					customer.c_delivery_count += 1;
					{
						BinaryWriter w(IncludeVersion());
						serializer(w, customer);
						tr.set(customer.key(), w.toValue());
					}
				}
			}
			wait(tr.commit());
		} catch (Error& e) {
			return false;
		}
		return true;
	}

	ACTOR static Future<bool> stockLevel(TPCC* self, Database cx, int w_id, int d_id) {
		state int threshold = deterministicRandom()->randomInt(10, 21);
		state Transaction tr(cx);
		state District district;
		state OrderLine orderLine;
		state Stock stock;
		state int ol_o_id;
		state int low_stock = 0;
		state int i;
		try {
			district.d_w_id = w_id;
			district.d_id = d_id;
			Optional<Value> dV = wait(tr.get(district.key()));
			ASSERT(dV.present());
			{
				BinaryReader r(dV.get(), IncludeVersion());
				serializer(r, district);
			}
			for (ol_o_id = district.d_next_o_id - 20; ol_o_id < district.d_next_o_id; ++ol_o_id) {
				orderLine.ol_w_id = w_id;
				orderLine.ol_d_id = d_id;
				orderLine.ol_o_id = ol_o_id;
				state Standalone<RangeResultRef> range =
				    wait(tr.getRange(orderLine.keyRange(1), CLIENT_KNOBS->TOO_MANY));
				ASSERT(!range.more);
				ASSERT(range.size() > 0);
				for (i = 0; i < range.size(); ++i) {
					{
						BinaryReader r(range[i].value, IncludeVersion());
						serializer(r, orderLine);
					}
					stock.s_i_id = orderLine.ol_i_id;
					stock.s_w_id = orderLine.ol_w_id;
					Optional<Value> sV = wait(tr.get(stock.key()));
					ASSERT(sV.present());
					{
						BinaryReader r(sV.get(), IncludeVersion());
						serializer(r, stock);
					}
					if (stock.s_quantity < threshold) {
						++low_stock;
					}
				}
			}
		} catch (Error& e) {
			return false;
		}
		return true;
	}

	ACTOR static Future<Void> emulatedUser(TPCC* self, Database cx, int w_id, int d_id) {
		// stagger users
		wait(delay(20.0 * deterministicRandom()->random01()));
		TraceEvent("StartingEmulatedUser").detail("Warehouse", w_id).detail("District", d_id);
		loop {
			auto type = deterministicRandom()->randomInt(0, 100);
			state Future<bool> tx;
			state double txnStartTime = g_network->now();

			if (type < 4) {
				loop {
					tx = stockLevel(self, cx, w_id, d_id);
					bool committed = wait(tx);
					if (self->recordMetrics()) {
						TPCCMetrics::updateMetrics(committed,
						                           self->metrics.successfulStockLevelTransactions,
						                           self->metrics.failedStockLevelTransactions,
						                           txnStartTime,
						                           self->metrics.stockLevelLatencies,
						                           self->metrics.stockLevelResponseTime,
						                           "StockLevel");
					}
					//				wait(delay(2 + deterministicRandom()->random01() * 10));
					if (committed) {
						break;
					}
				}
			} else if (type < 8) {
				loop {
					tx = delivery(self, cx, w_id);
					bool committed = wait(tx);
					if (self->recordMetrics()) {
						TPCCMetrics::updateMetrics(committed,
						                           self->metrics.successfulDeliveryTransactions,
						                           self->metrics.failedDeliveryTransactions,
						                           txnStartTime,
						                           self->metrics.deliveryLatencies,
						                           self->metrics.deliveryResponseTime,
						                           "Delivery");
					}
					//				wait(delay(2 + deterministicRandom()->random01() * 10));
					if (committed) {
						break;
					}
				}
			} else if (type < 12) {
				loop {
					tx = orderStatus(self, cx, w_id);
					bool committed = wait(tx);
					if (self->recordMetrics()) {
						TPCCMetrics::updateMetrics(committed,
						                           self->metrics.successfulOrderStatusTransactions,
						                           self->metrics.failedOrderStatusTransactions,
						                           txnStartTime,
						                           self->metrics.orderStatusLatencies,
						                           self->metrics.orderStatusResponseTime,
						                           "OrderStatus");
					}
					//				wait(delay(2 + deterministicRandom()->random01() * 20));
					if (committed) {
						break;
					}
				}
			} else if (type < 55) {
				loop {
					tx = payment(self, cx, w_id);
					bool committed = wait(tx);
					if (self->recordMetrics()) {
						TPCCMetrics::updateMetrics(committed,
						                           self->metrics.successfulPaymentTransactions,
						                           self->metrics.failedPaymentTransactions,
						                           txnStartTime,
						                           self->metrics.paymentLatencies,
						                           self->metrics.paymentResponseTime,
						                           "Payment");
					}
					//				wait(delay(3 + deterministicRandom()->random01() * 24));
					if (committed) {
						break;
					}
				}
			} else {
				loop {
					tx = newOrder(self, cx, w_id);
					bool committed = wait(tx);
					if (self->recordMetrics()) {
						TPCCMetrics::updateMetrics(committed,
						                           self->metrics.successfulNewOrderTransactions,
						                           self->metrics.failedNewOrderTransactions,
						                           txnStartTime,
						                           self->metrics.newOrderLatencies,
						                           self->metrics.newOrderResponseTime,
						                           "NewOrder");
					}
					//				wait(delay(18 + deterministicRandom()->random01() * 24));
					if (committed) {
						break;
					}
				}
			}
		}
	}

	double transactionsPerMinute() {
		return metrics.successfulNewOrderTransactions * 60.0 / (testDuration - 2 * warmupTime);
	}

	bool recordMetrics() {
		auto now = g_network->now();
		return (now > startTime + warmupTime && now < startTime + testDuration - warmupTime);
	}

	virtual Future<Void> start(Database const& cx) override {
		if (clientId >= clientProcessesUsed)
			return Void();
		return _start(cx, this);
	}

	ACTOR Future<Void> _start(Database cx, TPCC* self) {
		wait(readGlobalState(self, cx));
		self->startTime = g_network->now();
		int startWID, endWID;
		int remain = self->warehousesNum - self->warehousesPerClientProcess * self->clientProcessesUsed;
		if(self->clientId < remain){
			startWID = self->clientId * (self->warehousesPerClientProcess + 1);
			endWID = startWID + self->warehousesPerClientProcess + 1;
		}
		else{
			startWID = remain * (self->warehousesPerClientProcess + 1) +
			           (self->clientId - remain) * self->warehousesPerClientProcess;
			endWID = startWID + self->warehousesPerClientProcess;
		}
		TraceEvent("Start a Client Process")
			.details("warehousesNum", self->warehousesNum)
			.details("clientsProcessesUsed", self->clientProcessesUsed)
			.details("warehousesPerClientProcess", self->warehousesPerClientProcess)
			.details("remain", remain)
		    .details("clientId", self->clientId)
		    .details("startWID", startWID)
		    .details("endWID", endWID);
		ASSERT(remain >= 0);
		ASSERT(endWID <= self->warehousesNum);
		state int w_id;
		state int d_id = 0;
		state int cnt;
		state vector<Future<Void>> emulatedUsers;
		for (w_id = startWID; w_id < endWID; ++w_id) {
			for (cnt = 0; cnt < self->clientsPerWarehouse; ++cnt) {
				emulatedUsers.push_back(
				    timeout(emulatedUser(self, cx, w_id, (d_id++) % 10), self->testDuration, Void()));
			}
		}
		wait(waitForAll(emulatedUsers));
		return Void();
	}

	virtual Future<bool> check(Database const& cx) override {
		return (transactionsPerMinute() > expectedTransactionsPerMinute);
	}
	virtual void getMetrics(vector<PerfMetric>& m) override {
		double multiplier = static_cast<double>(clientCount) / static_cast<double>(clientProcessesUsed);

		m.push_back(PerfMetric("Transactions Per Minute", transactionsPerMinute(), false));

		m.push_back(PerfMetric("Successful StockLevel Transactions", metrics.successfulStockLevelTransactions, false));
		m.push_back(PerfMetric("Successful Delivery Transactions", metrics.successfulDeliveryTransactions, false));
		m.push_back(
		    PerfMetric("Successful OrderStatus Transactions", metrics.successfulOrderStatusTransactions, false));
		m.push_back(PerfMetric("Successful Payment Transactions", metrics.successfulPaymentTransactions, false));
		m.push_back(PerfMetric("Successful NewOrder Transactions", metrics.successfulNewOrderTransactions, false));

		m.push_back(PerfMetric("Failed StockLevel Transactions", metrics.failedStockLevelTransactions, false));
		m.push_back(PerfMetric("Failed Delivery Transactions", metrics.failedDeliveryTransactions, false));
		m.push_back(PerfMetric("Failed OrderStatus Transactions", metrics.failedOrderStatusTransactions, false));
		m.push_back(PerfMetric("Failed Payment Transactions", metrics.failedPaymentTransactions, false));
		m.push_back(PerfMetric("Failed NewOrder Transactions", metrics.failedNewOrderTransactions, false));

		m.push_back(PerfMetric("Mean StockLevel Latency",
		                       (clientId < clientProcessesUsed) ? (multiplier * metrics.stockLevelResponseTime /
		                                                   metrics.successfulStockLevelTransactions)
		                                                : 0.0,
		                       true));
		m.push_back(PerfMetric("Mean Delivery Latency",
		                       (clientId < clientProcessesUsed) ? (multiplier * metrics.deliveryResponseTime /
		                                                   metrics.successfulDeliveryTransactions)
		                                                : 0.0,
		                       true));
		m.push_back(PerfMetric("Mean OrderStatus Repsonse Time",
		                       (clientId < clientProcessesUsed) ? (multiplier * metrics.orderStatusResponseTime /
		                                                   metrics.successfulOrderStatusTransactions)
		                                                : 0.0,
		                       true));
		m.push_back(PerfMetric("Mean Payment Latency",
		                       (clientId < clientProcessesUsed)
		                           ? (multiplier * metrics.paymentResponseTime / metrics.successfulPaymentTransactions)
		                           : 0.0,
		                       true));
		m.push_back(PerfMetric("Mean NewOrder Latency",
		                       (clientId < clientProcessesUsed) ? (multiplier * metrics.newOrderResponseTime /
		                                                   metrics.successfulNewOrderTransactions)
		                                                : 0.0,
		                       true));

		metrics.sort();

		m.push_back(PerfMetric(
		    "Median StockLevel Latency", multiplier * TPCCMetrics::median(metrics.stockLevelLatencies), true));
		m.push_back(
		    PerfMetric("Median Delivery Latency", multiplier * TPCCMetrics::median(metrics.deliveryLatencies), true));
		m.push_back(PerfMetric(
		    "Median OrderStatus Latency", multiplier * TPCCMetrics::median(metrics.orderStatusLatencies), true));
		m.push_back(
		    PerfMetric("Median Payment Latency", multiplier * TPCCMetrics::median(metrics.paymentLatencies), true));
		m.push_back(
		    PerfMetric("Median NewOrder Latency", multiplier * TPCCMetrics::median(metrics.newOrderLatencies), true));

		m.push_back(PerfMetric("90th Percentile StockLevel Latency",
		                       multiplier * TPCCMetrics::percentile_90(metrics.stockLevelLatencies),
		                       true));
		m.push_back(PerfMetric("90th Percentile Delivery Latency",
		                       multiplier * TPCCMetrics::percentile_90(metrics.deliveryLatencies),
		                       true));
		m.push_back(PerfMetric("90th Percentile OrderStatus Latency",
		                       multiplier * TPCCMetrics::percentile_90(metrics.orderStatusLatencies),
		                       true));
		m.push_back(PerfMetric("90th Percentile Payment Latency",
		                       multiplier * TPCCMetrics::percentile_90(metrics.paymentLatencies),
		                       true));
		m.push_back(PerfMetric("90th Percentile NewOrder Latency",
		                       multiplier * TPCCMetrics::percentile_90(metrics.newOrderLatencies),
		                       true));

		m.push_back(PerfMetric("99th Percentile StockLevel Latency",
		                       multiplier * TPCCMetrics::percentile_99(metrics.stockLevelLatencies),
		                       true));
		m.push_back(PerfMetric("99th Percentile Delivery Latency",
		                       multiplier * TPCCMetrics::percentile_99(metrics.deliveryLatencies),
		                       true));
		m.push_back(PerfMetric("99th Percentile OrderStatus Latency",
		                       multiplier * TPCCMetrics::percentile_99(metrics.orderStatusLatencies),
		                       true));
		m.push_back(PerfMetric("99th Percentile Payment Latency",
		                       multiplier * TPCCMetrics::percentile_99(metrics.paymentLatencies),
		                       true));
		m.push_back(PerfMetric("99th Percentile NewOrder Latency",
		                       multiplier * TPCCMetrics::percentile_99(metrics.newOrderLatencies),
		                       true));
	}
};

} // namespace

WorkloadFactory<TPCC> TPCCWorkloadFactory(TPCC::DESCRIPTION);
