/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/hw/test/HwTestCoppUtils.h"

#include "fboss/agent/hw/bcm/BcmControlPlane.h"
#include "fboss/agent/hw/bcm/BcmFieldProcessorUtils.h"
#include "fboss/agent/hw/bcm/BcmPlatform.h"
#include "fboss/agent/hw/bcm/BcmSwitch.h"
#include "fboss/agent/hw/bcm/types.h"
#include "fboss/agent/hw/test/TrafficPolicyUtils.h"

#include "fboss/agent/FbossError.h"
#include "fboss/agent/LacpTypes.h"

namespace {
constexpr uint32_t kCoppLowPriSharedBytes = 10192;
constexpr uint32_t kCoppDefaultPriSharedBytes = 10192;
const std::string kMplsDestNoMatchAclName = "cpuPolicing-mpls-dest-nomatch";
const std::string kMplsDestNoMatchCounterName = "mpls-dest-nomatch-counter";
} // unnamed namespace

namespace facebook::fboss::utility {

std::pair<uint64_t, uint64_t> getCpuQueueOutPacketsAndBytes(
    HwSwitch* hwSwitch,
    int queueId) {
  auto* bcmSwitch = static_cast<BcmSwitch*>(hwSwitch);
  BcmEgressQueueTrafficCounterStats stats;
  stats[cfg::StreamType::MULTICAST][queueId][cfg::CounterType::PACKETS] = 0;
  stats[cfg::StreamType::MULTICAST][queueId][cfg::CounterType::BYTES] = 0;
  bcmSwitch->getControlPlane()->updateQueueCounters(&stats);
  return std::make_pair(
      stats[cfg::StreamType::MULTICAST][queueId][cfg::CounterType::PACKETS],
      stats[cfg::StreamType::MULTICAST][queueId][cfg::CounterType::BYTES]);
}

std::vector<std::pair<cfg::AclEntry, cfg::MatchAction>> defaultCpuAcls(
    const HwAsic* hwAsic,
    cfg::SwitchConfig& config) {
  std::vector<std::pair<cfg::AclEntry, cfg::MatchAction>> acls;

  // multicast link local dst ip
  addNoActionAclForNw(kIPv6LinkLocalMcastNetwork(), acls);

  // slow-protocols dst mac
  {
    cfg::AclEntry acl;
    acl.name_ref() = "cpuPolicing-high-slow-protocols-mac";
    acl.dstMac_ref() = LACPDU::kSlowProtocolsDstMac().toString();
    acls.push_back(std::make_pair(
        acl,
        createQueueMatchAction(
            getCoppHighPriQueueId(hwAsic), getCpuActionType(hwAsic))));
  }

  // dstClassL3 w/ BGP port to high pri queue
  // Preffered L4 ports. Combine these with local interfaces
  // to put locally destined traffic to these ports to hi-pri queue.
  auto addHighPriDstClassL3BgpAcl = [&](bool isV4, bool isSrcPort) {
    cfg::AclEntry acl;
    acl.name_ref() = folly::to<std::string>(
        "cpuPolicing-high-",
        isV4 ? "dstLocalIp4-" : "dstLocalIp6-",
        isSrcPort ? "srcPort:" : "dstPrt:",
        utility::kBgpPort);
    acl.lookupClassNeighbor_ref() = isV4
        ? cfg::AclLookupClass::DST_CLASS_L3_LOCAL_IP4
        : cfg::AclLookupClass::DST_CLASS_L3_LOCAL_IP6;

    if (isSrcPort) {
      acl.l4SrcPort_ref() = utility::kBgpPort;
    } else {
      acl.l4DstPort_ref() = utility::kBgpPort;
    }

    acls.push_back(std::make_pair(
        acl,
        createQueueMatchAction(
            getCoppHighPriQueueId(hwAsic), getCpuActionType(hwAsic))));
  };
  addHighPriDstClassL3BgpAcl(true /*v4*/, true /*srcPort*/);
  addHighPriDstClassL3BgpAcl(true /*v4*/, false /*dstPort*/);
  addHighPriDstClassL3BgpAcl(false /*v6*/, true /*srcPort*/);
  addHighPriDstClassL3BgpAcl(false /*v6*/, false /*dstPort*/);

  // Dst IP local + DSCP 48 to high pri queue
  auto addHigPriLocalIpNetworkControlAcl = [&](bool isV4) {
    cfg::AclEntry acl;
    acl.name_ref() = folly::to<std::string>(
        "cpuPolicing-high-",
        isV4 ? "dstLocalIp4" : "dstLocalIp6",
        "-network-control");
    acl.dscp_ref() = 48;
    acl.lookupClassNeighbor_ref() = isV4
        ? cfg::AclLookupClass::DST_CLASS_L3_LOCAL_IP4
        : cfg::AclLookupClass::DST_CLASS_L3_LOCAL_IP6;

    acls.push_back(std::make_pair(
        acl,
        createQueueMatchAction(
            getCoppHighPriQueueId(hwAsic), getCpuActionType(hwAsic))));
  };
  addHigPriLocalIpNetworkControlAcl(true);
  addHigPriLocalIpNetworkControlAcl(false);
  // Link local IPv6 + DSCP 48 to high pri queue
  auto addHighPriLinkLocalV6NetworkControlAcl =
      [&](const folly::CIDRNetwork& dstNetwork) {
        cfg::AclEntry acl;
        auto dstNetworkStr =
            folly::to<std::string>(dstNetwork.first, "/", dstNetwork.second);
        acl.name_ref() = folly::to<std::string>(
            "cpuPolicing-high-", dstNetworkStr, "-network-control");
        acl.dstIp_ref() = dstNetworkStr;
        acl.dscp_ref() = 48;
        acls.push_back(std::make_pair(
            acl,
            createQueueMatchAction(
                getCoppHighPriQueueId(hwAsic), getCpuActionType(hwAsic))));
      };
  addHighPriLinkLocalV6NetworkControlAcl(kIPv6LinkLocalMcastNetwork());
  addHighPriLinkLocalV6NetworkControlAcl(kIPv6LinkLocalUcastNetwork());

  // Now steer traffic destined to this (local) interface IP
  // to mid pri queue. Note that we add this Acl entry *after*
  // (with a higher Acl ID) than locally destined protocol
  // traffic. Acl entries are matched in order, so we need to
  // go from more specific to less specific matches.
  auto addMidPriDstClassL3Acl = [&](bool isV4) {
    cfg::AclEntry acl;
    acl.name_ref() = folly::to<std::string>(
        "cpuPolicing-mid-", isV4 ? "dstLocalIp4" : "dstLocalIp6");
    acl.lookupClassNeighbor_ref() = isV4
        ? cfg::AclLookupClass::DST_CLASS_L3_LOCAL_IP4
        : cfg::AclLookupClass::DST_CLASS_L3_LOCAL_IP6;

    acls.push_back(std::make_pair(
        acl,
        createQueueMatchAction(
            utility::kCoppMidPriQueueId, getCpuActionType(hwAsic))));
  };
  addMidPriDstClassL3Acl(true);
  addMidPriDstClassL3Acl(false);

  // unicast and multicast link local dst ip
  addMidPriAclForNw(
      kIPv6LinkLocalMcastNetwork(), getCpuActionType(hwAsic), acls);
  // All fe80::/10 to mid pri queue
  addMidPriAclForNw(
      kIPv6LinkLocalUcastNetwork(), getCpuActionType(hwAsic), acls);

  // mpls no match
  {
    if (BCM_FIELD_QSET_TEST(
            getAclQset(hwAsic->getAsicType()), bcmFieldQualifyPacketRes)) {
      cfg::AclEntry acl;
      acl.name_ref() = kMplsDestNoMatchAclName;
      acl.packetLookupResult_ref() =
          cfg::PacketLookupResultType::PACKET_LOOKUP_RESULT_MPLS_NO_MATCH;
      utility::addTrafficCounter(&config, kMplsDestNoMatchCounterName);
      auto action = createQueueMatchAction(
          utility::kCoppLowPriQueueId, getCpuActionType(hwAsic));
      action.counter_ref() = kMplsDestNoMatchCounterName;
      acls.push_back(std::make_pair(acl, action));
    }
  }
  return acls;
}

std::string getMplsDestNoMatchCounterName(void) {
  return kMplsDestNoMatchCounterName;
}

std::vector<cfg::PacketRxReasonToQueue> getCoppRxReasonToQueues(
    const HwAsic* hwAsic) {
  std::vector<cfg::PacketRxReasonToQueue> rxReasonToQueues;
  auto coppHighPriQueueId = utility::getCoppHighPriQueueId(hwAsic);
  std::vector<std::pair<cfg::PacketRxReason, uint16_t>>
      rxReasonToQueueMappings = {
          std::pair(cfg::PacketRxReason::ARP, coppHighPriQueueId),
          std::pair(cfg::PacketRxReason::DHCP, kCoppMidPriQueueId),
          std::pair(cfg::PacketRxReason::BPDU, kCoppMidPriQueueId),
          std::pair(cfg::PacketRxReason::L3_MTU_ERROR, kCoppLowPriQueueId),
          std::pair(cfg::PacketRxReason::L3_SLOW_PATH, kCoppLowPriQueueId),
          std::pair(cfg::PacketRxReason::L3_DEST_MISS, kCoppLowPriQueueId),
          std::pair(cfg::PacketRxReason::TTL_1, kCoppLowPriQueueId),
          std::pair(cfg::PacketRxReason::CPU_IS_NHOP, kCoppLowPriQueueId)};
  for (auto rxEntry : rxReasonToQueueMappings) {
    auto rxReasonToQueue = cfg::PacketRxReasonToQueue();
    rxReasonToQueue.rxReason_ref() = rxEntry.first;
    rxReasonToQueue.queueId_ref() = rxEntry.second;
    rxReasonToQueues.push_back(rxReasonToQueue);
  }
  return rxReasonToQueues;
}

void setPortQueueSharedBytes(cfg::PortQueue& queue) {
  // Set sharedBytes for Low and Default Pri-Queue
  if (queue.id_ref() == kCoppLowPriQueueId) {
    queue.sharedBytes_ref() = kCoppLowPriSharedBytes;
  } else if (queue.id_ref() == kCoppDefaultPriQueueId) {
    queue.sharedBytes_ref() = kCoppDefaultPriSharedBytes;
  }
}

} // namespace facebook::fboss::utility
