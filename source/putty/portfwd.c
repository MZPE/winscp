/*
 * SSH port forwarding.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "putty.h"
#include "ssh.h"
#include "sshchan.h"

static void logeventf(Frontend *frontend, const char *fmt, ...)
{
    va_list ap;
    char *buf;

    va_start(ap, fmt);
    buf = dupvprintf(fmt, ap);
    va_end(ap);
    logevent(frontend, buf);
    sfree(buf);
}

/*
 * Enumeration of values that live in the 'socks_state' field of
 * struct PortForwarding.
 */
typedef enum {
    SOCKS_NONE, /* direct connection (no SOCKS, or SOCKS already done) */
    SOCKS_INITIAL,       /* don't know if we're SOCKS 4 or 5 yet */
    SOCKS_4,             /* expect a SOCKS 4 (or 4A) connection message */
    SOCKS_5_INITIAL,     /* expect a SOCKS 5 preliminary message */
    SOCKS_5_CONNECT      /* expect a SOCKS 5 connection message */
} SocksState;

typedef struct PortForwarding {
    SshChannel *c;         /* channel structure held by SSH connection layer */
    ConnectionLayer *cl;   /* the connection layer itself */
    /* Note that ssh need not be filled in if c is non-NULL */
    Socket s;
    int input_wanted;
    int ready;
    SocksState socks_state;
    /*
     * `hostname' and `port' are the real hostname and port, once
     * we know what we're connecting to.
     */
    char *hostname;
    int port;
    /*
     * `socksbuf' is the buffer we use to accumulate the initial SOCKS
     * segment of the incoming data, plus anything after that that we
     * receive before we're ready to send data to the SSH server.
     */
    strbuf *socksbuf;
    size_t socksbuf_consumed;

    const Plug_vtable *plugvt;
    Channel chan;
} PortForwarding;

struct PortListener {
    ConnectionLayer *cl;
    Socket s;
    int is_dynamic;
    /*
     * `hostname' and `port' are the real hostname and port, for
     * ordinary forwardings.
     */
    char *hostname;
    int port;

    const Plug_vtable *plugvt;
};

static struct PortForwarding *new_portfwd_state(void)
{
    struct PortForwarding *pf = snew(struct PortForwarding);
    pf->hostname = NULL;
    pf->socksbuf = NULL;
    return pf;
}

static void free_portfwd_state(struct PortForwarding *pf)
{
    if (!pf)
        return;
    sfree(pf->hostname);
    if (pf->socksbuf)
        strbuf_free(pf->socksbuf);
    sfree(pf);
}

static struct PortListener *new_portlistener_state(void)
{
    struct PortListener *pl = snew(struct PortListener);
    pl->hostname = NULL;
    return pl;
}

static void free_portlistener_state(struct PortListener *pl)
{
    if (!pl)
        return;
    sfree(pl->hostname);
    sfree(pl);
}

static void pfd_log(Plug plug, int type, SockAddr addr, int port,
		    const char *error_msg, int error_code)
{
    /* we have to dump these since we have no interface to logging.c */
}

static void pfl_log(Plug plug, int type, SockAddr addr, int port,
		    const char *error_msg, int error_code)
{
    /* we have to dump these since we have no interface to logging.c */
}

static void pfd_close(struct PortForwarding *pf);

static void pfd_closing(Plug plug, const char *error_msg, int error_code,
			int calling_back)
{
    struct PortForwarding *pf = FROMFIELD(plug, struct PortForwarding, plugvt);

    if (error_msg) {
        /*
         * Socket error. Slam the connection instantly shut.
         */
        if (pf->c) {
            sshfwd_unclean_close(pf->c, error_msg);
        } else {
            /*
             * We might not have an SSH channel, if a socket error
             * occurred during SOCKS negotiation. If not, we must
             * clean ourself up without sshfwd_unclean_close's call
             * back to pfd_close.
             */
            pfd_close(pf);
        }
    } else {
        /*
         * Ordinary EOF received on socket. Send an EOF on the SSH
         * channel.
         */
        if (pf->c)
            sshfwd_write_eof(pf->c);
    }
}

static void pfl_terminate(struct PortListener *pl);

static void pfl_closing(Plug plug, const char *error_msg, int error_code,
			int calling_back)
{
    struct PortListener *pl = (struct PortListener *) plug;
    pfl_terminate(pl);
}

static SshChannel *wrap_lportfwd_open(
    ConnectionLayer *cl, const char *hostname, int port,
    Socket s, Channel *chan)
{
    char *peerinfo, *description;
    SshChannel *toret;

    peerinfo = sk_peer_info(s);
    if (peerinfo) {
        description = dupprintf("forwarding from %s", peerinfo);
        sfree(peerinfo);
    } else {
        description = dupstr("forwarding");
    }

    toret = ssh_lportfwd_open(cl, hostname, port, description, chan);

    sfree(description);
    return toret;
}

static char *ipv4_to_string(unsigned ipv4)
{
    return dupprintf("%u.%u.%u.%u",
                     (ipv4 >> 24) & 0xFF, (ipv4 >> 16) & 0xFF,
                     (ipv4 >>  8) & 0xFF, (ipv4      ) & 0xFF);
}

static char *ipv6_to_string(ptrlen ipv6)
{
    const unsigned char *addr = ipv6.ptr;
    assert(ipv6.len == 16);
    return dupprintf("%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
                     (unsigned)GET_16BIT_MSB_FIRST(addr + 0),
                     (unsigned)GET_16BIT_MSB_FIRST(addr + 2),
                     (unsigned)GET_16BIT_MSB_FIRST(addr + 4),
                     (unsigned)GET_16BIT_MSB_FIRST(addr + 6),
                     (unsigned)GET_16BIT_MSB_FIRST(addr + 8),
                     (unsigned)GET_16BIT_MSB_FIRST(addr + 10),
                     (unsigned)GET_16BIT_MSB_FIRST(addr + 12),
                     (unsigned)GET_16BIT_MSB_FIRST(addr + 14));
}

static void pfd_receive(Plug plug, int urgent, char *data, int len)
{
    struct PortForwarding *pf = FROMFIELD(plug, struct PortForwarding, plugvt);

    if (len == 0)
        return;

    if (pf->socks_state != SOCKS_NONE) {
        BinarySource src[1];

        /*
         * Store all the data we've got in socksbuf.
         */
        put_data(pf->socksbuf, data, len);

        /*
         * Check the start of socksbuf to see if it's a valid and
         * complete message in the SOCKS exchange.
         */

        if (pf->socks_state == SOCKS_INITIAL) {
            /* Preliminary: check the first byte of the data (which we
             * _must_ have by now) to find out which SOCKS major
             * version we're speaking. */
            switch (pf->socksbuf->u[0]) {
              case 4:
                pf->socks_state = SOCKS_4;
                break;
              case 5:
                pf->socks_state = SOCKS_5_INITIAL;
                break;
              default:
                pfd_close(pf);         /* unrecognised version */
                return;
            }
        }

        BinarySource_BARE_INIT(src, pf->socksbuf->u, pf->socksbuf->len);
        get_data(src, pf->socksbuf_consumed);

        while (pf->socks_state != SOCKS_NONE) {
            unsigned socks_version, message_type, reserved_byte;
            unsigned reply_code, port, ipv4, method;
            ptrlen methods;
            const char *socks4_hostname;
            strbuf *output;

            switch (pf->socks_state) {
              case SOCKS_INITIAL:
              case SOCKS_NONE:
                assert(0 && "These case values cannot appear");

              case SOCKS_4:
                /* SOCKS 4/4A connect message */
                socks_version = get_byte(src);
                message_type = get_byte(src);

                if (get_err(src) == BSE_OUT_OF_DATA)
                    return;
                if (socks_version == 4 && message_type == 1) {
                    /* CONNECT message */
                    int name_based = FALSE;

                    port = get_uint16(src);
                    ipv4 = get_uint32(src);
                    if (ipv4 > 0x00000000 && ipv4 < 0x00000100) {
                        /*
                         * Addresses in this range indicate the SOCKS 4A
                         * extension to specify a hostname, which comes
                         * after the username.
                         */
                        name_based = TRUE;
                    }
                    get_asciz(src);        /* skip username */
                    socks4_hostname = name_based ? get_asciz(src) : NULL;

                    if (get_err(src) == BSE_OUT_OF_DATA)
                        return;
                    if (get_err(src))
                        goto socks4_reject;

                    pf->port = port;
                    if (name_based) {
                        pf->hostname = dupstr(socks4_hostname);
                    } else {
                        pf->hostname = ipv4_to_string(ipv4);
                    }

                    output = strbuf_new();
                    put_byte(output, 0);       /* reply version */
                    put_byte(output, 90);      /* SOCKS 4 'request granted' */
                    put_uint16(output, 0);     /* null port field */
                    put_uint32(output, 0);     /* null address field */
                    sk_write(pf->s, output->u, output->len);
                    strbuf_free(output);

                    pf->socks_state = SOCKS_NONE;
                    pf->socksbuf_consumed = src->pos;
                    break;
                }

              socks4_reject:
                output = strbuf_new();
                put_byte(output, 0);       /* reply version */
                put_byte(output, 91);      /* SOCKS 4 'request rejected' */
                put_uint16(output, 0);     /* null port field */
                put_uint32(output, 0);     /* null address field */
                sk_write(pf->s, output->u, output->len);
                strbuf_free(output);
                pfd_close(pf);
                return;

              case SOCKS_5_INITIAL:
                /* SOCKS 5 initial method list */
                socks_version = get_byte(src);
                methods = get_pstring(src);

                method = 0xFF;         /* means 'no usable method found' */
                {
                    int i;
                    for (i = 0; i < methods.len; i++) {
                        if (((const unsigned char *)methods.ptr)[i] == 0 ) {
                            method = 0;        /* no auth */
                            break;
                        }
                    }
                }

                if (get_err(src) == BSE_OUT_OF_DATA)
                    return;
                if (get_err(src))
                    method = 0xFF;

                output = strbuf_new();
                put_byte(output, 5);       /* SOCKS version */
                put_byte(output, method);  /* selected auth method */
                sk_write(pf->s, output->u, output->len);
                strbuf_free(output);

                if (method == 0xFF) {
                    pfd_close(pf);
                    return;
                }

                pf->socks_state = SOCKS_5_CONNECT;
                pf->socksbuf_consumed = src->pos;
                break;

              case SOCKS_5_CONNECT:
                /* SOCKS 5 connect message */
                socks_version = get_byte(src);
                message_type = get_byte(src);
                reserved_byte = get_byte(src);

                if (socks_version == 5 && message_type == 1 &&
                    reserved_byte == 0) {

                    reply_code = 0;        /* success */

                    switch (get_byte(src)) {
                      case 1:              /* IPv4 */
                        pf->hostname = ipv4_to_string(get_uint32(src));
                        break;
                      case 4:              /* IPv6 */
                        pf->hostname = ipv6_to_string(get_data(src, 16));
                        break;
                      case 3:              /* unresolved domain name */
                        pf->hostname = mkstr(get_pstring(src));
                        break;
                      default:
                        pf->hostname = NULL;
                        reply_code = 8;    /* address type not supported */
                        break;
                    }

                    pf->port = get_uint16(src);
                } else {
                    reply_code = 7;        /* command not supported */
                }

                if (get_err(src) == BSE_OUT_OF_DATA)
                    return;
                if (get_err(src))
                    reply_code = 1;        /* general server failure */

                output = strbuf_new();
                put_byte(output, 5);       /* SOCKS version */
                put_byte(output, reply_code);
                put_byte(output, 0);       /* reserved */
                put_byte(output, 1);       /* IPv4 address follows */
                put_uint32(output, 0);     /* bound IPv4 address (unused) */
                put_uint16(output, 0);     /* bound port number (unused) */
                sk_write(pf->s, output->u, output->len);
                strbuf_free(output);

                if (reply_code != 0) {
                    pfd_close(pf);
                    return;
                }

                pf->socks_state = SOCKS_NONE;
                pf->socksbuf_consumed = src->pos;
                break;
            }
        }

	/*
	 * We come here when we're ready to make an actual
	 * connection.
	 */

	/*
	 * Freeze the socket until the SSH server confirms the
	 * connection.
	 */
	sk_set_frozen(pf->s, 1);

        pf->c = wrap_lportfwd_open(pf->cl, pf->hostname, pf->port, pf->s,
                                   &pf->chan);
    }
    if (pf->ready)
        sshfwd_write(pf->c, data, len);
}

static void pfd_sent(Plug plug, int bufsize)
{
    struct PortForwarding *pf = FROMFIELD(plug, struct PortForwarding, plugvt);

    if (pf->c)
	sshfwd_unthrottle(pf->c, bufsize);
}

static const Plug_vtable PortForwarding_plugvt = {
    pfd_log,
    pfd_closing,
    pfd_receive,
    pfd_sent,
    NULL
};

static void pfd_chan_free(Channel *chan);
static void pfd_open_confirmation(Channel *chan);
static void pfd_open_failure(Channel *chan, const char *errtext);
static int pfd_send(Channel *chan, int is_stderr, const void *data, int len);
static void pfd_send_eof(Channel *chan);
static void pfd_set_input_wanted(Channel *chan, int wanted);
static char *pfd_log_close_msg(Channel *chan);

static const struct ChannelVtable PortForwarding_channelvt = {
    pfd_chan_free,
    pfd_open_confirmation,
    pfd_open_failure,
    pfd_send,
    pfd_send_eof,
    pfd_set_input_wanted,
    pfd_log_close_msg,
    chan_no_eager_close,
};

/*
 called when someone connects to the local port
 */

static int pfl_accepting(Plug p, accept_fn_t constructor, accept_ctx_t ctx)
{
    struct PortForwarding *pf;
    struct PortListener *pl;
    Socket s;
    const char *err;

    pl = FROMFIELD(p, struct PortListener, plugvt);
    pf = new_portfwd_state();
    pf->plugvt = &PortForwarding_plugvt;
    pf->chan.initial_fixed_window_size = 0;
    pf->chan.vt = &PortForwarding_channelvt;
    pf->input_wanted = TRUE;

    pf->c = NULL;
    pf->cl = pl->cl;

    pf->s = s = constructor(ctx, &pf->plugvt);
    if ((err = sk_socket_error(s)) != NULL) {
	free_portfwd_state(pf);
	return err != NULL;
    }

    pf->input_wanted = TRUE;
    pf->ready = 0;

    if (pl->is_dynamic) {
	pf->socks_state = SOCKS_INITIAL;
        pf->socksbuf = strbuf_new();
        pf->socksbuf_consumed = 0;
	pf->port = 0;		       /* "hostname" buffer is so far empty */
	sk_set_frozen(s, 0);	       /* we want to receive SOCKS _now_! */
    } else {
	pf->socks_state = SOCKS_NONE;
	pf->hostname = dupstr(pl->hostname);
	pf->port = pl->port;	
        pf->c = wrap_lportfwd_open(pl->cl, pf->hostname, pf->port,
                                   s, &pf->chan);
    }

    return 0;
}

static const Plug_vtable PortListener_plugvt = {
    pfl_log,
    pfl_closing,
    NULL,                          /* recv */
    NULL,                          /* send */
    pfl_accepting
};

/*
 * Add a new port-forwarding listener from srcaddr:port -> desthost:destport.
 *
 * desthost == NULL indicates dynamic SOCKS port forwarding.
 *
 * On success, returns NULL and fills in *pl_ret. On error, returns a
 * dynamically allocated error message string.
 */
static char *pfl_listen(char *desthost, int destport, char *srcaddr,
                        int port, ConnectionLayer *cl, Conf *conf,
                        struct PortListener **pl_ret, int address_family)
{
    const char *err;
    struct PortListener *pl;

    /*
     * Open socket.
     */
    pl = *pl_ret = new_portlistener_state();
    pl->plugvt = &PortListener_plugvt;
    if (desthost) {
	pl->hostname = dupstr(desthost);
	pl->port = destport;
	pl->is_dynamic = FALSE;
    } else
	pl->is_dynamic = TRUE;
    pl->cl = cl;

    pl->s = new_listener(srcaddr, port, &pl->plugvt,
                         !conf_get_int(conf, CONF_lport_acceptall),
                         conf, address_family);
    if ((err = sk_socket_error(pl->s)) != NULL) {
        char *err_ret = dupstr(err);
        sk_close(pl->s);
	free_portlistener_state(pl);
        *pl_ret = NULL;
	return err_ret;
    }

    return NULL;
}

static char *pfd_log_close_msg(Channel *chan)
{
    return dupstr("Forwarded port closed");
}

static void pfd_close(struct PortForwarding *pf)
{
    if (!pf)
	return;

    sk_close(pf->s);
    free_portfwd_state(pf);
}

/*
 * Terminate a listener.
 */
static void pfl_terminate(struct PortListener *pl)
{
    if (!pl)
	return;

    sk_close(pl->s);
    free_portlistener_state(pl);
}

static void pfd_set_input_wanted(Channel *chan, int wanted)
{
    pinitassert(chan->vt == &PortForwarding_channelvt);
    PortForwarding *pf = FROMFIELD(chan, PortForwarding, chan);
    pf->input_wanted = wanted;
    sk_set_frozen(pf->s, !pf->input_wanted);
}

static void pfd_chan_free(Channel *chan)
{
    pinitassert(chan->vt == &PortForwarding_channelvt);
    PortForwarding *pf = FROMFIELD(chan, PortForwarding, chan);
    pfd_close(pf);
}

/*
 * Called to send data down the raw connection.
 */
static int pfd_send(Channel *chan, int is_stderr, const void *data, int len)
{
    pinitassert(chan->vt == &PortForwarding_channelvt);
    PortForwarding *pf = FROMFIELD(chan, PortForwarding, chan);
    return sk_write(pf->s, data, len);
}

static void pfd_send_eof(Channel *chan)
{
    pinitassert(chan->vt == &PortForwarding_channelvt);
    PortForwarding *pf = FROMFIELD(chan, PortForwarding, chan);
    sk_write_eof(pf->s);
}

static void pfd_open_confirmation(Channel *chan)
{
    pinitassert(chan->vt == &PortForwarding_channelvt);
    PortForwarding *pf = FROMFIELD(chan, PortForwarding, chan);

    pf->ready = 1;
    sk_set_frozen(pf->s, 0);
    sk_write(pf->s, NULL, 0);
    if (pf->socksbuf) {
	sshfwd_write(pf->c, pf->socksbuf->u + pf->socksbuf_consumed,
                     pf->socksbuf->len - pf->socksbuf_consumed);
        strbuf_free(pf->socksbuf);
        pf->socksbuf = NULL;
    }
}

static void pfd_open_failure(Channel *chan, const char *errtext)
{
    pinitassert(chan->vt == &PortForwarding_channelvt);
    PortForwarding *pf = FROMFIELD(chan, PortForwarding, chan);

    logeventf(pf->cl->frontend,
              "Forwarded connection refused by server%s%s",
              errtext ? ": " : "", errtext ? errtext : "");
}

/* ----------------------------------------------------------------------
 * Code to manage the complete set of currently active port
 * forwardings, and update it from Conf.
 */

struct PortFwdRecord {
    enum { DESTROY, KEEP, CREATE } status;
    int type;
    unsigned sport, dport;
    char *saddr, *daddr;
    char *sserv, *dserv;
    struct ssh_rportfwd *remote;
    int addressfamily;
    struct PortListener *local;
};

static int pfr_cmp(void *av, void *bv)
{
    PortFwdRecord *a = (PortFwdRecord *) av;
    PortFwdRecord *b = (PortFwdRecord *) bv;
    int i;
    if (a->type > b->type)
        return +1;
    if (a->type < b->type)
        return -1;
    if (a->addressfamily > b->addressfamily)
        return +1;
    if (a->addressfamily < b->addressfamily)
        return -1;
    if ( (i = nullstrcmp(a->saddr, b->saddr)) != 0)
        return i < 0 ? -1 : +1;
    if (a->sport > b->sport)
        return +1;
    if (a->sport < b->sport)
        return -1;
    if (a->type != 'D') {
        if ( (i = nullstrcmp(a->daddr, b->daddr)) != 0)
            return i < 0 ? -1 : +1;
        if (a->dport > b->dport)
            return +1;
        if (a->dport < b->dport)
            return -1;
    }
    return 0;
}

void pfr_free(PortFwdRecord *pfr)
{
    /* Dispose of any listening socket. */
    if (pfr->local)
        pfl_terminate(pfr->local);

    sfree(pfr->saddr);
    sfree(pfr->daddr);
    sfree(pfr->sserv);
    sfree(pfr->dserv);
    sfree(pfr);
}

struct PortFwdManager {
    ConnectionLayer *cl;
    Conf *conf;
    tree234 *forwardings;
};

PortFwdManager *portfwdmgr_new(ConnectionLayer *cl)
{
    PortFwdManager *mgr = snew(PortFwdManager);

    mgr->cl = cl;
    mgr->conf = NULL;
    mgr->forwardings = newtree234(pfr_cmp);

    return mgr;
}

void portfwdmgr_close(PortFwdManager *mgr, PortFwdRecord *pfr)
{
    PortFwdRecord *realpfr = del234(mgr->forwardings, pfr);
    if (realpfr == pfr)
        pfr_free(pfr);
}

void portfwdmgr_close_all(PortFwdManager *mgr)
{
    PortFwdRecord *pfr;

    while ((pfr = delpos234(mgr->forwardings, 0)) != NULL)
        pfr_free(pfr);
}

void portfwdmgr_free(PortFwdManager *mgr)
{
    portfwdmgr_close_all(mgr);
    freetree234(mgr->forwardings);
    if (mgr->conf)
        conf_free(mgr->conf);
    sfree(mgr);
}

void portfwdmgr_config(PortFwdManager *mgr, Conf *conf)
{
    PortFwdRecord *pfr;
    int i;
    char *key, *val;

    if (mgr->conf)
        conf_free(mgr->conf);
    mgr->conf = conf_copy(conf);

    /*
     * Go through the existing port forwardings and tag them
     * with status==DESTROY. Any that we want to keep will be
     * re-enabled (status==KEEP) as we go through the
     * configuration and find out which bits are the same as
     * they were before.
     */
    for (i = 0; (pfr = index234(mgr->forwardings, i)) != NULL; i++)
        pfr->status = DESTROY;

    for (val = conf_get_str_strs(conf, CONF_portfwd, NULL, &key);
         val != NULL;
         val = conf_get_str_strs(conf, CONF_portfwd, key, &key)) {
        char *kp, *kp2, *vp, *vp2;
        char address_family, type;
        int sport, dport, sserv, dserv;
        char *sports, *dports, *saddr, *host;

        kp = key;

        address_family = 'A';
        type = 'L';
        if (*kp == 'A' || *kp == '4' || *kp == '6')
            address_family = *kp++;
        if (*kp == 'L' || *kp == 'R')
            type = *kp++;

        if ((kp2 = host_strchr(kp, ':')) != NULL) {
            /*
             * There's a colon in the middle of the source port
             * string, which means that the part before it is
             * actually a source address.
             */
            char *saddr_tmp = dupprintf("%.*s", (int)(kp2 - kp), kp);
            saddr = host_strduptrim(saddr_tmp);
            sfree(saddr_tmp);
            sports = kp2+1;
        } else {
            saddr = NULL;
            sports = kp;
        }
        sport = atoi(sports);
        sserv = 0;
        if (sport == 0) {
            sserv = 1;
            sport = net_service_lookup(sports);
            if (!sport) {
                logeventf(mgr->cl->frontend, "Service lookup failed for source"
                          " port \"%s\"", sports);
            }
        }

        if (type == 'L' && !strcmp(val, "D")) {
            /* dynamic forwarding */
            host = NULL;
            dports = NULL;
            dport = -1;
            dserv = 0;
            type = 'D';
        } else {
            /* ordinary forwarding */
            vp = val;
            vp2 = vp + host_strcspn(vp, ":");
            host = dupprintf("%.*s", (int)(vp2 - vp), vp);
            if (*vp2)
                vp2++;
            dports = vp2;
            dport = atoi(dports);
            dserv = 0;
            if (dport == 0) {
                dserv = 1;
                dport = net_service_lookup(dports);
                if (!dport) {
                    logeventf(mgr->cl->frontend,
                              "Service lookup failed for destination"
                              " port \"%s\"", dports);
                }
            }
        }

        if (sport && dport) {
            /* Set up a description of the source port. */
            pfr = snew(PortFwdRecord);
            pfr->type = type;
            pfr->saddr = saddr;
            pfr->sserv = sserv ? dupstr(sports) : NULL;
            pfr->sport = sport;
            pfr->daddr = host;
            pfr->dserv = dserv ? dupstr(dports) : NULL;
            pfr->dport = dport;
            pfr->local = NULL;
            pfr->remote = NULL;
            pfr->addressfamily = (address_family == '4' ? ADDRTYPE_IPV4 :
                                  address_family == '6' ? ADDRTYPE_IPV6 :
                                  ADDRTYPE_UNSPEC);

            { // WINSCP
            PortFwdRecord *existing = add234(mgr->forwardings, pfr);
            if (existing != pfr) {
                if (existing->status == DESTROY) {
                    /*
                     * We already have a port forwarding up and running
                     * with precisely these parameters. Hence, no need
                     * to do anything; simply re-tag the existing one
                     * as KEEP.
                     */
                    existing->status = KEEP;
                }
                /*
                 * Anything else indicates that there was a duplicate
                 * in our input, which we'll silently ignore.
                 */
                pfr_free(pfr);
            } else {
                pfr->status = CREATE;
            }
            } // WINSCP
        } else {
            sfree(saddr);
            sfree(host);
        }
    }

    /*
     * Now go through and destroy any port forwardings which were
     * not re-enabled.
     */
    for (i = 0; (pfr = index234(mgr->forwardings, i)) != NULL; i++) {
        if (pfr->status == DESTROY) {
            char *message;

            message = dupprintf("%s port forwarding from %s%s%d",
                                pfr->type == 'L' ? "local" :
                                pfr->type == 'R' ? "remote" : "dynamic",
                                pfr->saddr ? pfr->saddr : "",
                                pfr->saddr ? ":" : "",
                                pfr->sport);

            if (pfr->type != 'D') {
                char *msg2 = dupprintf("%s to %s:%d", message,
                                       pfr->daddr, pfr->dport);
                sfree(message);
                message = msg2;
            }

            logeventf(mgr->cl->frontend, "Cancelling %s", message);
            sfree(message);

            /* pfr->remote or pfr->local may be NULL if setting up a
             * forwarding failed. */
            if (pfr->remote) {
                /*
                 * Cancel the port forwarding at the server
                 * end.
                 *
                 * Actually closing the listening port on the server
                 * side may fail - because in SSH-1 there's no message
                 * in the protocol to request it!
                 *
                 * Instead, we simply remove the record of the
                 * forwarding from our local end, so that any
                 * connections the server tries to make on it are
                 * rejected.
                 */
                ssh_rportfwd_remove(mgr->cl, pfr->remote);
            } else if (pfr->local) {
                pfl_terminate(pfr->local);
            }

            delpos234(mgr->forwardings, i);
            pfr_free(pfr);
            i--;                       /* so we don't skip one in the list */
        }
    }

    /*
     * And finally, set up any new port forwardings (status==CREATE).
     */
    for (i = 0; (pfr = index234(mgr->forwardings, i)) != NULL; i++) {
        if (pfr->status == CREATE) {
            char *sportdesc, *dportdesc;
            sportdesc = dupprintf("%s%s%s%s%d%s",
                                  pfr->saddr ? pfr->saddr : "",
                                  pfr->saddr ? ":" : "",
                                  pfr->sserv ? pfr->sserv : "",
                                  pfr->sserv ? "(" : "",
                                  pfr->sport,
                                  pfr->sserv ? ")" : "");
            if (pfr->type == 'D') {
                dportdesc = NULL;
            } else {
                dportdesc = dupprintf("%s:%s%s%d%s",
                                      pfr->daddr,
                                      pfr->dserv ? pfr->dserv : "",
                                      pfr->dserv ? "(" : "",
                                      pfr->dport,
                                      pfr->dserv ? ")" : "");
            }

            if (pfr->type == 'L') {
                char *err = pfl_listen(pfr->daddr, pfr->dport,
                                       pfr->saddr, pfr->sport,
                                       mgr->cl, conf, &pfr->local,
                                       pfr->addressfamily);

                logeventf(mgr->cl->frontend,
                          "Local %sport %s forwarding to %s%s%s",
                          pfr->addressfamily == ADDRTYPE_IPV4 ? "IPv4 " :
                          pfr->addressfamily == ADDRTYPE_IPV6 ? "IPv6 " : "",
                          sportdesc, dportdesc,
                          err ? " failed: " : "", err ? err : "");
                if (err)
                    sfree(err);
            } else if (pfr->type == 'D') {
                char *err = pfl_listen(NULL, -1, pfr->saddr, pfr->sport,
                                       mgr->cl, conf, &pfr->local,
                                       pfr->addressfamily);

                logeventf(mgr->cl->frontend,
                          "Local %sport %s SOCKS dynamic forwarding%s%s",
                          pfr->addressfamily == ADDRTYPE_IPV4 ? "IPv4 " :
                          pfr->addressfamily == ADDRTYPE_IPV6 ? "IPv6 " : "",
                          sportdesc,
                          err ? " failed: " : "", err ? err : "");

                if (err)
                    sfree(err);
            } else {
                const char *shost;

                if (pfr->saddr) {
                    shost = pfr->saddr;
                } else if (conf_get_int(conf, CONF_rport_acceptall)) {
                    shost = "";
                } else {
                    shost = "localhost";
                }

                pfr->remote = ssh_rportfwd_alloc(
                    mgr->cl, shost, pfr->sport, pfr->daddr, pfr->dport,
                    pfr->addressfamily, sportdesc, pfr, NULL);

                if (!pfr->remote) {
                    logeventf(mgr->cl->frontend,
                              "Duplicate remote port forwarding to %s:%d",
                              pfr->daddr, pfr->dport);
                    pfr_free(pfr);
                } else {
                    logeventf(mgr->cl->frontend, "Requesting remote port %s"
                              " forward to %s", sportdesc, dportdesc);
                }
            }
            sfree(sportdesc);
            sfree(dportdesc);
        }
    }
}

/*
 * Called when receiving a PORT OPEN from the server to make a
 * connection to a destination host.
 *
 * On success, returns NULL and fills in *pf_ret. On error, returns a
 * dynamically allocated error message string.
 */
char *portfwdmgr_connect(PortFwdManager *mgr, Channel **chan_ret,
                         char *hostname, int port, SshChannel *c,
                         int addressfamily)
{
    SockAddr addr;
    const char *err;
    char *dummy_realhost = NULL;
    struct PortForwarding *pf;

    /*
     * Try to find host.
     */
    addr = name_lookup(hostname, port, &dummy_realhost, mgr->conf,
                       addressfamily, NULL, NULL);
    if ((err = sk_addr_error(addr)) != NULL) {
        char *err_ret = dupstr(err);
        sk_addr_free(addr);
        sfree(dummy_realhost);
        return err_ret;
    }

    /*
     * Open socket.
     */
    pf = new_portfwd_state();
    *chan_ret = &pf->chan;
    pf->plugvt = &PortForwarding_plugvt;
    pf->chan.initial_fixed_window_size = 0;
    pf->chan.vt = &PortForwarding_channelvt;
    pf->input_wanted = TRUE;
    pf->ready = 1;
    pf->c = c;
    pf->cl = mgr->cl;
    pf->socks_state = SOCKS_NONE;

    pf->s = new_connection(addr, dummy_realhost, port,
                           0, 1, 0, 0, &pf->plugvt, mgr->conf);
    sfree(dummy_realhost);
    if ((err = sk_socket_error(pf->s)) != NULL) {
        char *err_ret = dupstr(err);
        sk_close(pf->s);
        free_portfwd_state(pf);
        *chan_ret = NULL;
        return err_ret;
    }

    return NULL;
}

#ifdef MPEXT

#include "puttyexp.h"

int is_pfwd(Plug plug)
{
  return
    ((*plug)->closing == pfd_closing) ||
    ((*plug)->closing == pfl_closing);
}

Frontend * get_pfwd_frontend(Plug plug)
{
  Ssh ssh = NULL;
  if ((*plug)->closing == pfl_closing)
  {
    struct PortListener *pl = FROMFIELD(plug, struct PortListener, plugvt);
    ssh = pl->cl->frontend;
  }
  else if ((*plug)->closing == pfd_closing)
  {
    struct PortForwarding *pf = FROMFIELD(plug, struct PortForwarding, plugvt);
    ssh = pf->cl->frontend;
  }
  return ssh;
}

#endif
