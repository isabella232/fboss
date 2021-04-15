/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/hw/sai/hw_test/SaiSwitchEnsemble.h"

#include "fboss/agent/SetupThrift.h"
#include "fboss/agent/hw/sai/hw_test/SaiLinkStateToggler.h"
#include "fboss/agent/hw/sai/switch/SaiPortManager.h"

#include <folly/io/async/AsyncSignalHandler.h>
#include "fboss/agent/hw/sai/diag/SaiRepl.h"
#include "fboss/agent/hw/sai/hw_test/SaiTestHandler.h"
#include "fboss/agent/hw/switch_asics/HwAsic.h"
#include "fboss/agent/hw/test/HwLinkStateToggler.h"
#include "fboss/agent/platforms/sai/SaiPlatformInit.h"

#include "fboss/agent/HwSwitch.h"
#include "fboss/agent/SwitchStats.h"

#include <folly/gen/Base.h>

#include <boost/container/flat_set.hpp>

#include <csignal>
#include <memory>
#include <thread>

DECLARE_int32(thrift_port);
DECLARE_bool(setup_thrift);
DECLARE_string(config);

namespace {
using folly::AsyncSignalHandler;

void initFlagDefaults(const std::map<std::string, std::string>& defaults) {
  for (auto item : defaults) {
    gflags::SetCommandLineOptionWithMode(
        item.first.c_str(), item.second.c_str(), gflags::SET_FLAGS_DEFAULT);
  }
}

class SignalHandler : public AsyncSignalHandler {
 public:
  explicit SignalHandler(folly::EventBase* eventBase)
      : AsyncSignalHandler(eventBase) {
    registerSignalHandler(SIGINT);
    registerSignalHandler(SIGTERM);
  }
  void signalReceived(int /*signum*/) noexcept override {
    auto evb = getEventBase();
    evb->runInEventBaseThread([evb] { evb->terminateLoopSoon(); });
  }

 private:
};

} // namespace

namespace facebook::fboss {

SaiSwitchEnsemble::SaiSwitchEnsemble(
    const HwSwitchEnsemble::Features& featuresDesired)
    : HwSwitchEnsemble(featuresDesired) {}

std::unique_ptr<std::thread> SaiSwitchEnsemble::createThriftThread(
    const SaiSwitch* hwSwitch) {
  return std::make_unique<std::thread>([hwSwitch] {
    folly::EventBase* eventBase = new folly::EventBase();
    auto handler = std::make_shared<SaiTestHandler>(hwSwitch);
    auto server = setupThriftServer(
        *eventBase,
        handler,
        FLAGS_thrift_port,
        false /* isDuplex */,
        false /* setupSSL*/,
        true /* isStreaming */);
    SignalHandler signalHandler(eventBase);
    // Run the EventBase main loop
    eventBase->loopForever();
  });
}

std::vector<PortID> SaiSwitchEnsemble::masterLogicalPortIds() const {
  return getPlatform()->masterLogicalPortIds();
}

std::vector<PortID> SaiSwitchEnsemble::getAllPortsInGroup(PortID portID) const {
  return getPlatform()->getAllPortsInGroup(portID);
}

std::vector<FlexPortMode> SaiSwitchEnsemble::getSupportedFlexPortModes() const {
  return getPlatform()->getSupportedFlexPortModes();
}

void SaiSwitchEnsemble::dumpHwCounters() const {
  // TODO once hw shell access is supported
}

std::map<PortID, HwPortStats> SaiSwitchEnsemble::getLatestPortStats(
    const std::vector<PortID>& ports) {
  SwitchStats dummy{};
  getHwSwitch()->updateStats(&dummy);
  auto allPortStats =
      getHwSwitch()->managerTable()->portManager().getPortStats();
  boost::container::flat_set<PortID> portIds(ports.begin(), ports.end());
  return folly::gen::from(allPortStats) |
      folly::gen::filter([&portIds](const auto& portIdAndStat) {
           return portIds.find(portIdAndStat.first) != portIds.end();
         }) |
      folly::gen::as<std::map<PortID, HwPortStats>>();
}

uint64_t SaiSwitchEnsemble::getSwitchId() const {
  return getHwSwitch()->getSwitchId();
}

void SaiSwitchEnsemble::runDiagCommand(
    const std::string& input,
    std::string& output) {
  ClientInformation clientInfo;
  clientInfo.username_ref() = "hw_test";
  clientInfo.hostname_ref() = "hw_test";
  output = diagCmdServer_->diagCmd(
      std::make_unique<fbstring>(input),
      std::make_unique<ClientInformation>(clientInfo));
}

void SaiSwitchEnsemble::init(
    const HwSwitchEnsemble::HwSwitchEnsembleInitInfo* info) {
  std::unique_ptr<AgentConfig> agentConfig;
  if (!FLAGS_config.empty()) {
    agentConfig = AgentConfig::fromFile(FLAGS_config);
  } else {
    agentConfig = AgentConfig::fromDefaultFile();
  }
  initFlagDefaults(*agentConfig->thrift.defaultCommandLineArgs_ref());
  auto platform =
      initSaiPlatform(std::move(agentConfig), getHwSwitchFeatures());
  if (info) {
    if (info->port2OverrideTransceiverInfo) {
      platform->setPort2OverrideTransceiverInfo(
          info->port2OverrideTransceiverInfo.value());
    }
    if (info->overrideTransceiverInfo) {
      platform->setOverrideTransceiverInfo(
          info->overrideTransceiverInfo.value());
    }
  }
  std::unique_ptr<HwLinkStateToggler> linkToggler;
  if (haveFeature(HwSwitchEnsemble::LINKSCAN)) {
    linkToggler = std::make_unique<SaiLinkStateToggler>(
        this, platform->getAsic()->desiredLoopbackMode());
  }
  std::unique_ptr<std::thread> thriftThread;
  if (FLAGS_setup_thrift) {
    thriftThread =
        createThriftThread(static_cast<SaiSwitch*>(platform->getHwSwitch()));
  }
  setupEnsemble(
      std::move(platform), std::move(linkToggler), std::move(thriftThread));
  auto hw = static_cast<SaiSwitch*>(getHwSwitch());
  diagShell_ = std::make_unique<DiagShell>(hw);
  diagCmdServer_ = std::make_unique<DiagCmdServer>(hw, diagShell_.get());
}
} // namespace facebook::fboss
