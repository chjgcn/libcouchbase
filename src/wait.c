/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010-2013 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */
#include "internal.h"

/**
 * Returns non zero if the event loop is running now
 *
 * @param instance the instance to run the event loop for.
 */
LIBCOUCHBASE_API
int lcb_is_waiting(lcb_t instance)
{
    return instance->wait != 0;
}

/**
 * Run the event loop until we've got a response for all of our spooled
 * commands. You should not call this function from within your callbacks.
 *
 * @param instance the instance to run the event loop for.
 *
 * @author Trond Norbye
 */
LIBCOUCHBASE_API
lcb_error_t lcb_wait(lcb_t instance)
{

    if (instance->wait != 0) {
        return instance->last_error;
    }

    /*
     * The API is designed for you to run your own event loop,
     * but should also work if you don't do that.. In order to be
     * able to know when to break out of the event loop, we're setting
     * the wait flag to 1
     */
    instance->last_error = LCB_SUCCESS;
    instance->wait = 1;
    if (LCBT_VBCONFIG(instance) == NULL ||
            lcb_flushing_buffers(instance) ||
            hashset_num_items(instance->timers) > 0 ||
            hashset_num_items(instance->durability_polls) > 0) {

        lcb_size_t ii;

        for (ii = 0; ii < LCBT_NSERVERS(instance); ii++) {
            lcb_server_t *c = LCBT_GET_SERVER(instance, ii);

            if (lcb_server_has_pending(c)) {
                lcb_timer_rearm(c->io_timer,
                                instance->settings.operation_timeout);
            }
        }
        instance->settings.io->loop.start(instance->settings.io->p);
    }

    instance->wait = 0;

    if (LCBT_VBCONFIG(instance)) {
        return LCB_SUCCESS;
    }

    return instance->last_error;
}

/**
 * Stop event loop
 *
 * @param instance the instance to run the event loop for.
 */
LIBCOUCHBASE_API
void lcb_breakout(lcb_t instance)
{
    if (instance->wait) {
        instance->settings.io->loop.stop(instance->settings.io->p);
        instance->wait = 0;
    }
}
