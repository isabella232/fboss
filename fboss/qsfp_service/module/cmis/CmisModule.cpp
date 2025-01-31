// Copyright 2004-present Facebook. All Rights Reserved.

#include "fboss/qsfp_service/module/cmis/CmisModule.h"

#include <boost/assign.hpp>
#include <cmath>
#include <iomanip>
#include <string>
#include "fboss/agent/FbossError.h"
#include "fboss/lib/usb/TransceiverI2CApi.h"
#include "fboss/qsfp_service/StatsPublisher.h"
#include "fboss/qsfp_service/TransceiverManager.h"
#include "fboss/qsfp_service/if/gen-cpp2/transceiver_types.h"
#include "fboss/qsfp_service/module/TransceiverImpl.h"
#include "fboss/qsfp_service/module/cmis/CmisFieldInfo.h"

#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>

#include <thrift/lib/cpp/util/EnumUtils.h>

using folly::IOBuf;
using std::lock_guard;
using std::memcpy;
using std::mutex;
using namespace apache::thrift;

namespace {

constexpr int kUsecBetweenPowerModeFlap = 100000;
constexpr int kUsecBetweenLaneInit = 10000;
constexpr int kResetCounterLimit = 5;

} // namespace

namespace facebook {
namespace fboss {

enum CmisPages {
  LOWER,
  PAGE00,
  PAGE01,
  PAGE02,
  PAGE10,
  PAGE11,
  PAGE13,
  PAGE14
};

enum DiagnosticFeatureEncoding {
  NONE = 0x0,
  BER = 0x1,
  SNR = 0x6,
  LATCHED_BER = 0x11,
};

// As per CMIS4.0
static CmisFieldInfo::CmisFieldMap cmisFields = {
    // Lower Page
    {CmisField::IDENTIFIER, {CmisPages::LOWER, 0, 1}},
    {CmisField::REVISION_COMPLIANCE, {CmisPages::LOWER, 1, 1}},
    {CmisField::FLAT_MEM, {CmisPages::LOWER, 2, 1}},
    {CmisField::MODULE_STATE, {CmisPages::LOWER, 3, 1}},
    {CmisField::BANK0_FLAGS, {CmisPages::LOWER, 4, 1}},
    {CmisField::BANK1_FLAGS, {CmisPages::LOWER, 5, 1}},
    {CmisField::BANK2_FLAGS, {CmisPages::LOWER, 6, 1}},
    {CmisField::BANK3_FLAGS, {CmisPages::LOWER, 7, 1}},
    {CmisField::MODULE_FLAG, {CmisPages::LOWER, 8, 1}},
    {CmisField::MODULE_ALARMS, {CmisPages::LOWER, 9, 3}},
    {CmisField::TEMPERATURE, {CmisPages::LOWER, 14, 2}},
    {CmisField::VCC, {CmisPages::LOWER, 16, 2}},
    {CmisField::MODULE_CONTROL, {CmisPages::LOWER, 26, 1}},
    {CmisField::FIRMWARE_REVISION, {CmisPages::LOWER, 39, 2}},
    {CmisField::MEDIA_TYPE_ENCODINGS, {CmisPages::LOWER, 85, 1}},
    {CmisField::APPLICATION_ADVERTISING1, {CmisPages::LOWER, 86, 4}},
    {CmisField::BANK_SELECT, {CmisPages::LOWER, 126, 1}},
    {CmisField::PAGE_SELECT_BYTE, {CmisPages::LOWER, 127, 1}},
    // Page 00h
    {CmisField::VENDOR_NAME, {CmisPages::PAGE00, 129, 16}},
    {CmisField::VENDOR_OUI, {CmisPages::PAGE00, 145, 3}},
    {CmisField::PART_NUMBER, {CmisPages::PAGE00, 148, 16}},
    {CmisField::REVISION_NUMBER, {CmisPages::PAGE00, 164, 2}},
    {CmisField::VENDOR_SERIAL_NUMBER, {CmisPages::PAGE00, 166, 16}},
    {CmisField::MFG_DATE, {CmisPages::PAGE00, 182, 8}},
    {CmisField::EXTENDED_SPECIFICATION_COMPLIANCE, {CmisPages::PAGE00, 192, 1}},
    {CmisField::LENGTH_COPPER, {CmisPages::PAGE00, 202, 1}},
    {CmisField::MEDIA_INTERFACE_TECHNOLOGY, {CmisPages::PAGE00, 212, 1}},
    // Page 01h
    {CmisField::LENGTH_SMF, {CmisPages::PAGE01, 132, 1}},
    {CmisField::LENGTH_OM5, {CmisPages::PAGE01, 133, 1}},
    {CmisField::LENGTH_OM4, {CmisPages::PAGE01, 134, 1}},
    {CmisField::LENGTH_OM3, {CmisPages::PAGE01, 135, 1}},
    {CmisField::LENGTH_OM2, {CmisPages::PAGE01, 136, 1}},
    {CmisField::TX_SIG_INT_CONT_AD, {CmisPages::PAGE01, 161, 1}},
    {CmisField::RX_SIG_INT_CONT_AD, {CmisPages::PAGE01, 162, 1}},
    {CmisField::DSP_FW_VERSION, {CmisPages::PAGE01, 194, 2}},
    {CmisField::BUILD_REVISION, {CmisPages::PAGE01, 196, 2}},
    // Page 02h
    {CmisField::TEMPERATURE_THRESH, {CmisPages::PAGE02, 128, 8}},
    {CmisField::VCC_THRESH, {CmisPages::PAGE02, 136, 8}},
    {CmisField::TX_PWR_THRESH, {CmisPages::PAGE02, 176, 8}},
    {CmisField::TX_BIAS_THRESH, {CmisPages::PAGE02, 184, 8}},
    {CmisField::RX_PWR_THRESH, {CmisPages::PAGE02, 192, 8}},
    // Page 10h
    {CmisField::DATA_PATH_DEINIT, {CmisPages::PAGE10, 128, 1}},
    {CmisField::TX_POLARITY_FLIP, {CmisPages::PAGE10, 129, 1}},
    {CmisField::TX_DISABLE, {CmisPages::PAGE10, 130, 1}},
    {CmisField::TX_SQUELCH_DISABLE, {CmisPages::PAGE10, 131, 1}},
    {CmisField::TX_FORCE_SQUELCH, {CmisPages::PAGE10, 132, 1}},
    {CmisField::TX_ADAPTATION_FREEZE, {CmisPages::PAGE10, 134, 1}},
    {CmisField::TX_ADAPTATION_STORE, {CmisPages::PAGE10, 135, 2}},
    {CmisField::RX_POLARITY_FLIP, {CmisPages::PAGE10, 137, 1}},
    {CmisField::RX_DISABLE, {CmisPages::PAGE10, 138, 1}},
    {CmisField::RX_SQUELCH_DISABLE, {CmisPages::PAGE10, 139, 1}},
    {CmisField::STAGE_CTRL_SET_0, {CmisPages::PAGE10, 143, 1}},
    {CmisField::APP_SEL_LANE_1, {CmisPages::PAGE10, 145, 1}},
    {CmisField::APP_SEL_LANE_2, {CmisPages::PAGE10, 146, 1}},
    {CmisField::APP_SEL_LANE_3, {CmisPages::PAGE10, 147, 1}},
    {CmisField::APP_SEL_LANE_4, {CmisPages::PAGE10, 148, 1}},
    // Page 11h
    {CmisField::DATA_PATH_STATE, {CmisPages::PAGE11, 128, 4}},
    {CmisField::TX_FAULT_FLAG, {CmisPages::PAGE11, 135, 1}},
    {CmisField::TX_LOS_FLAG, {CmisPages::PAGE11, 136, 1}},
    {CmisField::TX_LOL_FLAG, {CmisPages::PAGE11, 137, 1}},
    {CmisField::TX_EQ_FLAG, {CmisPages::PAGE11, 138, 1}},
    {CmisField::TX_PWR_FLAG, {CmisPages::PAGE11, 139, 4}},
    {CmisField::TX_BIAS_FLAG, {CmisPages::PAGE11, 143, 4}},
    {CmisField::RX_LOS_FLAG, {CmisPages::PAGE11, 147, 1}},
    {CmisField::RX_LOL_FLAG, {CmisPages::PAGE11, 148, 1}},
    {CmisField::RX_PWR_FLAG, {CmisPages::PAGE11, 149, 4}},
    {CmisField::CHANNEL_TX_PWR, {CmisPages::PAGE11, 154, 16}},
    {CmisField::CHANNEL_TX_BIAS, {CmisPages::PAGE11, 170, 16}},
    {CmisField::CHANNEL_RX_PWR, {CmisPages::PAGE11, 186, 16}},
    {CmisField::ACTIVE_CTRL_LANE_1, {CmisPages::PAGE11, 206, 1}},
    {CmisField::ACTIVE_CTRL_LANE_2, {CmisPages::PAGE11, 207, 1}},
    {CmisField::ACTIVE_CTRL_LANE_3, {CmisPages::PAGE11, 208, 1}},
    {CmisField::ACTIVE_CTRL_LANE_4, {CmisPages::PAGE11, 209, 1}},
    {CmisField::TX_CDR_CONTROL, {CmisPages::PAGE11, 221, 1}},
    {CmisField::RX_CDR_CONTROL, {CmisPages::PAGE11, 222, 1}},
    // Page 13h
    {CmisField::LOOPBACK_CAPABILITY, {CmisPages::PAGE13, 128, 1}},
    {CmisField::PATTERN_CAPABILITY, {CmisPages::PAGE13, 129, 1}},
    {CmisField::DIAGNOSTIC_CAPABILITY, {CmisPages::PAGE13, 130, 1}},
    {CmisField::PATTERN_CHECKER_CAPABILITY, {CmisPages::PAGE13, 131, 1}},
    {CmisField::HOST_GEN_ENABLE, {CmisPages::PAGE13, 144, 1}},
    {CmisField::HOST_GEN_INV, {CmisPages::PAGE13, 145, 1}},
    {CmisField::HOST_GEN_PRE_FEC, {CmisPages::PAGE13, 147, 1}},
    {CmisField::HOST_PATTERN_SELECT_LANE_2_1, {CmisPages::PAGE13, 148, 1}},
    {CmisField::HOST_PATTERN_SELECT_LANE_4_3, {CmisPages::PAGE13, 149, 1}},
    {CmisField::MEDIA_GEN_ENABLE, {CmisPages::PAGE13, 152, 1}},
    {CmisField::MEDIA_GEN_INV, {CmisPages::PAGE13, 153, 1}},
    {CmisField::MEDIA_GEN_PRE_FEC, {CmisPages::PAGE13, 155, 1}},
    {CmisField::MEDIA_PATTERN_SELECT_LANE_2_1, {CmisPages::PAGE13, 156, 1}},
    {CmisField::MEDIA_PATTERN_SELECT_LANE_4_3, {CmisPages::PAGE13, 157, 1}},
    {CmisField::HOST_CHECKER_ENABLE, {CmisPages::PAGE13, 160, 1}},
    {CmisField::HOST_CHECKER_INV, {CmisPages::PAGE13, 161, 1}},
    {CmisField::HOST_CHECKER_POST_FEC, {CmisPages::PAGE13, 163, 1}},
    {CmisField::HOST_CHECKER_PATTERN_SELECT_LANE_2_1,
     {CmisPages::PAGE13, 164, 1}},
    {CmisField::HOST_CHECKER_PATTERN_SELECT_LANE_4_3,
     {CmisPages::PAGE13, 165, 1}},
    {CmisField::MEDIA_CHECKER_ENABLE, {CmisPages::PAGE13, 168, 1}},
    {CmisField::MEDIA_CHECKER_INV, {CmisPages::PAGE13, 169, 1}},
    {CmisField::MEDIA_CHECKER_POST_FEC, {CmisPages::PAGE13, 171, 1}},
    {CmisField::MEDIA_CHECKER_PATTERN_SELECT_LANE_2_1,
     {CmisPages::PAGE13, 172, 1}},
    {CmisField::MEDIA_CHECKER_PATTERN_SELECT_LANE_4_3,
     {CmisPages::PAGE13, 173, 1}},
    {CmisField::REF_CLK_CTRL, {CmisPages::PAGE13, 176, 1}},
    {CmisField::BER_CTRL, {CmisPages::PAGE13, 177, 1}},
    {CmisField::HOST_NEAR_LB_EN, {CmisPages::PAGE13, 180, 1}},
    {CmisField::MEDIA_NEAR_LB_EN, {CmisPages::PAGE13, 181, 1}},
    {CmisField::HOST_FAR_LB_EN, {CmisPages::PAGE13, 182, 1}},
    {CmisField::MEDIA_FAR_LB_EN, {CmisPages::PAGE13, 183, 1}},
    {CmisField::REF_CLK_LOSS, {CmisPages::PAGE13, 206, 1}},
    {CmisField::HOST_CHECKER_GATING_COMPLETE, {CmisPages::PAGE13, 208, 1}},
    {CmisField::MEDIA_CHECKER_GATING_COMPLETE, {CmisPages::PAGE13, 209, 1}},
    {CmisField::HOST_PPG_LOL, {CmisPages::PAGE13, 210, 1}},
    {CmisField::MEDIA_PPG_LOL, {CmisPages::PAGE13, 211, 1}},
    {CmisField::HOST_BERT_LOL, {CmisPages::PAGE13, 212, 1}},
    {CmisField::MEDIA_BERT_LOL, {CmisPages::PAGE13, 213, 1}},
    // Page 14h
    {CmisField::DIAG_SEL, {CmisPages::PAGE14, 128, 1}},
    {CmisField::HOST_LANE_CHECKER_LOL, {CmisPages::PAGE14, 138, 1}},
    {CmisField::HOST_BER, {CmisPages::PAGE14, 192, 16}},
    {CmisField::MEDIA_BER_HOST_SNR, {CmisPages::PAGE14, 208, 16}},
    {CmisField::MEDIA_SNR, {CmisPages::PAGE14, 240, 16}},
};

static CmisFieldMultiplier qsfpMultiplier = {
    {CmisField::LENGTH_SMF, 100},
    {CmisField::LENGTH_OM5, 2},
    {CmisField::LENGTH_OM4, 2},
    {CmisField::LENGTH_OM3, 2},
    {CmisField::LENGTH_OM2, 1},
    {CmisField::LENGTH_COPPER, 0.1},
};

static SpeedApplicationMapping speedApplicationMapping = {
    {cfg::PortSpeed::HUNDREDG, SMFMediaInterfaceCode::CWDM4_100G},
    {cfg::PortSpeed::TWOHUNDREDG, SMFMediaInterfaceCode::FR4_200G},
    {cfg::PortSpeed::FOURHUNDREDG, SMFMediaInterfaceCode::FR4_400G},
};

void getQsfpFieldAddress(
    CmisField field,
    int& dataAddress,
    int& offset,
    int& length) {
  auto info = CmisFieldInfo::getCmisFieldAddress(cmisFields, field);
  dataAddress = info.dataAddress;
  offset = info.offset;
  length = info.length;
}

CmisModule::CmisModule(
    TransceiverManager* transceiverManager,
    std::unique_ptr<TransceiverImpl> qsfpImpl,
    unsigned int portsPerTransceiver)
    : QsfpModule(transceiverManager, std::move(qsfpImpl), portsPerTransceiver) {
  CHECK_GT(portsPerTransceiver, 0);
}

CmisModule::~CmisModule() {}

FlagLevels CmisModule::getQsfpSensorFlags(CmisField fieldName, int offset) {
  int dataOffset;
  int dataLength;
  int dataAddress;

  getQsfpFieldAddress(fieldName, dataAddress, dataOffset, dataLength);
  const uint8_t* data = getQsfpValuePtr(dataAddress, dataOffset, dataLength);

  // CMIS uses different mappings for flags than Sff therefore not using
  // getQsfpFlags here
  FlagLevels flags;
  CHECK_GE(offset, 0);
  CHECK_LE(offset, 4);
  flags.alarm_ref()->high_ref() = (*data & (1 << offset));
  flags.alarm_ref()->low_ref() = (*data & (1 << ++offset));
  flags.warn_ref()->high_ref() = (*data & (1 << ++offset));
  flags.warn_ref()->low_ref() = (*data & (1 << ++offset));

  return flags;
}

double CmisModule::getQsfpDACLength() const {
  uint8_t value;
  getFieldValueLocked(CmisField::LENGTH_COPPER, &value);
  auto base = value & FieldMasks::CABLE_LENGTH_MASK;
  auto multiplier =
      std::pow(10, value >> 6) * qsfpMultiplier.at(CmisField::LENGTH_COPPER);
  return base * multiplier;
}

double CmisModule::getQsfpSMFLength() const {
  uint8_t value;
  getFieldValueLocked(CmisField::LENGTH_SMF, &value);
  auto base = value & FieldMasks::CABLE_LENGTH_MASK;
  auto multiplier =
      std::pow(10, value >> 6) * qsfpMultiplier.at(CmisField::LENGTH_SMF);
  return base * multiplier;
}

double CmisModule::getQsfpOMLength(CmisField field) const {
  uint8_t value;
  getFieldValueLocked(field, &value);
  return value * qsfpMultiplier.at(field);
}

GlobalSensors CmisModule::getSensorInfo() {
  GlobalSensors info = GlobalSensors();
  info.temp_ref()->value_ref() =
      getQsfpSensor(CmisField::TEMPERATURE, CmisFieldInfo::getTemp);
  info.temp_ref()->flags_ref() =
      getQsfpSensorFlags(CmisField::MODULE_ALARMS, 0);
  info.vcc_ref()->value_ref() =
      getQsfpSensor(CmisField::VCC, CmisFieldInfo::getVcc);
  info.vcc_ref()->flags_ref() = getQsfpSensorFlags(CmisField::MODULE_ALARMS, 4);
  return info;
}

Vendor CmisModule::getVendorInfo() {
  Vendor vendor = Vendor();
  *vendor.name_ref() = getQsfpString(CmisField::VENDOR_NAME);
  *vendor.oui_ref() = getQsfpString(CmisField::VENDOR_OUI);
  *vendor.partNumber_ref() = getQsfpString(CmisField::PART_NUMBER);
  *vendor.rev_ref() = getQsfpString(CmisField::REVISION_NUMBER);
  *vendor.serialNumber_ref() = getQsfpString(CmisField::VENDOR_SERIAL_NUMBER);
  *vendor.dateCode_ref() = getQsfpString(CmisField::MFG_DATE);
  return vendor;
}

std::array<std::string, 3> CmisModule::getFwRevisions() {
  int offset;
  int length;
  int dataAddress;
  std::array<std::string, 3> fwVersions;
  // Get module f/w version
  getQsfpFieldAddress(
      CmisField::FIRMWARE_REVISION, dataAddress, offset, length);
  const uint8_t* data = getQsfpValuePtr(dataAddress, offset, length);
  fwVersions[0] = fmt::format("{}.{}", data[0], data[1]);
  // Get DSP f/w version
  getQsfpFieldAddress(CmisField::DSP_FW_VERSION, dataAddress, offset, length);
  data = getQsfpValuePtr(dataAddress, offset, length);
  fwVersions[1] = fmt::format("{}.{}", data[0], data[1]);
  // Get the build revision
  getQsfpFieldAddress(CmisField::BUILD_REVISION, dataAddress, offset, length);
  data = getQsfpValuePtr(dataAddress, offset, length);
  fwVersions[2] = fmt::format("{}.{}", data[0], data[1]);
  return fwVersions;
}

Cable CmisModule::getCableInfo() {
  Cable cable = Cable();
  cable.transmitterTech_ref() = getQsfpTransmitterTechnology();

  if (auto length = getQsfpSMFLength(); length != 0) {
    cable.singleMode_ref() = length;
  }
  if (auto length = getQsfpOMLength(CmisField::LENGTH_OM5); length != 0) {
    cable.om5_ref() = length;
  }
  if (auto length = getQsfpOMLength(CmisField::LENGTH_OM4); length != 0) {
    cable.om4_ref() = length;
  }
  if (auto length = getQsfpOMLength(CmisField::LENGTH_OM3); length != 0) {
    cable.om3_ref() = length;
  }
  if (auto length = getQsfpOMLength(CmisField::LENGTH_OM2); length != 0) {
    cable.om2_ref() = length;
  }
  if (auto length = getQsfpDACLength(); length != 0) {
    cable.length_ref() = length;
  }
  return cable;
}

FirmwareStatus CmisModule::getFwStatus() {
  FirmwareStatus fwStatus;
  auto fwRevisions = getFwRevisions();
  fwStatus.version_ref() = fwRevisions[0];
  fwStatus.dspFwVer_ref() = fwRevisions[1];
  fwStatus.buildRev_ref() = fwRevisions[2];
  fwStatus.fwFault_ref() =
      (getSettingsValue(CmisField::MODULE_FLAG, FWFAULT_MASK) >> 1);
  return fwStatus;
}

ModuleStatus CmisModule::getModuleStatus() {
  ModuleStatus moduleStatus;
  moduleStatus.cmisModuleState_ref() =
      (CmisModuleState)(getSettingsValue(CmisField::MODULE_STATE) >> 1);
  moduleStatus.fwStatus_ref() = getFwStatus();
  return moduleStatus;
}

/*
 * Threhold values are stored just once;  they aren't per-channel,
 * so in all cases we simple assemble two-byte values and convert
 * them based on the type of the field.
 */
ThresholdLevels CmisModule::getThresholdValues(
    CmisField field,
    double (*conversion)(uint16_t value)) {
  int offset;
  int length;
  int dataAddress;

  CHECK(!flatMem_);

  ThresholdLevels thresh;

  getQsfpFieldAddress(field, dataAddress, offset, length);
  const uint8_t* data = getQsfpValuePtr(dataAddress, offset, length);

  CHECK_GE(length, 8);
  thresh.alarm_ref()->high_ref() = conversion(data[0] << 8 | data[1]);
  thresh.alarm_ref()->low_ref() = conversion(data[2] << 8 | data[3]);
  thresh.warn_ref()->high_ref() = conversion(data[4] << 8 | data[5]);
  thresh.warn_ref()->low_ref() = conversion(data[6] << 8 | data[7]);

  return thresh;
}

std::optional<AlarmThreshold> CmisModule::getThresholdInfo() {
  if (flatMem_) {
    return {};
  }
  AlarmThreshold threshold = AlarmThreshold();
  threshold.temp_ref() =
      getThresholdValues(CmisField::TEMPERATURE_THRESH, CmisFieldInfo::getTemp);
  threshold.vcc_ref() =
      getThresholdValues(CmisField::VCC_THRESH, CmisFieldInfo::getVcc);
  threshold.rxPwr_ref() =
      getThresholdValues(CmisField::RX_PWR_THRESH, CmisFieldInfo::getPwr);
  threshold.txPwr_ref() =
      getThresholdValues(CmisField::TX_PWR_THRESH, CmisFieldInfo::getPwr);
  threshold.txBias_ref() =
      getThresholdValues(CmisField::TX_BIAS_THRESH, CmisFieldInfo::getTxBias);
  return threshold;
}

uint8_t CmisModule::getSettingsValue(CmisField field, uint8_t mask) const {
  int offset;
  int length;
  int dataAddress;

  getQsfpFieldAddress(field, dataAddress, offset, length);
  const uint8_t* data = getQsfpValuePtr(dataAddress, offset, length);

  return data[0] & mask;
}

TransceiverSettings CmisModule::getTransceiverSettingsInfo() {
  TransceiverSettings settings = TransceiverSettings();
  settings.cdrTx_ref() = CmisFieldInfo::getFeatureState(
      getSettingsValue(CmisField::TX_SIG_INT_CONT_AD, CDR_IMPL_MASK),
      getSettingsValue(CmisField::TX_CDR_CONTROL));
  settings.cdrRx_ref() = CmisFieldInfo::getFeatureState(
      getSettingsValue(CmisField::RX_SIG_INT_CONT_AD, CDR_IMPL_MASK),
      getSettingsValue(CmisField::RX_CDR_CONTROL));

  settings.powerMeasurement_ref() =
      flatMem_ ? FeatureState::UNSUPPORTED : FeatureState::ENABLED;

  settings.powerControl_ref() = getPowerControlValue();
  settings.rateSelect_ref() = flatMem_
      ? RateSelectState::UNSUPPORTED
      : RateSelectState::APPLICATION_RATE_SELECT;
  settings.rateSelectSetting_ref() = RateSelectSetting::UNSUPPORTED;

  getApplicationCapabilities();

  settings.mediaLaneSettings_ref() =
      std::vector<MediaLaneSettings>(numMediaLanes());
  settings.hostLaneSettings_ref() =
      std::vector<HostLaneSettings>(numHostLanes());

  if (!getMediaLaneSettings(*(settings.mediaLaneSettings_ref()))) {
    settings.mediaLaneSettings_ref()->clear();
    settings.mediaLaneSettings_ref().reset();
  }

  if (!getHostLaneSettings(*(settings.hostLaneSettings_ref()))) {
    settings.hostLaneSettings_ref()->clear();
    settings.hostLaneSettings_ref().reset();
  }

  settings.mediaInterface_ref() =
      std::vector<MediaInterfaceId>(numMediaLanes());
  if (!getMediaInterfaceId(*(settings.mediaInterface_ref()))) {
    settings.mediaInterface_ref()->clear();
    settings.mediaInterface_ref().reset();
  }

  return settings;
}

bool CmisModule::getMediaLaneSettings(
    std::vector<MediaLaneSettings>& laneSettings) {
  assert(laneSettings.size() == numMediaLanes());

  auto txDisable = getSettingsValue(CmisField::TX_DISABLE);
  auto txSquelchDisable = getSettingsValue(CmisField::TX_SQUELCH_DISABLE);
  auto txSquelchForce = getSettingsValue(CmisField::TX_FORCE_SQUELCH);

  for (int lane = 0; lane < laneSettings.size(); lane++) {
    auto laneMask = (1 << lane);
    laneSettings[lane].lane_ref() = lane;
    laneSettings[lane].txDisable_ref() = txDisable & laneMask;
    laneSettings[lane].txSquelch_ref() = txSquelchDisable & laneMask;
    laneSettings[lane].txSquelchForce_ref() = txSquelchForce & laneMask;
  }

  return true;
}

bool CmisModule::getHostLaneSettings(
    std::vector<HostLaneSettings>& laneSettings) {
  assert(laneSettings.size() == numHostLanes());

  auto rxOutput = getSettingsValue(CmisField::RX_DISABLE);
  auto rxSquelchDisable = getSettingsValue(CmisField::RX_SQUELCH_DISABLE);

  for (int lane = 0; lane < laneSettings.size(); lane++) {
    auto laneMask = (1 << lane);
    laneSettings[lane].lane_ref() = lane;
    laneSettings[lane].rxOutput_ref() = rxOutput & laneMask;
    laneSettings[lane].rxSquelch_ref() = rxSquelchDisable & laneMask;
  }

  return true;
}

unsigned int CmisModule::numHostLanes() const {
  return numHostLanes_;
}

unsigned int CmisModule::numMediaLanes() const {
  return numMediaLanes_;
}

SMFMediaInterfaceCode CmisModule::getSmfMediaInterface() {
  uint8_t currentApplicationSel =
      getSettingsValue(CmisField::ACTIVE_CTRL_LANE_1, APP_SEL_MASK);
  // The application sel code is at the higher four bits of the field.
  currentApplicationSel = currentApplicationSel >> 4;

  uint8_t currentApplication;
  int offset;
  int length;
  int dataAddress;

  getQsfpFieldAddress(
      CmisField::APPLICATION_ADVERTISING1, dataAddress, offset, length);
  // We use the module Media Interface ID, which is located at the second byte
  // of the field, as Application ID here.
  offset += (currentApplicationSel - 1) * length + 1;
  getQsfpValue(dataAddress, offset, 1, &currentApplication);

  return (SMFMediaInterfaceCode)currentApplication;
}

bool CmisModule::getMediaInterfaceId(
    std::vector<MediaInterfaceId>& mediaInterface) {
  assert(mediaInterface.size() == numMediaLanes());
  MediaTypeEncodings encoding =
      (MediaTypeEncodings)getSettingsValue(CmisField::MEDIA_TYPE_ENCODINGS);
  if (encoding != MediaTypeEncodings::OPTICAL_SMF) {
    return false;
  }

  // Currently setting the same media interface for all media lanes
  auto smfMediaInterface = getSmfMediaInterface();
  for (int lane = 0; lane < mediaInterface.size(); lane++) {
    mediaInterface[lane].lane_ref() = lane;
    MediaInterfaceUnion media;
    media.set_smfCode(smfMediaInterface);
    mediaInterface[lane].media_ref() = media;
  }

  return true;
}

void CmisModule::getApplicationCapabilities() {
  const uint8_t* data;
  int offset;
  int length;
  int dataAddress;

  getQsfpFieldAddress(
      CmisField::APPLICATION_ADVERTISING1, dataAddress, offset, length);

  for (uint8_t i = 0; i < 8; i++) {
    data = getQsfpValuePtr(dataAddress, offset + i * length, length);

    if (data[0] == 0xff) {
      break;
    }

    XLOG(DBG3) << "Adding module capability: " << data[1] << " at position "
               << (i + 1);
    ApplicationAdvertisingField applicationAdvertisingField;
    applicationAdvertisingField.ApSelCode = (i + 1);
    applicationAdvertisingField.moduleMediaInterface = data[1];
    applicationAdvertisingField.hostLaneCount =
        (data[2] & FieldMasks::UPPER_FOUR_BITS_MASK) >> 4;
    applicationAdvertisingField.mediaLaneCount =
        data[2] & FieldMasks::LOWER_FOUR_BITS_MASK;

    moduleCapabilities_[data[1]] = applicationAdvertisingField;
  }
}

PowerControlState CmisModule::getPowerControlValue() {
  if (getSettingsValue(
          CmisField::MODULE_CONTROL, uint8_t(POWER_CONTROL_MASK))) {
    return PowerControlState::POWER_LPMODE;
  } else {
    return PowerControlState::HIGH_POWER_OVERRIDE;
  }
}

/*
 * For the specified field, collect alarm and warning flags for the channel.
 */

FlagLevels CmisModule::getChannelFlags(CmisField field, int channel) {
  FlagLevels flags;
  int offset;
  int length;
  int dataAddress;

  CHECK_GE(channel, 0);
  CHECK_LE(channel, 8);

  getQsfpFieldAddress(field, dataAddress, offset, length);
  const uint8_t* data = getQsfpValuePtr(dataAddress, offset, length);

  flags.warn_ref()->low_ref() = (data[3] & (1 << channel));
  flags.warn_ref()->high_ref() = (data[2] & (1 << channel));
  flags.alarm_ref()->low_ref() = (data[1] & (1 << channel));
  flags.alarm_ref()->high_ref() = (data[0] & (1 << channel));

  return flags;
}

/*
 * Iterate through channels collecting appropriate data;
 */

bool CmisModule::getSignalsPerMediaLane(
    std::vector<MediaLaneSignals>& signals) {
  assert(signals.size() == numMediaLanes());

  auto txLos = getSettingsValue(CmisField::TX_LOS_FLAG);
  auto rxLos = getSettingsValue(CmisField::RX_LOS_FLAG);
  auto txLol = getSettingsValue(CmisField::TX_LOL_FLAG);
  auto rxLol = getSettingsValue(CmisField::RX_LOL_FLAG);
  auto txFault = getSettingsValue(CmisField::TX_FAULT_FLAG);
  auto txEq = getSettingsValue(CmisField::TX_EQ_FLAG);

  for (int lane = 0; lane < signals.size(); lane++) {
    auto laneMask = (1 << lane);
    signals[lane].lane_ref() = lane;
    signals[lane].txLos_ref() = txLos & laneMask;
    signals[lane].rxLos_ref() = rxLos & laneMask;
    signals[lane].txLol_ref() = txLol & laneMask;
    signals[lane].rxLol_ref() = rxLol & laneMask;
    signals[lane].txFault_ref() = txFault & laneMask;
    signals[lane].txAdaptEqFault_ref() = txEq & laneMask;
  }

  return true;
}

/*
 * Iterate through channels collecting appropriate data;
 */

bool CmisModule::getSignalsPerHostLane(std::vector<HostLaneSignals>& signals) {
  const uint8_t* data;
  int offset;
  int length;
  int dataAddress;

  assert(signals.size() == numHostLanes());

  auto dataPathDeInit = getSettingsValue(CmisField::DATA_PATH_DEINIT);
  getQsfpFieldAddress(CmisField::DATA_PATH_STATE, dataAddress, offset, length);
  data = getQsfpValuePtr(dataAddress, offset, length);

  for (int lane = 0; lane < signals.size(); lane++) {
    signals[lane].lane_ref() = lane;
    signals[lane].dataPathDeInit_ref() = dataPathDeInit & (1 << lane);

    bool evenLane = (lane % 2 == 0);
    signals[lane].cmisLaneState_ref() = (CmisLaneState)(
        evenLane ? data[lane / 2] & 0xF : (data[lane / 2] >> 4) & 0xF);
  }

  return true;
}

/*
 * Iterate through channels collecting appropriate data;
 */

bool CmisModule::getSensorsPerChanInfo(std::vector<Channel>& channels) {
  const uint8_t* data;
  int offset;
  int length;
  int dataAddress;

  for (int channel = 0; channel < numMediaLanes(); channel++) {
    channels[channel].sensors_ref()->rxPwr_ref()->flags_ref() =
        getChannelFlags(CmisField::RX_PWR_FLAG, channel);
  }

  for (int channel = 0; channel < numMediaLanes(); channel++) {
    channels[channel].sensors_ref()->txBias_ref()->flags_ref() =
        getChannelFlags(CmisField::TX_BIAS_FLAG, channel);
  }

  for (int channel = 0; channel < numMediaLanes(); channel++) {
    channels[channel].sensors_ref()->txPwr_ref()->flags_ref() =
        getChannelFlags(CmisField::TX_PWR_FLAG, channel);
  }

  getQsfpFieldAddress(CmisField::CHANNEL_RX_PWR, dataAddress, offset, length);
  data = getQsfpValuePtr(dataAddress, offset, length);

  for (auto& channel : channels) {
    uint16_t value = data[0] << 8 | data[1];
    auto pwr = CmisFieldInfo::getPwr(value); // This is in mW
    channel.sensors_ref()->rxPwr_ref()->value_ref() = pwr;
    Sensor rxDbm;
    rxDbm.value_ref() = mwToDb(pwr);
    channel.sensors_ref()->rxPwrdBm_ref() = rxDbm;
    data += 2;
    length--;
  }
  CHECK_GE(length, 0);

  getQsfpFieldAddress(CmisField::CHANNEL_TX_BIAS, dataAddress, offset, length);
  data = getQsfpValuePtr(dataAddress, offset, length);
  for (auto& channel : channels) {
    uint16_t value = data[0] << 8 | data[1];
    channel.sensors_ref()->txBias_ref()->value_ref() =
        CmisFieldInfo::getTxBias(value);
    data += 2;
    length--;
  }
  CHECK_GE(length, 0);

  getQsfpFieldAddress(CmisField::CHANNEL_TX_PWR, dataAddress, offset, length);
  data = getQsfpValuePtr(dataAddress, offset, length);

  for (auto& channel : channels) {
    uint16_t value = data[0] << 8 | data[1];
    auto pwr = CmisFieldInfo::getPwr(value); // This is in mW
    channel.sensors_ref()->txPwr_ref()->value_ref() = pwr;
    Sensor txDbm;
    txDbm.value_ref() = mwToDb(pwr);
    channel.sensors_ref()->txPwrdBm_ref() = txDbm;
    data += 2;
    length--;
  }
  CHECK_GE(length, 0);

  getQsfpFieldAddress(
      CmisField::MEDIA_BER_HOST_SNR, dataAddress, offset, length);
  data = getQsfpValuePtr(dataAddress, offset, length);

  for (auto& channel : channels) {
    // SNR value are LSB.
    uint16_t value = data[1] << 8 | data[0];
    channel.sensors_ref()->txSnr_ref() = Sensor();
    channel.sensors_ref()->txSnr_ref()->value_ref() =
        CmisFieldInfo::getSnr(value);
    data += 2;
    length--;
  }
  CHECK_GE(length, 0);

  getQsfpFieldAddress(CmisField::MEDIA_SNR, dataAddress, offset, length);
  data = getQsfpValuePtr(dataAddress, offset, length);

  for (auto& channel : channels) {
    // SNR value are LSB.
    uint16_t value = data[1] << 8 | data[0];
    channel.sensors_ref()->rxSnr_ref() = Sensor();
    channel.sensors_ref()->rxSnr_ref()->value_ref() =
        CmisFieldInfo::getSnr(value);
    data += 2;
    length--;
  }
  CHECK_GE(length, 0);

  return true;
}

std::string CmisModule::getQsfpString(CmisField field) const {
  int offset;
  int length;
  int dataAddress;

  getQsfpFieldAddress(field, dataAddress, offset, length);
  const uint8_t* data = getQsfpValuePtr(dataAddress, offset, length);

  while (length > 0 && data[length - 1] == ' ') {
    --length;
  }

  std::string value(reinterpret_cast<const char*>(data), length);
  return validateQsfpString(value) ? value : "UNKNOWN";
}

double CmisModule::getQsfpSensor(
    CmisField field,
    double (*conversion)(uint16_t value)) {
  auto info = CmisFieldInfo::getCmisFieldAddress(cmisFields, field);
  const uint8_t* data =
      getQsfpValuePtr(info.dataAddress, info.offset, info.length);
  return conversion(data[0] << 8 | data[1]);
}

TransmitterTechnology CmisModule::getQsfpTransmitterTechnology() const {
  auto info = CmisFieldInfo::getCmisFieldAddress(
      cmisFields, CmisField::MEDIA_INTERFACE_TECHNOLOGY);
  const uint8_t* data =
      getQsfpValuePtr(info.dataAddress, info.offset, info.length);

  uint8_t transTech = *data;
  if (transTech == DeviceTechnology::UNKNOWN_VALUE) {
    return TransmitterTechnology::UNKNOWN;
  } else if (transTech <= DeviceTechnology::OPTICAL_MAX_VALUE) {
    return TransmitterTechnology::OPTICAL;
  } else {
    return TransmitterTechnology::COPPER;
  }
}

SignalFlags CmisModule::getSignalFlagInfo() {
  SignalFlags signalFlags = SignalFlags();

  signalFlags.txLos_ref() = getSettingsValue(CmisField::TX_LOS_FLAG);
  signalFlags.rxLos_ref() = getSettingsValue(CmisField::RX_LOS_FLAG);
  signalFlags.txLol_ref() = getSettingsValue(CmisField::TX_LOL_FLAG);
  signalFlags.rxLol_ref() = getSettingsValue(CmisField::RX_LOL_FLAG);

  return signalFlags;
}

ExtendedSpecComplianceCode CmisModule::getExtendedSpecificationComplianceCode()
    const {
  return (ExtendedSpecComplianceCode)getSettingsValue(
      CmisField::EXTENDED_SPECIFICATION_COMPLIANCE);
}

TransceiverModuleIdentifier CmisModule::getIdentifier() {
  return (TransceiverModuleIdentifier)getSettingsValue(CmisField::IDENTIFIER);
}

void CmisModule::setQsfpFlatMem() {
  uint8_t flatMem;
  int offset;
  int length;
  int dataAddress;

  if (!present_) {
    throw FbossError("Failed setting QSFP flatMem: QSFP is not present");
  }

  getQsfpFieldAddress(CmisField::FLAT_MEM, dataAddress, offset, length);
  getQsfpValue(dataAddress, offset, length, &flatMem);
  flatMem_ = flatMem & (1 << 7);
  XLOG(DBG3) << "Detected QSFP " << qsfpImpl_->getName()
             << ", flatMem=" << flatMem_;
}

const uint8_t*
CmisModule::getQsfpValuePtr(int dataAddress, int offset, int length) const {
  /* if the cached values are not correct */
  if (!cacheIsValid()) {
    throw FbossError("Qsfp is either not present or the data is not read");
  }
  if (dataAddress == CmisPages::LOWER) {
    CHECK_LE(offset + length, sizeof(lowerPage_));
    /* Copy data from the cache */
    return (lowerPage_ + offset);
  } else {
    offset -= MAX_QSFP_PAGE_SIZE;
    CHECK_GE(offset, 0);
    CHECK_LE(offset, MAX_QSFP_PAGE_SIZE);

    // If this is a flatMem module, we will only have PAGE00 here.
    // Only when flatMem is false will we have data for other pages.

    if (dataAddress == CmisPages::PAGE00) {
      CHECK_LE(offset + length, sizeof(page0_));
      return (page0_ + offset);
    }

    if (flatMem_) {
      throw FbossError(
          "Accessing data address 0x%d on flatMem module.", dataAddress);
    }

    switch (dataAddress) {
      case CmisPages::PAGE01:
        CHECK_LE(offset + length, sizeof(page01_));
        return (page01_ + offset);
      case CmisPages::PAGE02:
        CHECK_LE(offset + length, sizeof(page02_));
        return (page02_ + offset);
      case CmisPages::PAGE10:
        CHECK_LE(offset + length, sizeof(page10_));
        return (page10_ + offset);
      case CmisPages::PAGE11:
        CHECK_LE(offset + length, sizeof(page11_));
        return (page11_ + offset);
      case CmisPages::PAGE13:
        CHECK_LE(offset + length, sizeof(page13_));
        return (page13_ + offset);
      case CmisPages::PAGE14:
        CHECK_LE(offset + length, sizeof(page14_));
        return (page14_ + offset);
      default:
        throw FbossError("Invalid Data Address 0x%d", dataAddress);
    }
  }
}

RawDOMData CmisModule::getRawDOMData() {
  lock_guard<std::mutex> g(qsfpModuleMutex_);
  RawDOMData data;
  if (present_) {
    *data.lower_ref() =
        IOBuf::wrapBufferAsValue(lowerPage_, MAX_QSFP_PAGE_SIZE);
    *data.page0_ref() = IOBuf::wrapBufferAsValue(page0_, MAX_QSFP_PAGE_SIZE);
    data.page10_ref() = IOBuf::wrapBufferAsValue(page10_, MAX_QSFP_PAGE_SIZE);
    data.page11_ref() = IOBuf::wrapBufferAsValue(page11_, MAX_QSFP_PAGE_SIZE);
  }
  return data;
}

DOMDataUnion CmisModule::getDOMDataUnion() {
  lock_guard<std::mutex> g(qsfpModuleMutex_);
  CmisData cmisData;
  if (present_) {
    *cmisData.lower_ref() =
        IOBuf::wrapBufferAsValue(lowerPage_, MAX_QSFP_PAGE_SIZE);
    *cmisData.page0_ref() =
        IOBuf::wrapBufferAsValue(page0_, MAX_QSFP_PAGE_SIZE);
    if (!flatMem_) {
      cmisData.page01_ref() =
          IOBuf::wrapBufferAsValue(page01_, MAX_QSFP_PAGE_SIZE);
      cmisData.page02_ref() =
          IOBuf::wrapBufferAsValue(page02_, MAX_QSFP_PAGE_SIZE);
      cmisData.page10_ref() =
          IOBuf::wrapBufferAsValue(page10_, MAX_QSFP_PAGE_SIZE);
      cmisData.page11_ref() =
          IOBuf::wrapBufferAsValue(page11_, MAX_QSFP_PAGE_SIZE);
      cmisData.page13_ref() =
          IOBuf::wrapBufferAsValue(page13_, MAX_QSFP_PAGE_SIZE);
      cmisData.page14_ref() =
          IOBuf::wrapBufferAsValue(page14_, MAX_QSFP_PAGE_SIZE);
    }
  }
  cmisData.timeCollected_ref() = lastRefreshTime_;
  DOMDataUnion data;
  data.set_cmis(cmisData);
  return data;
}

void CmisModule::getFieldValue(CmisField fieldName, uint8_t* fieldValue) const {
  lock_guard<std::mutex> g(qsfpModuleMutex_);
  int offset;
  int length;
  int dataAddress;
  getQsfpFieldAddress(fieldName, dataAddress, offset, length);
  getQsfpValue(dataAddress, offset, length, fieldValue);
}

void CmisModule::getFieldValueLocked(CmisField fieldName, uint8_t* fieldValue)
    const {
  // Expect lock being held here.
  int offset;
  int length;
  int dataAddress;
  getQsfpFieldAddress(fieldName, dataAddress, offset, length);
  getQsfpValue(dataAddress, offset, length, fieldValue);
}

void CmisModule::updateQsfpData(bool allPages) {
  // expects the lock to be held
  if (!present_) {
    return;
  }
  try {
    XLOG(DBG2) << "Performing " << ((allPages) ? "full" : "partial")
               << " qsfp data cache refresh for transceiver "
               << folly::to<std::string>(qsfpImpl_->getName());
    qsfpImpl_->readTransceiver(
        TransceiverI2CApi::ADDR_QSFP, 0, sizeof(lowerPage_), lowerPage_);
    lastRefreshTime_ = std::time(nullptr);
    dirty_ = false;
    setQsfpFlatMem();
    if ((getSettingsValue(CmisField::MODULE_STATE) >> 1 & 0x7) ==
        static_cast<uint8_t>(CmisModuleState::READY)) {
      opticsModuleStateMachine_.get_attribute(cmisModuleReady) = true;
    } else {
      opticsModuleStateMachine_.get_attribute(cmisModuleReady) = false;
    }

    // If we have flat memory, we don't have to set the page
    if (!flatMem_) {
      uint8_t page = 0x00;
      qsfpImpl_->writeTransceiver(
          TransceiverI2CApi::ADDR_QSFP, 127, sizeof(page), &page);
    }
    qsfpImpl_->readTransceiver(
        TransceiverI2CApi::ADDR_QSFP, 128, sizeof(page0_), page0_);
    if (!flatMem_) {
      uint8_t page = 0x10;
      qsfpImpl_->writeTransceiver(
          TransceiverI2CApi::ADDR_QSFP, 127, sizeof(page), &page);
      qsfpImpl_->readTransceiver(
          TransceiverI2CApi::ADDR_QSFP, 128, sizeof(page10_), page10_);

      page = 0x11;
      qsfpImpl_->writeTransceiver(
          TransceiverI2CApi::ADDR_QSFP, 127, sizeof(page), &page);
      qsfpImpl_->readTransceiver(
          TransceiverI2CApi::ADDR_QSFP, 128, sizeof(page11_), page11_);

      if (opticsModuleStateMachine_.get_attribute(cmisModuleReady)) {
        page = 0x14;
        auto diagFeature = (uint8_t)DiagnosticFeatureEncoding::SNR;
        qsfpImpl_->writeTransceiver(
            TransceiverI2CApi::ADDR_QSFP, 127, sizeof(page), &page);
        qsfpImpl_->writeTransceiver(
            TransceiverI2CApi::ADDR_QSFP,
            128,
            sizeof(diagFeature),
            &diagFeature);
        qsfpImpl_->readTransceiver(
            TransceiverI2CApi::ADDR_QSFP, 128, sizeof(page14_), page14_);
      }
    }

    if (!allPages) {
      // The information on the following pages are static. Thus no need to
      // fetch them every time. We just need to do it when we first retriving
      // the data from this module.
      return;
    }

    if (!flatMem_) {
      uint8_t page = 0x01;
      qsfpImpl_->writeTransceiver(
          TransceiverI2CApi::ADDR_QSFP, 127, sizeof(page), &page);
      qsfpImpl_->readTransceiver(
          TransceiverI2CApi::ADDR_QSFP, 128, sizeof(page01_), page01_);

      page = 0x02;
      qsfpImpl_->writeTransceiver(
          TransceiverI2CApi::ADDR_QSFP, 127, sizeof(page), &page);
      qsfpImpl_->readTransceiver(
          TransceiverI2CApi::ADDR_QSFP, 128, sizeof(page02_), page02_);

      page = 0x13;
      qsfpImpl_->writeTransceiver(
          TransceiverI2CApi::ADDR_QSFP, 127, sizeof(page), &page);
      qsfpImpl_->readTransceiver(
          TransceiverI2CApi::ADDR_QSFP, 128, sizeof(page13_), page13_);
    }
  } catch (const std::exception& ex) {
    // No matter what kind of exception throws, we need to set the dirty_ flag
    // to true.
    dirty_ = true;
    XLOG(ERR) << "Error update data for transceiver:"
              << folly::to<std::string>(qsfpImpl_->getName()) << ": "
              << ex.what();
    throw;
  }
}

void CmisModule::setApplicationCode(cfg::PortSpeed speed) {
  auto applicationIter = speedApplicationMapping.find(speed);

  // Currently we will have the same application across all the lanes. So here
  // we only take one of them to look at.
  uint8_t currentApplicationSel =
      getSettingsValue(CmisField::ACTIVE_CTRL_LANE_1, APP_SEL_MASK);

  // The application sel code is at the higher four bits of the field.
  currentApplicationSel = currentApplicationSel >> 4;

  XLOG(INFO) << "currentApplicationSel: " << currentApplicationSel;

  uint8_t currentApplication;
  int offset;
  int length;
  int dataAddress;

  getQsfpFieldAddress(
      CmisField::APPLICATION_ADVERTISING1, dataAddress, offset, length);
  // We use the module Media Interface ID, which is located at the second byte
  // of the field, as Application ID here.
  offset += (currentApplicationSel - 1) * length + 1;
  getQsfpValue(dataAddress, offset, 1, &currentApplication);

  XLOG(INFO) << "currentApplication: " << std::hex << (int)currentApplication;

  if (static_cast<uint8_t>(applicationIter->second) == currentApplication) {
    XLOG(INFO) << "speed matches. Doing nothing.";
    return;
  } else if (applicationIter == speedApplicationMapping.end()) {
    XLOG(INFO) << "Unsupported Speed.";
    throw FbossError(folly::to<std::string>(
        "Port: ",
        qsfpImpl_->getName(),
        " Unsupported speed: ",
        apache::thrift::util::enumNameSafe(speed)));
  }

  auto capabilityIter =
      moduleCapabilities_.find(static_cast<uint8_t>(applicationIter->second));

  if (capabilityIter == moduleCapabilities_.end()) {
    XLOG(INFO) << "Unsupported Application";
    throw FbossError(folly::to<std::string>(
        "Port: ",
        qsfpImpl_->getName(),
        " Unsupported Application by the module: ",
        applicationIter->second));
  } else if (capabilityIter->second.ApSelCode == currentApplicationSel) {
    // There shouldn't be a valid path to get here. But just to be safe, having
    // another check here.
    throw FbossError(folly::to<std::string>(
        "Port: ",
        qsfpImpl_->getName(),
        " confused about the application settings, currentApplicationSel: ",
        currentApplicationSel,
        ". Trying to switch to ",
        capabilityIter->second.ApSelCode));
  }

  // Flip to page 0x10 to get prepared.
  uint8_t page = 0x10;
  qsfpImpl_->writeTransceiver(
      TransceiverI2CApi::ADDR_QSFP, 127, sizeof(page), &page);

  // In 400G-FR4 case we will have 8 host lanes instead of 4. Further more,
  // we need to deactivate all the lanes when we switch to an application with
  // a different lane count. CMIS4.0-8.8.4
  getQsfpFieldAddress(CmisField::DATA_PATH_DEINIT, dataAddress, offset, length);
  uint8_t dataPathDeInit = 0xff;
  qsfpImpl_->writeTransceiver(
      TransceiverI2CApi::ADDR_QSFP, offset, length, &dataPathDeInit);
  /* sleep override */
  usleep(kUsecBetweenLaneInit);

  // Currently we will have only one data path and apply the default settings.
  // So assume the lower four bits are all zero here. CMIS4.0-8.7.3
  uint8_t newApSelCode = capabilityIter->second.ApSelCode << 4;

  // Update the numHostLanes and numMediaLanes of the module.
  numHostLanes_ = capabilityIter->second.hostLaneCount;
  numMediaLanes_ = capabilityIter->second.mediaLaneCount;

  XLOG(INFO) << "newApSelCode: " << std::hex << (int)newApSelCode;

  getQsfpFieldAddress(CmisField::APP_SEL_LANE_1, dataAddress, offset, length);

  for (int channel = 0; channel < numHostLanes(); channel++) {
    // For now we don't have complicated lane assignment. Either using first
    // four lanes for 100G/200G or all eight lanes for 400G.
    uint8_t laneApSelCode =
        (channel < capabilityIter->second.hostLaneCount) ? newApSelCode : 0;
    qsfpImpl_->writeTransceiver(
        TransceiverI2CApi::ADDR_QSFP,
        offset + channel,
        sizeof(laneApSelCode),
        &laneApSelCode);
  }

  uint8_t applySet0 = (capabilityIter->second.hostLaneCount == 8) ? 0xff : 0x0f;

  getQsfpFieldAddress(CmisField::STAGE_CTRL_SET_0, dataAddress, offset, length);
  qsfpImpl_->writeTransceiver(
      TransceiverI2CApi::ADDR_QSFP, offset, sizeof(applySet0), &applySet0);

  XLOG(INFO) << "Port: " << folly::to<std::string>(qsfpImpl_->getName())
             << " set application to " << capabilityIter->first;

  // Release the lanes from DeInit.
  getQsfpFieldAddress(CmisField::DATA_PATH_DEINIT, dataAddress, offset, length);
  dataPathDeInit = 0x0;
  qsfpImpl_->writeTransceiver(
      TransceiverI2CApi::ADDR_QSFP, offset, length, &dataPathDeInit);
}

/*
 * Put logic here that should only be run on ports that have been
 * down for a long time. These are actions that are potentially more
 * disruptive, but have worked in the past to recover a transceiver.
 */
void CmisModule::remediateFlakyTransceiver() {
  XLOG(INFO) << "Performing potentially disruptive remediations on "
             << qsfpImpl_->getName();

  if (moduleResetCounter_ < kResetCounterLimit) {
    // This api accept 1 based module id however the module id in WedgeManager
    // is 0 based.
    transceiverManager_->getQsfpPlatformApi()->triggerQsfpHardReset(
        static_cast<unsigned int>(getID()) + 1);
    moduleResetCounter_++;
  } else {
    XLOG(DBG2) << "Reached reset limit for module " << qsfpImpl_->getName();
  }

  lastRemediateTime_ = std::time(nullptr);
}

void CmisModule::setPowerOverrideIfSupported(PowerControlState currentState) {
  /* Wedge forces Low Power mode via a pin;  we have to reset this
   * to force High Power mode on all transceivers except SR4-40G.
   *
   * Note that this function expects to be called with qsfpModuleMutex_
   * held.
   */

  int offset;
  int length;
  int dataAddress;

  auto portStr = folly::to<std::string>(qsfpImpl_->getName());

  if (currentState == PowerControlState::HIGH_POWER_OVERRIDE) {
    XLOG(INFO) << "Port: " << folly::to<std::string>(qsfpImpl_->getName())
               << " Power override already correctly set, doing nothing";
    return;
  }

  getQsfpFieldAddress(CmisField::MODULE_CONTROL, dataAddress, offset, length);

  uint8_t currentModuleControl;
  getFieldValueLocked(CmisField::MODULE_CONTROL, &currentModuleControl);

  // LowPwr is on the 6 bit of ModuleControl.
  currentModuleControl = currentModuleControl | (1 << 6);

  // first set to low power
  qsfpImpl_->writeTransceiver(
      TransceiverI2CApi::ADDR_QSFP, offset, length, &currentModuleControl);

  // Transceivers need a bit of time to handle the low power setting
  // we just sent. We should be able to use the status register to be
  // smarter about this, but just sleeping 0.1s for now.
  usleep(kUsecBetweenPowerModeFlap);

  // then enable target power class
  currentModuleControl = currentModuleControl & 0x3f;

  qsfpImpl_->writeTransceiver(
      TransceiverI2CApi::ADDR_QSFP, offset, length, &currentModuleControl);

  XLOG(INFO) << "Port " << portStr << ": QSFP module control field set to "
             << std::hex << (int)currentModuleControl;
}

void CmisModule::ensureRxOutputSquelchEnabled(
    const std::vector<HostLaneSettings>& hostLanesSettings) const {
  bool allLanesRxOutputSquelchEnabled = true;
  for (auto& hostLaneSettings : hostLanesSettings) {
    if (hostLaneSettings.rxSquelch_ref().has_value() &&
        *hostLaneSettings.rxSquelch_ref()) {
      allLanesRxOutputSquelchEnabled = false;
      break;
    }
  }

  if (!allLanesRxOutputSquelchEnabled) {
    int offset;
    int length;
    int dataAddress;
    uint8_t enableAllLaneRxOutputSquelch = 0x0;

    // Flip to page 0x10 to get prepared.
    uint8_t page = 0x10;
    qsfpImpl_->writeTransceiver(
        TransceiverI2CApi::ADDR_QSFP, 127, sizeof(page), &page);

    getQsfpFieldAddress(
        CmisField::RX_SQUELCH_DISABLE, dataAddress, offset, length);

    qsfpImpl_->writeTransceiver(
        TransceiverI2CApi::ADDR_QSFP,
        offset,
        length,
        &enableAllLaneRxOutputSquelch);
    XLOG(INFO) << "Transceiver " << folly::to<std::string>(qsfpImpl_->getName())
               << ": Enabled Rx output squelch on all lanes.";
  }
}

void CmisModule::customizeTransceiverLocked(cfg::PortSpeed speed) {
  /*
   * This must be called with a lock held on qsfpModuleMutex_
   */
  if (customizationSupported()) {
    TransceiverSettings settings = getTransceiverSettingsInfo();

    // We want this on regardless of speed
    setPowerOverrideIfSupported(*settings.powerControl_ref());

    if (speed != cfg::PortSpeed::DEFAULT) {
      setApplicationCode(speed);
    }
  } else {
    XLOG(DBG1) << "Customization not supported on " << qsfpImpl_->getName();
  }

  lastCustomizeTime_ = std::time(nullptr);
  needsCustomization_ = false;
}

} // namespace fboss
} // namespace facebook
