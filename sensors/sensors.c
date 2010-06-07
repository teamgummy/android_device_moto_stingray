/*
 * Copyright (C) 2009 Motorola, Inc.
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

#define LOG_TAG "Sensors"

#include <hardware/sensors.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>

#include <linux/input.h>
#include <linux/uinput.h>
#include <linux/kxtf9.h>
#include <linux/bmp085.h>

#include <cutils/atomic.h>
#include <cutils/log.h>
#include <cutils/native_handle.h>

/*****************************************************************************/

#define MAX_NUM_SENSORS 6

#define SUPPORTED_SENSORS  ((1<<MAX_NUM_SENSORS)-1)

#define ID_A  (0)
#define ID_M  (1)
#define ID_O  (2)
#define ID_T  (3)
#define ID_P  (4)
#define ID_L  (5)

#define SENSORS_ACCELERATION   (1<<ID_A)
#define SENSORS_MAGNETIC_FIELD (1<<ID_M)
#define SENSORS_ORIENTATION    (1<<ID_O)
#define SENSORS_TEMPERATURE    (1<<ID_T)
#define SENSORS_PROXIMITY      (1<<ID_P)
#define SENSORS_LIGHT          (1<<ID_L)

#define MAX_NUM_DRIVERS 4

#define ID_LIS  (0)
#define ID_AKM  (1)
#define ID_SFH  (2)

struct driver_t {
    char *name;		/* name reported to input module */
    char *loc;		/* driver sys location */
    uint32_t mask;
};

static const struct driver_t dDriverList[] = {
    {"accelerometer", "/dev/kxtf9", (SENSORS_ACCELERATION) },
    {"compass", "/dev/akm8973_aot", (SENSORS_MAGNETIC_FIELD | SENSORS_ORIENTATION | SENSORS_TEMPERATURE) },
    {"max9635", "", (SENSORS_LIGHT) },
};

/*****************************************************************************/

struct sensors_control_context_t {
    struct sensors_control_device_t device;
    int dev_fd[MAX_NUM_DRIVERS];
    sensors_data_t filter_sensors[MAX_NUM_SENSORS];
    uint32_t active_sensors;
    uint32_t active_drivers;
    int uinput;
    pthread_t poll_thread;
};

struct sensors_data_context_t {
    struct sensors_data_device_t device;
    int events_fd;
    sensors_data_t sensors[MAX_NUM_SENSORS];
    uint32_t pendingSensors;
};

/*
 * The SENSORS Module
 */

/* the SFH7743 is a binary proximity sensor that triggers around 6 cm on
 * this hardware */
#define PROXIMITY_THRESHOLD_CM  6.0f

/*
 * the AK8973 has a 8-bit ADC but the firmware seems to average 16 samples,
 * or at least makes its calibration on 12-bits values. This increases the
 * resolution by 4 bits.
 *
 * The orientation sensor also seems to have a 1/64 resolution.
 */

static const struct sensor_t sSensorList[] = {
        { "KXTF9 3-axis Accelerometer",
                "Kionix",
                1, SENSORS_HANDLE_BASE+ID_A,
                SENSOR_TYPE_ACCELEROMETER, 4.0f*9.81f, 9.81f/1000.0f, 0.25f, { } },
        { "AK8973 3-axis Magnetic field sensor",
                "Asahi Kasei",
                1, SENSORS_HANDLE_BASE+ID_M,
                SENSOR_TYPE_MAGNETIC_FIELD, 2000.0f, 1.0f/16.0f, 6.8f, { } },
        { "AK8973 Temperature sensor",
                "Asahi Kasei",
                1, SENSORS_HANDLE_BASE+ID_T,
                SENSOR_TYPE_TEMPERATURE, 115.0f, 1.6f, 3.0f, { } },
        { "Orientation sensor",
                "Asahi Kasei",
                1, SENSORS_HANDLE_BASE+ID_O,
                SENSOR_TYPE_ORIENTATION, 360.0f, 1.0f/64.0f, 7.05f, { } },
        { "MAX9635 Light sensor",
                "Maxim",
                1, SENSORS_HANDLE_BASE+ID_L,
                SENSOR_TYPE_LIGHT, 27000.0f, 1.0f, 0.0f, { } },
};

static int open_sensors(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device);

static uint32_t sensors__get_sensors_list(struct sensors_module_t* module,
        struct sensor_t const** list)
{
    *list = sSensorList;
    return sizeof(sSensorList)/sizeof(sSensorList[0]);
}

static struct hw_module_methods_t sensors_module_methods = {
    .open = open_sensors
};

const struct sensors_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .version_major = 1,
        .version_minor = 0,
        .id = SENSORS_HARDWARE_MODULE_ID,
        .name = "Stingray SENSORS Module",
        .author = "Motorola",
        .methods = &sensors_module_methods,
    },
    .get_sensors_list = sensors__get_sensors_list
};

/*****************************************************************************/

// sensor IDs must be a power of two and
// must match values in SensorManager.java
#define EVENT_TYPE_ACCEL_X          ABS_X
#define EVENT_TYPE_ACCEL_Y          ABS_Y
#define EVENT_TYPE_ACCEL_Z          ABS_Z
#define EVENT_TYPE_ACCEL_STATUS     ABS_WHEEL

#define EVENT_TYPE_YAW              ABS_RX
#define EVENT_TYPE_PITCH            ABS_RY
#define EVENT_TYPE_ROLL             ABS_RZ
#define EVENT_TYPE_ORIENT_STATUS    ABS_RUDDER

#define EVENT_TYPE_MAGV_X           ABS_HAT0X
#define EVENT_TYPE_MAGV_Y           ABS_HAT0Y
#define EVENT_TYPE_MAGV_Z           ABS_BRAKE

#define EVENT_TYPE_TEMPERATURE      ABS_THROTTLE
#define EVENT_TYPE_PROXIMITY        ABS_DISTANCE
#define EVENT_TYPE_LIGHT            LED_MISC

// 1000 LSG = 1G
#define LSG                         (1000.0f)

// conversion of acceleration data to SI units (m/s^2)
#define CONVERT_A                   (GRAVITY_EARTH / LSG)
#define CONVERT_A_X                 (CONVERT_A)
#define CONVERT_A_Y                 (CONVERT_A)
#define CONVERT_A_Z                 (CONVERT_A)

// conversion of magnetic data to uT units
#define CONVERT_M                   (1.0f/16.0f)
#define CONVERT_M_X                 (CONVERT_M)
#define CONVERT_M_Y                 (-CONVERT_M)
#define CONVERT_M_Z                 (-CONVERT_M)

#define CONVERT_O                   (1.0f/64.0f)
#define CONVERT_O_Y                 (CONVERT_O)
#define CONVERT_O_P                 (CONVERT_O)
#define CONVERT_O_R                 (-CONVERT_O)

#define CONVERT_P                   (1.0f/5.0f)

#define SENSOR_STATE_MASK           (0x7FFF)

/*****************************************************************************/

static int open_input(char *dev_name, int mode)
{
    /* scan all input drivers and look for "dev_name" */
    int fd = -1;
    const char *dirname = "/dev/input";
    char devname[PATH_MAX];
    char *filename;
    DIR *dir;
    struct dirent *de;
    dir = opendir(dirname);
    if(dir == NULL)
        return -1;
    strcpy(devname, dirname);
    filename = devname + strlen(devname);
    *filename++ = '/';
    while((de = readdir(dir))) {
        if(de->d_name[0] == '.' &&
           (de->d_name[1] == '\0' ||
            (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;
        strcpy(filename, de->d_name);
        fd = open(devname, mode);
        if (fd>=0) {
            char name[80];
            if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), &name) < 1) {
                name[0] = '\0';
            }
            if (!strcmp(name, dev_name)) {
                LOGD("using %s (name=%s)", dev_name, name);
                break;
            }
            close(fd);
            fd = -1;
        }
    }
    closedir(dir);

    if (fd < 0) {
        LOGE("Couldn't find or open '%s' driver (%s)", dev_name, strerror(errno));
    }
    return fd;
}


static int open_dev(struct sensors_control_context_t *dev, int dev_num)
{
    if (dev->dev_fd[dev_num] < 0) {
        dev->dev_fd[dev_num] = open(dDriverList[dev_num].loc, O_RDONLY);
        //LOGD("%s, fd=%d", __PRETTY_FUNCTION__, dev->dev_fd[dev_num]);
        LOGE_IF(dev->dev_fd[dev_num] < 0, "Couldn't open %s (%s)",
                dDriverList[dev_num].loc, strerror(errno));
    }
    return dev->dev_fd[dev_num];
}

static void close_dev(struct sensors_control_context_t *dev, int dev_num, uint32_t enabled)
{
    //LOGD("%s: (enabled & dDriverList[dev_num].mask) = 0x%X", __PRETTY_FUNCTION__, (enabled & dDriverList[dev_num].mask));
    if ((dev->dev_fd[dev_num] >= 0) && ((enabled & dDriverList[dev_num].mask) == 0)) {
        //LOGD("%s, fd=%d", __PRETTY_FUNCTION__, dev->dev_fd[dev_num]);
        close(dev->dev_fd[dev_num]);
        dev->dev_fd[dev_num] = -1;
    }
}

static int send_event(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event event;

    memset(&event, 0, sizeof(event));
    event.type      = type;
    event.code      = code;
    event.value     = value;

    return write(fd, &event, sizeof(event));
}

static int uinput_create(char *name)
{
    struct uinput_user_dev udev;
    int i = 0;

    // check if "name" has been registered already
    int fd = open_input(name, O_RDWR);
    if (fd >= 0) {
        return fd;
    }

    int ufd = open("/dev/uinput", O_RDWR);
    if(ufd < 0) {
        LOGE("Can't open uinput device (%s)", strerror(errno));
        return -errno;
    }

    memset(&udev, 0, sizeof(udev));
    strncpy(udev.name, name, UINPUT_MAX_NAME_SIZE);

    ioctl(ufd, UI_SET_EVBIT, EV_SYN);
    ioctl(ufd, UI_SET_EVBIT, EV_ABS);
    ioctl(ufd, UI_SET_EVBIT, EV_LED);

    ioctl(ufd, UI_SET_ABSBIT, EVENT_TYPE_ACCEL_X);
    ioctl(ufd, UI_SET_ABSBIT, EVENT_TYPE_ACCEL_Y);
    ioctl(ufd, UI_SET_ABSBIT, EVENT_TYPE_ACCEL_Z);

    ioctl(ufd, UI_SET_ABSBIT, EVENT_TYPE_YAW);
    ioctl(ufd, UI_SET_ABSBIT, EVENT_TYPE_PITCH);
    ioctl(ufd, UI_SET_ABSBIT, EVENT_TYPE_ROLL);

    ioctl(ufd, UI_SET_ABSBIT, EVENT_TYPE_MAGV_X);
    ioctl(ufd, UI_SET_ABSBIT, EVENT_TYPE_MAGV_Y);
    ioctl(ufd, UI_SET_ABSBIT, EVENT_TYPE_MAGV_Z);

    ioctl(ufd, UI_SET_ABSBIT, EVENT_TYPE_TEMPERATURE);
    ioctl(ufd, UI_SET_ABSBIT, EVENT_TYPE_PROXIMITY);
    ioctl(ufd, UI_SET_LEDBIT, EVENT_TYPE_LIGHT);

    /* no need to filter since drivers already do */
    for (i = 0; i < ABS_MAX; i++) {
        udev.absmax[i] = 8000;
        udev.absmin[i] = -8000;
    }

    if (write(ufd, &udev, sizeof(udev)) < 0) {
        LOGE("Can't write uinput device information (%s)", strerror(errno));
        close(ufd);
        return -errno;
    }

    if (ioctl(ufd, UI_DEV_CREATE))
    {
        LOGE("Can't create uinput device (%s)", strerror(errno));
        close(ufd);
        return -errno;
    }

    return ufd;
}

static void *poll_thread(void *arg)
{
    struct sensors_control_context_t *dev= (struct sensors_control_context_t *)arg;
    struct pollfd event_fd[MAX_NUM_DRIVERS];

    int err = 0;
    int i = 0;
    int j = 0;

    for (i =0; i < MAX_NUM_DRIVERS; i++) {
        int fd = open_input(dDriverList[i].name, O_RDONLY);
        if (fd < 0) {
            LOGE("invalid file descriptor, fd=%d", fd);
            for (j = 0; j < i; j++) {
                close(event_fd[j].fd);
            }
            pthread_exit(0);
        }
        event_fd[i].fd = fd;
        event_fd[i].events = POLLIN;
    }

    uint32_t new_sensors = 0;
    while(1) {
        int pollres = poll(event_fd, MAX_NUM_DRIVERS, -1);
        if (pollres <= 0) {
            if (errno != EINTR) {
                LOGW("select failed (errno=%d)\n", errno);
                usleep(100000);
            }
            continue;
        }

        for (i = 0; i < MAX_NUM_DRIVERS; i++) {
            if (event_fd[i].revents) {
                if (event_fd[i].revents & POLLIN) {
                    struct input_event event;
                    int nread = read(event_fd[i].fd, &event, sizeof(event));
                    if (nread == sizeof(event)) {
                        uint32_t active_sensors = dev->active_sensors;
                        uint32_t write_event = 0;
                        if (dev->uinput >= 0) {
                            if (event.type == EV_ABS) {
                                switch (event.code) {
                                    case EVENT_TYPE_ACCEL_X:
                                        new_sensors |= SENSORS_ACCELERATION;
                                        dev->filter_sensors[ID_A].acceleration.x = event.value;
                                        break;
                                    case EVENT_TYPE_ACCEL_Y:
                                        new_sensors |= SENSORS_ACCELERATION;
                                        dev->filter_sensors[ID_A].acceleration.y = event.value;
                                        break;
                                    case EVENT_TYPE_ACCEL_Z:
                                        new_sensors |= SENSORS_ACCELERATION;
                                        dev->filter_sensors[ID_A].acceleration.z = event.value;
                                        break;

                                    case EVENT_TYPE_MAGV_X:
                                        new_sensors |= SENSORS_MAGNETIC_FIELD;
                                        dev->filter_sensors[ID_M].magnetic.x = event.value;
                                        break;
                                    case EVENT_TYPE_MAGV_Y:
                                        new_sensors |= SENSORS_MAGNETIC_FIELD;
                                        dev->filter_sensors[ID_M].magnetic.y = event.value;
                                        break;
                                    case EVENT_TYPE_MAGV_Z:
                                        new_sensors |= SENSORS_MAGNETIC_FIELD;
                                        dev->filter_sensors[ID_M].magnetic.z = event.value;
                                        break;

                                   case EVENT_TYPE_YAW:
                                        new_sensors |= SENSORS_ORIENTATION;
                                        dev->filter_sensors[ID_O].orientation.azimuth =  event.value;
                                        break;
                                   case EVENT_TYPE_PITCH:
                                        new_sensors |= SENSORS_ORIENTATION;
                                        dev->filter_sensors[ID_O].orientation.pitch = event.value;
                                        break;
                                   case EVENT_TYPE_ROLL:
                                        new_sensors |= SENSORS_ORIENTATION;
                                        dev->filter_sensors[ID_O].orientation.roll = event.value;
                                        break;

                                    case EVENT_TYPE_TEMPERATURE:
                                        new_sensors |= SENSORS_TEMPERATURE;
                                        dev->filter_sensors[ID_T].temperature = event.value;
                                        break;

                                    case EVENT_TYPE_PROXIMITY:
                                        new_sensors |= SENSORS_PROXIMITY;
                                        dev->filter_sensors[ID_P].distance = event.value;
                                        break;
                                }
                            } else if (event.type == EV_LED) {
                                if (event.code == LED_MISC) {
                                    new_sensors |= SENSORS_LIGHT;
                                    dev->filter_sensors[ID_L].light = event.value;
                                    break;
                                }
                            } else if (event.type == EV_SYN) {
                                if (event.code == SYN_CONFIG) {
                                    // we use SYN_CONFIG to signal that we need to exit the
                                    // main loop.
                                    //LOGD("got empty message: value=%d", event.value);
                                    if (event.value == 0) {
                                        if(!write(dev->uinput, &event, sizeof(event)))
                                            LOGE("%s: failed to write event (%s)", __PRETTY_FUNCTION__,
                                                    strerror(errno));
                                    }
                                }
                                while(new_sensors) {
                                    // all data manipulation other than units should be done here
                                    uint32_t i = 31 - __builtin_clz(new_sensors);
                                    new_sensors &= ~(1<<i);
                                    switch (1<<i) {
                                        case SENSORS_ACCELERATION:
                                            if(active_sensors & SENSORS_ACCELERATION) {
                                                send_event(dev->uinput, EV_ABS, EVENT_TYPE_ACCEL_X,
                                                    dev->filter_sensors[ID_A].acceleration.x);
                                                send_event(dev->uinput, EV_ABS, EVENT_TYPE_ACCEL_Y,
                                                    dev->filter_sensors[ID_A].acceleration.y);
                                                send_event(dev->uinput, EV_ABS, EVENT_TYPE_ACCEL_Z,
                                                    dev->filter_sensors[ID_A].acceleration.z);
                                                send_event(dev->uinput, EV_SYN, 0, 0);
                                            }
                                            break;
                                        case SENSORS_MAGNETIC_FIELD:
                                            send_event(dev->uinput, EV_ABS, EVENT_TYPE_MAGV_X,
                                                dev->filter_sensors[ID_M].magnetic.x);
                                            send_event(dev->uinput, EV_ABS, EVENT_TYPE_MAGV_Y,
                                                dev->filter_sensors[ID_M].magnetic.y);
                                            send_event(dev->uinput, EV_ABS, EVENT_TYPE_MAGV_Z,
                                                dev->filter_sensors[ID_M].magnetic.z);
                                            send_event(dev->uinput, EV_SYN, 0, 0);
                                        case SENSORS_ORIENTATION:
                                            send_event(dev->uinput, EV_ABS, EVENT_TYPE_YAW,
                                                dev->filter_sensors[ID_O].orientation.azimuth);
                                            send_event(dev->uinput, EV_ABS, EVENT_TYPE_PITCH,
                                                dev->filter_sensors[ID_O].orientation.pitch);
                                            send_event(dev->uinput, EV_ABS, EVENT_TYPE_ROLL,
                                                dev->filter_sensors[ID_O].orientation.roll);
                                            send_event(dev->uinput, EV_SYN, 0, 0);
                                            break;
                                        case SENSORS_TEMPERATURE:
                                            send_event(dev->uinput, EV_ABS, EVENT_TYPE_TEMPERATURE,
                                                dev->filter_sensors[ID_T].temperature);
                                            send_event(dev->uinput, EV_SYN, 0, 0);
                                            break;
                                        case SENSORS_PROXIMITY:
                                            send_event(dev->uinput, EV_ABS, EVENT_TYPE_PROXIMITY,
                                                dev->filter_sensors[ID_P].distance);
                                            send_event(dev->uinput, EV_SYN, 0, 0);
                                            break;
                                        case SENSORS_LIGHT:
                                            send_event(dev->uinput, EV_LED, EVENT_TYPE_LIGHT,
                                                dev->filter_sensors[ID_L].light);
                                            send_event(dev->uinput, EV_SYN, 0, 0);
                                            break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return ((void*)0);
}


/*****************************************************************************/

static native_handle_t* control__open_data_source(struct sensors_control_context_t *dev)
{
    native_handle_t* handle;

    if (dev->uinput < 0) {
        int ufd = uinput_create("sensors");
        if (ufd < 0) {
            LOGE("%s: uinput_create failed to create sensors", __PRETTY_FUNCTION__);
            return NULL;
        }
        dev->uinput = ufd;
    }

    /* need to spawn a thread to listen and handle data */
    if (dev->poll_thread == 0)
        pthread_create(&dev->poll_thread, NULL, poll_thread, (void*)dev);

    int fd = open_input("sensors", O_RDONLY);
    if (fd < 0) {
        LOGE("%s: open_input failed to find sensors", __PRETTY_FUNCTION__);
        return NULL;
    }

    handle = native_handle_create(1, 0);
    handle->data[0] = fd;
    return handle;
}

static int control__close_data_source(struct sensors_control_context_t *dev)
{
    /* nothing to do here */
    return 0;
}

static int control__activate(struct sensors_control_context_t *dev,
        int handle, int enabled)
{
    unsigned int flags;
    int err = 0;

    if ((handle<SENSORS_HANDLE_BASE) ||
            (handle>=SENSORS_HANDLE_BASE+MAX_NUM_SENSORS)) {
        return -1;
    }

    uint32_t handle_mask = (1<<handle);
    uint32_t enabled_mask = enabled ? handle_mask : 0;

    uint32_t current_active = dev->active_sensors;
    uint32_t new_active = (current_active & ~handle_mask) | (enabled_mask & handle_mask);

    uint32_t current_enabled = dev->active_drivers;
    uint32_t new_enabled = new_active;

    if (new_active & SENSORS_ORIENTATION)
        new_enabled |= SENSORS_ACCELERATION;

    uint32_t changed_enabled = current_enabled ^ new_enabled;

    //LOGD("%s: handle=%d enabled=%d", __PRETTY_FUNCTION__, handle, enabled);
    //LOGD("%s: current_active=%d new_active=%d", __PRETTY_FUNCTION__, current_active, new_active);
    //LOGD("%s: current_enabled=%d new_enabled=%d", __PRETTY_FUNCTION__, current_enabled, new_enabled);

    if (changed_enabled) {
        //LOGD("%s: changed_enabled=%d", __PRETTY_FUNCTION__, changed_enabled);

        if (changed_enabled & SENSORS_ACCELERATION) {
            int fd = open_dev(dev, ID_LIS);
            if (fd >= 0) {
                flags = (new_enabled & SENSORS_ACCELERATION) ? 1 : 0;
                //LOGD("KXTF9_IOCTL_SET_ENABLE: flag = %d", flags);
                if (ioctl(fd, KXTF9_IOCTL_SET_ENABLE, &flags) < 0) {
                    LOGE("KXTF9_IOCTL_SET_ENABLE error (%s)", strerror(errno));
                    err = -errno;
                }
                close_dev(dev, ID_LIS, new_enabled);
            } else {
                LOGE("ID_LIS open error");
                err = fd;
            }
        }

        if (changed_enabled & SENSORS_PROXIMITY) {
            int fd = open_dev(dev, ID_SFH);
            if (fd >= 0) {
                flags = (new_enabled & SENSORS_PROXIMITY) ? 1 : 0;
                /*LOGD("SFH7743_IOCTL_SET_ENABLE: flag = %d", flags);
                if (ioctl(fd, SFH7743_IOCTL_SET_ENABLE, &flags) < 0) {
                    LOGE("SFH7743_IOCTL_SET_ENABLE error (%s)", strerror(errno));
                    err = -errno;
                }*/
                close_dev(dev, ID_SFH, new_enabled);
            } else {
                LOGE("ID_SFH open error");
                err = fd;
            }
        }

        if (changed_enabled & (SENSORS_ORIENTATION | SENSORS_TEMPERATURE | SENSORS_MAGNETIC_FIELD)) {
            int fd = open_dev(dev, ID_AKM);
            if (fd >= 0) {
                if (changed_enabled & SENSORS_ORIENTATION) {
                    flags = (new_enabled & SENSORS_ORIENTATION) ? 1 : 0;
                    //LOGD("ECS_IOCTL_APP_SET_MFLAG: flag = %d", flags);
                /*    if (ioctl(fd, ECS_IOCTL_APP_SET_MFLAG, &flags) < 0) {
                        LOGE("ECS_IOCTL_APP_SET_MFLAG error (%s)", strerror(errno));
                    }
                }
                if (changed_enabled & SENSORS_TEMPERATURE) {
                    flags = (new_enabled & SENSORS_TEMPERATURE) ? 1 : 0;
                    //LOGD("ECS_IOCTL_APP_SET_TFLAG: flag = %d", flags);
                    if (ioctl(fd, ECS_IOCTL_APP_SET_TFLAG, &flags) < 0) {
                        LOGE("ECS_IOCTL_APP_SET_TFLAG error (%s)", strerror(errno));
                    }
                }
                if (changed_enabled & SENSORS_MAGNETIC_FIELD) {
                    flags = (new_enabled & SENSORS_MAGNETIC_FIELD) ? 1 : 0;
                    //LOGD("ECS_IOCTL_APP_SET_MVFLAG: flag = %d", flags);
                    if (ioctl(fd, ECS_IOCTL_APP_SET_MVFLAG, &flags) < 0) {
                        LOGE("ECS_IOCTL_APP_SET_MVFLAG error (%s)", strerror(errno));
                    }*/
                }
            } else {
                LOGE("ID_AKM open error");
                err = fd;
            }
            close_dev(dev, ID_AKM, new_enabled);
        }

        if (err < 0)
            return err;

    }

    dev->active_sensors = current_active = new_active;
    dev->active_drivers = current_enabled = new_enabled;

    return 0;
}

static int control__set_delay(struct sensors_control_context_t *dev, int32_t ms)
{
    int delay = ms;
    short sdelay = ms;
    int err = 0;

    int fd = dev->dev_fd[ID_LIS];
    if (fd >= 0) {
        //LOGD("KXTF9_IOCTL_SET_DELAY: delay = %d", delay);
        if (ioctl(fd, KXTF9_IOCTL_SET_DELAY, &delay) < 0) {
            LOGE("KXTF9_IOCTL_SET_DELAY error (%s)", strerror(errno));
            err = -errno;
        }
    }

    fd = dev->dev_fd[ID_AKM];
    if (fd >= 0) {
        /*LOGD("ECS_IOCTL_APP_SET_DELAY: delay = %d", sdelay);
        if (ioctl(fd, ECS_IOCTL_APP_SET_DELAY, &sdelay) < 0) {
            LOGE("ECS_IOCTL_APP_SET_DELAY error (%s)", strerror(errno));
            err = -errno;
        }*/
    }

    return err;
}

static int control__wake(struct sensors_control_context_t *dev)
{
    int err = 0;
    int fd = open_input(dDriverList[0].name, O_WRONLY);
    if (fd >= 0) {
        err = send_event(fd, EV_SYN, SYN_CONFIG, 0);
        LOGD_IF(err<0, "control__wake, err=%d (%s)", errno, strerror(errno));
        close(fd);
    }
    return err;
}

/*****************************************************************************/

static int data__data_open(struct sensors_data_context_t *dev, native_handle_t* handle)
{
    int i;
    memset(&dev->sensors, 0, sizeof(dev->sensors));

    for (i=0 ; i<MAX_NUM_SENSORS ; i++) {
        // by default all sensors have high accuracy
        // (we do this because we don't get an update if the value doesn't
        // change).
        dev->sensors[i].vector.status = SENSOR_STATUS_ACCURACY_HIGH;
    }
    dev->pendingSensors = 0;
    dev->events_fd = dup(handle->data[0]);
    //LOGD("data__data_open: fd = %d", handle->data[0]);
    native_handle_close(handle);
    native_handle_delete(handle);
    return 0;
}

static int data__data_close(struct sensors_data_context_t *dev)
{
    if (dev->events_fd >= 0) {
        //LOGD("(data close) about to close fd=%d", dev->events_fd);
        close(dev->events_fd);
        dev->events_fd = -1;
    }
    return 0;
}

static int pick_sensor(struct sensors_data_context_t *dev,
        sensors_data_t* values)
{
    uint32_t mask = SUPPORTED_SENSORS;
    while (mask) {
        uint32_t i = 31 - __builtin_clz(mask);
        mask &= ~(1<<i);
        if (dev->pendingSensors & (1<<i)) {
            dev->pendingSensors &= ~(1<<i);
            *values = dev->sensors[i];
            values->sensor = (1<<i);
            LOGD_IF(0, "%d [%f, %f, %f]", (1<<i),
                    values->vector.x,
                    values->vector.y,
                    values->vector.z);
            return i;
        }
    }
    LOGE("No sensor to return!!! pendingSensors=%08x", dev->pendingSensors);
    // we may end-up in a busy loop, slow things down, just in case.
    usleep(100000);
    return -1;
}

static int data__poll(struct sensors_data_context_t *dev, sensors_data_t* values)
{
    int fd = dev->events_fd;
    if (fd < 0) {
        LOGE("invalid file descriptor, fd=%d", fd);
        return -1;
    }

    // there are pending sensors, returns them now...
    if (dev->pendingSensors) {
        return pick_sensor(dev, values);
    }

    // wait until we get a complete event for an enabled sensor
    uint32_t new_sensors = 0;
    while (1) {
        /* read the next event */
        struct input_event event;
        uint32_t v;
        int nread = read(fd, &event, sizeof(event));
        if (nread != sizeof(event))
            return -1;
        if (event.type == EV_ABS) {
            //LOGD("type: %d code: %d value: %-5d time: %ds",
            //        event.type, event.code, event.value,
            //      (int)event.time.tv_sec);
            switch (event.code) {

                case EVENT_TYPE_ACCEL_X:
                    new_sensors |= SENSORS_ACCELERATION;
                    dev->sensors[ID_A].acceleration.x = event.value * CONVERT_A_X;
                    break;
                case EVENT_TYPE_ACCEL_Y:
                    new_sensors |= SENSORS_ACCELERATION;
                    dev->sensors[ID_A].acceleration.y = event.value * CONVERT_A_Y;
                    break;
                case EVENT_TYPE_ACCEL_Z:
                    new_sensors |= SENSORS_ACCELERATION;
                    dev->sensors[ID_A].acceleration.z = event.value * CONVERT_A_Z;
                    break;

                case EVENT_TYPE_MAGV_X:
                    new_sensors |= SENSORS_MAGNETIC_FIELD;
                    dev->sensors[ID_M].magnetic.x = event.value * CONVERT_M_X;
                    break;
                case EVENT_TYPE_MAGV_Y:
                    new_sensors |= SENSORS_MAGNETIC_FIELD;
                    dev->sensors[ID_M].magnetic.y = event.value * CONVERT_M_Y;
                    break;
                case EVENT_TYPE_MAGV_Z:
                    new_sensors |= SENSORS_MAGNETIC_FIELD;
                    dev->sensors[ID_M].magnetic.z = event.value * CONVERT_M_Z;
                    break;

                case EVENT_TYPE_YAW:
                    new_sensors |= SENSORS_ORIENTATION;
                    dev->sensors[ID_O].orientation.azimuth =  event.value * CONVERT_O_Y;
                    break;
                case EVENT_TYPE_PITCH:
                    new_sensors |= SENSORS_ORIENTATION;
                    dev->sensors[ID_O].orientation.pitch = event.value * CONVERT_O_P;
                    break;
                case EVENT_TYPE_ROLL:
                    new_sensors |= SENSORS_ORIENTATION;
                    dev->sensors[ID_O].orientation.roll = event.value * CONVERT_O_R;
                    break;

                case EVENT_TYPE_TEMPERATURE:
                    new_sensors |= SENSORS_TEMPERATURE;
                    dev->sensors[ID_T].temperature = event.value;
                    break;

                case EVENT_TYPE_ACCEL_STATUS:
                    // accuracy of the calibration (never returned!)
                    //LOGD("G-Sensor status %d", event.value);
                    break;
                case EVENT_TYPE_ORIENT_STATUS:
                    // accuracy of the calibration
                    v = (uint32_t)(event.value & SENSOR_STATE_MASK);
                    LOGD_IF(dev->sensors[ID_O].orientation.status != (uint8_t)v,
                            "M-Sensor status %d", v);
                    dev->sensors[ID_O].orientation.status = (uint8_t)v;
                    break;

                case EVENT_TYPE_PROXIMITY:
                    new_sensors |= SENSORS_PROXIMITY;
                    if ((event.value * CONVERT_P) <= PROXIMITY_THRESHOLD_CM) {
                        dev->sensors[ID_P].distance = 0;
                    } else {
                        dev->sensors[ID_P].distance = PROXIMITY_THRESHOLD_CM;
                    }
                    break;
            }
        } else if (event.type == EV_LED) {
            if (event.code == LED_MISC) {
                new_sensors |= SENSORS_LIGHT;
                dev->sensors[ID_L].light = event.value;
            }
        } else if (event.type == EV_SYN) {
            if (event.code == SYN_CONFIG) {
                // we use SYN_CONFIG to signal that we need to exit the
                // main loop.
                //LOGD("got empty message: value=%d", event.value);
                return 0x7FFFFFFF;
            }
            if (new_sensors) {
                dev->pendingSensors = new_sensors;
                int64_t t = event.time.tv_sec*1000000000LL +
                        event.time.tv_usec*1000;
                while (new_sensors) {
                    uint32_t i = 31 - __builtin_clz(new_sensors);
                    new_sensors &= ~(1<<i);
                    dev->sensors[i].time = t;
                }
                return pick_sensor(dev, values);
            }
        }
    }
}

/*****************************************************************************/

static int control__close(struct hw_device_t *dev)
{
    int i = 0;
    struct sensors_control_context_t* ctx = (struct sensors_control_context_t*)dev;
    if (ctx) {
        control__close_data_source(ctx);
        for (i = 0; i < MAX_NUM_DRIVERS; i++) {
            if (ctx->dev_fd[i] >= 0)
                close(ctx->dev_fd[i]);
        }
        free(ctx);
    }

    return 0;
}

static int data__close(struct hw_device_t *dev)
{
    int i = 0;
    struct sensors_data_context_t* ctx = (struct sensors_data_context_t*)dev;
    if (ctx) {
        if (ctx->events_fd >= 0) {
            //LOGD("(device close) about to close fd=%d", ctx->events_fd);
            close(ctx->events_fd);
        }
        free(ctx);
    }
    return 0;
}

/** Open a new instance of a sensor device using name */
static int open_sensors(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device)
{
    int i = 0;
    int status = -EINVAL;
    if (!strcmp(name, SENSORS_HARDWARE_CONTROL)) {
        struct sensors_control_context_t *dev;
        dev = malloc(sizeof(*dev));
        memset(dev, 0, sizeof(*dev));
        for (i = 0; i < MAX_NUM_DRIVERS; i++)
            dev->dev_fd[i] = -1;
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = module;
        dev->device.common.close = control__close;
        dev->device.open_data_source = control__open_data_source;
        dev->device.close_data_source = control__close_data_source;
        dev->device.activate = control__activate;
        dev->device.set_delay= control__set_delay;
        dev->device.wake = control__wake;
        dev->active_sensors = 0;
        dev->active_drivers= 0;
        dev->uinput = -1;
       *device = &dev->device.common;
        status = 0;
    } else if (!strcmp(name, SENSORS_HARDWARE_DATA)) {
        struct sensors_data_context_t *dev;
        dev = malloc(sizeof(*dev));
        memset(dev, 0, sizeof(*dev));
        dev->events_fd = -1;
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = module;
        dev->device.common.close = data__close;
        dev->device.data_open = data__data_open;
        dev->device.data_close = data__data_close;
        dev->device.poll = data__poll;
        *device = &dev->device.common;
        status = 0;
    }
    return status;
}