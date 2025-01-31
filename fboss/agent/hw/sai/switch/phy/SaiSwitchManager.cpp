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

namespace facebook::fboss {

void SaiSwitchManager::initCpuPort() {}

PortSaiId SaiSwitchManager::getCpuPort() const {
  throw FbossError("getCpuPort not supported on Phy");
}

} // namespace facebook::fboss
