/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <folly/IPAddressV6.h>

#include "fboss/agent/hw/switch_asics/HwAsic.h"
#include "fboss/agent/hw/test/ConfigFactory.h"
#include "fboss/agent/hw/test/HwLinkStateDependentTest.h"
#include "fboss/agent/hw/test/HwTestAclUtils.h"
#include "fboss/agent/hw/test/HwTestCoppUtils.h"
#include "fboss/agent/hw/test/HwTestMplsUtils.h"
#include "fboss/agent/hw/test/HwTestPacketSnooper.h"
#include "fboss/agent/hw/test/HwTestPacketTrapEntry.h"
#include "fboss/agent/hw/test/HwTestPacketUtils.h"
#include "fboss/agent/hw/test/TrafficPolicyUtils.h"
#include "fboss/agent/packet/PktFactory.h"
#include "fboss/agent/packet/PktUtil.h"
#include "fboss/agent/state/LabelForwardingEntry.h"
#include "fboss/agent/state/RouteNextHop.h"
#include "fboss/agent/test/EcmpSetupHelper.h"

namespace {
const facebook::fboss::LabelForwardingEntry::Label kTopLabel{1101};
constexpr auto kGetQueueOutPktsRetryTimes = 5;
} // namespace

namespace facebook::fboss {

class HwMPLSTest : public HwLinkStateDependentTest {
 protected:
  void SetUp() override {
    HwLinkStateDependentTest::SetUp();
    ecmpHelper_ = std::make_unique<utility::EcmpSetupTargetedPorts6>(
        getProgrammedState(), RouterID(0));

    ecmpSwapHelper_ = std::make_unique<
        utility::MplsEcmpSetupTargetedPorts<folly::IPAddressV6>>(
        getProgrammedState(),
        kTopLabel,
        LabelForwardingAction::LabelForwardingType::SWAP);
  }

  cfg::SwitchConfig initialConfig() const override {
    std::vector<PortID> ports = {
        masterLogicalPortIds()[0],
        masterLogicalPortIds()[1],
    };
    auto config = utility::onePortPerVlanConfig(
        getHwSwitch(), std::move(ports), cfg::PortLoopbackMode::MAC, true);

    cfg::QosMap qosMap;
    for (auto tc = 0; tc < 8; tc++) {
      // setup ingress qos map for dscp
      cfg::DscpQosMap dscpMap;
      *dscpMap.internalTrafficClass_ref() = tc;
      for (auto dscp = 0; dscp < 8; dscp++) {
        dscpMap.fromDscpToTrafficClass_ref()->push_back(8 * tc + dscp);
      }

      // setup egress qos map for tc
      cfg::ExpQosMap expMap;
      *expMap.internalTrafficClass_ref() = tc;
      expMap.fromExpToTrafficClass_ref()->push_back(tc);
      expMap.fromTrafficClassToExp_ref() = 7 - tc;

      qosMap.dscpMaps_ref()->push_back(dscpMap);
      qosMap.expMaps_ref()->push_back(expMap);
    }
    config.qosPolicies_ref()->resize(1);
    *config.qosPolicies_ref()[0].name_ref() = "qp";
    config.qosPolicies_ref()[0].qosMap_ref() = qosMap;

    cfg::TrafficPolicyConfig policy;
    policy.defaultQosPolicy_ref() = "qp";
    config.dataPlaneTrafficPolicy_ref() = policy;

    utility::setDefaultCpuTrafficPolicyConfig(config, getAsic());
    utility::addCpuQueueConfig(config, getAsic());

    return config;
  }

  HwSwitchEnsemble::Features featuresDesired() const override {
    return {HwSwitchEnsemble::LINKSCAN, HwSwitchEnsemble::PACKET_RX};
  }

  void addRoute(
      folly::IPAddressV6 prefix,
      uint8_t mask,
      PortDescriptor port,
      LabelForwardingAction::LabelStack stack = {}) {
    applyNewState(ecmpHelper_->resolveNextHops(
        getProgrammedState(),
        {
            port,
        }));

    if (stack.empty()) {
      ecmpHelper_->programRoutes(
          getRouteUpdater(), {port}, {RoutePrefixV6{prefix, mask}});
    } else {
      ecmpHelper_->programIp2MplsRoutes(
          getRouteUpdater(),
          {port},
          {{port, std::move(stack)}},
          {RoutePrefixV6{prefix, mask}});
    }
  }

  LabelForwardingEntry::Label programLabelSwap(PortDescriptor port) {
    auto state = ecmpHelper_->resolveNextHops(
        getProgrammedState(),
        {
            port,
        });
    applyNewState(ecmpSwapHelper_->setupECMPForwarding(state, {port}));
    auto swapNextHop = ecmpSwapHelper_->nhop(port);
    return swapNextHop.action.swapWith().value();
  }

  void programLabelPop(LabelForwardingEntry::Label label) {
    // program MPLS route to POP
    auto state = getProgrammedState();
    state = state->clone();
    auto* labelFib = state->getLabelForwardingInformationBase()->modify(&state);

    LabelForwardingAction popAndLookup(
        LabelForwardingAction::LabelForwardingType::POP_AND_LOOKUP);
    UnresolvedNextHop nexthop(folly::IPAddress("::1"), 1, popAndLookup);
    labelFib->programLabel(
        &state,
        label,
        ClientID::STATIC_ROUTE,
        AdminDistance::STATIC_ROUTE,
        {nexthop});
    applyNewState(state);
  }

  std::unique_ptr<utility::MplsEcmpSetupTargetedPorts<folly::IPAddressV6>>
      ecmpSwapHelper_;

  void sendL3Packet(
      folly::IPAddressV6 dst,
      PortID from,
      std::optional<DSCP> dscp = std::nullopt) {
    CHECK(ecmpHelper_);
    auto vlanId = utility::firstVlanID(initialConfig());
    // construct eth hdr
    const auto intfMac = utility::getInterfaceMac(getProgrammedState(), vlanId);

    EthHdr::VlanTags_t vlans{
        VlanTag(vlanId, static_cast<uint16_t>(ETHERTYPE::ETHERTYPE_VLAN))};

    EthHdr eth{intfMac, intfMac, std::move(vlans), 0x86DD};
    // construct l3 hdr
    IPv6Hdr ip6{folly::IPAddressV6("1::10"), dst};
    ip6.nextHeader = 59; /* no next header */
    if (dscp) {
      ip6.trafficClass = (dscp.value() << 2); // last two bits are ECN
    }
    // get tx packet
    auto pkt =
        utility::EthFrame(eth, utility::IPPacket<folly::IPAddressV6>(ip6))
            .getTxPacket(getHwSwitch());
    // send pkt on src port, let it loop back in switch and be l3 switched
    getHwSwitchEnsemble()->ensureSendPacketOutOfPort(std::move(pkt), from);
  }

  void sendMplsPacket(
      uint32_t topLabel,
      PortID from,
      std::optional<EXP> exp = std::nullopt) {
    // construct eth hdr
    const auto srcMac = utility::kLocalCpuMac();
    const auto dstMac = utility::kLocalCpuMac(); /* for l3 switching */

    uint8_t tc = exp.has_value() ? static_cast<uint8_t>(exp.value()) : 0;
    MPLSHdr::Label mplsLabel{topLabel, tc, true, 128};

    // construct l3 hdr
    auto srcIp = folly::IPAddressV6(("1001::"));
    auto dstIp = folly::IPAddressV6(("2001::"));

    // get tx packet
    auto frame = utility::getEthFrame(
        srcMac, dstMac, {mplsLabel}, srcIp, dstIp, 10000, 20000);

    auto pkt = frame.getTxPacket(getHwSwitch());

    // send pkt on src port, let it loop back in switch and be l3 switched
    getHwSwitchEnsemble()->ensureSendPacketOutOfPort(std::move(pkt), from);
  }
  bool skipTest() {
    return !getPlatform()->getAsic()->isSupported(HwAsic::Feature::MPLS);
  }

  void sendMplsPktAndVerifyTrappedCpuQueue(
      int queueId,
      int label = 1101,
      const int numPktsToSend = 1,
      const int expectedPktDelta = 1) {
    auto beforeOutPkts = utility::getQueueOutPacketsWithRetry(
        getHwSwitch(), queueId, 0 /* retryTimes */, 0 /* expectedNumPkts */);
    for (int i = 0; i < numPktsToSend; i++) {
      sendMplsPacket(label, masterLogicalPortIds()[1], EXP(5));
    }
    auto afterOutPkts = utility::getQueueOutPacketsWithRetry(
        getHwSwitch(),
        queueId,
        kGetQueueOutPktsRetryTimes,
        beforeOutPkts + numPktsToSend);
    XLOG(DBG0) << ". Queue=" << queueId << ", before pkts:" << beforeOutPkts
               << ", after pkts:" << afterOutPkts;
    EXPECT_EQ(expectedPktDelta, afterOutPkts - beforeOutPkts);
  }

  std::unique_ptr<utility::EcmpSetupTargetedPorts6> ecmpHelper_;
};

TEST_F(HwMPLSTest, Push) {
  if (skipTest()) {
    return;
  }
  auto setup = [=]() {
    // setup ip2mpls route to 2401::201:ab00/120 through
    // port 0 w/ stack {101, 102}
    addRoute(
        folly::IPAddressV6("2401::201:ab00"),
        120,
        PortDescriptor(masterLogicalPortIds()[0]),
        {101, 102});
  };
  auto verify = [=]() {
    // capture packet exiting port 0 (entering due to loopback)
    auto packetCapture =
        HwTestPacketTrapEntry(getHwSwitch(), masterLogicalPortIds()[0]);
    HwTestPacketSnooper snooper(getHwSwitchEnsemble());
    // generate the packet entering  port 1
    sendL3Packet(
        folly::IPAddressV6("2401::201:ab01"),
        masterLogicalPortIds()[1],
        DSCP(16)); // tc = 2 for dscp = 16
    auto pkt = snooper.waitForPacket(10);
    auto mplsPayLoad = pkt ? pkt->mplsPayLoad() : std::nullopt;
    EXPECT_TRUE(mplsPayLoad.has_value());
    if (!mplsPayLoad) {
      return;
    }
    auto expectedMplsHdr = MPLSHdr({
        MPLSHdr::Label{102, 5, 0, 254}, // exp = 5 for tc = 2
        MPLSHdr::Label{101, 5, 1, 254}, // exp = 5 for tc = 2
    });
    EXPECT_EQ(mplsPayLoad->header(), expectedMplsHdr);
  };
  verifyAcrossWarmBoots(setup, verify);
}

TEST_F(HwMPLSTest, Swap) {
  if (skipTest()) {
    return;
  }
  auto setup = [=]() {
    // setup ip2mpls route to 2401::201:ab00/120 through
    // port 0 w/ stack {101, 102}
    programLabelSwap(PortDescriptor(masterLogicalPortIds()[0]));
  };
  auto verify = [=]() {
    // capture packet exiting port 0 (entering due to loopback)
    auto packetCapture =
        HwTestPacketTrapEntry(getHwSwitch(), masterLogicalPortIds()[0]);
    HwTestPacketSnooper snooper(getHwSwitchEnsemble());
    // generate the packet entering  port 1
    sendMplsPacket(
        1101, masterLogicalPortIds()[1], EXP(5)); // send packet with exp 5
    auto pkt = snooper.waitForPacket(10);

    auto mplsPayLoad = pkt ? pkt->mplsPayLoad() : std::nullopt;

    EXPECT_TRUE(mplsPayLoad.has_value());
    if (!mplsPayLoad) {
      return;
    }
    uint32_t expectedOutLabel =
        utility::getLabelSwappedWithForTopLabel(getHwSwitch(), kTopLabel);
    auto expectedMplsHdr = MPLSHdr({
        MPLSHdr::Label{expectedOutLabel, 2, true, 127}, // exp is remarked to 2
    });
    EXPECT_EQ(mplsPayLoad->header(), expectedMplsHdr);
  };
  verifyAcrossWarmBoots(setup, verify);
}

TEST_F(HwMPLSTest, MplsNoMatchPktsToLowPriQ) {
  if (skipTest()) {
    return;
  }
  auto setup = [=]() {};

  auto verify = [=]() {
    const auto& mplsNoMatchCounter = utility::getMplsDestNoMatchCounterName();
    auto statBefore = utility::getAclInOutPackets(
        getHwSwitch(), getProgrammedState(), "", mplsNoMatchCounter);

    sendMplsPktAndVerifyTrappedCpuQueue(utility::kCoppLowPriQueueId);

    auto statAfter = utility::getAclInOutPackets(
        getHwSwitch(), getProgrammedState(), "", mplsNoMatchCounter);
    EXPECT_EQ(statBefore + 1, statAfter);
  };

  verifyAcrossWarmBoots(setup, verify);
}

TEST_F(HwMPLSTest, MplsMatchPktsNottrapped) {
  if (skipTest()) {
    return;
  }
  auto setup = [=]() {
    programLabelSwap(PortDescriptor(masterLogicalPortIds()[0]));
  };

  auto verify = [=]() {
    const auto& mplsNoMatchCounter = utility::getMplsDestNoMatchCounterName();
    auto statBefore = utility::getAclInOutPackets(
        getHwSwitch(), getProgrammedState(), "", mplsNoMatchCounter);

    sendMplsPktAndVerifyTrappedCpuQueue(
        utility::kCoppLowPriQueueId, 1101, 1 /* To send*/, 0 /* expected*/);

    auto statAfter = utility::getAclInOutPackets(
        getHwSwitch(), getProgrammedState(), "", mplsNoMatchCounter);
    EXPECT_EQ(statBefore, statAfter);
  };

  verifyAcrossWarmBoots(setup, verify);
}

TEST_F(HwMPLSTest, Pop) {
  if (skipTest()) {
    return;
  }
  auto setup = [=]() {
    // pop and lookup 1101
    programLabelPop(1101);
    // setup route for 2001::, dest ip under label 1101
    addRoute(
        folly::IPAddressV6("2001::"),
        128,
        PortDescriptor(masterLogicalPortIds()[0]));
  };
  auto verify = [=]() {
    auto outPktsBefore =
        getPortOutPkts(getLatestPortStats(masterLogicalPortIds()[0]));
    // send mpls packet with label and let it pop
    sendMplsPacket(1101, masterLogicalPortIds()[1]);
    // ip packet should be forwarded as per route for 2001::/128
    auto outPktsAfter =
        getPortOutPkts(getLatestPortStats(masterLogicalPortIds()[0]));
    EXPECT_EQ((outPktsAfter - outPktsBefore), 1);
  };
  verifyAcrossWarmBoots(setup, verify);
}
} // namespace facebook::fboss
