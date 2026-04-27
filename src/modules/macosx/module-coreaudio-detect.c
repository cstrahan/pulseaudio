/***
  This file is part of PulseAudio.

  Copyright 2009,2010 Daniel Mack <daniel@caiaq.de>

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/xmalloc.h>

#include <pulsecore/module.h>
#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/llist.h>
#include <pulsecore/sink.h>
#include <pulsecore/source.h>

#include <CoreAudio/CoreAudio.h>

#define DEVICE_MODULE_NAME "module-coreaudio-device"

/* Event tags multiplexed over the self-pipe from CoreAudio property
 * listeners to the main thread. */
#define CA_EVENT_DEVICE_LIST      '\x01'
#define CA_EVENT_DEFAULT_OUTPUT   '\x02'
#define CA_EVENT_DEFAULT_INPUT    '\x03'

PA_MODULE_AUTHOR("Daniel Mack");
PA_MODULE_DESCRIPTION("CoreAudio device detection");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_USAGE("ioproc_frames=<passed on to module-coreaudio-device> "
                "record=<enable source?> "
                "playback=<enable sink?> "
                "follow_system_default=<track macOS default output/input device?> ");

static const char* const valid_modargs[] = {
    "ioproc_frames",
    "record",
    "playback",
    "follow_system_default",
    NULL
};

typedef struct ca_device ca_device;

struct ca_device {
    AudioObjectID id;
    unsigned int module_index;
    PA_LLIST_FIELDS(ca_device);
};

struct userdata {
    int detect_fds[2];
    pa_io_event *detect_io;
    unsigned int ioproc_frames;
    bool record;
    bool playback;
    bool follow_system_default;
    pa_hook_slot *sink_put_slot, *source_put_slot;
    PA_LLIST_HEAD(ca_device, devices);
};

static pa_sink *ca_find_sink_for_object_id(pa_module *m, AudioObjectID id);
static pa_source *ca_find_source_for_object_id(pa_module *m, AudioObjectID id);
static void ca_apply_system_default_sink(pa_module *m);
static void ca_apply_system_default_source(pa_module *m);

static int ca_device_added(struct pa_module *m, AudioObjectID id) {
    AudioObjectPropertyAddress property_address;
    OSStatus err;
    pa_module *mod;
    struct userdata *u;
    struct ca_device *dev;
    char *args, tmp[64];
    UInt32 size;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    /* To prevent generating a black hole that will suck us in,
       don't create sources/sinks for PulseAudio virtual devices */

    property_address.mSelector = kAudioDevicePropertyDeviceManufacturer;
    property_address.mScope = kAudioObjectPropertyScopeGlobal;
    property_address.mElement = kAudioObjectPropertyElementMaster;

    size = sizeof(tmp);
    err = AudioObjectGetPropertyData(id, &property_address, 0, NULL, &size, tmp);

    if (!err && pa_streq(tmp, "pulseaudio.org"))
        return 0;

    if (u->ioproc_frames)
        args = pa_sprintf_malloc("object_id=%d ioproc_frames=%d record=%d playback=%d", (int) id, u->ioproc_frames, (int) u->record, (int) u->playback);
    else
        args = pa_sprintf_malloc("object_id=%d record=%d playback=%d", (int) id, (int) u->record, (int) u->playback);

    pa_log_debug("Loading %s with arguments '%s'", DEVICE_MODULE_NAME, args);
    pa_module_load(&mod, m->core, DEVICE_MODULE_NAME, args);
    pa_xfree(args);

    if (!mod) {
        pa_log_info("Failed to load module %s with arguments '%s'", DEVICE_MODULE_NAME, args);
        return -1;
    }

    dev = pa_xnew0(ca_device, 1);
    dev->module_index = mod->index;
    dev->id = id;

    PA_LLIST_INIT(ca_device, dev);
    PA_LLIST_PREPEND(ca_device, u->devices, dev);

    return 0;
}

static int ca_update_device_list(struct pa_module *m) {
    AudioObjectPropertyAddress property_address;
    OSStatus err;
    UInt32 i, size, num_devices;
    AudioDeviceID *device_id;
    struct ca_device *dev;
    struct userdata *u;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    property_address.mSelector = kAudioHardwarePropertyDevices;
    property_address.mScope = kAudioObjectPropertyScopeGlobal;
    property_address.mElement = kAudioObjectPropertyElementMaster;

    /* get the number of currently available audio devices */
    err = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &property_address, 0, NULL, &size);
    if (err) {
        pa_log("Unable to get data size for kAudioHardwarePropertyDevices.");
        return -1;
    }

    num_devices = size / sizeof(AudioDeviceID);
    device_id = pa_xnew(AudioDeviceID, num_devices);

    err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &property_address, 0, NULL, &size, device_id);
    if (err) {
        pa_log("Unable to get kAudioHardwarePropertyDevices.");
        pa_xfree(device_id);
        return -1;
    }

    /* scan for devices which are reported but not in our cached list */
    for (i = 0; i < num_devices; i++) {
        bool found = false;

        PA_LLIST_FOREACH(dev, u->devices)
            if (dev->id == device_id[i]) {
                found = true;
                break;
            }

        if (!found)
            ca_device_added(m, device_id[i]);
    }

    /* scan for devices which are in our cached list but are not reported */
scan_removed:

    PA_LLIST_FOREACH(dev, u->devices) {
        bool found = false;

        for (i = 0; i < num_devices; i++)
            if (dev->id == device_id[i]) {
                found = true;
                break;
            }

        if (!found) {
            pa_log_debug("object id %d has been removed (module index %d) %p", (unsigned int) dev->id, dev->module_index, dev);
            pa_module_unload_request_by_index(m->core, dev->module_index, true);
            PA_LLIST_REMOVE(ca_device, u->devices, dev);
            pa_xfree(dev);
            /* the current list item pointer is not valid anymore, so start over. */
            goto scan_removed;
        }
    }

    pa_xfree(device_id);
    return 0;
}

static pa_sink *ca_find_sink_for_object_id(pa_module *m, AudioObjectID id) {
    struct userdata *u;
    struct ca_device *dev;
    pa_sink *sink;
    uint32_t idx;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    PA_LLIST_FOREACH(dev, u->devices) {
        if (dev->id != id)
            continue;

        PA_IDXSET_FOREACH(sink, m->core->sinks, idx) {
            if (sink->module && sink->module->index == dev->module_index)
                return sink;
        }
    }

    return NULL;
}

static pa_source *ca_find_source_for_object_id(pa_module *m, AudioObjectID id) {
    struct userdata *u;
    struct ca_device *dev;
    pa_source *source;
    uint32_t idx;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    PA_LLIST_FOREACH(dev, u->devices) {
        if (dev->id != id)
            continue;

        PA_IDXSET_FOREACH(source, m->core->sources, idx) {
            /* Skip sinks' monitor sources -- those belong to the sink's module
             * but are inputs of a different nature. Prefer real hardware sources. */
            if (source->monitor_of)
                continue;
            if (source->module && source->module->index == dev->module_index)
                return source;
        }
    }

    return NULL;
}

static AudioDeviceID ca_get_system_default(AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress property_address;
    AudioDeviceID id = kAudioObjectUnknown;
    UInt32 size = sizeof(id);
    OSStatus err;

    property_address.mSelector = selector;
    property_address.mScope = kAudioObjectPropertyScopeGlobal;
    property_address.mElement = kAudioObjectPropertyElementMaster;

    err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &property_address,
                                     0, NULL, &size, &id);
    if (err) {
        pa_log_debug("Unable to read CoreAudio default device (selector %u): %d",
                     (unsigned int) selector, (int) err);
        return kAudioObjectUnknown;
    }

    return id;
}

static void ca_apply_system_default_sink(pa_module *m) {
    struct userdata *u;
    AudioDeviceID id;
    pa_sink *sink;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    if (!u->follow_system_default || !u->playback)
        return;

    id = ca_get_system_default(kAudioHardwarePropertyDefaultOutputDevice);
    if (id == kAudioObjectUnknown)
        return;

    sink = ca_find_sink_for_object_id(m, id);
    if (!sink) {
        /* The device module may not have finished creating its sink yet; the
         * sink_put hook will pick this up when it does. */
        pa_log_debug("CoreAudio default output device %u has no matching sink yet",
                     (unsigned int) id);
        return;
    }

    pa_log_info("Following macOS default output: setting policy default sink to '%s'", sink->name);
    pa_core_set_policy_default_sink(m->core, sink->name);
}

static void ca_apply_system_default_source(pa_module *m) {
    struct userdata *u;
    AudioDeviceID id;
    pa_source *source;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    if (!u->follow_system_default || !u->record)
        return;

    id = ca_get_system_default(kAudioHardwarePropertyDefaultInputDevice);
    if (id == kAudioObjectUnknown)
        return;

    source = ca_find_source_for_object_id(m, id);
    if (!source) {
        pa_log_debug("CoreAudio default input device %u has no matching source yet",
                     (unsigned int) id);
        return;
    }

    pa_log_info("Following macOS default input: setting policy default source to '%s'", source->name);
    pa_core_set_policy_default_source(m->core, source->name);
}

static pa_hook_result_t sink_put_hook_cb(pa_core *c, pa_sink *sink, pa_module *m) {
    pa_assert(c);
    pa_assert(sink);
    pa_assert(m);

    /* A new sink appeared; it may belong to the device that CoreAudio
     * considers the current default. Re-apply so startup and hot-plug
     * races end up with the correct default. */
    ca_apply_system_default_sink(m);
    return PA_HOOK_OK;
}

static pa_hook_result_t source_put_hook_cb(pa_core *c, pa_source *source, pa_module *m) {
    pa_assert(c);
    pa_assert(source);
    pa_assert(m);

    ca_apply_system_default_source(m);
    return PA_HOOK_OK;
}

static OSStatus property_listener_proc(AudioObjectID objectID, UInt32 numberAddresses,
                                       const AudioObjectPropertyAddress inAddresses[],
                                       void *clientData) {
    struct userdata *u = clientData;
    UInt32 i;

    pa_assert(u);

    /* Translate each triggered CoreAudio property into a tagged wake-up for the
     * main thread. CoreAudio may batch several selectors into one callback. */
    for (i = 0; i < numberAddresses; i++) {
        char tag;

        switch (inAddresses[i].mSelector) {
            case kAudioHardwarePropertyDevices:
                tag = CA_EVENT_DEVICE_LIST;
                break;
            case kAudioHardwarePropertyDefaultOutputDevice:
                tag = CA_EVENT_DEFAULT_OUTPUT;
                break;
            case kAudioHardwarePropertyDefaultInputDevice:
                tag = CA_EVENT_DEFAULT_INPUT;
                break;
            default:
                continue;
        }

        write(u->detect_fds[1], &tag, 1);
    }

    return 0;
}

static void detect_handle(pa_mainloop_api *a, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata) {
    pa_module *m = userdata;
    char tag;
    ssize_t n;

    pa_assert(m);

    n = read(fd, &tag, 1);
    if (n != 1)
        return;

    switch (tag) {
        case CA_EVENT_DEVICE_LIST:
            ca_update_device_list(m);
            break;
        case CA_EVENT_DEFAULT_OUTPUT:
            ca_apply_system_default_sink(m);
            break;
        case CA_EVENT_DEFAULT_INPUT:
            ca_apply_system_default_source(m);
            break;
        default:
            pa_log_debug("Unknown coreaudio detect event tag: %d", (int) tag);
            break;
    }
}

int pa__init(pa_module *m) {
    struct userdata *u = pa_xnew0(struct userdata, 1);
    AudioObjectPropertyAddress property_address;
    pa_modargs *ma;

    pa_assert(m);
    pa_assert(m->core);

    u->detect_fds[0] = u->detect_fds[1] = -1;
    m->userdata = u;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    /*
     * Set default value to true if not given as a modarg.
     * In such a case, pa_modargs_get_value_boolean() will not touch the
     * buffer.
     */
    u->playback = u->record = true;
    u->follow_system_default = true;

    if (pa_modargs_get_value_boolean(ma, "record", &u->record) < 0 || pa_modargs_get_value_boolean(ma, "playback", &u->playback) < 0) {
        pa_log("record= and playback= expect boolean argument.");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "follow_system_default", &u->follow_system_default) < 0) {
        pa_log("follow_system_default= expects a boolean argument.");
        goto fail;
    }

    if (!u->playback && !u->record) {
        pa_log("neither playback nor record enabled for device.");
        goto fail;
    }

    pa_modargs_get_value_u32(ma, "ioproc_frames", &u->ioproc_frames);

    property_address.mScope = kAudioObjectPropertyScopeGlobal;
    property_address.mElement = kAudioObjectPropertyElementMaster;

    property_address.mSelector = kAudioHardwarePropertyDevices;
    if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &property_address, property_listener_proc, u)) {
        pa_log("AudioObjectAddPropertyListener() failed for device list.");
        goto fail;
    }

    if (u->follow_system_default) {
        if (u->playback) {
            property_address.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
            if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &property_address, property_listener_proc, u))
                pa_log_warn("AudioObjectAddPropertyListener() failed for default output; macOS default tracking disabled for output.");
        }

        if (u->record) {
            property_address.mSelector = kAudioHardwarePropertyDefaultInputDevice;
            if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &property_address, property_listener_proc, u))
                pa_log_warn("AudioObjectAddPropertyListener() failed for default input; macOS default tracking disabled for input.");
        }
    }

    if (ca_update_device_list(m))
       goto fail;

    pa_assert_se(pipe(u->detect_fds) == 0);
    pa_assert_se(u->detect_io = m->core->mainloop->io_new(m->core->mainloop, u->detect_fds[0], PA_IO_EVENT_INPUT, detect_handle, m));

    if (u->follow_system_default) {
        /* React to sinks/sources becoming live after a default-change
         * notification arrived too early (hot-plug race). */
        u->sink_put_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_PUT],
                                           PA_HOOK_LATE, (pa_hook_cb_t) sink_put_hook_cb, m);
        u->source_put_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_PUT],
                                             PA_HOOK_LATE, (pa_hook_cb_t) source_put_hook_cb, m);

        /* Sync the initial default with whatever CoreAudio currently reports. */
        ca_apply_system_default_sink(m);
        ca_apply_system_default_source(m);
    }

    pa_modargs_free(ma);
    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);
    pa__done(m);
    return -1;
}

void pa__done(pa_module *m) {
    struct userdata *u;
    struct ca_device *dev;
    AudioObjectPropertyAddress property_address;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    dev = u->devices;

    property_address.mScope = kAudioObjectPropertyScopeGlobal;
    property_address.mElement = kAudioObjectPropertyElementMaster;

    property_address.mSelector = kAudioHardwarePropertyDevices;
    AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &property_address, property_listener_proc, u);

    /* These are no-ops if the listeners were never installed. */
    property_address.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
    AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &property_address, property_listener_proc, u);
    property_address.mSelector = kAudioHardwarePropertyDefaultInputDevice;
    AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &property_address, property_listener_proc, u);

    if (u->sink_put_slot)
        pa_hook_slot_free(u->sink_put_slot);
    if (u->source_put_slot)
        pa_hook_slot_free(u->source_put_slot);

    while (dev) {
        struct ca_device *next = dev->next;

        pa_module_unload_request_by_index(m->core, dev->module_index, true);
        pa_xfree(dev);

        dev = next;
    }

    if (u->detect_fds[0] >= 0)
        close(u->detect_fds[0]);

    if (u->detect_fds[1] >= 0)
        close(u->detect_fds[1]);

    if (u->detect_io)
        m->core->mainloop->io_free(u->detect_io);

    pa_xfree(u);
    m->userdata = NULL;
}
