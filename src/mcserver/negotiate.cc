/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2014 Couchbase, Inc.
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

#include <algorithm>
#include <string>
#include <sstream>
#include <vector>

#include "packetutils.h"
#include "mcserver.h"
#include "logging.h"
#include "settings.h"
#include <lcbio/lcbio.h>
#include <lcbio/timer-ng.h>
#include <lcbio/ssl.h>
#include <cbsasl/cbsasl.h>
#include "negotiate.h"
#include "ctx-log-inl.h"
#include "auth-priv.h"

using namespace lcb;

#define LOGARGS(ctx, lvl) ctx->settings, "negotiation", LCB_LOG_##lvl, __FILE__, __LINE__
static void cleanup_negotiated(SessionInfo* info);
static void handle_ioerr(lcbio_CTX *ctx, lcb_error_t err);
#define SESSREQ_LOGFMT "<%s:%s> (SASLREQ=%p) "


static void timeout_handler(void *arg);

#define SESSREQ_LOGID(s) get_ctx_host(s->ctx), get_ctx_port(s->ctx), (void*)s

static void
close_cb(lcbio_SOCKET *s, int reusable, void *arg)
{
    *(lcbio_SOCKET **)arg = s;
    lcbio_ref(s);
    lcb_assert(reusable);
}

/**
 * Structure used only for initialization. This is only used for the duration
 * of the request for negotiation and is deleted once negotiation has
 * completed (or failed).
 */
class lcb::SessionRequestImpl : public SessionRequest {
public:
    static SessionRequestImpl *get(void *arg) {
        return reinterpret_cast<SessionRequestImpl*>(arg);
    }

    bool setup(const lcbio_NAMEINFO& nistrs, const lcb_host_t& host,
        const lcb::Authenticator& auth);
    void start(lcbio_SOCKET *sock);
    void send_list_mechs();
    bool send_hello();
    bool send_step(const lcb::MemcachedResponse& packet);
    bool read_hello(const lcb::MemcachedResponse& packet);
    void send_auth(const char *sasl_data, unsigned ndata);
    void handle_read(lcbio_CTX *ioctx);
    bool maybe_select_bucket();

    enum MechStatus { MECH_UNAVAILABLE, MECH_NOT_NEEDED, MECH_OK };
    MechStatus set_chosen_mech(std::string& mechlist, const char **data, unsigned int *ndata);
    bool request_errmap();
    bool update_errmap(const lcb::MemcachedResponse& packet);

    SessionRequestImpl(lcbio_CONNDONE_cb callback, void *data, uint32_t timeout, lcbio_TABLE *iot, lcb_settings* settings_)
        : ctx(NULL), cb(callback), cbdata(data),
          timer(lcbio_timer_new(iot, this, timeout_handler)),
          last_err(LCB_SUCCESS), info(NULL),
          settings(settings_) {

        if (timeout) {
            lcbio_timer_rearm(timer, timeout);
        }
        memset(&u_auth, 0, sizeof(u_auth));
    }

    virtual ~SessionRequestImpl();

    void cancel() {
        cb = NULL;
        delete this;
    }

    void fail() {
        if (cb != NULL) {
            cb(NULL, cbdata, last_err, 0);
            cb = NULL;
        }
        delete this;
    }

    void fail(lcb_error_t error, const char *msg) {
        set_error(error, msg);
        fail();
    }

    void success() {
        /** Dislodge the connection, and return it back to the caller */
        lcbio_SOCKET *s;

        lcbio_ctx_close(ctx, close_cb, &s);
        ctx = NULL;

        lcbio_protoctx_add(s, info);
        info = NULL;

        /** Invoke the callback, marking it a success */
        cb(s, cbdata, LCB_SUCCESS, 0);
        lcbio_unref(s);

        delete this;
    }

    void set_error(lcb_error_t error, const char *msg, const lcb::MemcachedResponse *packet = NULL) {
        char *err_ref = NULL, *err_ctx = NULL;
        if (packet) {
            MemcachedResponse::parse_enhanced_error(packet->value(), packet->vallen(), &err_ref, &err_ctx);
        }
        if (err_ref || err_ctx) {
            std::stringstream emsg;
            if (err_ref) {
                emsg << "ref: \"" << err_ref << "\"";
            }
            if (err_ctx) {
                if (err_ref) {
                    emsg << ", ";
                }
                emsg << "context: \"" << err_ctx << "\"";
            }
            lcb_log(LOGARGS(this, ERR), SESSREQ_LOGFMT "Error: 0x%x, %s (%s)",
                    SESSREQ_LOGID(this), error, msg, emsg.str().c_str());
            free(err_ref);
            free(err_ctx);
        } else {
            lcb_log(LOGARGS(this, ERR), SESSREQ_LOGFMT "Error: 0x%x, %s", SESSREQ_LOGID(this), error, msg);
        }
        if (last_err == LCB_SUCCESS) {
            last_err = error;
        }
    }

    bool has_error() const {
        return last_err != LCB_SUCCESS;
    }

    union {
        cbsasl_secret_t secret;
        char buffer[256];
    } u_auth;
    std::string username;

    lcbio_CTX *ctx;
    lcbio_CONNDONE_cb cb;
    void *cbdata;
    lcbio_pTIMER timer;
    lcb_error_t last_err;
    cbsasl_conn_t *sasl_client;
    SessionInfo* info;
    lcb_settings *settings;
};

static void handle_read(lcbio_CTX *ioctx, unsigned) {
    SessionRequestImpl::get(lcbio_ctx_data(ioctx))->handle_read(ioctx);
}

static int
sasl_get_username(void *context, int id, const char **result, unsigned int *len)
{
    SessionRequestImpl *ctx = SessionRequestImpl::get(context);
    if (!context || !result || (id != CBSASL_CB_USER && id != CBSASL_CB_AUTHNAME)) {
        return SASL_BADPARAM;
    }

    *result = ctx->username.c_str();
    *len = ctx->username.size();

    return SASL_OK;
}

static int
sasl_get_password(cbsasl_conn_t *conn, void *context, int id,
                  cbsasl_secret_t **psecret)
{
    SessionRequestImpl *ctx = SessionRequestImpl::get(context);
    if (!conn || ! psecret || id != CBSASL_CB_PASS || ctx == NULL) {
        return SASL_BADPARAM;
    }

    *psecret = &ctx->u_auth.secret;
    return SASL_OK;
}

SessionInfo::SessionInfo()
{
    lcbio_PROTOCTX::id = LCBIO_PROTOCTX_SESSINFO;
    lcbio_PROTOCTX::dtor = (void (*)(lcbio_PROTOCTX *))cleanup_negotiated;
}

bool
SessionRequestImpl::setup(const lcbio_NAMEINFO& nistrs, const lcb_host_t& host,
    const lcb::Authenticator& auth)
{
    cbsasl_callback_t sasl_callbacks[4];
    sasl_callbacks[0].id = CBSASL_CB_USER;
    sasl_callbacks[0].proc = (int( *)(void)) &sasl_get_username;

    sasl_callbacks[1].id = CBSASL_CB_AUTHNAME;
    sasl_callbacks[1].proc = (int( *)(void)) &sasl_get_username;

    sasl_callbacks[2].id = CBSASL_CB_PASS;
    sasl_callbacks[2].proc = (int( *)(void)) &sasl_get_password;

    sasl_callbacks[3].id = CBSASL_CB_LIST_END;
    sasl_callbacks[3].proc = NULL;
    sasl_callbacks[3].context = NULL;

    for (size_t ii = 0; ii < 3; ii++) {
        sasl_callbacks[ii].context = this;
    }

    // Get the credentials
    username = auth.username_for(settings->bucket);
    const std::string& pass = auth.password_for(settings->bucket);

    if (!pass.empty()) {
        size_t maxlen = sizeof(u_auth.buffer) - offsetof(cbsasl_secret_t, data);
        u_auth.secret.len = pass.size();

        if (pass.size() < maxlen) {
            memcpy(u_auth.secret.data, pass.c_str(), pass.size());
        } else {
            return false;
        }
    }

    cbsasl_error_t saslerr = cbsasl_client_new(
            "couchbase", host.host, nistrs.local, nistrs.remote,
            sasl_callbacks, 0, &sasl_client);
    return saslerr == SASL_OK;
}

static void
timeout_handler(void *arg)
{
    SessionRequestImpl *sreq = SessionRequestImpl::get(arg);
    sreq->fail(LCB_ETIMEDOUT, "Negotiation timed out");
}

/**
 * Called to retrive the mechlist from the packet.
 * @return 0 to continue authentication, 1 if no authentication needed, or
 * -1 on error.
 */
SessionRequestImpl::MechStatus
SessionRequestImpl::set_chosen_mech(std::string& mechlist,
    const char **data, unsigned int *ndata)
{
    cbsasl_error_t saslerr;
    if (settings->sasl_mech_force) {
        char *forcemech = settings->sasl_mech_force;
        if (mechlist.find(forcemech) == std::string::npos) {
            /** Requested mechanism not found */
            set_error(LCB_SASLMECH_UNAVAILABLE, mechlist.c_str());
            return MECH_UNAVAILABLE;
        }
        mechlist.assign(forcemech);
    }

    const char *chosenmech;
    saslerr = cbsasl_client_start(sasl_client, mechlist.c_str(),
                                  NULL, data, ndata, &chosenmech);
    switch (saslerr) {
    case SASL_OK:
        info->mech.assign(chosenmech);
        return MECH_OK;
    case SASL_NOMECH:
        lcb_log(LOGARGS(this, INFO), SESSREQ_LOGFMT "Server does not support SASL (no mechanisms supported)", SESSREQ_LOGID(this));
        return MECH_NOT_NEEDED;
        break;
    default:
        lcb_log(LOGARGS(this, INFO), SESSREQ_LOGFMT "cbsasl_client_start returned %d", SESSREQ_LOGID(this), saslerr);
        set_error(LCB_EINTERNAL, "Couldn't start SASL client");
        return MECH_UNAVAILABLE;
    }
}

/**
 * Given the specific mechanisms, send the auth packet to the server.
 */
void
SessionRequestImpl::send_auth(const char *sasl_data, unsigned ndata)
{
    lcb::MemcachedRequest hdr(PROTOCOL_BINARY_CMD_SASL_AUTH);
    hdr.sizes(0, info->mech.size(), ndata);

    lcbio_ctx_put(ctx, hdr.data(), hdr.size());
    lcbio_ctx_put(ctx, info->mech.c_str(), info->mech.size());
    lcbio_ctx_put(ctx, sasl_data, ndata);
    lcbio_ctx_rwant(ctx, 24);
}

bool
SessionRequestImpl::send_step(const lcb::MemcachedResponse& packet)
{
    cbsasl_error_t saslerr;
    const char *step_data;
    unsigned int ndata;

    saslerr = cbsasl_client_step(sasl_client,
        packet.body<const char*>(), packet.bodylen(), NULL, &step_data, &ndata);

    if (saslerr != SASL_CONTINUE) {
        set_error(LCB_EINTERNAL, "Unable to perform SASL STEP");
        return false;
    }

    lcb::MemcachedRequest hdr(PROTOCOL_BINARY_CMD_SASL_STEP);
    hdr.sizes(0, info->mech.size(), ndata);
    lcbio_ctx_put(ctx, hdr.data(), hdr.size());
    lcbio_ctx_put(ctx, info->mech.c_str(), info->mech.size());
    lcbio_ctx_put(ctx, step_data, ndata);
    lcbio_ctx_rwant(ctx, 24);
    return true;
}

#define LCB_HELLO_DEFL_STRING "libcouchbase/" LCB_VERSION_STRING
#define LCB_HELLO_DEFL_LENGTH (sizeof(LCB_HELLO_DEFL_STRING)-1)

bool
SessionRequestImpl::send_hello()
{
    lcb_U16 features[MEMCACHED_TOTAL_HELLO_FEATURES];

    unsigned nfeatures = 0;
    features[nfeatures++] = PROTOCOL_BINARY_FEATURE_TLS;
    features[nfeatures++] = PROTOCOL_BINARY_FEATURE_XATTR;
    features[nfeatures++] = PROTOCOL_BINARY_FEATURE_SELECT_BUCKET;
    if (settings->use_errmap) {
        features[nfeatures++] = PROTOCOL_BINARY_FEATURE_XERROR;
    }
    if (settings->tcp_nodelay) {
        features[nfeatures++] = PROTOCOL_BINARY_FEATURE_TCPNODELAY;
    }

#ifndef LCB_NO_SNAPPY
    if (settings->compressopts != LCB_COMPRESS_NONE) {
        features[nfeatures++] = PROTOCOL_BINARY_FEATURE_DATATYPE;
    }
#endif

    if (settings->fetch_mutation_tokens) {
        features[nfeatures++] = PROTOCOL_BINARY_FEATURE_MUTATION_SEQNO;
    }

    std::string client_string;
    const char *clistr = LCB_HELLO_DEFL_STRING;
    size_t nclistr = LCB_HELLO_DEFL_LENGTH;

    if (settings->client_string) {
        client_string.assign(LCB_HELLO_DEFL_STRING);
        client_string += ", ";
        client_string += settings->client_string;

        clistr = client_string.c_str();
        nclistr = client_string.size();
    }

    lcb::MemcachedRequest hdr(PROTOCOL_BINARY_CMD_HELLO);
    hdr.sizes(0, nclistr, (sizeof features[0]) * nfeatures);

    lcbio_ctx_put(ctx, hdr.data(), hdr.size());
    lcbio_ctx_put(ctx, clistr, nclistr);
    for (size_t ii = 0; ii < nfeatures; ii++) {
        lcb_U16 tmp = htons(features[ii]);
        lcbio_ctx_put(ctx, &tmp, sizeof tmp);
    }

    lcbio_ctx_rwant(ctx, 24);
    return true;
}

void
SessionRequestImpl::send_list_mechs()
{
    lcb::MemcachedRequest req(PROTOCOL_BINARY_CMD_SASL_LIST_MECHS);
    lcbio_ctx_put(ctx, req.data(), req.size());
    LCBIO_CTX_RSCHEDULE(ctx, 24);
}

bool
SessionRequestImpl::read_hello(const lcb::MemcachedResponse& resp)
{
    /* some caps */
    const char *cur;
    const char *payload = resp.body<const char*>();
    const char *limit = payload + resp.bodylen();
    for (cur = payload; cur < limit; cur += 2) {
        lcb_U16 tmp;
        memcpy(&tmp, cur, sizeof(tmp));
        tmp = ntohs(tmp);
        lcb_log(LOGARGS(this, DEBUG), SESSREQ_LOGFMT "Server supports feature: 0x%x (%s)", SESSREQ_LOGID(this), tmp, protocol_feature_2_text(tmp));
        info->server_features.push_back(tmp);
    }
    return true;
}

bool
SessionRequestImpl::request_errmap() {
    lcb::MemcachedRequest hdr(PROTOCOL_BINARY_CMD_GET_ERROR_MAP);
    uint16_t version = htons(1);
    hdr.sizes(0, 0, 2);
    const char *p = reinterpret_cast<const char *>(&version);

    lcbio_ctx_put(ctx, hdr.data(), hdr.size());
    lcbio_ctx_put(ctx, p, 2);
    lcbio_ctx_rwant(ctx, 24);
    return true;
}

bool
SessionRequestImpl::update_errmap(const lcb::MemcachedResponse& resp)
{
    // Get the error map object
    using lcb::errmap::ErrorMap;

    std::string errmsg;
    ErrorMap& mm = *settings->errmap;
    ErrorMap::ParseStatus status = mm.parse(
        resp.body<const char*>(), resp.bodylen(), errmsg);

    if (status != ErrorMap::UPDATED && status != ErrorMap::NOT_UPDATED) {
        errmsg = "Couldn't update error map: " + errmsg;
        set_error(LCB_PROTOCOL_ERROR, errmsg.c_str());
        return false;
    }

    return true;
}

// Returns true if sending the SELECT_BUCKET command, false otherwise.
bool
SessionRequestImpl::maybe_select_bucket() {

    // Only send a SELECT_BUCKET if we have the SELECT_BUCKET bit enabled.
    if (!info->has_feature(PROTOCOL_BINARY_FEATURE_SELECT_BUCKET)) {
        return false;
    }

    if (!settings->select_bucket) {
        lcb_log(LOGARGS(this, WARN), SESSREQ_LOGFMT "SELECT_BUCKET Disabled by application", SESSREQ_LOGID(this));
        return false;
    }

    // send the SELECT_BUCKET command:
    lcb_log(LOGARGS(this, INFO), SESSREQ_LOGFMT "Sending SELECT_BUCKET", SESSREQ_LOGID(this));
    lcb::MemcachedRequest req(PROTOCOL_BINARY_CMD_SELECT_BUCKET);
    req.sizes(0, strlen(settings->bucket), 0);
    lcbio_ctx_put(ctx, req.data(), req.size());
    lcbio_ctx_put(ctx, settings->bucket, strlen(settings->bucket));
    LCBIO_CTX_RSCHEDULE(ctx, 24);
    return true;
}

static bool isUnsupported(uint16_t status) {
    return status == PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED ||
            status == PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND ||
            status == PROTOCOL_BINARY_RESPONSE_EACCESS;
}

/**
 * It's assumed the server buffers will be reset upon close(), so we must make
 * sure to _not_ release the ringbuffer if that happens.
 */
void
SessionRequestImpl::handle_read(lcbio_CTX *ioctx)
{
    lcb::MemcachedResponse resp;
    unsigned required;
    bool completed = false;

    GT_NEXT_PACKET:

    if (!resp.load(ioctx, &required)) {
        LCBIO_CTX_RSCHEDULE(ioctx, required);
        return;
    }
    const uint16_t status = resp.status();

    switch (resp.opcode()) {
    case PROTOCOL_BINARY_CMD_SASL_LIST_MECHS: {
        const char *mechlist_data;
        unsigned int nmechlist_data;
        std::string mechs(resp.body<const char*>(), resp.bodylen());

        MechStatus mechrc = set_chosen_mech(mechs, &mechlist_data, &nmechlist_data);
        if (mechrc == MECH_OK) {
            send_auth(mechlist_data, nmechlist_data);
        } else if (mechrc == MECH_UNAVAILABLE) {
            // Do nothing - error already set
        } else {
            completed = !maybe_select_bucket();
        }
        break;
    }

    case PROTOCOL_BINARY_CMD_SASL_AUTH: {
        if (status == PROTOCOL_BINARY_RESPONSE_SUCCESS) {
            completed = !maybe_select_bucket();
            break;
        } else if (status == PROTOCOL_BINARY_RESPONSE_AUTH_CONTINUE) {
            send_step(resp);
        } else {
            set_error(LCB_AUTH_ERROR, "SASL AUTH failed", &resp);
            break;
        }
        break;
    }

    case PROTOCOL_BINARY_CMD_SASL_STEP: {
        if (status == PROTOCOL_BINARY_RESPONSE_SUCCESS) {
            completed = !maybe_select_bucket();
        } else {
            lcb_log(LOGARGS(this, WARN), SESSREQ_LOGFMT "SASL auth failed with STATUS=0x%x", SESSREQ_LOGID(this), status);
            set_error(LCB_AUTH_ERROR, "SASL Step failed", &resp);
        }
        break;
    }

    case PROTOCOL_BINARY_CMD_HELLO: {
        if (status == PROTOCOL_BINARY_RESPONSE_SUCCESS) {
            if (!read_hello(resp)) {
                set_error(LCB_PROTOCOL_ERROR, "Couldn't parse HELLO");
                break;
            }
        } else if (isUnsupported(status)) {
            lcb_log(LOGARGS(this, DEBUG), SESSREQ_LOGFMT "Server does not support HELLO", SESSREQ_LOGID(this));
        } else {
            lcb_log(LOGARGS(this, ERROR), SESSREQ_LOGFMT "Unexpected status 0x%x received for HELLO", SESSREQ_LOGID(this), status);
            set_error(LCB_PROTOCOL_ERROR, "Hello response unexpected", &resp);
            break;
        }

        if (info->has_feature(PROTOCOL_BINARY_FEATURE_XERROR)) {
            request_errmap();
        } else {
            lcb_log(LOGARGS(this, TRACE), SESSREQ_LOGFMT "GET_ERRORMAP unsupported/disabled", SESSREQ_LOGID(this));
        }

        // In any event, it's also time to send the LIST_MECHS request
        send_list_mechs();
        break;
    }

    case PROTOCOL_BINARY_CMD_GET_ERROR_MAP: {
        if (status == PROTOCOL_BINARY_RESPONSE_SUCCESS) {
            if (!update_errmap(resp)) {
            }
        } else if (isUnsupported(status)) {
            lcb_log(LOGARGS(this, DEBUG), SESSREQ_LOGFMT "Server does not support GET_ERRMAP (0x%x)", SESSREQ_LOGID(this), status);
        } else {
            lcb_log(LOGARGS(this, ERROR), SESSREQ_LOGFMT "Unexpected status 0x%x received for GET_ERRMAP", SESSREQ_LOGID(this), status);
            set_error(LCB_PROTOCOL_ERROR, "GET_ERRMAP response unexpected", &resp);
        }
        // Note, there is no explicit state transition here. LIST_MECHS is
        // pipelined after this request.
        break;
    }

    case PROTOCOL_BINARY_CMD_SELECT_BUCKET: {
        if (status == PROTOCOL_BINARY_RESPONSE_SUCCESS) {
            completed = true;
        } else if (status == PROTOCOL_BINARY_RESPONSE_EACCESS) {
            set_error(LCB_AUTH_ERROR, "Provided credentials not allowed for bucket", &resp);
        } else {
            lcb_log(LOGARGS(this, ERROR), SESSREQ_LOGFMT "Unexpected status 0x%x received for SELECT_BUCKET", SESSREQ_LOGID(this), status);
            set_error(LCB_PROTOCOL_ERROR, "Other auth error", &resp);
        }
        break;
    }

    default: {
        lcb_log(LOGARGS(this, ERROR), SESSREQ_LOGFMT "Received unknown response. OP=0x%x. RC=0x%x", SESSREQ_LOGID(this), resp.opcode(), resp.status());
        set_error(LCB_NOT_SUPPORTED, "Received unknown response", &resp);
        break;
    }
    }

    // We need to release the packet's buffers before actually destroying the
    // underlying socket and/or buffers!
    resp.release(ioctx);

    // Once there is no more any dependencies on the buffers, we can succeed
    // or fail the request, potentially destroying the underlying connection
    if (has_error()) {
        fail();
    } else if (completed) {
        success();
    } else {
        goto GT_NEXT_PACKET;
    }
}

static void
handle_ioerr(lcbio_CTX *ctx, lcb_error_t err)
{
    SessionRequestImpl* sreq = SessionRequestImpl::get(lcbio_ctx_data(ctx));
    sreq->fail(err, "IO Error");
}

static void cleanup_negotiated(SessionInfo* ctx) {
    delete ctx;
}

void
SessionRequestImpl::start(lcbio_SOCKET *sock) {
    info = new SessionInfo();

    lcb_error_t err = lcbio_sslify_if_needed(sock, settings);
    if (err != LCB_SUCCESS) {
        set_error(err, "Couldn't initialized SSL on socket");
        lcbio_async_signal(timer);
        return;
    }

    lcbio_CTXPROCS procs;
    procs.cb_err = ::handle_ioerr;
    procs.cb_read = ::handle_read;
    ctx = lcbio_ctx_new(sock, this, &procs);
    ctx->subsys = "sasl";

    const lcb_host_t *curhost = lcbio_get_host(sock);
    lcbio_NAMEINFO nistrs;
    lcbio_get_nameinfo(sock, &nistrs);

    if (!setup(nistrs, *curhost, *settings->auth)) {
        set_error(LCB_EINTERNAL, "Couldn't start SASL client");
        lcbio_async_signal(timer);
        return;
    }

    if (settings->send_hello) {
        send_hello();
    } else {
        lcb_log(LOGARGS(this, INFO), SESSREQ_LOGFMT "HELLO negotiation disabled by user", SESSREQ_LOGID(this));
        send_list_mechs();
    }
    LCBIO_CTX_RSCHEDULE(ctx, 24);
}

SessionRequestImpl::~SessionRequestImpl()
{
    if (info) {
        delete info;
    }
    if (timer) {
        lcbio_timer_destroy(timer);
    }
    if (ctx) {
        lcbio_ctx_close(ctx, NULL, NULL);
    }
    if (sasl_client) {
        cbsasl_dispose(&sasl_client);
    }
}

SessionRequest *
SessionRequest::start(lcbio_SOCKET *sock, lcb_settings_st *settings,
             uint32_t tmo, lcbio_CONNDONE_cb callback, void *data)
{
    SessionRequestImpl* sreq = new SessionRequestImpl(callback, data, tmo, sock->io, settings);
    sreq->start(sock);
    return sreq;
}

SessionInfo*
SessionInfo::get(lcbio_SOCKET *sock) {
    return static_cast<SessionInfo*>(lcbio_protoctx_get(sock, LCBIO_PROTOCTX_SESSINFO));
}

bool
SessionInfo::has_feature(uint16_t feature) const {
    return std::find(server_features.begin(), server_features.end(), feature)
        != server_features.end();
}
