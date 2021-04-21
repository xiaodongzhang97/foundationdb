/*
 * TLogGroup.actor.cpp
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

#include "fdbclient/CommitProxyInterface.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/SystemData.h"
#include "fdbrpc/Replication.h"
#include "fdbrpc/ReplicationPolicy.h"
#include "fdbserver/TLogGroup.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "flow/Arena.h"
#include "flow/FastRef.h"
#include "flow/flow.h"
#include "flow/Error.h"
#include "flow/UnitTest.h"
#include "flow/serialize.h"
#include <iostream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

TLogGroupCollection::TLogGroupCollection(const Reference<IReplicationPolicy>& policy, int numGroups, int groupSize)
  : policy(policy), targetNumGroups(numGroups), GROUP_SIZE(groupSize) {}

const std::vector<TLogGroupRef>& TLogGroupCollection::groups() const {
	return recruitedGroups;
}

int TLogGroupCollection::groupSize() const {
	return GROUP_SIZE;
}

void TLogGroupCollection::addWorkers(const std::vector<WorkerInterface>& logWorkers) {
	for (const auto& worker : logWorkers) {
		recruitMap.emplace(worker.id(), TLogWorkerData::fromInterface(worker));
	}
}

void TLogGroupCollection::recruitEverything() {
	std::unordered_set<UID> selectedServers;
	std::vector<TLogWorkerData*> bestSet;
	auto localityMap = buildLocalityMap(selectedServers);

	printf("---> Going to recruit %d groups out of %d servers\n", targetNumGroups, localityMap.size());
	while (recruitedGroups.size() < targetNumGroups) {
		bestSet.clear();

		// TODO: We are doing this randomly for now, but should make sure number of teams served by each
		//   tLog server is approximately same.
		if (localityMap.selectReplicas(policy, bestSet)) {
			// ASSERT_WE_THINK(bestSet.size() == GROUP_SIZE);

			printf("----> Recruiting Group: %d\n", bestSet.size());
			Reference<TLogGroup> group(new TLogGroup());
			for (auto& entry : bestSet) {
				printf("Recruited Group: %s\n", entry->locality.processId().get().toString().c_str());
				group->addServer(Reference<TLogWorkerData>(entry));
				selectedServers.insert(entry->id);
			}

			newGroups.push_back(group);
		} else {
			// TODO: We may have scenarios (simulation), with recruits/zone's < RF. Handle that case.
		}
	}

	// ASSERT_EQ(newGroups.size(), numTeams);
}

LocalityMap<TLogWorkerData> TLogGroupCollection::buildLocalityMap(const std::unordered_set<UID>& ignoreServers) {
	LocalityMap<TLogWorkerData> localityMap;
	for (const auto& [_, logInterf] : recruitMap) {
		if (ignoreServers.find(logInterf->id) != ignoreServers.end()) {
			// Server already selected.
			continue;
		}
		localityMap.add(logInterf->locality, logInterf.getPtr());
	}
	return localityMap;
}

void TLogGroupCollection::storeState(CommitTransactionRequest* recoveryCommitReq) {
	CommitTransactionRef& tr = recoveryCommitReq->transaction;
	const auto& serversPrefix = LiteralStringRef("/servers");

	tr.clear(recoveryCommitReq->arena, tLogGroupKeys);
	for (const auto& group : recruitedGroups) {
		const auto& groupPrefix = tLogGroupKeyFor(group->id());
		std::cout << "---> Storing " << group->toString() << std::endl;
		tr.set(recoveryCommitReq->arena, groupPrefix.withSuffix(serversPrefix), group->toValue());
	}
}

void TLogGroupCollection::loadState(const Standalone<RangeResultRef>& store,
                                    const std::vector<WorkerInterface>& recruits) {

	std::unordered_map<UID, WorkerInterface> idToInterfMap;
	for (const auto& interf : recruits) {
		idToInterfMap[interf.id()] = interf;
	}

	for (int ii = 0; ii < store.size(); ++ii) {
		auto groupId = decodeTLogGroupKey(store[ii].key);
		auto group = TLogGroup::fromValue(groupId, store[ii].value, idToInterfMap);
		recruitedGroups.push_back(group);
	}
}

void TLogGroup::addServer(const TLogWorkerDataRef& workerData) {
	serverMap.emplace(workerData->id, workerData);
}

std::vector<TLogWorkerDataRef> TLogGroup::servers() const {
	std::vector<TLogWorkerDataRef> results;
	for (auto& [_, worker] : serverMap) {
		results.push_back(worker);
	}
	return results;
}

Standalone<StringRef> TLogGroup::toValue() const {
	BinaryWriter result(Unversioned()); // TODO: Add version
	result << serverMap.size();
	for (auto& [id, _] : serverMap) {
		result << id;
	}
	return result.toValue();
}

TLogGroupRef TLogGroup::fromValue(UID groupId,
                                  StringRef value,
                                  const std::unordered_map<UID, WorkerInterface>& recruits) {
	BinaryReader reader(value, Unversioned()); // TODO : Add version
	int size;
	reader >> size;

	auto group = makeReference<TLogGroup>(groupId);
	for (int ii = 0; ii < size; ++ii) {
		UID id;
		reader >> id;

		auto interf = recruits.find(id);
		if (interf == recruits.end()) {
			// TODO: Can happen if the worker died since. Handle the case.
		}
		group->addServer(TLogWorkerData::fromInterface(interf->second));
	}

	return group;
}

std::vector<UID> TLogGroup::serverIds() const {
	std::vector<UID> results;
	for (auto& [id, _] : serverMap) {
		results.push_back(id);
	}
	return results;
}

std::string TLogGroup::toString() const {
	return format("TLogGroup[%s]{logs=%s}", groupId.toString().c_str(), describe(serverIds()).c_str());
}

std::string TLogWorkerData::toString() const {
	return format("TLogWorkerData{id=%s, address=%s, locality=%s",
	              id.toString().c_str(),
	              address.toString().c_str(),
	              locality.toString().c_str());
}

//-------------------------------------------------------------------------------------------------------------------
// Unit Tests

namespace testTLogGroup {

// Returns a vector of size 'processCount' containing mocked WorkerInterface, spread across diffeent localities.
std::vector<WorkerInterface> testTLogGroupRecruits(int processCount) {
	std::vector<WorkerInterface> recruits;
	for (int id = 1; id <= processCount; id++) {
		UID uid(id, 0);
		WorkerInterface interface;
		interface.initEndpoints();

		int process_id = id;
		int dc_id = process_id / 1000;
		int data_hall_id = process_id / 100;
		int zone_id = process_id / 10;
		int machine_id = process_id / 5;

		printf("testMachine: process_id:%d zone_id:%d machine_id:%d ip_addr:%s\n",
		       process_id,
		       zone_id,
		       machine_id,
		       interface.address().toString().c_str());
		interface.locality.set(LiteralStringRef("processid"), Standalone<StringRef>(std::to_string(process_id)));
		interface.locality.set(LiteralStringRef("machineid"), Standalone<StringRef>(std::to_string(machine_id)));
		interface.locality.set(LiteralStringRef("zoneid"), Standalone<StringRef>(std::to_string(zone_id)));
		interface.locality.set(LiteralStringRef("data_hall"), Standalone<StringRef>(std::to_string(data_hall_id)));
		interface.locality.set(LiteralStringRef("dcid"), Standalone<StringRef>(std::to_string(dc_id)));
		recruits.push_back(interface);
	}
	return recruits;
}

void printTLogGroup(const TLogGroupRef& group) {
	using std::cout;
	using std::endl;

	cout << format("  --> TLogGroup [id = %s]", group->id().toString().c_str()) << endl;
	for (const auto& server : group->servers()) {
		cout << "      - " << server->toString() << endl;
	}
}

void printTLogGroupCollection(const TLogGroupCollection& collection) {
	using std::cout;
	using std::endl;

	cout << format("-> TLogGroupCollection {GroupSize = %d [NumRecruits = %d, NumRecruitedGroups = %d]",
	               collection.groupSize(),
	               0, // TODO
	               collection.groups().size())
	     << endl;

	for (const auto& group : collection.groups()) {
		printTLogGroup(group);
	}
}

// Checks if each TLog belongs to only one TLogGroup in 'collection', number of workers inside
// each group is equal to 'groupSize' and the total number of recruited workers iseequal to
// 'totalProcesses', or else will fail assertion.
void checkGroupMembersUnique(const TLogGroupCollection& collection, int groupSize, int totalProcesses) {
	const auto& groups = collection.groups();
	std::unordered_set<UID> foundSoFar;

	int total = 0;
	for (const auto& group : groups) {
		auto servers = group->servers();
		ASSERT_EQ(servers.size(), groupSize);
		for (const auto& s : servers) {
			ASSERT(foundSoFar.find(s->id) == foundSoFar.end());
			foundSoFar.insert(s->id);
			++total;
		}
	}

	ASSERT_EQ(total, totalProcesses);
}

} // namespace testTLogGroup

TEST_CASE("/fdbserver/TLogGroup/basic") {
	using namespace testTLogGroup;

	const int TOTAL_PROCESSES = 27;
	const int GROUP_SIZE = 3;
	const int NUM_GROUPS = 20;

	Reference<IReplicationPolicy> policy = Reference<IReplicationPolicy>(
	    new PolicyAcross(GROUP_SIZE, "zoneid", Reference<IReplicationPolicy>(new PolicyOne())));
	// Reference<IReplicationPolicy> policy = makeReference<PolicyAnd>({
	//     makeReference<PolicyAcross>(GROUP_SIZE, "machineid", Reference<IReplicationPolicy>(new PolicyOne())),
	//     makeReference<PolicyAcross>(GROUP_SIZE, "zoneid", Reference<IReplicationPolicy>(new PolicyOne())),
	// });
	std::vector<WorkerInterface> recruits = testTLogGroupRecruits(TOTAL_PROCESSES);

	TLogGroupCollection collection(policy, NUM_GROUPS, GROUP_SIZE);
	collection.addWorkers(recruits);
	collection.recruitEverything();

	printTLogGroupCollection(collection);
	checkGroupMembersUnique(collection, GROUP_SIZE, TOTAL_PROCESSES);
	return Void();
}
