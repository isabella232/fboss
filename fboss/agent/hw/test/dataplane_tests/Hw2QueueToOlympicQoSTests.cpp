/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/hw/test/HwLinkStateDependentTest.h"
#include "fboss/agent/hw/test/HwPortUtils.h"
#include "fboss/agent/hw/test/HwTestPacketUtils.h"
#include "fboss/agent/hw/test/TrafficPolicyUtils.h"
#include "fboss/agent/hw/test/dataplane_tests/HwTest2QueueUtils.h"
#include "fboss/agent/hw/test/dataplane_tests/HwTestOlympicUtils.h"
#include "fboss/agent/test/EcmpSetupHelper.h"
#include "fboss/agent/test/ResourceLibUtil.h"

#include "fboss/agent/hw/test/ConfigFactory.h"

#include <folly/IPAddress.h>

namespace facebook::fboss {

class Hw2QueueToOlympicQoSTest : public HwLinkStateDependentTest {
 private:
  void SetUp() override {
    HwLinkStateDependentTest::SetUp();
    helper_ = std::make_unique<utility::EcmpSetupAnyNPorts6>(
        getProgrammedState(), RouterID(0));
  }
  cfg::SwitchConfig initialConfig() const override {
    auto cfg = utility::twoL3IntfConfig(
        getHwSwitch(),
        masterLogicalPortIds()[0],
        masterLogicalPortIds()[1],
        cfg::PortLoopbackMode::MAC);
    /*
     * N.B., On one platform, we have to program qos maps before we program l3
     * interfaces. Even if we enforce that ordering in SaiSwitch, we must still
     * send the qos maps in the same delta as the config with the interface.
     *
     * Since we may want to vary the qos maps per test, we shouldn't program
     * l3 interfaces as part of initial config, and only together with the
     * qos maps.
     */
    utility::add2QueueQosMaps(cfg);
    return cfg;
  }

  void setupECMPForwarding() {
    auto newState = helper_->setupECMPForwarding(
        helper_->resolveNextHops(getProgrammedState(), kEcmpWidth), kEcmpWidth);
    applyNewState(newState);
  }

  std::unique_ptr<facebook::fboss::TxPacket> createUdpPkt(
      uint8_t dscpVal) const {
    auto cpuMac = getPlatform()->getLocalMac();
    auto srcMac = utility::MacAddressGenerator().get(cpuMac.u64NBO() + 1);

    return utility::makeUDPTxPacket(
        getHwSwitch(),
        VlanID(*initialConfig().vlanPorts[0].vlanID_ref()),
        srcMac,
        cpuMac,
        folly::IPAddressV6("2620:0:1cfe:face:b00c::3"),
        folly::IPAddressV6("2620:0:1cfe:face:b00c::4"),
        8000,
        8001,
        static_cast<uint8_t>(dscpVal << 2)); // Trailing 2 bits are for ECN
  }

  void sendUdpPkt(int dscpVal, bool frontPanel) {
    auto txPacket = createUdpPkt(dscpVal);
    // port is in LB mode, so it will egress and immediately loop back.
    // Since it is not re-written, it should hit the pipeline as if it
    // ingressed on the port, and be properly queued.
    if (frontPanel) {
      auto outPort = helper_->ecmpPortDescriptorAt(kEcmpWidth).phyPortID();
      getHwSwitchEnsemble()->ensureSendPacketOutOfPort(
          std::move(txPacket), outPort);
    } else {
      getHwSwitchEnsemble()->ensureSendPacketSwitched(std::move(txPacket));
    }
  }
  void _verifyDscpQueueMappingHelper(
      const std::map<int, std::vector<uint8_t>>& queueToDscp,
      bool frontPanel) {
    for (const auto& entry : queueToDscp) {
      auto queueId = entry.first;
      auto dscpVals = entry.second;

      for (const auto& dscpVal : dscpVals) {
        auto beforeQueueOutPkts = getLatestPortStats(masterLogicalPortIds()[0])
                                      .get_queueOutPackets_()
                                      .at(queueId);
        sendUdpPkt(dscpVal, frontPanel);
        auto afterQueueOutPkts = getLatestPortStats(masterLogicalPortIds()[0])
                                     .get_queueOutPackets_()
                                     .at(queueId);

        EXPECT_EQ(1, afterQueueOutPkts - beforeQueueOutPkts);
      }
    }
  }

 protected:
  void runTest(bool frontPanel) {
    if (!isSupported(HwAsic::Feature::L3_QOS)) {
      return;
    }

    auto setup = [=]() { setupECMPForwarding(); };

    auto verify = [=]() {
      _verifyDscpQueueMappingHelper(utility::k2QueueToDscp(), frontPanel);
    };

    auto setupPostWarmboot = [=]() {
      auto newCfg{initialConfig()};
      utility::addOlympicQueueConfig(&newCfg);
      utility::addOlympicQosMaps(newCfg);
      applyNewConfig(newCfg);
    };

    auto verifyPostWarmboot = [=]() {
      _verifyDscpQueueMappingHelper(utility::kOlympicQueueToDscp(), frontPanel);
    };

    verifyAcrossWarmBoots(setup, verify, setupPostWarmboot, verifyPostWarmboot);
  }
  static inline constexpr auto kEcmpWidth = 1;
  std::unique_ptr<utility::EcmpSetupAnyNPorts6> helper_;
};

TEST_F(Hw2QueueToOlympicQoSTest, verifyDscpToQueueMappingCpu) {
  runTest(false);
}

TEST_F(Hw2QueueToOlympicQoSTest, verifyDscpToQueueMappingFrontPanel) {
  runTest(true);
}

} // namespace facebook::fboss
