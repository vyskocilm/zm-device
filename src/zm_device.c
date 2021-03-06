/*  =========================================================================
    zm_device - zm device actor

    Copyright (c) the Contributors as noted in the AUTHORS file.  This file is part
    of zmon.it, the fast and scalable monitoring system.                           
                                                                                   
    This Source Code Form is subject to the terms of the Mozilla Public License, v.
    2.0. If a copy of the MPL was not distributed with this file, You can obtain   
    one at http://mozilla.org/MPL/2.0/.                                            
    =========================================================================
*/

/*
@header
    zm_device - zm device actor
@discuss

zm-device have three main mode of operation

# PUBLISH on ZM_PROTO_DEVICE_STREAM (not yet implemented)

In this mode actor simple publish information about devices with subjects
INSERT and DELETE. INSERT means that new device has been added. DELETE means
device is gone.

# CONSUME (not implemented - what will be the use-case? inventory stream can be done via special MAILBOX command)

# MAILBOX

In this mode actor provide three commands (subjects)

    * INSERT - adds or update device in internal cache, PUBLISH it on STREAM
        returns ZM_PROTO_OK
    * DELETE - delete device from cache and PUBLISH it on stream
        returns ZM_PROTO_OK
    * LOOKUP - search by device name
        returns ZM_PROTO_DEVICE if found
        returns ZM_PROTO_ERROR if not found
    * GET-ALL - return all devices
        return ZM_PROTO_ERROR if there are no devices
        return M ZM_PROTO_DEVICE messages, where ext have
        _seq : "N"
        _cnt : "M"
    * PUBLISH-ALL - publish all the devices
        publish M ZM_PROTO_DEVICE messages, where ext have
        _seq : "N"
        _cnt : "M"

@end
*/

#include "zm_device_classes.h"

//  Structure of our actor

struct _zm_device_t {
    zsock_t *pipe;              //  Actor command pipe
    zpoller_t *poller;          //  Socket poller
    bool terminated;            //  Did caller ask us to quit?
    bool verbose;               //  Verbose logging enabled?
    //  TODO: Declare properties
    zconfig_t *config;          //  Server configuration
    mlm_client_t *client;       //  Malamute client
    zhash_t *consumers;         //  List of streams to subscribe
    zm_proto_t *msg;            //  Last received message
    zm_devices_t *devices;      //  List of devices to maintain
};


//  --------------------------------------------------------------------------
//  Create a new zm_device instance

static zm_device_t *
zm_device_new (zsock_t *pipe, void *args)
{
    zm_device_t *self = (zm_device_t *) zmalloc (sizeof (zm_device_t));
    assert (self);

    self->pipe = pipe;
    self->terminated = false;
    self->poller = zpoller_new (self->pipe, NULL);
    self->devices = zm_devices_new (NULL);

    self->config = NULL;
    self->consumers = NULL;
    self->msg = zm_proto_new ();
    self->client = mlm_client_new ();
    assert (self->client);
    zpoller_add (self->poller, mlm_client_msgpipe (self->client));

    return self;
}


//  --------------------------------------------------------------------------
//  Destroy the zm_device instance

static void
zm_device_destroy (zm_device_t **self_p)
{
    assert (self_p);
    if (*self_p) {
        zm_device_t *self = *self_p;

        zconfig_destroy (&self->config);
        zhash_destroy (&self->consumers);
        zm_proto_destroy (&self->msg);
        mlm_client_destroy (&self->client);
        zpoller_destroy (&self->poller);

        zm_devices_store (self->devices);
        zm_devices_destroy (&self->devices);

        //  Free object itself
        free (self);
        *self_p = NULL;
    }
}

static const char*
zm_device_cfg_endpoint (zm_device_t *self)
{
    assert (self);
    if (self->config) {
        return zconfig_resolve (self->config, "malamute/endpoint", NULL);
    }
    return NULL;
}

static const char*
zm_device_cfg_address (zm_device_t *self)
{
    assert (self);
    if (self->config) {
        return zconfig_resolve (self->config, "malamute/address", NULL);
    }
    return NULL;
}

static const char *
zm_device_cfg_producer (zm_device_t *self) {
    assert (self);
    if (self->config) {
        return zconfig_resolve (self->config, "malamute/producer", NULL);
    }
    return NULL;
}

static const char *
zm_device_cfg_file (zm_device_t *self) {
    assert (self);
    if (self->config) {
        return zconfig_resolve (self->config, "server/file", NULL);
    }
    return NULL;
}

static const char*
zm_device_cfg_consumer_first (zm_device_t *self) {
    assert (self);
    zhash_destroy (&self->consumers);
    self->consumers = zhash_new ();
    
    zconfig_t *cfg = zconfig_locate (self->config, "malamute/consumer");
    if (cfg) {
        zconfig_t *child = zconfig_child (cfg);
        while (child) {
            zhash_insert (self->consumers, zconfig_name (child), zconfig_value (child));
            child = zconfig_next (child);
        }
    }

    return (const char*) zhash_first (self->consumers);
}

static const char*
zm_device_cfg_consumer_next (zm_device_t *self) {
    assert (self);
    assert (self->consumers);
     return (const char*) zhash_next (self->consumers);
}

static const char*
zm_device_cfg_consumer_stream (zm_device_t *self) {
    assert (self);
    assert (self->consumers);
     return zhash_cursor (self->consumers);
}

static int
zm_device_connect_to_malamute (zm_device_t *self)
{
    if (!self->config) {
        zsys_warning ("zm-device: no CONFIGuration provided, there is nothing to do");
        return -1;
    }

    const char *endpoint = zm_device_cfg_endpoint (self);
    const char *address = zm_device_cfg_address (self);

    if (!endpoint) {
        zsys_error ("malamute/endpoint is missing");
        return -1;
    }

    if (!address) {
        zsys_error ("malamute/address is missing");
        return -1;
    }

    if (!self->client) {
        self->client = mlm_client_new ();
        zpoller_add (self->poller, mlm_client_msgpipe (self->client));
    }

    int r = mlm_client_connect (self->client, endpoint, 5000, address);
    if (r == -1) {
        zsys_warning ("Can't connect to malamute endpoint %", endpoint);
        return -1;
    }

    if (zm_device_cfg_producer (self)) {
        r = mlm_client_set_producer (self->client, zm_device_cfg_producer (self));
        if (r == -1) {
            zsys_warning ("Can't setup publisher on stream %", zm_device_cfg_producer (self));
            return -1;
        }
    }

    const char *pattern = zm_device_cfg_consumer_first (self);
    while (pattern) {
        const char *stream = zm_device_cfg_consumer_stream (self);
        r = mlm_client_set_consumer (self->client, stream, pattern);
        if (r == -1) {
            zsys_warning ("Can't setup consumer %s/%s", stream, pattern);
            return -1;
        }
        pattern = zm_device_cfg_consumer_next (self);
    }
    return 0;
}

//  Start this actor. Return a value greater or equal to zero if initialization
//  was successful. Otherwise -1.

static int
zm_device_start (zm_device_t *self)
{
    assert (self);

    int r = zm_device_connect_to_malamute (self);
    if (r == -1)
        return r;

    return 0;
}


//  Stop this actor. Return a value greater or equal to zero if stopping 
//  was successful. Otherwise -1.

static int
zm_device_stop (zm_device_t *self)
{
    assert (self);

    zpoller_remove (self->poller, mlm_client_msgpipe (self->client));
    mlm_client_destroy (&self->client);
    zm_devices_store (self->devices);

    return 0;
}

//  Config message, second argument is string representation of config file
static int
zm_device_config (zm_device_t *self, zmsg_t *request)
{
    assert (self);
    assert (request);

    char *str_config = zmsg_popstr (request);
    if (str_config) {
        zconfig_t *foo = zconfig_str_load (str_config);
        zstr_free (&str_config);
        if (foo) {
            zconfig_destroy (&self->config);
            self->config = foo;
            if (zm_device_cfg_file (self)) {
                if (!zm_devices_file (self->devices))
                    zm_devices_set_file (self->devices, zm_device_cfg_file (self));
                zm_devices_store (self->devices);
                zm_devices_destroy (&self->devices);
                self->devices = zm_devices_new (zm_device_cfg_file (self));
            }
        }
        else {
            zsys_warning ("zm_device: can't load config file from string");
            return -1;
        }
    }
    else
        return -1;
    return 0;
}


//  Here we handle incoming message from the node

static void
zm_device_recv_api (zm_device_t *self)
{
    //  Get the whole message of the pipe in one go
    zmsg_t *request = zmsg_recv (self->pipe);
    if (!request)
       return;        //  Interrupted

    char *command = zmsg_popstr (request);
    if (streq (command, "START"))
        zm_device_start (self);
    else
    if (streq (command, "STOP"))
        zm_device_stop (self);
    else
    if (streq (command, "VERBOSE"))
        self->verbose = true;
    else
    if (streq (command, "$TERM"))
        //  The $TERM command is send by zactor_destroy() method
        self->terminated = true;
    else
    if (streq (command, "CONFIG"))
        zm_device_config (self, request);
    else {
        zsys_error ("invalid command '%s'", command);
        assert (false);
    }
    zstr_free (&command);
    zmsg_destroy (&request);
}

static int
zm_device_publish (zm_device_t *self, zm_proto_t *device, const char *subject)
{
    assert (self);
    assert (device);
    assert (subject);

    zmsg_t *msg = zmsg_new ();
    zm_proto_send (device, msg);
    return mlm_client_send (self->client, subject, &msg);
}

static void
zm_device_recv_mlm_mailbox (zm_device_t *self)
{
    assert (self);

    const char *subject = mlm_client_subject (self->client);
    zm_proto_t *msg = self->msg;    // message to send
    zm_proto_t *reply = NULL;

    if (streq (subject, "INSERT")) {
        zm_devices_insert (self->devices, self->msg);
        zm_device_publish (self, self->msg, subject);
        zm_proto_encode_ok (self->msg);
    }
    else
    if (streq (subject, "DELETE")) {
        const char *device = zm_proto_device (self->msg);
        zm_devices_delete (self->devices, device);
        zm_device_publish (self, self->msg, subject);
        zm_proto_encode_ok (self->msg);
    }
    else
    if (streq (subject, "LOOKUP")) {
        const char *device = zm_proto_device (self->msg);
        reply = zm_devices_lookup (self->devices, device);

        if (reply)
            msg = reply;
        else
            zm_proto_encode_error (self->msg, 404, "Requested device does not exists");
    }
    else
    if (streq (subject, "GET-ALL")) {
        if (zm_devices_size (self->devices) == 0) {
            zm_proto_encode_error (self->msg, 404, "No devices");
            goto send;
        }

        msg = zm_devices_first (self->devices);
        zm_proto_ext_set_int (msg, "_cnt", zm_devices_size (self->devices));
        size_t i = 0;
        while (msg) {
            zm_proto_ext_set_int (msg, "_seq", i++);
            zm_proto_sendto (
                msg,
                self->client,
                mlm_client_sender (self->client),
                subject);
            msg = zm_devices_next (self->devices);
        }
        return;
    }
    else
    if (streq (subject, "PUBLISH-ALL")) {

        msg = zm_devices_first (self->devices);
        zm_proto_ext_set_int (msg, "_cnt", zm_devices_size (self->devices));
        size_t i = 0;
        while (msg) {
            zm_proto_ext_set_int (msg, "_seq", i++);
            zm_proto_send_mlm (
                msg,
                self->client,
                subject);
            msg = zm_devices_next (self->devices);
        }
        return;
    }
    else
        zm_proto_encode_error (self->msg, 403, "Subject not found");

send:
    zm_proto_sendto (
        msg,
        self->client,
        mlm_client_sender (self->client),
        "LOOKUP");
}

static void
zm_device_recv_mlm_stream (zm_device_t *self)
{
    assert (self);

    if (zm_proto_id (self->msg) != ZM_PROTO_DEVICE) {
        if (self->verbose)
            zsys_warning ("message from sender=%s, with subject=%s os not DEVICE",
            mlm_client_sender (self->client),
            mlm_client_subject (self->client));
        return;
    }
}

static void
zm_device_recv_mlm (zm_device_t *self)
{
    assert (self);
    zmsg_t *request = mlm_client_recv (self->client);
    int r = zm_proto_recv (self->msg, request);
    zmsg_destroy (&request);
    if (r != 0) {
        if (self->verbose)
            zsys_warning ("can't read message from sender=%s, with subject=%s",
            mlm_client_sender (self->client),
            mlm_client_subject (self->client));
        return;
    }

    if (streq (mlm_client_command (self->client), "MAILBOX DELIVER"))
        zm_device_recv_mlm_mailbox (self);
    else
    if (streq (mlm_client_command (self->client), "STREAM DELIVER"))
        zm_device_recv_mlm_stream (self);
}

//  --------------------------------------------------------------------------
//  This is the actor which runs in its own thread.

void
zm_device_actor (zsock_t *pipe, void *args)
{
    zm_device_t * self = zm_device_new (pipe, args);
    if (!self)
        return;          //  Interrupted

    //  Signal actor successfully initiated
    zsock_signal (self->pipe, 0);

    while (!self->terminated) {
        zsock_t *which = (zsock_t *) zpoller_wait (self->poller, 0);

        if (which == self->pipe)
            zm_device_recv_api (self);
        else
        if (which == mlm_client_msgpipe (self->client))
            zm_device_recv_mlm (self);
       //  Add other sockets when you need them.
    }
    zm_device_destroy (&self);
}

//  --------------------------------------------------------------------------
//  Self test of this actor.

void
zm_device_test (bool verbose)
{
    printf (" * zm_device: ");
    //  @selftest
    //  Simple create/destroy test
    // actor test

    static const char* endpoint = "inproc://zm-device-test";
    zactor_t *server = zactor_new (mlm_server, "Malamute");
    if (verbose)
        zstr_sendx (server, "VERBOSE", NULL);
    zstr_sendx (server, "BIND", endpoint, NULL);

    zactor_t *zm_device = zactor_new (zm_device_actor, NULL);
    zstr_sendx (zm_device, "CONFIG",
        "malamute\n"
        "    endpoint = inproc://zm-device-test\n"
        "    address = it.zmon.device\n"
        "    consumer\n"
        "        " ZM_PROTO_DEVICE_STREAM " = .*\n"
        "    producer = " ZM_PROTO_DEVICE_STREAM "\n",
        NULL);
    zstr_sendx (zm_device, "START", NULL);

    int r;
    mlm_client_t *reader = mlm_client_new ();
    assert (reader);
    r = mlm_client_connect (reader, endpoint, 1000, "reader");
    assert (r == 0);
    mlm_client_set_consumer (reader, ZM_PROTO_DEVICE_STREAM, ".*");

    mlm_client_t *writer = mlm_client_new ();
    assert (writer);
    r = mlm_client_connect (writer, endpoint, 1000, "writer");
    assert (r == 0);
    mlm_client_set_producer (writer, ZM_PROTO_DEVICE_STREAM);

    zmsg_t *request = zm_proto_encode_device_v1 ("device1", zclock_mono (), 1024, NULL);
    zmsg_t *zreply;
    zm_proto_t *reply = zm_proto_new ();

    mlm_client_sendto (writer, "it.zmon.device", "INSERT", NULL, 1000, &request);
    zreply = mlm_client_recv (writer);
    zm_proto_recv (reply, zreply);
    zmsg_destroy (&zreply);

    request = zm_proto_encode_device_v1 ("device1", 0, 0, NULL);
    mlm_client_sendto (writer, "it.zmon.device", "LOOKUP", NULL, 1000, &request);
    zreply = mlm_client_recv (writer);
    zm_proto_recv (reply, zreply);
    zmsg_destroy (&zreply);

    assert (zm_proto_id (reply) == ZM_PROTO_DEVICE);
    assert (streq (zm_proto_device (reply), "device1"));
    
    zm_proto_encode_ok (reply);
    zm_proto_sendto (reply, writer, "it.zmon.device", "GET-ALL");

    zm_proto_recv_mlm (reply, writer);
    assert (zm_proto_ext_int (reply, "_seq", -1) == 0);
    assert (zm_proto_ext_int (reply, "_cnt", -1) == 1);

    zreply = mlm_client_recv (reader);
    zm_proto_recv (reply, zreply);
    zmsg_destroy (&zreply);
    assert (streq (mlm_client_subject (reader), "INSERT"));
    assert (streq (zm_proto_device (reply), "device1"));

    zm_proto_encode_ok (reply);
    zm_proto_sendto (reply, writer, "it.zmon.device", "PUBLISH-ALL");

    zm_proto_recv_mlm (reply, reader);
    assert (zm_proto_ext_int (reply, "_seq", -1) == 0);
    assert (zm_proto_ext_int (reply, "_cnt", -1) == 1);

    zm_proto_destroy (&reply);
    
    mlm_client_destroy (&writer);
    mlm_client_destroy (&reader);
    zstr_sendx (zm_device, "STOP", NULL);
    zactor_destroy (&zm_device);
    zactor_destroy (&server);
    //  @end

    printf ("OK\n");
}
