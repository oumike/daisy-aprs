#include "runtime_config.h"

#include <Preferences.h>

#include "app_config.h"

namespace {
constexpr const char* kPrefsNs = "aprs_cfg";

void copyString(char* dst, size_t dstLen, const String& src) {
  if (dstLen == 0) {
    return;
  }

  const size_t copyLen = (src.length() < (dstLen - 1)) ? src.length() : (dstLen - 1);
  memcpy(dst, src.c_str(), copyLen);
  dst[copyLen] = '\0';
}

void sanitize(RuntimeConfig& cfg) {
  if (cfg.spreadingFactor < 7) {
    cfg.spreadingFactor = 7;
  }
  if (cfg.spreadingFactor > 12) {
    cfg.spreadingFactor = 12;
  }

  if (cfg.bandwidthKhz < 7.8f) {
    cfg.bandwidthKhz = 7.8f;
  }
  if (cfg.bandwidthKhz > 500.0f) {
    cfg.bandwidthKhz = 500.0f;
  }

  if (cfg.codingRate < 5) {
    cfg.codingRate = 5;
  }
  if (cfg.codingRate > 8) {
    cfg.codingRate = 8;
  }

  if (cfg.txPowerDbm < -9) {
    cfg.txPowerDbm = -9;
  }
  if (cfg.txPowerDbm > 22) {
    cfg.txPowerDbm = 22;
  }

  if (cfg.beaconIntervalMs < 300000UL) {
    cfg.beaconIntervalMs = 300000UL;
  }
  cfg.noFixLogIntervalMs = AppConfig::kNoFixLogIntervalMs;

  if (cfg.screenTimeoutSec == 0) {
    cfg.screenTimeoutSec = AppConfig::kScreenTimeoutSec;
  }
  if (cfg.screenTimeoutSec > AppConfig::kScreenTimeoutMaxSec) {
    cfg.screenTimeoutSec = AppConfig::kScreenTimeoutMaxSec;
  }

  if (cfg.symbolTable == '\0') {
    cfg.symbolTable = AppConfig::kSymbolTable;
  }
  if (cfg.symbol == '\0') {
    cfg.symbol = AppConfig::kSymbol;
  }

  if (cfg.aprsphMessage[0] == '\0') {
    copyString(cfg.aprsphMessage, sizeof(cfg.aprsphMessage), String(AppConfig::kAprsphMessage));
  }
  if (cfg.hotgMessage[0] == '\0') {
    copyString(cfg.hotgMessage, sizeof(cfg.hotgMessage), String(AppConfig::kHotgMessage));
  }
}
}  // namespace

void runtimeConfigSetDefaults(RuntimeConfig& cfg) {
  memset(&cfg, 0, sizeof(cfg));

  copyString(cfg.callsign, sizeof(cfg.callsign), String(AppConfig::kCallsign));
  copyString(cfg.destination, sizeof(cfg.destination), String(AppConfig::kDestination));
  copyString(cfg.path, sizeof(cfg.path), String(AppConfig::kPath));
  copyString(cfg.comment, sizeof(cfg.comment), String(AppConfig::kComment));
  copyString(cfg.aprsphMessage, sizeof(cfg.aprsphMessage), String(AppConfig::kAprsphMessage));
  copyString(cfg.hotgMessage, sizeof(cfg.hotgMessage), String(AppConfig::kHotgMessage));

  cfg.symbolTable = AppConfig::kSymbolTable;
  cfg.symbol = AppConfig::kSymbol;

  cfg.beaconIntervalMs = AppConfig::kBeaconIntervalMs;
  cfg.noFixLogIntervalMs = AppConfig::kNoFixLogIntervalMs;
  cfg.screenTimeoutSec = AppConfig::kScreenTimeoutSec;

  cfg.frequencyMhz = AppConfig::kFrequencyMhz;
  cfg.spreadingFactor = AppConfig::kSpreadingFactor;
  cfg.bandwidthKhz = AppConfig::kBandwidthKhz;
  cfg.codingRate = AppConfig::kCodingRate;
  cfg.txPowerDbm = AppConfig::kTxPowerDbm;

  cfg.allowManualPosition = false;
  cfg.manualLatE7 = 0;
  cfg.manualLonE7 = 0;
  cfg.manualAltMeters = 0;

  copyString(cfg.wifiSsid, sizeof(cfg.wifiSsid), String(AppConfig::kDefaultWifiSsid));
  copyString(cfg.wifiPass, sizeof(cfg.wifiPass), String(AppConfig::kDefaultWifiPass));

  sanitize(cfg);
}

void runtimeConfigLoad(RuntimeConfig& cfg) {
  runtimeConfigSetDefaults(cfg);

  Preferences prefs;
  if (!prefs.begin(kPrefsNs, false)) {
    return;
  }

  if (prefs.isKey("callsign")) {
    copyString(cfg.callsign, sizeof(cfg.callsign), prefs.getString("callsign", ""));
  }
  if (prefs.isKey("dest")) {
    copyString(cfg.destination, sizeof(cfg.destination), prefs.getString("dest", ""));
  }
  if (prefs.isKey("path")) {
    copyString(cfg.path, sizeof(cfg.path), prefs.getString("path", ""));
  }
  if (prefs.isKey("comment")) {
    copyString(cfg.comment, sizeof(cfg.comment), prefs.getString("comment", ""));
  }
  if (prefs.isKey("aprsph_msg")) {
    copyString(cfg.aprsphMessage, sizeof(cfg.aprsphMessage), prefs.getString("aprsph_msg", ""));
  }
  if (prefs.isKey("hotg_msg")) {
    copyString(cfg.hotgMessage, sizeof(cfg.hotgMessage), prefs.getString("hotg_msg", ""));
  }

  if (prefs.isKey("symtbl")) {
    const String symTab = prefs.getString("symtbl", "");
    if (symTab.length() > 0) {
      cfg.symbolTable = symTab[0];
    }
  }

  if (prefs.isKey("symbol")) {
    const String sym = prefs.getString("symbol", "");
    if (sym.length() > 0) {
      cfg.symbol = sym[0];
    }
  }

  if (prefs.isKey("bcn_ms")) {
    cfg.beaconIntervalMs = prefs.getULong("bcn_ms", cfg.beaconIntervalMs);
  }
  if (prefs.isKey("scr_to_s")) {
    cfg.screenTimeoutSec = static_cast<uint16_t>(prefs.getUShort("scr_to_s", cfg.screenTimeoutSec));
  }

  if (prefs.isKey("freq")) {
    cfg.frequencyMhz = prefs.getFloat("freq", cfg.frequencyMhz);
  }
  if (prefs.isKey("sf")) {
    cfg.spreadingFactor = prefs.getInt("sf", cfg.spreadingFactor);
  }
  if (prefs.isKey("bw")) {
    cfg.bandwidthKhz = prefs.getFloat("bw", cfg.bandwidthKhz);
  }
  if (prefs.isKey("cr")) {
    cfg.codingRate = prefs.getInt("cr", cfg.codingRate);
  }
  if (prefs.isKey("pwr")) {
    cfg.txPowerDbm = prefs.getInt("pwr", cfg.txPowerDbm);
  }

  if (prefs.isKey("man_pos")) {
    cfg.allowManualPosition = prefs.getBool("man_pos", cfg.allowManualPosition);
  }
  if (prefs.isKey("lat_e7")) {
    cfg.manualLatE7 = prefs.getLong("lat_e7", cfg.manualLatE7);
  }
  if (prefs.isKey("lon_e7")) {
    cfg.manualLonE7 = prefs.getLong("lon_e7", cfg.manualLonE7);
  }
  if (prefs.isKey("alt_m")) {
    cfg.manualAltMeters = prefs.getInt("alt_m", cfg.manualAltMeters);
  }

  if (prefs.isKey("wifi_ssid")) {
    copyString(cfg.wifiSsid, sizeof(cfg.wifiSsid), prefs.getString("wifi_ssid", ""));
  }
  if (prefs.isKey("wifi_pass")) {
    copyString(cfg.wifiPass, sizeof(cfg.wifiPass), prefs.getString("wifi_pass", ""));
  }

  if (cfg.wifiSsid[0] == '\0') {
    copyString(cfg.wifiSsid, sizeof(cfg.wifiSsid), String(AppConfig::kDefaultWifiSsid));
  }
  if (cfg.wifiPass[0] == '\0') {
    copyString(cfg.wifiPass, sizeof(cfg.wifiPass), String(AppConfig::kDefaultWifiPass));
  }

  prefs.end();

  sanitize(cfg);
}

bool runtimeConfigSave(const RuntimeConfig& inCfg) {
  RuntimeConfig cfg = inCfg;
  sanitize(cfg);

  Preferences prefs;
  if (!prefs.begin(kPrefsNs, false)) {
    return false;
  }

  prefs.putString("callsign", cfg.callsign);
  prefs.putString("dest", cfg.destination);
  prefs.putString("path", cfg.path);
  prefs.putString("comment", cfg.comment);
  prefs.putString("aprsph_msg", cfg.aprsphMessage);
  prefs.putString("hotg_msg", cfg.hotgMessage);

  char symTab[2] = {cfg.symbolTable, '\0'};
  char sym[2] = {cfg.symbol, '\0'};
  prefs.putString("symtbl", symTab);
  prefs.putString("symbol", sym);

  prefs.putULong("bcn_ms", cfg.beaconIntervalMs);
  prefs.remove("nofix_ms");
  prefs.putUShort("scr_to_s", cfg.screenTimeoutSec);

  prefs.putFloat("freq", cfg.frequencyMhz);
  prefs.putInt("sf", cfg.spreadingFactor);
  prefs.putFloat("bw", cfg.bandwidthKhz);
  prefs.putInt("cr", cfg.codingRate);
  prefs.putInt("pwr", cfg.txPowerDbm);

  prefs.putBool("man_pos", cfg.allowManualPosition);
  prefs.putLong("lat_e7", cfg.manualLatE7);
  prefs.putLong("lon_e7", cfg.manualLonE7);
  prefs.putInt("alt_m", cfg.manualAltMeters);

  prefs.putString("wifi_ssid", cfg.wifiSsid);
  prefs.putString("wifi_pass", cfg.wifiPass);

  prefs.end();
  return true;
}

bool runtimeConfigFactoryReset(RuntimeConfig& cfg) {
  Preferences prefs;
  if (!prefs.begin(kPrefsNs, false)) {
    return false;
  }

  prefs.clear();
  prefs.end();

  runtimeConfigSetDefaults(cfg);
  return true;
}
