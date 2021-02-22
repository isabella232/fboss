// Copyright 2004-present Facebook. All Rights Reserved.

#include "fboss/agent/hw/sai/switch/SaiLagManager.h"

#include "fboss/agent/hw/sai/switch/ConcurrentIndices.h"
#include "fboss/agent/hw/sai/switch/SaiManagerTable.h"
#include "fboss/agent/hw/sai/switch/SaiPortManager.h"

#include "fboss/agent/hw/sai/store/SaiStore.h"

#include <folly/container/Enumerate.h>

namespace facebook::fboss {

void SaiLagManager::addLag(
    const std::shared_ptr<AggregatePort>& aggregatePort) {
  XLOG(INFO) << "adding aggregate port : " << aggregatePort->getID();

  auto name = aggregatePort->getName();
  std::array<char, 32> labelValue{};
  for (auto i = 0; i < 32 && i < name.length(); i++) {
    labelValue[i] = name[i];
  }
  auto& lagStore = SaiStore::getInstance()->get<SaiLagTraits>();
  auto lag = lagStore.setObject(
      SaiLagTraits::Attributes::Label{labelValue}, std::tuple<>());
  std::map<PortSaiId, std::shared_ptr<SaiLagMember>> members;
  for (auto iter : folly::enumerate(aggregatePort->subportAndFwdState())) {
    auto [subPort, fwdState] = *iter;
    if (fwdState == AggregatePort::Forwarding::DISABLED) {
      // subport disabled for forwarding
      continue;
    }
    members.emplace(addMember(lag, aggregatePort->getID(), subPort));
  }

  auto handle = std::make_unique<SaiLagHandle>();
  // create bridge port for LAG
  handle->bridgePort = managerTable_->bridgeManager().addBridgePort(
      SaiPortDescriptor(aggregatePort->getID()),
      PortDescriptorSaiId(lag->adapterKey()));
  handle->members = std::move(members);
  handle->lag = std::move(lag);
  handle->minimumLinkCount = aggregatePort->getMinimumLinkCount();
  handles_.emplace(aggregatePort->getID(), std::move(handle));
}

void SaiLagManager::removeLag(
    const std::shared_ptr<AggregatePort>& aggregatePort) {
  XLOG(INFO) << "removing aggregate port : " << aggregatePort->getID();
  auto iter = handles_.find(aggregatePort->getID());
  if (iter == handles_.end()) {
    throw FbossError(
        "attempting to non-existing remove LAG ", aggregatePort->getID());
  }
  removeLagHandle(iter->first, iter->second.get());
  handles_.erase(iter->first);
}

void SaiLagManager::changeLag(
    const std::shared_ptr<AggregatePort>& oldAggregatePort,
    const std::shared_ptr<AggregatePort>& newAggregatePort) {
  auto handleIter = handles_.find(oldAggregatePort->getID());
  CHECK(handleIter != handles_.end());
  auto& saiLagHandle = handleIter->second;

  saiLagHandle->minimumLinkCount = newAggregatePort->getMinimumLinkCount();

  auto oldPortAndFwdState = oldAggregatePort->subportAndFwdState();
  auto newPortAndFwdState = newAggregatePort->subportAndFwdState();
  auto oldIter = oldPortAndFwdState.begin();
  auto newIter = newPortAndFwdState.begin();

  while (oldIter != oldPortAndFwdState.end() &&
         newIter != newPortAndFwdState.end()) {
    if (oldIter->first < newIter->first) {
      if (oldIter->second == AggregatePort::Forwarding::ENABLED) {
        // remove member
        removeMember(oldAggregatePort->getID(), oldIter->first);
      }
      oldIter++;
    } else if (newIter->first < oldIter->first) {
      if (newIter->second == AggregatePort::Forwarding::ENABLED) {
        // add member
        saiLagHandle->members.emplace(addMember(
            saiLagHandle->lag, newAggregatePort->getID(), newIter->first));
      }
      newIter++;
    } else if (oldIter->second != newIter->second) {
      // forwarding state changed
      if (newIter->second == AggregatePort::Forwarding::ENABLED) {
        // add member
        saiLagHandle->members.emplace(addMember(
            saiLagHandle->lag, newAggregatePort->getID(), newIter->first));

      } else {
        // remove member
        removeMember(newAggregatePort->getID(), newIter->first);
      }
      newIter++;
      oldIter++;
    }
    while (oldIter != oldPortAndFwdState.end()) {
      if (oldIter->second == AggregatePort::Forwarding::ENABLED) {
        // remove member
        removeMember(oldAggregatePort->getID(), oldIter->first);
      }
      oldIter++;
    }
    while (newIter != newPortAndFwdState.end()) {
      if (newIter->second == AggregatePort::Forwarding::ENABLED) {
        // remove member
        saiLagHandle->members.emplace(addMember(
            saiLagHandle->lag, newAggregatePort->getID(), newIter->first));
      }
      newIter++;
    }
  }
}

std::pair<PortSaiId, std::shared_ptr<SaiLagMember>> SaiLagManager::addMember(
    const std::shared_ptr<SaiLag>& lag,
    AggregatePortID aggregatePortID,
    PortID subPort) {
  auto portHandle = managerTable_->portManager().getPortHandle(subPort);
  CHECK(portHandle);
  auto saiPortId = portHandle->port->adapterKey();
  auto saiLagId = lag->adapterKey();

  SaiLagMemberTraits::CreateAttributes attrs{saiLagId, saiPortId};
  auto& lagMemberStore = SaiStore::getInstance()->get<SaiLagMemberTraits>();
  auto member = lagMemberStore.setObject(attrs, attrs);
  concurrentIndices_->memberPort2AggregatePortIds.emplace(
      saiPortId, aggregatePortID);
  return {saiPortId, member};
}

void SaiLagManager::removeMember(AggregatePortID aggPort, PortID subPort) {
  auto iter = handles_.find(aggPort);
  CHECK(iter != handles_.end());
  auto portHandle = managerTable_->portManager().getPortHandle(subPort);
  if (!portHandle) {
    // link down will remove lag member, resulting in LACP machine processing
    // lag shrink. this  will also cause LACP machine to issue state delta to
    // remove  lag member. so  ignore the lag member  removal which  could be
    // issued second time by sw switch.
    XLOG(DBG6) << "member " << subPort << " of aggregate port " << aggPort
               << " was already removed.";
    return;
  }
  auto saiPortId = portHandle->port->adapterKey();
  iter->second->members.erase(saiPortId);
  concurrentIndices_->memberPort2AggregatePortIds.erase(saiPortId);
}

SaiLagHandle* FOLLY_NULLABLE
SaiLagManager::getLagHandleIf(AggregatePortID aggregatePortID) const {
  auto iter = handles_.find(aggregatePortID);
  if (iter == handles_.end()) {
    return nullptr;
  }
  return iter->second.get();
}

SaiLagHandle* SaiLagManager::getLagHandle(
    AggregatePortID aggregatePortID) const {
  auto* handle = getLagHandleIf(aggregatePortID);
  if (!handle) {
    return handle;
  }
  throw FbossError("handle for aggregate port ", aggregatePortID, " not found");
}

bool SaiLagManager::isMinimumLinkMet(AggregatePortID aggregatePortID) const {
  const auto* handle = getLagHandle(aggregatePortID);
  return handle->minimumLinkCount <= handle->members.size();
}

void SaiLagManager::removeLagHandle(
    AggregatePortID aggPort,
    SaiLagHandle* handle) {
  // remove members
  while (!handle->members.empty()) {
    auto membersIter = handle->members.begin();
    auto portSaiId = membersIter->first;
    auto iter = concurrentIndices_->portIds.find(portSaiId);
    CHECK(iter != concurrentIndices_->portIds.end());
    removeMember(aggPort, iter->second);
  }
  // remove bridge port
  handle->bridgePort.reset();
  // remove lag
  handle->lag.reset();
}

SaiLagManager::~SaiLagManager() {
  for (auto& handlesIter : handles_) {
    auto& [aggPortID, handle] = handlesIter;
    removeLagHandle(aggPortID, handle.get());
  }
}
} // namespace facebook::fboss
