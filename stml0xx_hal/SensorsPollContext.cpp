/*
 * Copyright (C) 2008 The Android Open Source Project
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

/*
 * Copyright (C) 2015 Motorola Mobility LLC
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <new>
#include <string.h>

#include <linux/input.h>

#include <cutils/atomic.h>
#include <cutils/log.h>

#include <hardware/sensors.h>
#include <hardware/mot_sensorhub_stml0xx.h>

#include "SensorList.h"
#include "Sensors.h"
#include "SensorsPollContext.h"
#if defined(_ENABLE_MAGNETOMETER) || defined(_ENABLE_REARPROX)
#include "SensorBase.h"
#endif
#ifdef _ENABLE_REARPROX
#include "RearProxSensor.h"
#endif
#include "AkmSensor.h"
#include "HubSensors.h"

/*****************************************************************************/

SensorsPollContext SensorsPollContext::self;

SensorsPollContext::SensorsPollContext()
{
    mSensors[sensor_hub] = HubSensors::getInstance();
    if (mSensors[sensor_hub]) {
        mPollFds[sensor_hub].fd = mSensors[sensor_hub]->getFd();
        mPollFds[sensor_hub].events = POLLIN;
        mPollFds[sensor_hub].revents = 0;
    } else {
        ALOGE("out of memory: new failed for HubSensors");
    }

#ifdef _ENABLE_MAGNETOMETER
    mSensors[akm] = AkmSensor::getInstance();
    if (mSensors[akm]) {
        mPollFds[akm].fd = mSensors[akm]->getFd();
        mPollFds[akm].events = POLLIN;
        mPollFds[akm].revents = 0;
    } else {
        ALOGE("out of memory: new failed for AkmSensor");
    }

    int wakeFds[2];
    int result = pipe(wakeFds);
    ALOGE_IF(result<0, "error creating wake pipe (%s)", strerror(errno));
    fcntl(wakeFds[0], F_SETFL, O_NONBLOCK);
    fcntl(wakeFds[1], F_SETFL, O_NONBLOCK);
    mWritePipeFd = wakeFds[1];

    mPollFds[wake].fd = wakeFds[0];
    mPollFds[wake].events = POLLIN;
    mPollFds[wake].revents = 0;
#endif
#ifdef _ENABLE_REARPROX
    mSensors[rearprox] = RearProxSensor::getInstance();
    ALOGE("rearprox sensor created");
    if (mSensors[rearprox]) {
        mPollFds[rearprox].fd = mSensors[rearprox]->getFd();
        mPollFds[rearprox].events = POLLIN;
        mPollFds[rearprox].revents = 0;
    } else {
        ALOGE("out of memory: new failed for rearprox sensor");
    }
#endif

    // Add all supported sensors to the mIdToSensor map
    for( int i = 0; i < sSensorListSize; ++i ) {
        mIdToSensor.insert(std::make_pair(sSensorList[i].handle, sSensorList+i));
    }
}

SensorsPollContext::~SensorsPollContext()
{
#ifdef _ENABLE_MAGNETOMETER
    close(mPollFds[wake].fd);
    close(mWritePipeFd);
#endif
}

SensorsPollContext *SensorsPollContext::getInstance()
{
    return &self;
}

int SensorsPollContext::handleToDriver(int handle)
{
    switch (handle) {
        case ID_A:
#ifdef _ENABLE_GYROSCOPE
        case ID_G:
        case ID_UNCALIB_GYRO:
        case ID_GAME_RV:
        case ID_LA:
        case ID_GRAVITY:
#endif
        case ID_L:
        case ID_DR:
        case ID_P:
        case ID_FU:
        case ID_FD:
        case ID_S:
        case ID_CA:
#ifdef _ENABLE_ACCEL_SECONDARY
        case ID_A2:
#endif
#ifdef _ENABLE_CHOPCHOP
        case ID_CC:
#endif
#ifdef _ENABLE_LIFT
        case ID_LF:
#endif
#ifdef _ENABLE_PEDO
        case ID_STEP_COUNTER:
        case ID_STEP_DETECTOR:
#endif
        case ID_GLANCE_GESTURE:
            return sensor_hub;
#ifdef _ENABLE_MAGNETOMETER
        case ID_M:
        case ID_UM:
        case ID_OR:
            return akm;
#endif
#ifdef _ENABLE_REARPROX
        case ID_RP:
            return rearprox;
#endif
    }
    return -EINVAL;
}

int SensorsPollContext::activate(int handle, int enabled)
{
    int drv = handleToDriver(handle);
    int err = 0;

    if (drv < 0)
        return -EINVAL;

    if( !mIdToSensor.count(handle) ) {
        ALOGE("Sensorhub hal activate: %d - %d (bad handle)", handle, enabled);
        return -EINVAL;
    }

    err = mSensors[drv]->setEnable(handle, enabled);

#ifdef _ENABLE_MAGNETOMETER
    if (!err && (handle == ID_OR)) {
        err = mSensors[sensor_hub]->setEnable(handle, enabled);
    }

    if (((handle == ID_M) || (handle == ID_OR))  && enabled && !err) {
        const char wakeMessage(WAKE_MESSAGE);
        int result = write(mWritePipeFd, &wakeMessage, 1);
        ALOGE_IF(result<0, "error sending wake message (%s)", strerror(errno));
    }
#endif

    return err;
}

int SensorsPollContext::setDelay(int handle, int64_t ns)
{
    int drv = handleToDriver(handle);
    int err = 0;

    if (drv < 0)
        return -EINVAL;

    if( !mIdToSensor.count(handle) ) {
        ALOGE("Sensorhub hal setDelay: %d - %" PRId64 " (bad handle)", handle, ns);
        return -EINVAL;
    }

    err = mSensors[drv]->setDelay(handle, ns);

#ifdef _ENABLE_MAGNETOMETER
    if (!err && (handle == ID_OR)) {
        err = mSensors[sensor_hub]->setDelay(handle, ns);
    }
#endif

    return err;
}

int SensorsPollContext::pollEvents(sensors_event_t* data, int count)
{
    int nbEvents = 0;
    int n = 0;
    int ret;
    int err;

    while (true) {
#ifdef _ENABLE_MAGNETOMETER
        ret = poll(mPollFds, numFds, nbEvents ? 0 : -1);
#else
        ret = poll(mPollFds, numSensorDrivers, nbEvents ? 0 : -1);
#endif
        err = errno;
        // Success
        if (ret >= 0)
            break;
        // EINTR is OK
        if (err == EINTR) {
            ALOGE("poll() restart (%s)", strerror(err));
            continue;
        } else {
            ALOGE("poll() failed (%s)", strerror(err));
            return -err;
        }
    }

    for (int i=0 ; count && i<numSensorDrivers ; i++) {
        SensorBase* const sensor(mSensors[i]);
        if ((mPollFds[i].revents & POLLIN) || (sensor->hasPendingEvents())) {
            int nb = sensor->readEvents(data, count);
            // Need to relay any errors upward.
            if (nb < 0)
                return nb;
            count -= nb;
            nbEvents += nb;
            data += nb;
            mPollFds[i].revents = 0;

#ifdef _ENABLE_MAGNETOMETER
            if ((0 != nb) && (sensor_hub == i)) {
                sensors_event_t* sensor_data = data - nb;
                for (int j=0; j<nb; j++) {
                    if (ID_A == sensor_data->sensor) {
                        static_cast<AkmSensor*>(mSensors[akm])->setAccel(sensor_data);
                    }
                    sensor_data--;
                }
            }
#endif
        }
    }

#ifdef _ENABLE_MAGNETOMETER
    if (mPollFds[wake].revents & POLLIN) {
        char msg;
        int result = read(mPollFds[wake].fd, &msg, 1);
        ALOGE_IF(result<0,
            "error reading from wake pipe (%s)",
            strerror(errno));
        ALOGE_IF(msg != WAKE_MESSAGE,
            "unknown message on wake queue (0x%02x)",
            int(msg));
        mPollFds[wake].revents = 0;
    }
#endif

    return nbEvents;
}

int SensorsPollContext::batch(int handle, int flags, int64_t ns, int64_t timeout)
{
    (void)flags;
    (void)timeout;
    return setDelay(handle, ns);
}

int SensorsPollContext::flush(int handle)
{
    if (!mIdToSensor.count(handle)) {
        ALOGE("Sensorhub hal flush: %d (bad handle)", handle);
        return -EINVAL;
    }

    // Have to return -EINVAL for one-shot sensors per Android spec
    if ((mIdToSensor[handle]->flags & REPORTING_MODE_MASK) == SENSOR_FLAG_ONE_SHOT_MODE) {
        return -EINVAL;
    }

    return mSensors[sensor_hub]->flush(handle);
}
