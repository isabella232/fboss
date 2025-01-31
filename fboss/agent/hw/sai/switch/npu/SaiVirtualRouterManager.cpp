/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/hw/sai/switch/SaiVirtualRouterManager.h"

#include "fboss/agent/hw/sai/api/SwitchApi.h"
#include "fboss/agent/hw/sai/store/SaiStore.h"
#include "fboss/agent/hw/sai/switch/SaiManagerTable.h"
#include "fboss/agent/hw/sai/switch/SaiSwitchManager.h"
#include "fboss/agent/platforms/sai/SaiPlatform.h"

namespace facebook::fboss {

SaiVirtualRouterManager::SaiVirtualRouterManager(
    SaiStore* saiStore,
    SaiManagerTable* managerTable,
    const SaiPlatform* platform)
    : saiStore_(saiStore), managerTable_(managerTable), platform_(platform) {
  auto& store = saiStore_->get<SaiVirtualRouterTraits>();
  auto virtualRouterHandle = std::make_unique<SaiVirtualRouterHandle>();
  SwitchSaiId switchId = managerTable_->switchManager().getSwitchSaiId();
  VirtualRouterSaiId defaultVrfId{
      SaiApiTable::getInstance()->switchApi().getAttribute(
          switchId, SaiSwitchTraits::Attributes::DefaultVirtualRouterId{})};
  virtualRouterHandle->virtualRouter = store.loadObjectOwnedByAdapter(
      SaiVirtualRouterTraits::AdapterKey{defaultVrfId});
  virtualRouterHandle->mplsRouterInterface =
      createMplsRouterInterface(defaultVrfId);

  CHECK(virtualRouterHandle->virtualRouter);
  handles_.emplace(std::make_pair(RouterID(0), std::move(virtualRouterHandle)));
}

} // namespace facebook::fboss
