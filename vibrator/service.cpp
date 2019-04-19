/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define LOG_TAG "android.hardware.vibrator@1.3-service.coral"

#include <hidl/HidlSupport.h>
#include <hidl/HidlTransportSupport.h>
#include <utils/Errors.h>
#include <utils/StrongPointer.h>

#include "Vibrator.h"

using android::hardware::configureRpcThreadpool;
using android::hardware::joinRpcThreadpool;
using android::hardware::vibrator::V1_3::implementation::Vibrator;
using namespace android;

static const uint32_t Q_FLOAT_TO_FIXED = 1 << 16;
static const float Q_INDEX_TO_FLOAT = 1.5f;
static const uint32_t Q_INDEX_TO_FIXED = Q_INDEX_TO_FLOAT * Q_FLOAT_TO_FIXED;
static const uint32_t Q_INDEX_OFFSET = 2.0f * Q_FLOAT_TO_FIXED;

static constexpr uint32_t Q_DEFAULT = 15.5 * Q_FLOAT_TO_FIXED;
static const std::vector<uint32_t> V_LEVELS_DEFAULT = {60, 70, 80, 90, 100, 76};

static constexpr char ACTIVATE_PATH[] = "/sys/class/leds/vibrator/activate";
static constexpr char DURATION_PATH[] = "/sys/class/leds/vibrator/duration";
static constexpr char STATE_PATH[] = "/sys/class/leds/vibrator/state";
static constexpr char EFFECT_DURATION_PATH[] =
    "/sys/class/leds/vibrator/device/cp_trigger_duration";
static constexpr char EFFECT_INDEX_PATH[] = "/sys/class/leds/vibrator/device/cp_trigger_index";
static constexpr char EFFECT_QUEUE_PATH[] = "/sys/class/leds/vibrator/device/cp_trigger_queue";
static constexpr char EFFECT_SCALE_PATH[] = "/sys/class/leds/vibrator/device/cp_dig_scale";
static constexpr char GLOBAL_SCALE_PATH[] = "/sys/class/leds/vibrator/device/dig_scale";
static constexpr char ASP_ENABLE_PATH[] = "/sys/class/leds/vibrator/device/asp_enable";
static constexpr char GPIO_FALL_INDEX[] = "/sys/class/leds/vibrator/device/gpio1_fall_index";
static constexpr char GPIO_FALL_SCALE[] = "/sys/class/leds/vibrator/device/gpio1_fall_dig_scale";
static constexpr char GPIO_RISE_INDEX[] = "/sys/class/leds/vibrator/device/gpio1_rise_index";
static constexpr char GPIO_RISE_SCALE[] = "/sys/class/leds/vibrator/device/gpio1_rise_dig_scale";

// File path to the calibration file
static constexpr char CALIBRATION_FILEPATH[] = "/mnt/vendor/persist/haptics/cs40l25a.cal";

// Kernel ABIs for updating the calibration data
static constexpr char F0_CONFIG[] = "f0_measured";
static constexpr char REDC_CONFIG[] = "redc_measured";
static constexpr char Q_CONFIG[] = "q_measured";
static constexpr char Q_INDEX[] = "q_index";
static constexpr char VOLTAGES_CONFIG[] = "v_levels";
static constexpr char F0_FILEPATH[] = "/sys/class/leds/vibrator/device/f0_stored";
static constexpr char REDC_FILEPATH[] = "/sys/class/leds/vibrator/device/redc_stored";
static constexpr char Q_FILEPATH[] = "/sys/class/leds/vibrator/device/q_stored";

class HwApi : public Vibrator::HwApi {
  public:
    HwApi();

    bool setActivate(bool value) override { return set(value, mActivate); }
    bool setDuration(uint32_t value) override { return set(value, mDuration); }
    bool getEffectDuration(uint32_t *value) override { return get(value, mEffectDuration); }
    bool setEffectIndex(uint32_t value) override { return set(value, mEffectIndex); }
    bool setEffectQueue(std::string value) override { return set(value, mEffectQueue); }
    bool hasEffectScale() override { return has(mEffectScale); }
    bool setEffectScale(uint32_t value) override { return set(value, mEffectScale); }
    bool setGlobalScale(uint32_t value) override { return set(value, mGlobalScale); }
    bool setState(bool value) override { return set(value, mState); }
    bool hasAspEnable() override { return has(mAspEnable); }
    bool getAspEnable(bool *value) override { return get(value, mAspEnable); }
    bool setAspEnable(bool value) override { return set(value, mAspEnable); }
    bool setGpioFallIndex(uint32_t value) override { return set(value, mGpioFallIndex); }
    bool setGpioFallScale(uint32_t value) override { return set(value, mGpioFallScale); }
    bool setGpioRiseIndex(uint32_t value) override { return set(value, mGpioRiseIndex); }
    bool setGpioRiseScale(uint32_t value) override { return set(value, mGpioRiseScale); }

  private:
    bool has(std::ostream &stream) { return !!stream; }
    template <typename T>
    bool get(T *value, std::istream &stream) {
        bool ret;
        stream.seekg(0);
        stream >> *value;
        ret = !!stream;
        stream.clear();
        return ret;
    }
    template <typename T>
    bool set(const T &value, std::ostream &stream) {
        bool ret;
        stream << value << std::endl;
        if (!(ret = !!stream)) {
            stream.clear();
        }
        return ret;
    }

  private:
    std::ofstream mActivate;
    std::ofstream mDuration;
    std::ifstream mEffectDuration;
    std::ofstream mEffectIndex;
    std::ofstream mEffectQueue;
    std::ofstream mEffectScale;
    std::ofstream mGlobalScale;
    std::ofstream mState;
    std::fstream mAspEnable;
    std::ofstream mGpioFallIndex;
    std::ofstream mGpioFallScale;
    std::ofstream mGpioRiseIndex;
    std::ofstream mGpioRiseScale;
};

HwApi::HwApi() {
    // ostreams below are required

    mActivate.open(ACTIVATE_PATH);
    if (!mActivate) {
        ALOGE("Failed to open %s (%d): %s", ACTIVATE_PATH, errno, strerror(errno));
    }

    mDuration.open(DURATION_PATH);
    if (!mDuration) {
        ALOGE("Failed to open %s (%d): %s", DURATION_PATH, errno, strerror(errno));
    }

    mState.open(STATE_PATH);
    if (!mState) {
        ALOGE("Failed to open %s (%d): %s", STATE_PATH, errno, strerror(errno));
    }

    mEffectDuration.open(EFFECT_DURATION_PATH);
    if (!mEffectDuration) {
        ALOGE("Failed to open %s (%d): %s", EFFECT_DURATION_PATH, errno, strerror(errno));
    }

    mEffectIndex.open(EFFECT_INDEX_PATH);
    if (!mEffectIndex) {
        ALOGE("Failed to open %s (%d): %s", EFFECT_INDEX_PATH, errno, strerror(errno));
    }

    mEffectQueue.open(EFFECT_QUEUE_PATH);
    if (!mEffectQueue) {
        ALOGE("Failed to open %s (%d): %s", EFFECT_QUEUE_PATH, errno, strerror(errno));
    }

    mEffectScale.open(EFFECT_SCALE_PATH);
    if (!mEffectScale) {
        ALOGE("Failed to open %s (%d): %s", EFFECT_SCALE_PATH, errno, strerror(errno));
    }

    mGlobalScale.open(GLOBAL_SCALE_PATH);
    if (!mGlobalScale) {
        ALOGE("Failed to open %s (%d): %s", GLOBAL_SCALE_PATH, errno, strerror(errno));
    }

    mAspEnable.open(ASP_ENABLE_PATH);
    if (!mAspEnable) {
        ALOGE("Failed to open %s (%d): %s", ASP_ENABLE_PATH, errno, strerror(errno));
    }

    mGpioFallIndex.open(GPIO_FALL_INDEX);
    if (!mGpioFallIndex) {
        ALOGE("Failed to open %s (%d): %s", GPIO_FALL_INDEX, errno, strerror(errno));
    }

    mGpioFallScale.open(GPIO_FALL_SCALE);
    if (!mGpioFallScale) {
        ALOGE("Failed to open %s (%d): %s", GPIO_FALL_SCALE, errno, strerror(errno));
    }

    mGpioRiseIndex.open(GPIO_RISE_INDEX);
    if (!mGpioRiseIndex) {
        ALOGE("Failed to open %s (%d): %s", GPIO_RISE_INDEX, errno, strerror(errno));
    }

    mGpioRiseScale.open(GPIO_RISE_SCALE);
    if (!mGpioRiseScale) {
        ALOGE("Failed to open %s (%d): %s", GPIO_RISE_SCALE, errno, strerror(errno));
    }
}

static std::string trim(const std::string &str, const std::string &whitespace = " \t") {
    const auto str_begin = str.find_first_not_of(whitespace);
    if (str_begin == std::string::npos) {
        return "";
    }

    const auto str_end = str.find_last_not_of(whitespace);
    const auto str_range = str_end - str_begin + 1;

    return str.substr(str_begin, str_range);
}

static bool loadCalibrationData(std::vector<uint32_t> &outVLevels) {
    std::map<std::string, std::string> config_data;
    bool ret = true;

    std::ofstream f0{F0_FILEPATH};
    if (!f0) {
        ALOGE("Failed to open %s (%d): %s", F0_FILEPATH, errno, strerror(errno));
    }

    std::ofstream redc{REDC_FILEPATH};
    if (!redc) {
        ALOGE("Failed to open %s (%d): %s", REDC_FILEPATH, errno, strerror(errno));
    }

    std::ofstream q{Q_FILEPATH};
    if (!q) {
        ALOGE("Failed to open %s (%d): %s", Q_FILEPATH, errno, strerror(errno));
    }

    std::ifstream cal_data{CALIBRATION_FILEPATH};
    if (!cal_data) {
        ALOGE("Failed to open %s (%d): %s", CALIBRATION_FILEPATH, errno, strerror(errno));
        ret = false;
    }

    for (std::string line; std::getline(cal_data, line);) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream is_line(line);
        std::string key, value;
        if (std::getline(is_line, key, ':') && std::getline(is_line, value)) {
            config_data[trim(key)] = trim(value);
        }
    }

    if (config_data.find(VOLTAGES_CONFIG) != config_data.end()) {
        std::istringstream voltages(config_data[VOLTAGES_CONFIG]);
        std::vector<uint32_t> vLevels;
        uint32_t v;
        while (voltages >> v) {
            vLevels.push_back(v);
        }
        if (voltages.eof() && outVLevels.size() == vLevels.size()) {
            outVLevels = std::move(vLevels);
        } else {
            ALOGE("invalid v_levels config!");
            ret = false;
        }
    }

    if (config_data.find(F0_CONFIG) != config_data.end()) {
        f0 << config_data[F0_CONFIG] << std::endl;
    }

    if (config_data.find(REDC_CONFIG) != config_data.end()) {
        redc << config_data[REDC_CONFIG] << std::endl;
    }

    if (config_data.find(Q_CONFIG) != config_data.end()) {
        q << config_data[Q_CONFIG] << std::endl;
    } else if (config_data.find(Q_INDEX) != config_data.end()) {
        q << std::stoul(config_data[Q_INDEX]) * Q_INDEX_TO_FIXED + Q_INDEX_OFFSET << std::endl;
    } else {
        q << Q_DEFAULT << std::endl;
    }

    return ret;
}

status_t registerVibratorService() {
    // Calibration data
    std::vector<uint32_t> v_levels(V_LEVELS_DEFAULT);
    auto hwapi = std::make_unique<HwApi>();

    if (!hwapi->setState(true)) {
        ALOGE("Failed to set state (%d): %s", errno, strerror(errno));
    }

    if (!loadCalibrationData(v_levels)) {
        ALOGW("Failed to load calibration data");
    }

    sp<Vibrator> vibrator = new Vibrator(std::move(hwapi), std::move(v_levels));

    return vibrator->registerAsService();
}

int main() {
    configureRpcThreadpool(1, true);
    status_t status = registerVibratorService();

    if (status != OK) {
        return status;
    }

    joinRpcThreadpool();
}
