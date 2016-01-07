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

#define ALOG_TAG "SSP_Sensorhub"

#include <hardware/sensors.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <cstring>

#include <linux/input.h>

#include <utils/Atomic.h>
#include <utils/Log.h>

#include "sensors.h"

#include "LightSensor.h"
#include "GeoMagneticSensor.h"
#include "GyroSensor.h"
#include "AccelSensor.h"
#include "PressureSensor.h"

#include "RotationSensor.h"
#include "SSPContextSensor.h"
#include "BioHRM.h"

/*****************************************************************************/

#define DELAY_OUT_TIME 0x7FFFFFFF

#define LIGHT_SENSOR_POLLTIME    2000000000

#define SENSOR_PATH /sys/devices/virtual/sensors
#define SSP_PATH ssp_sensor

#define SENSORS_ACCELERATION     (1<<ID_A)
#define SENSORS_MAGNETIC_FIELD   (1<<ID_M)
#define SENSORS_ORIENTATION      (1<<ID_O)
#define SENSORS_LIGHT            (1<<ID_L)
#define SENSORS_GYROSCOPE        (1<<ID_GY)
#define SENSORS_PRESSURE         (1<<ID_PR)
#define SENSORS_BIO_HRM			 (1<<ID_HRM)
#define SENSORS_ROT_VECTOR		 (1<<ID_ROT)
#define SENSORS_SSP_CONTEXT		 (1<<ID_SSP)

#define SENSORS_ACCELERATION_HANDLE     0
#define SENSORS_MAGNETIC_FIELD_HANDLE   1
#define SENSORS_ORIENTATION_HANDLE      2
#define SENSORS_LIGHT_HANDLE            3
#define SENSORS_GYROSCOPE_HANDLE        5
#define SENSORS_PRESSURE_HANDLE         6
#define SENSORS_ROTVECTOR_HANDLE		7
#define SENSORS_BIOHRM_HANDLE			8
#define SENSORS_SSPCONTEXT_HANDLE		9

/*
	GEAR S:
AK09911 3-axis Electronic Compass
ICM20628 InvenSense
AL3320 Optical / Light sensor Dyna Image
LPS25H - STMicroelectronics PRESSURE
ADPD142 Heart Rate Monitor
UVIS25 - STMicroelectronics UV Sensor
	*/
/*****************************************************************************/

/* SensorHub Modules */

/* *NAME* |
  *VENDOR*|
  ? | HANDLE |
  SENSOR TYPE | RANGE | RESOLUTION | ENERGY CONSUMPTION | ? | ? | ? |
  SENS. STRING|  ?    |     ?      | SENSOR FLAGS		|? |

*/
static const struct sensor_t sSensorList[] = {
        { "ICM20628 Acceleration Sensor", // ICM20628 Accel+Gyro
          "InvenSense",
          1, SENSORS_ACCELERATION_HANDLE,
          SENSOR_TYPE_ACCELEROMETER, RANGE_A, CONVERT_A, 0.23f, 20000, 0, 0, 
          SENSOR_STRING_TYPE_ACCELEROMETER, "", 0, SENSOR_FLAG_CONTINUOUS_MODE, { } },
        { "AK09911 Magnetic field Sensor", // Electronic Compass
          "Asahi Kasei Microdevices",
          1, SENSORS_MAGNETIC_FIELD_HANDLE,
          SENSOR_TYPE_MAGNETIC_FIELD, 2000.0f, CONVERT_M, 6.8f, 16667, 0, 0, 
          SENSOR_STRING_TYPE_MAGNETIC_FIELD, "", 0, SENSOR_FLAG_CONTINUOUS_MODE, { } },
        { "AK0911 Orientation Sensor", // Same device as above
          "Asahi Kasei Microdevices",
          1, SENSORS_ORIENTATION_HANDLE,
          SENSOR_TYPE_ORIENTATION, 360.0f, CONVERT_O, 7.8f, 16667, 0, 0, 
          SENSOR_STRING_TYPE_ORIENTATION, "", 0, SENSOR_FLAG_CONTINUOUS_MODE, { } },
        { "ICM20628 Gyroscope Sensor", // Same device as accel
          "InvenSense",
          1, SENSORS_GYROSCOPE_HANDLE,
          SENSOR_TYPE_GYROSCOPE, RANGE_GYRO, CONVERT_GYRO, 6.1f, 1190, 0, 0, 
          SENSOR_STRING_TYPE_GYROSCOPE, "", 0, SENSOR_FLAG_CONTINUOUS_MODE, { } },
        { "LPS25H Pressure sensor",
          "STMicroelectronics",
          1, SENSORS_PRESSURE_HANDLE,
          SENSOR_TYPE_PRESSURE, 1260.0f, 0.002083f, 0.06f, 50000, 0, 0, 
          SENSOR_STRING_TYPE_PRESSURE, "", 0, SENSOR_FLAG_CONTINUOUS_MODE, { } },
        { "AL3320 Optical Sensor",
          "Dyna Image",
          1, SENSORS_LIGHT_HANDLE,
          SENSOR_TYPE_LIGHT, 10240.0f, 1.0f, 0.75f, 0, 0, 0, 
          SENSOR_STRING_TYPE_LIGHT, "", 0, SENSOR_FLAG_CONTINUOUS_MODE, { } },
		// These values down here will be all wrooong
       { "SSP Rotation Vector",
          "Samsung",
          1, SENSORS_ROTVECTOR_HANDLE,
          SENSOR_TYPE_ROTATION_VECTOR, 360.0f, 1.0f, 0.23f, 0, 0, 0, 
          SENSOR_STRING_TYPE_ROTATION_VECTOR, "", 0, SENSOR_FLAG_CONTINUOUS_MODE, { } },		
       { "ADPD1242",
          "Analog Devices Inc.",
          1, SENSORS_BIOHRM_HANDLE,
          SENSOR_TYPE_HEART_RATE, 300.0f, 1.0f, 0.23f, 0, 0, 0, 
          SENSOR_STRING_TYPE_HEART_RATE, "", 0, SENSOR_FLAG_CONTINUOUS_MODE, { } },			  
		 { "Seamless Sensor Platform",
          "Samsung Electronics Corporation",
          1, SENSORS_SSPCONTEXT_HANDLE,
          SENSOR_TYPE_STEP_COUNTER, 65536.0f, 1.0f, 0.23f, 0, 0, 0, 
          SENSOR_STRING_TYPE_STEP_COUNTER, "", 0, SENSOR_FLAG_CONTINUOUS_MODE, { } },			  
		   
		  // Missing ADPD142, UVIS25, Context
};


static int open_sensors(const struct hw_module_t* module, const char* id,
                        struct hw_device_t** device);


static int sensors__get_sensors_list(struct sensors_module_t* module,
                                     struct sensor_t const** list)
{
        *list = sSensorList;
        return ARRAY_SIZE(sSensorList);
}

static struct hw_module_methods_t sensors_module_methods = {
        open: open_sensors
};

struct sensors_module_t HAL_MODULE_INFO_SYM = {
        common: {
                tag: HARDWARE_MODULE_TAG,
                version_major: 1,
                version_minor: 0,
                id: SENSORS_HARDWARE_MODULE_ID,
                name: "Seamless Sensor Platform HAL Driver",
                author: "Cyanogenmod & Bik",
                methods: &sensors_module_methods,
        },
        get_sensors_list: sensors__get_sensors_list,
};

struct sensors_poll_context_t {
    sensors_poll_device_1_t device; // must be first

        sensors_poll_context_t();
        ~sensors_poll_context_t();
    int activate(int handle, int enabled);
    int setDelay(int handle, int64_t ns);
    int pollEvents(sensors_event_t* data, int count);
    int batch(int handle, int flags, int64_t period_ns, int64_t timeout);

    // return true if the constructor is completed
    bool isValid() { return mInitialized; };
    int flush(int handle);

private:
    enum {
        light           = 0,
        magnetic        = 1,
        gyro            = 2,
        accel           = 3,
        pressure        = 4,
		rotationvector  = 5,
		biohrm			= 6,
		contextsensor	= 7,
        numSensorDrivers,
        numFds,
    };

    static const size_t wake = numFds - 1;
    static const char WAKE_MESSAGE = 'W';
    struct pollfd mPollFds[numFds];
    int mWritePipeFd;
    SensorBase* mSensors[numSensorDrivers];
    // return true if the constructor is completed
    bool mInitialized;

    int handleToDriver(int handle) const {
      switch (handle) {
            case ID_A:
                return accel;
            case ID_M:
            case ID_O:
                return magnetic;
				case ID_L:
                return light;
            case ID_GY:
                return gyro;
            case ID_PR:
                return pressure;
			case ID_ROT:
				return rotationvector;
			case ID_HRM:
				return biohrm;
			case ID_SSP:
				return contextsensor;
        }
        return -EINVAL;
    }
};

/*****************************************************************************/

sensors_poll_context_t::sensors_poll_context_t()
{
    mSensors[light] = new LightSensor();
    mPollFds[light].fd = mSensors[light]->getFd();
    mPollFds[light].events = POLLIN;
    mPollFds[light].revents = 0;

    mSensors[magnetic] = new MagneticSensor();
    mPollFds[magnetic].fd = mSensors[magnetic]->getFd();
    mPollFds[magnetic].events = POLLIN;
    mPollFds[magnetic].revents = 0;

    mSensors[gyro] = new GyroSensor();
    mPollFds[gyro].fd = mSensors[gyro]->getFd();
    mPollFds[gyro].events = POLLIN;
    mPollFds[gyro].revents = 0;

    mSensors[accel] = new AccelSensor();
    mPollFds[accel].fd = mSensors[accel]->getFd();
    mPollFds[accel].events = POLLIN;
    mPollFds[accel].revents = 0;

    mSensors[pressure] = new PressureSensor();
    mPollFds[pressure].fd = mSensors[pressure]->getFd();
    mPollFds[pressure].events = POLLIN;
    mPollFds[pressure].revents = 0;

    mSensors[rotationvector] = new RotationSensor();
    mPollFds[rotationvector].fd = mSensors[rotationvector]->getFd();
    mPollFds[rotationvector].events = POLLIN;
    mPollFds[rotationvector].revents = 0;
	
	mSensors[biohrm] = new BioHRMSensor();
    mPollFds[biohrm].fd = mSensors[biohrm]->getFd();
    mPollFds[biohrm].events = POLLIN;
    mPollFds[biohrm].revents = 0;
	
    mSensors[contextsensor] = new SSPContextSensor();
    mPollFds[contextsensor].fd = mSensors[contextsensor]->getFd();
    mPollFds[contextsensor].events = POLLIN;
    mPollFds[contextsensor].revents = 0;
	
    int wakeFds[2];
    int result = pipe(wakeFds);
    ALOGE_IF(result<0, "error creating wake pipe (%s)", strerror(errno));
    fcntl(wakeFds[0], F_SETFL, O_NONBLOCK);
    fcntl(wakeFds[1], F_SETFL, O_NONBLOCK);
    mWritePipeFd = wakeFds[1];

    mPollFds[wake].fd = wakeFds[0];
    mPollFds[wake].events = POLLIN;
    mPollFds[wake].revents = 0;
    mInitialized = true;
}

sensors_poll_context_t::~sensors_poll_context_t() {
    for (int i=0 ; i<numSensorDrivers ; i++) {
        delete mSensors[i];
    }
    close(mPollFds[wake].fd);
    close(mWritePipeFd);
    mInitialized = false;
}

int sensors_poll_context_t::activate(int handle, int enabled) {
    if (!mInitialized) return -EINVAL;
    int index = handleToDriver(handle);
    ALOGI("Sensors: handle: %i", handle);
    if (index < 0) return index;
    int err =  mSensors[index]->enable(handle, enabled);
    if (enabled && !err) {
        const char wakeMessage(WAKE_MESSAGE);
        int result = write(mWritePipeFd, &wakeMessage, 1);
        ALOGE_IF(result<0, "error sending wake message (%s)", strerror(errno));
    }
    return err;
}

int sensors_poll_context_t::setDelay(int handle, int64_t ns) {

    int index = handleToDriver(handle);
    if (index < 0) return index;
    return mSensors[index]->setDelay(handle, ns);
}

int sensors_poll_context_t::pollEvents(sensors_event_t* data, int count)
{
    int nbEvents = 0;
    int n = 0;

    do {
        // see if we have some leftover from the last poll()
        for (int i=0 ; count && i<numSensorDrivers ; i++) {
            SensorBase* const sensor(mSensors[i]);
            if ((mPollFds[i].revents & POLLIN) || (sensor->hasPendingEvents())) {
                int nb = sensor->readEvents(data, count);
                if (nb < count) {
                    // no more data for this sensor
                    mPollFds[i].revents = 0;
                }
                count -= nb;
                nbEvents += nb;
                data += nb;
            }
        }

        if (count) {
            // we still have some room, so try to see if we can get
            // some events immediately or just wait if we don't have
            // anything to return
            n = poll(mPollFds, numFds, nbEvents ? 0 : -1);
            if (n<0) {
                ALOGE("poll() failed (%s)", strerror(errno));
                return -errno;
            }
            if (mPollFds[wake].revents & POLLIN) {
                char msg;
                int result = read(mPollFds[wake].fd, &msg, 1);
                ALOGE_IF(result<0, "error reading from wake pipe (%s)", strerror(errno));
                ALOGE_IF(msg != WAKE_MESSAGE, "unknown message on wake queue (0x%02x)", int(msg));
                mPollFds[wake].revents = 0;
            }
        }
        // if we have events and space, go read them
    } while (n && count);

    return nbEvents;
}

int sensors_poll_context_t::batch(int handle, int flags, int64_t period_ns, int64_t timeout)
{
    int index = handleToDriver(handle);
    if (index < 0) return index;
    return mSensors[index]->batch(handle, flags, period_ns, timeout);
}

int sensors_poll_context_t::flush(int handle)
{
    int index = handleToDriver(handle);
    if (index < 0) return index;
    return mSensors[index]->flush(handle);
}

/*****************************************************************************/

static int poll__close(struct hw_device_t *dev)
{
    sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;
    if (ctx) {
        delete ctx;
    }
    return 0;
}

static int poll__activate(struct sensors_poll_device_t *dev,
        int handle, int enabled) {
    sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;
    return ctx->activate(handle, enabled);
}

static int poll__setDelay(struct sensors_poll_device_t *dev,
        int handle, int64_t ns) {
    sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;
    return ctx->setDelay(handle, ns);
}

static int poll__poll(struct sensors_poll_device_t *dev,
        sensors_event_t* data, int count) {
    sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;
    return ctx->pollEvents(data, count);
}

static int poll__batch(struct sensors_poll_device_1 *dev,
                      int handle, int flags, int64_t period_ns, int64_t timeout)
{
    sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;
    return ctx->batch(handle, flags, period_ns, timeout);
}

static int poll__flush(struct sensors_poll_device_1 *dev,
                      int handle)
{
    sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;
    return ctx->flush(handle);
}

/*****************************************************************************/

/** Open a new instance of a sensor device using name */
static int open_sensors(const struct hw_module_t* module, const char* id,
                        struct hw_device_t** device)
{
        int status = -EINVAL;
        sensors_poll_context_t *dev = new sensors_poll_context_t();

        memset(&dev->device, 0, sizeof(sensors_poll_device_1));

        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version  = SENSORS_DEVICE_API_VERSION_1_0;
        dev->device.common.module   = const_cast<hw_module_t*>(module);
        dev->device.common.close    = poll__close;
        dev->device.activate        = poll__activate;
        dev->device.setDelay        = poll__setDelay;
        dev->device.poll            = poll__poll;

        /* Batch processing */
        dev->device.batch           = poll__batch;
        dev->device.flush           = poll__flush;

        *device = &dev->device.common;
        status = 0;

        return status;
}

