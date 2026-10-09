/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/smc.h"
#include "xenia/kernel/util/shim_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

DECLARE_int32(avpack);

DEFINE_double(smc_temp_cpu, 0.0,
              "Force the CPU temperature (Celsius) reported by the virtual "
              "SMC. 0 = derive it from the host CPU load.",
              "Kernel");
DEFINE_double(smc_temp_gpu, 0.0,
              "Force the GPU temperature (Celsius) reported by the virtual "
              "SMC. 0 = derive it from the host CPU load.",
              "Kernel");
DEFINE_double(smc_temp_edram, 0.0,
              "Force the eDRAM temperature (Celsius) reported by the virtual "
              "SMC. 0 = derive it from the host CPU load.",
              "Kernel");
DEFINE_double(smc_temp_board, 0.0,
              "Force the motherboard temperature (Celsius) reported by the "
              "virtual SMC. 0 = derive it from the host CPU load.",
              "Kernel");

namespace {
// Host CPU load in [0,1] since the previous call (-1 when unavailable).
double SampleHostCpuLoad() {
#if XE_PLATFORM_WIN32
  static ULONGLONG last_idle = 0, last_total = 0;
  FILETIME idle_ft, kernel_ft, user_ft;
  if (!GetSystemTimes(&idle_ft, &kernel_ft, &user_ft)) return -1.0;
  auto to_u64 = [](const FILETIME& f) {
    return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
  };
  const ULONGLONG idle = to_u64(idle_ft);
  const ULONGLONG total = to_u64(kernel_ft) + to_u64(user_ft);  // incl. idle
  const ULONGLONG d_idle = idle - last_idle, d_total = total - last_total;
  last_idle = idle;
  last_total = total;
  if (!d_total) return -1.0;
  return 1.0 - static_cast<double>(d_idle) / static_cast<double>(d_total);
#else
  static unsigned long long last_idle = 0, last_total = 0;
  FILE* f = std::fopen("/proc/stat", "r");
  if (!f) return -1.0;
  unsigned long long v[8] = {};
  const int n = std::fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                            &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
                            &v[7]);
  std::fclose(f);
  if (n < 4) return -1.0;
  unsigned long long total = 0;
  for (auto x : v) total += x;
  const unsigned long long idle = v[3] + v[4];
  const auto d_idle = idle - last_idle, d_total = total - last_total;
  last_idle = idle;
  last_total = total;
  if (!d_total) return -1.0;
  return 1.0 - static_cast<double>(d_idle) / static_cast<double>(d_total);
#endif
}
}  // namespace

namespace xe {
namespace kernel {

SystemManagementController::SystemManagementController()
    : dvd_tray_state_(X_DVD_TRAY_STATE::OPEN) {
  auto registerQuery =
      [&](X_SMC_CMD command,
          void (SystemManagementController::*fn)(X_SMC_DATA*, X_SMC_DATA*)) {
        smc_commands_[command] = [this, fn](X_SMC_DATA* message,
                                            X_SMC_DATA* response) {
          (this->*fn)(message, response);
        };
      };

  registerQuery(X_SMC_CMD::QUERY_TEMP_SENSOR,
                &SystemManagementController::QueryTemperatureSensor);
  registerQuery(X_SMC_CMD::QUERY_TRAY,
                &SystemManagementController::QueryDriveTraySensor);
  registerQuery(X_SMC_CMD::QUERY_AV_PACK,
                &SystemManagementController::QueryAvPack);
  registerQuery(X_SMC_CMD::QUERY_SMC_VERSION,
                &SystemManagementController::QuerySmcVersion);
  registerQuery(X_SMC_CMD::QUERY_IR_ADDRESS,
                &SystemManagementController::QueryIRAddress);
  registerQuery(X_SMC_CMD::QUERY_TILT_SENSOR,
                &SystemManagementController::QueryTiltState);
  registerQuery(X_SMC_CMD::SET_FAN_SPEED_CPU,
                &SystemManagementController::SetFanSpeed);
  registerQuery(X_SMC_CMD::SET_FAN_SPEED_GPU,
                &SystemManagementController::SetFanSpeed);
  registerQuery(X_SMC_CMD::SET_DVD_TRAY,
                &SystemManagementController::SetDriveTray);
  registerQuery(X_SMC_CMD::SET_IR_ADDRESS,
                &SystemManagementController::SetIRAddress);
  registerQuery(X_SMC_CMD::SET_POWER_LED,
                &SystemManagementController::SetPowerLed);
  registerQuery(X_SMC_CMD::SET_LEDS, &SystemManagementController::SetLedState);
};
SystemManagementController::~SystemManagementController() {};

void SystemManagementController::SetTrayState(X_DVD_TRAY_STATE state) {
  dvd_tray_state_ = state;
  kernel_state()->BroadcastNotification(kXNotificationSystemTrayStateChanged,
                                        static_cast<uint8_t>(state));
}

void SystemManagementController::CallCommand(X_SMC_DATA* smc_message,
                                             X_SMC_DATA* smc_response) {
  const auto itr = smc_commands_.find(smc_message->command);
  if (itr == smc_commands_.cend()) {
    XELOGW("Unimplemented SMC Command: {:02X}",
           static_cast<uint8_t>(smc_message->command));
    return;
  }

  itr->second(smc_message, smc_response);
}

void SystemManagementController::QueryTemperatureSensor(
    X_SMC_DATA* smc_message, X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;

  // Virtual thermal model. A real console reads thermistors; here the values
  // follow the host CPU load (smoothed), and each can be forced with the
  // smc_temp_* cvars.
  const auto now = std::chrono::steady_clock::now();
  if (now - last_temp_sample_ >= std::chrono::milliseconds(500) ||
      last_temp_sample_ == std::chrono::steady_clock::time_point{}) {
    last_temp_sample_ = now;
    const double load = SampleHostCpuLoad();
    if (load >= 0.0) {
      host_load_ = host_load_ * 0.7 + std::clamp(load, 0.0, 1.0) * 0.3;
    }
  }
  const float load = static_cast<float>(host_load_);
  const float cpu = 48.0f + 32.0f * load;
  const float gpu = 46.0f + 30.0f * load;
  const float edram = 44.0f + 28.0f * load;
  const float board = 40.0f + 18.0f * load;
  auto pick = [](double forced, float modelled) {
    return forced > 0.0 ? static_cast<float>(forced) : modelled;
  };
  last_temps_[0] = pick(cvars::smc_temp_cpu, cpu);
  last_temps_[1] = pick(cvars::smc_temp_gpu, gpu);
  last_temps_[2] = pick(cvars::smc_temp_edram, edram);
  last_temps_[3] = pick(cvars::smc_temp_board, board);
  smc_response->temps.cpu.SetTemp(last_temps_[0]);
  smc_response->temps.gpu.SetTemp(last_temps_[1]);
  smc_response->temps.edram.SetTemp(last_temps_[2]);
  smc_response->temps.mb.SetTemp(last_temps_[3]);
}

void SystemManagementController::QueryDriveTraySensor(
    X_SMC_DATA* smc_message, X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;
  smc_response->dvd_tray.state = dvd_tray_state_;
};

void SystemManagementController::QueryAvPack(X_SMC_DATA* smc_message,
                                             X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;
  smc_response->av_pack.av_pack = 0;

  const auto entry = av_pack_to_smc_value.find(cvars::avpack);
  if (entry == av_pack_to_smc_value.cend()) {
    return;
  }

  smc_response->av_pack.av_pack = entry->second;
}

void SystemManagementController::QuerySmcVersion(X_SMC_DATA* smc_message,
                                                 X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;
  smc_response->smc_version.unk = smc_version[0];
  smc_response->smc_version.major = smc_version[1];
  smc_response->smc_version.minor = smc_version[2];
}

void SystemManagementController::QueryIRAddress(X_SMC_DATA* smc_message,
                                                X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;
  smc_response->ir_address.ir_address = ir_address_;
}

void SystemManagementController::QueryTiltState(X_SMC_DATA* smc_message,
                                                X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;
  smc_response->tilt_state.tilt_state = tilt_state_;
}

void SystemManagementController::SetIRAddress(X_SMC_DATA* smc_message,
                                              X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;
  smc_response->ir_address.ir_address = ir_address_;
}

void SystemManagementController::SetDriveTray(X_SMC_DATA* smc_message,
                                              X_SMC_DATA* smc_response) {
  SetTrayState(
      static_cast<X_DVD_TRAY_STATE>((smc_message->smc_data[0] & 0xF) % 5));
}

void SystemManagementController::SetFanSpeed(X_SMC_DATA* smc_message,
                                             X_SMC_DATA* smc_response) {
  if (smc_message->command == X_SMC_CMD::SET_FAN_SPEED_CPU) {
    cpu_fan_speed_ = (smc_message->smc_data[0] - 0x80);
  }
  if (smc_message->command == X_SMC_CMD::SET_FAN_SPEED_GPU) {
    gpu_fan_speed_ = (smc_message->smc_data[0] - 0x80);
  }
}

void SystemManagementController::SetPowerLed(X_SMC_DATA* smc_message,
                                             X_SMC_DATA* smc_response) {
  power_led_state_ = {smc_message->power_led_state.state,
                      smc_message->power_led_state.animate};
}

void SystemManagementController::SetLedState(X_SMC_DATA* smc_message,
                                             X_SMC_DATA* smc_response) {
  led_state_ = {smc_message->led_state.state, smc_message->led_state.region};
}

}  // namespace kernel
}  // namespace xe
