/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/hw/sai/switch/SaiSwitchManager.h"

#include "fboss/agent/FbossError.h"
#include "fboss/agent/hw/sai/api/AdapterKeySerializers.h"
#include "fboss/agent/hw/sai/api/SaiApiTable.h"
#include "fboss/agent/hw/sai/api/SwitchApi.h"
#include "fboss/agent/hw/sai/switch/SaiAclTableGroupManager.h"
#include "fboss/agent/hw/sai/switch/SaiManagerTable.h"
#include "fboss/agent/hw/switch_asics/HwAsic.h"
#include "fboss/agent/platforms/sai/SaiPlatform.h"
#include "fboss/agent/state/DeltaFunctions.h"
#include "fboss/agent/state/LoadBalancer.h"
#include "fboss/agent/state/QosPolicy.h"
#include "fboss/agent/state/StateDelta.h"
#include "fboss/agent/state/SwitchState.h"

#include <folly/logging/xlog.h>

extern "C" {
#include <sai.h>
}

DEFINE_uint32(
    counter_refresh_interval,
    1,
    "Counter refresh interval in seconds. Set it to 0 to fetch stats from HW");

namespace {
using namespace facebook::fboss;
sai_hash_algorithm_t toSaiHashAlgo(cfg::HashingAlgorithm algo) {
  switch (algo) {
    case cfg::HashingAlgorithm::CRC16_CCITT:
      return SAI_HASH_ALGORITHM_CRC_CCITT;
    case cfg::HashingAlgorithm::CRC32_LO:
      return SAI_HASH_ALGORITHM_CRC_32LO;
    case cfg::HashingAlgorithm::CRC32_HI:
      return SAI_HASH_ALGORITHM_CRC_32HI;
    case cfg::HashingAlgorithm::CRC32_ETHERNET_LO:
    case cfg::HashingAlgorithm::CRC32_ETHERNET_HI:
    case cfg::HashingAlgorithm::CRC32_KOOPMAN_LO:
    case cfg::HashingAlgorithm::CRC32_KOOPMAN_HI:
    default:
      throw FbossError("Unsupported hash algorithm :", algo);
  }
}
} // namespace

namespace facebook::fboss {

SaiSwitchManager::SaiSwitchManager(
    SaiManagerTable* managerTable,
    SaiPlatform* platform,
    BootType bootType)
    : managerTable_(managerTable), platform_(platform) {
  if (bootType == BootType::WARM_BOOT) {
    // Extract switch adapter key and create switch only with the mandatory
    // init attribute (warm boot path)
    auto& switchApi = SaiApiTable::getInstance()->switchApi();
    auto newSwitchId = switchApi.create<SaiSwitchTraits>(
        platform->getSwitchAttributes(true), 0 /* switch id; ignored */);
    // Load all switch attributes
    switch_ = std::make_unique<SaiSwitchObj>(newSwitchId);
    switch_->setOptionalAttribute(
        SaiSwitchTraits::Attributes::SrcMac{platform->getLocalMac()});
    switch_->setOptionalAttribute(SaiSwitchTraits::Attributes::MacAgingTime{
        platform->getDefaultMacAgingTime()});
  } else {
    switch_ = std::make_unique<SaiSwitchObj>(
        std::monostate(),
        platform->getSwitchAttributes(false),
        0 /* fake switch id; ignored */);
  }
  initCpuPort();
}

SwitchSaiId SaiSwitchManager::getSwitchSaiId() const {
  if (!switch_) {
    throw FbossError("failed to get switch id: switch not initialized");
  }
  return switch_->adapterKey();
}

void SaiSwitchManager::resetHashes() {
  ecmpV4Hash_.reset();
  ecmpV6Hash_.reset();
}

void SaiSwitchManager::resetQosMaps() {
  // Since Platform owns Asic, as well as SaiSwitch, which results
  // in blowing up asic before switch (due to destructor order details)
  // as a result, we can only rely on the validity of the global map pointer
  // to gate reset. This should only be true if resetting is supported and
  // would do something meaningful.
  if (globalDscpToTcQosMap_) {
    switch_->setOptionalAttribute(
        SaiSwitchTraits::Attributes::QosDscpToTcMap{SAI_NULL_OBJECT_ID});
    switch_->setOptionalAttribute(
        SaiSwitchTraits::Attributes::QosTcToQueueMap{SAI_NULL_OBJECT_ID});
    globalDscpToTcQosMap_.reset();
    globalTcToQueueQosMap_.reset();
  }
}

void SaiSwitchManager::programEcmpLoadBalancerParams(
    std::optional<sai_uint32_t> seed,
    std::optional<cfg::HashingAlgorithm> algo) {
  auto hashSeed = seed ? seed.value() : 0;
  auto hashAlgo = algo ? toSaiHashAlgo(algo.value()) : SAI_HASH_ALGORITHM_CRC;
  switch_->setOptionalAttribute(
      SaiSwitchTraits::Attributes::EcmpDefaultHashSeed{hashSeed});
  switch_->setOptionalAttribute(
      SaiSwitchTraits::Attributes::EcmpDefaultHashAlgorithm{hashAlgo});
}

void SaiSwitchManager::addOrUpdateEcmpLoadBalancer(
    const std::shared_ptr<LoadBalancer>& newLb) {
  programEcmpLoadBalancerParams(newLb->getSeed(), newLb->getAlgorithm());

  if (newLb->getIPv4Fields().size()) {
    // v4 ECMP
    cfg::Fields v4EcmpHashFields;
    v4EcmpHashFields.ipv4Fields_ref()->insert(
        newLb->getIPv4Fields().begin(), newLb->getIPv4Fields().end());
    v4EcmpHashFields.transportFields_ref()->insert(
        newLb->getTransportFields().begin(), newLb->getTransportFields().end());
    ecmpV4Hash_ = managerTable_->hashManager().getOrCreate(v4EcmpHashFields);
    // Set the new ecmp v4 hash attribute on switch obj
    switch_->setOptionalAttribute(
        SaiSwitchTraits::Attributes::EcmpHashV4{ecmpV4Hash_->adapterKey()});
  }
  if (newLb->getIPv6Fields().size()) {
    // v6 ECMP
    cfg::Fields v6EcmpHashFields;
    v6EcmpHashFields.ipv6Fields_ref()->insert(
        newLb->getIPv6Fields().begin(), newLb->getIPv6Fields().end());
    v6EcmpHashFields.transportFields_ref()->insert(
        newLb->getTransportFields().begin(), newLb->getTransportFields().end());
    ecmpV6Hash_ = managerTable_->hashManager().getOrCreate(v6EcmpHashFields);
    // Set the new ecmp v6 hash attribute on switch obj
    switch_->setOptionalAttribute(
        SaiSwitchTraits::Attributes::EcmpHashV6{ecmpV6Hash_->adapterKey()});
  }
}

void SaiSwitchManager::programLagLoadBalancerParams(
    std::optional<sai_uint32_t> seed,
    std::optional<cfg::HashingAlgorithm> algo) {
  auto hashSeed = seed ? seed.value() : 0;
  auto hashAlgo = algo ? toSaiHashAlgo(algo.value()) : SAI_HASH_ALGORITHM_CRC;
  switch_->setOptionalAttribute(
      SaiSwitchTraits::Attributes::LagDefaultHashSeed{hashSeed});
  switch_->setOptionalAttribute(
      SaiSwitchTraits::Attributes::LagDefaultHashAlgorithm{hashAlgo});
}

void SaiSwitchManager::addOrUpdateLagLoadBalancer(
    const std::shared_ptr<LoadBalancer>& newLb) {
  programLagLoadBalancerParams(newLb->getSeed(), newLb->getAlgorithm());

  if (newLb->getIPv4Fields().size()) {
    // v4 LAG
    cfg::Fields v4LagHashFields;
    v4LagHashFields.ipv4Fields_ref()->insert(
        newLb->getIPv4Fields().begin(), newLb->getIPv4Fields().end());
    v4LagHashFields.transportFields_ref()->insert(
        newLb->getTransportFields().begin(), newLb->getTransportFields().end());
    lagV4Hash_ = managerTable_->hashManager().getOrCreate(v4LagHashFields);
    // Set the new lag v4 hash attribute on switch obj
    switch_->setOptionalAttribute(
        SaiSwitchTraits::Attributes::LagHashV4{lagV4Hash_->adapterKey()});
  }
  if (newLb->getIPv6Fields().size()) {
    // v6 LAG
    cfg::Fields v6LagHashFields;
    v6LagHashFields.ipv6Fields_ref()->insert(
        newLb->getIPv6Fields().begin(), newLb->getIPv6Fields().end());
    v6LagHashFields.transportFields_ref()->insert(
        newLb->getTransportFields().begin(), newLb->getTransportFields().end());
    lagV6Hash_ = managerTable_->hashManager().getOrCreate(v6LagHashFields);
    // Set the new lag v6 hash attribute on switch obj
    switch_->setOptionalAttribute(
        SaiSwitchTraits::Attributes::LagHashV6{lagV6Hash_->adapterKey()});
  }
}

void SaiSwitchManager::addOrUpdateLoadBalancer(
    const std::shared_ptr<LoadBalancer>& newLb) {
  if (newLb->getID() == cfg::LoadBalancerID::AGGREGATE_PORT) {
    return addOrUpdateLagLoadBalancer(newLb);
  }
  addOrUpdateEcmpLoadBalancer(newLb);
}

void SaiSwitchManager::changeLoadBalancer(
    const std::shared_ptr<LoadBalancer>& /*oldLb*/,
    const std::shared_ptr<LoadBalancer>& newLb) {
  return addOrUpdateLoadBalancer(newLb);
}

void SaiSwitchManager::removeLoadBalancer(
    const std::shared_ptr<LoadBalancer>& oldLb) {
  if (oldLb->getID() == cfg::LoadBalancerID::AGGREGATE_PORT) {
    programLagLoadBalancerParams(std::nullopt, std::nullopt);
    lagV4Hash_.reset();
    lagV6Hash_.reset();
    return;
  }
  programEcmpLoadBalancerParams(std::nullopt, std::nullopt);
  ecmpV4Hash_.reset();
  ecmpV6Hash_.reset();
}

void SaiSwitchManager::setQosPolicy() {
  auto& qosMapManager = managerTable_->qosMapManager();
  XLOG(INFO) << "Set default qos map";
  auto qosMapHandle = qosMapManager.getQosMap();
  globalDscpToTcQosMap_ = qosMapHandle->dscpQosMap;
  globalTcToQueueQosMap_ = qosMapHandle->tcQosMap;
  // set switch attrs to oids
  switch_->setOptionalAttribute(SaiSwitchTraits::Attributes::QosDscpToTcMap{
      globalDscpToTcQosMap_->adapterKey()});
  switch_->setOptionalAttribute(SaiSwitchTraits::Attributes::QosTcToQueueMap{
      globalTcToQueueQosMap_->adapterKey()});
}

void SaiSwitchManager::clearQosPolicy() {
  XLOG(INFO) << "Reset default qos map";
  resetQosMaps();
}

void SaiSwitchManager::setIngressAcl() {
  auto aclTableGroupHandle = managerTable_->aclTableGroupManager()
                                 .getAclTableGroupHandle(SAI_ACL_STAGE_INGRESS)
                                 ->aclTableGroup;
  XLOG(INFO) << "Set ingress ACL; " << aclTableGroupHandle->adapterKey();
  switch_->setOptionalAttribute(SaiSwitchTraits::Attributes::IngressAcl{
      aclTableGroupHandle->adapterKey()});
}

void SaiSwitchManager::resetIngressAcl() {
  switch_->setOptionalAttribute(
      SaiSwitchTraits::Attributes::IngressAcl{SAI_NULL_OBJECT_ID});
}

void SaiSwitchManager::gracefulExit() {
  // On graceful exit we trigger the warm boot path on
  // ASIC by destroying the switch (and thus calling the
  // remove switch function
  // https://github.com/opencomputeproject/SAI/blob/master/inc/saiswitch.h#L2514
  // Other objects are left intact to preserve data plane
  // forwarding during warm boot
  switch_.reset();
}

void SaiSwitchManager::setMacAgingSeconds(sai_uint32_t agingSeconds) {
  switch_->setOptionalAttribute(
      SaiSwitchTraits::Attributes::MacAgingTime{agingSeconds});
}
sai_uint32_t SaiSwitchManager::getMacAgingSeconds() const {
  return GET_OPT_ATTR(Switch, MacAgingTime, switch_->attributes());
}

void SaiSwitchManager::setTamObject(std::vector<sai_object_id_t> tamObject) {
  switch_->setOptionalAttribute(
      SaiSwitchTraits::Attributes::TamObject{std::move(tamObject)});
}

void SaiSwitchManager::resetTamObject() {
  switch_->setOptionalAttribute(SaiSwitchTraits::Attributes::TamObject{
      std::vector<sai_object_id_t>{SAI_NULL_OBJECT_ID}});
}

void SaiSwitchManager::setupCounterRefreshInterval() {
  switch_->setOptionalAttribute(
      SaiSwitchTraits::Attributes::CounterRefreshInterval{
          FLAGS_counter_refresh_interval});
}

sai_object_id_t SaiSwitchManager::getDefaultVlanAdapterKey() const {
  return SaiApiTable::getInstance()->switchApi().getAttribute(
      switch_->adapterKey(), SaiSwitchTraits::Attributes::DefaultVlanId{});
}

} // namespace facebook::fboss
