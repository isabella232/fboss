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
#include "fboss/agent/hw/test/HwTestPacketUtils.h"
#include "fboss/agent/hw/test/HwTestRouteUtils.h"

#include "fboss/agent/hw/test/ConfigFactory.h"

using folly::IPAddressV4;
using folly::IPAddressV6;

namespace facebook::fboss {

class HwL3Test : public HwLinkStateDependentTest {
 protected:
  cfg::SwitchConfig initialConfig() const override {
    auto cfg = utility::oneL3IntfConfig(
        getHwSwitch(), masterLogicalPortIds()[0], cfg::PortLoopbackMode::MAC);
    return cfg;
  }

  RoutePrefix<folly::IPAddressV4> kGetRoutePrefixIPv4() const {
    return RoutePrefix<folly::IPAddressV4>{folly::IPAddressV4{"1.1.1.0"}, 24};
  }

  RoutePrefix<folly::IPAddressV6> kGetRoutePrefixIPv6() const {
    return RoutePrefix<folly::IPAddressV6>{folly::IPAddressV6{"1::"}, 64};
  }

  folly::IPAddressV4 kSrcIPv4() {
    return folly::IPAddressV4("1.1.1.1");
  }

  folly::IPAddressV6 kSrcIPv6() {
    return folly::IPAddressV6("1::0");
  }

  folly::IPAddressV4 kDstIPv4() {
    return folly::IPAddressV4("1.1.1.3");
  }

  folly::IPAddressV6 kDstIPv6() {
    return folly::IPAddressV6("1::3");
  }

  void testRouteHitBit() {
    auto setup = [=]() {};

    auto verify = [=]() {
      auto vlanId = utility::firstVlanID(initialConfig());
      auto intfMac = utility::getInterfaceMac(getProgrammedState(), vlanId);
      RoutePrefix<folly::IPAddressV4> prefix4(kGetRoutePrefixIPv4());
      RoutePrefix<folly::IPAddressV6> prefix6(kGetRoutePrefixIPv6());
      auto cidr4 = folly::CIDRNetwork(prefix4.network, prefix4.mask);
      auto cidr6 = folly::CIDRNetwork(prefix6.network, prefix6.mask);

      // Ensure hit bit is NOT set
      EXPECT_FALSE(
          utility::isHwRouteHit(this->getHwSwitch(), RouterID(0), cidr4));
      EXPECT_FALSE(
          utility::isHwRouteHit(this->getHwSwitch(), RouterID(0), cidr6));

      // Construct and send IPv4 packet
      auto pkt4 = utility::makeIpTxPacket(
          getHwSwitch(),
          vlanId,
          intfMac,
          intfMac,
          this->kSrcIPv4(),
          this->kDstIPv4());
      getHwSwitchEnsemble()->ensureSendPacketSwitched(std::move(pkt4));

      // Verify hit bit is set for IPv4 route and NOT set for IPv6 route
      EXPECT_TRUE(
          utility::isHwRouteHit(this->getHwSwitch(), RouterID(0), cidr4));
      EXPECT_FALSE(
          utility::isHwRouteHit(this->getHwSwitch(), RouterID(0), cidr6));

      // Construct and send IPv6 packet
      auto pkt6 = utility::makeIpTxPacket(
          getHwSwitch(),
          vlanId,
          intfMac,
          intfMac,
          this->kSrcIPv6(),
          this->kDstIPv6());
      getHwSwitchEnsemble()->ensureSendPacketSwitched(std::move(pkt6));

      // Verify hit bit is set for both IPv4 and IPv6 routes
      EXPECT_TRUE(
          utility::isHwRouteHit(this->getHwSwitch(), RouterID(0), cidr4));
      EXPECT_TRUE(
          utility::isHwRouteHit(this->getHwSwitch(), RouterID(0), cidr6));

      // Clear IPv4 route hit bit and verify
      utility::clearHwRouteHit(this->getHwSwitch(), RouterID(0), cidr4);
      EXPECT_FALSE(
          utility::isHwRouteHit(this->getHwSwitch(), RouterID(0), cidr4));
      EXPECT_TRUE(
          utility::isHwRouteHit(this->getHwSwitch(), RouterID(0), cidr6));

      // Clear IPv6 route hit bit and verify
      utility::clearHwRouteHit(this->getHwSwitch(), RouterID(0), cidr6);
      EXPECT_FALSE(
          utility::isHwRouteHit(this->getHwSwitch(), RouterID(0), cidr4));
      EXPECT_FALSE(
          utility::isHwRouteHit(this->getHwSwitch(), RouterID(0), cidr6));
    };

    // TODO(vsp): Once hitbit is set, ensure its preserved after warm boot.
    verifyAcrossWarmBoots(setup, verify);
  }
};

TEST_F(HwL3Test, TestRouteHitBit) {
  this->testRouteHitBit();
}

} // namespace facebook::fboss
