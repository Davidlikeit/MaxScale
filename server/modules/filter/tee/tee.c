/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl.
 *
 * Change Date: 2019-07-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

/**
 * @file tee.c  A filter that splits the processing pipeline in two
 * @verbatim
 *
 * Conditionally duplicate requests and send the duplicates to another service
 * within MaxScale.
 *
 * Parameters
 * ==========
 *
 * service  The service to send the duplicates to
 * source   The source address to match in order to duplicate (optional)
 * match    A regular expression to match in order to perform duplication
 *          of the request (optional)
 * nomatch  A regular expression to match in order to prevent duplication
 *          of the request (optional)
 * user     A user name to match against. If present only requests that
 *          originate from this user will be duplciated (optional)
 *
 * Revision History
 * ================
 *
 * Date         Who             Description
 * 20/06/2014   Mark Riddoch    Initial implementation
 * 24/06/2014   Mark Riddoch    Addition of support for multi-packet queries
 * 12/12/2014   Mark Riddoch    Add support for otehr packet types
 *
 * @endverbatim
 */

#include <stdio.h>
#include <fcntl.h>
#include <maxscale/filter.h>
#include <maxscale/modinfo.h>
#include <maxscale/modutil.h>
#include <maxscale/log_manager.h>
#include <sys/time.h>
#include <regex.h>
#include <string.h>
#include <maxscale/service.h>
#include <maxscale/router.h>
#include <maxscale/dcb.h>
#include <sys/time.h>
#include <maxscale/poll.h>
#include <maxscale/protocol/mysql.h>
#include <maxscale/housekeeper.h>
#include <maxscale/alloc.h>
#include <maxscale/listmanager.h>

#define MYSQL_COM_QUIT                  0x01
#define MYSQL_COM_INITDB                0x02
#define MYSQL_COM_FIELD_LIST            0x04
#define MYSQL_COM_CHANGE_USER           0x11
#define MYSQL_COM_STMT_PREPARE          0x16
#define MYSQL_COM_STMT_EXECUTE          0x17
#define MYSQL_COM_STMT_SEND_LONG_DATA   0x18
#define MYSQL_COM_STMT_CLOSE            0x19
#define MYSQL_COM_STMT_RESET            0x1a
#define MYSQL_COM_CONNECT               0x1b

#define REPLY_TIMEOUT_SECOND            5
#define REPLY_TIMEOUT_MILLISECOND       1
#define PARENT                          0
#define CHILD                           1

#ifdef SS_DEBUG
static int debug_seq = 0;
#endif

static unsigned char required_packets[] =
{
    MYSQL_COM_QUIT,
    MYSQL_COM_INITDB,
    MYSQL_COM_CHANGE_USER,
    MYSQL_COM_STMT_PREPARE,
    MYSQL_COM_STMT_EXECUTE,
    MYSQL_COM_STMT_SEND_LONG_DATA,
    MYSQL_COM_STMT_CLOSE,
    MYSQL_COM_STMT_RESET,
    MYSQL_COM_CONNECT,
    0
};

MODULE_INFO info =
{
    MODULE_API_FILTER,
    MODULE_GA,
    FILTER_VERSION,
    "A tee piece in the filter plumbing"
};

static char *version_str = "V1.0.0";

/*
 * The filter entry points
 */
static FILTER *createInstance(const char* name, char **options, FILTER_PARAMETER **);
static void *newSession(FILTER *instance, SESSION *session);
static void closeSession(FILTER *instance, void *session);
static void freeSession(FILTER *instance, void *session);
static void setDownstream(FILTER *instance, void *fsession, DOWNSTREAM *downstream);
static void setUpstream(FILTER *instance, void *fsession, UPSTREAM *upstream);
static int routeQuery(FILTER *instance, void *fsession, GWBUF *queue);
static int clientReply(FILTER *instance, void *fsession, GWBUF *queue);
static void diagnostic(FILTER *instance, void *fsession, DCB *dcb);
static uint64_t getCapabilities(void);

static FILTER_OBJECT MyObject =
{
    createInstance,
    newSession,
    closeSession,
    freeSession,
    setDownstream,
    setUpstream,
    routeQuery,
    clientReply,
    diagnostic,
    getCapabilities,
    NULL, // No destroyInstance
};

/**
 * The instance structure for the TEE filter - this holds the configuration
 * information for the filter.
 */
typedef struct
{
    SERVICE *service; /* The service to duplicate requests to */
    char *source; /* The source of the client connection */
    char *userName; /* The user name to filter on */
    char *match; /* Optional text to match against */
    regex_t re; /* Compiled regex text */
    char *nomatch; /* Optional text to match against for exclusion */
    regex_t nore; /* Compiled regex nomatch text */
} TEE_INSTANCE;

/**
 * The session structure for this TEE filter.
 * This stores the downstream filter information, such that the
 * filter is able to pass the query on to the next filter (or router)
 * in the chain.
 *
 * It also holds the file descriptor to which queries are written.
 */
typedef struct
{
    DOWNSTREAM down; /* The downstream filter */
    UPSTREAM up; /* The upstream filter */
    FILTER_DEF* dummy_filterdef;
    int active; /* filter is active? */
    bool use_ok;
    int client_multistatement;
    bool multipacket[2];
    unsigned char command;
    bool waiting[2]; /* if the client is waiting for a reply */
    int eof[2];
    int replies[2]; /* Number of queries received */
    int reply_packets[2]; /* Number of OK, ERR, LOCAL_INFILE_REQUEST or RESULT_SET packets received */
    DCB *branch_dcb; /* Client DCB for "branch" service */
    SESSION *branch_session; /* The branch service session */
    TEE_INSTANCE *instance;
    int n_duped; /* Number of duplicated queries */
    int n_rejected; /* Number of rejected queries */
    int residual; /* Any outstanding SQL text */
    GWBUF* tee_replybuf; /* Buffer for reply */
    GWBUF* tee_partials[2];
    GWBUF* queue;
    SPINLOCK tee_lock;
    DCB* client_dcb;

#ifdef SS_DEBUG
    long d_id;
#endif
} TEE_SESSION;

typedef struct orphan_session_tt
{
    SESSION* session; /*< The child branch session whose parent was freed before
               * the child session was in a suitable state. */
    struct orphan_session_tt* next;
} orphan_session_t;

#ifdef SS_DEBUG
static SPINLOCK debug_lock;
static long debug_id = 0;
#endif

static orphan_session_t* allOrphans = NULL;

static SPINLOCK orphanLock;
static int packet_is_required(GWBUF *queue);
static int detect_loops(TEE_INSTANCE *instance, HASHTABLE* ht, SERVICE* session);
int internal_route(DCB* dcb);
GWBUF* clone_query(TEE_INSTANCE* my_instance, TEE_SESSION* my_session, GWBUF* buffer);
int route_single_query(TEE_INSTANCE* my_instance,
                       TEE_SESSION* my_session,
                       GWBUF* buffer,
                       GWBUF* clone);
int reset_session_state(TEE_SESSION* my_session, GWBUF* buffer);
void create_orphan(SESSION* ses);

static void
orphan_free(void* data)
{
    spinlock_acquire(&orphanLock);
    orphan_session_t *ptr = allOrphans, *finished = NULL, *tmp = NULL;
#ifdef SS_DEBUG
    int o_stopping = 0, o_ready = 0, o_freed = 0;
#endif
    while (ptr)
    {
        if (ptr->session->state == SESSION_STATE_TO_BE_FREED)
        {
            if (ptr == allOrphans)
            {
                tmp = ptr;
                allOrphans = ptr->next;
            }
            else
            {
                tmp = allOrphans;
                while (tmp && tmp->next != ptr)
                {
                    tmp = tmp->next;
                }
                if (tmp)
                {
                    tmp->next = ptr->next;
                    tmp = ptr;
                }
            }
        }

        /*
         * The session has been unlinked from all the DCBs and it is ready to be freed.
         */

        if (ptr->session->state == SESSION_STATE_STOPPING &&
            ptr->session->refcount == 0 && ptr->session->client_dcb == NULL)
        {
            ptr->session->state = SESSION_STATE_TO_BE_FREED;
        }
#ifdef SS_DEBUG
        else if (ptr->session->state == SESSION_STATE_STOPPING)
        {
            o_stopping++;
        }
        else if (ptr->session->state == SESSION_STATE_ROUTER_READY)
        {
            o_ready++;
        }
#endif
        ptr = ptr->next;
        if (tmp)
        {
            tmp->next = finished;
            finished = tmp;
            tmp = NULL;
        }
    }

    spinlock_release(&orphanLock);

#ifdef SS_DEBUG
    if (o_stopping + o_ready > 0)
    {
        MXS_DEBUG("tee.c: %d orphans in "
                  "SESSION_STATE_STOPPING, %d orphans in "
                  "SESSION_STATE_ROUTER_READY. ", o_stopping, o_ready);
    }
#endif

    while (finished)
    {
#ifdef SS_DEBUG
        o_freed++;
#endif
        tmp = finished;
        finished = finished->next;

        tmp->session->service->router->freeSession(
            tmp->session->service->router_instance,
            tmp->session->router_session);

        tmp->session->state = SESSION_STATE_FREE;
        MXS_FREE(tmp->session);
        MXS_FREE(tmp);
    }

#ifdef SS_DEBUG
    MXS_DEBUG("tee.c: %d orphans freed.", o_freed);
#endif
}

/**
 * Implementation of the mandatory version entry point
 *
 * @return version string of the module
 */
char *
version()
{
    return version_str;
}

/**
 * The module initialisation routine, called when the module
 * is first loaded.
 * @see function load_module in load_utils.c for explanation of lint
 */
/*lint -e14 */
void
ModuleInit()
{
    spinlock_init(&orphanLock);
#ifdef SS_DEBUG
    spinlock_init(&debug_lock);
#endif
}
/*lint +e14 */

/**
 * The module entry point routine. It is this routine that
 * must populate the structure that is referred to as the
 * "module object", this is a structure with the set of
 * external entry points for this module.
 *
 * @return The module object
 */
FILTER_OBJECT *
GetModuleObject()
{
    return &MyObject;
}

/**
 * Create an instance of the filter for a particular service
 * within MaxScale.
 *
 * @param name      The name of the instance (as defined in the config file).
 * @param options   The options for this filter
 * @param params    The array of name/value pair parameters for the filter
 *
 * @return The instance data for this new instance
 */
static FILTER *
createInstance(const char *name, char **options, FILTER_PARAMETER **params)
{
    TEE_INSTANCE *my_instance;
    int i;

    if ((my_instance = MXS_CALLOC(1, sizeof(TEE_INSTANCE))) != NULL)
    {
        if (options)
        {
            MXS_ERROR("tee: The tee filter has been passed an option, "
                      "this filter does not support any options.");
        }
        my_instance->service = NULL;
        my_instance->source = NULL;
        my_instance->userName = NULL;
        my_instance->match = NULL;
        my_instance->nomatch = NULL;
        if (params)
        {
            for (i = 0; params[i]; i++)
            {
                if (!strcmp(params[i]->name, "service"))
                {
                    if ((my_instance->service = service_find(params[i]->value)) == NULL)
                    {
                        MXS_ERROR("tee: service '%s' not found.\n",
                                  params[i]->value);
                    }
                }
                else if (!strcmp(params[i]->name, "match"))
                {
                    my_instance->match = MXS_STRDUP_A(params[i]->value);
                }
                else if (!strcmp(params[i]->name, "exclude"))
                {
                    my_instance->nomatch = MXS_STRDUP_A(params[i]->value);
                }
                else if (!strcmp(params[i]->name, "source"))
                {
                    my_instance->source = MXS_STRDUP_A(params[i]->value);
                }
                else if (!strcmp(params[i]->name, "user"))
                {
                    my_instance->userName = MXS_STRDUP_A(params[i]->value);
                }
                else if (!filter_standard_parameter(params[i]->name))
                {
                    MXS_ERROR("tee: Unexpected parameter '%s'.",
                              params[i]->name);
                }
            }
        }

        int cflags = REG_ICASE;

        if (options)
        {
            for (i = 0; options[i]; i++)
            {
                if (!strcasecmp(options[i], "ignorecase"))
                {
                    cflags |= REG_ICASE;
                }
                else if (!strcasecmp(options[i], "case"))
                {
                    cflags &= ~REG_ICASE;
                }
                else if (!strcasecmp(options[i], "extended"))
                {
                    cflags |= REG_EXTENDED;
                }
                else
                {
                    MXS_ERROR("tee: unsupported option '%s'.",
                              options[i]);
                }
            }
        }

        if (my_instance->service == NULL)
        {
            MXS_FREE(my_instance->match);
            MXS_FREE(my_instance->source);
            MXS_FREE(my_instance);
            return NULL;
        }

        if (my_instance->match &&
            regcomp(&my_instance->re, my_instance->match, cflags))
        {
            MXS_ERROR("tee: Invalid regular expression '%s'"
                      " for the match parameter.",
                      my_instance->match);
            MXS_FREE(my_instance->match);
            MXS_FREE(my_instance->nomatch);
            MXS_FREE(my_instance->source);
            MXS_FREE(my_instance);
            return NULL;
        }
        if (my_instance->nomatch &&
            regcomp(&my_instance->nore, my_instance->nomatch, cflags))
        {
            MXS_ERROR("tee: Invalid regular expression '%s'"
                      " for the nomatch paramter.\n",
                      my_instance->nomatch);
            if (my_instance->match)
            {
                regfree(&my_instance->re);
                MXS_FREE(my_instance->match);
            }
            MXS_FREE(my_instance->nomatch);
            MXS_FREE(my_instance->source);
            MXS_FREE(my_instance);
            return NULL;
        }
    }
    return (FILTER *) my_instance;
}

/**
 * Associate a new session with this instance of the filter.
 *
 * Create the file to log to and open it.
 *
 * @param instance  The filter instance data
 * @param session   The session itself
 * @return Session specific data for this session
 */
static void *
newSession(FILTER *instance, SESSION *session)
{
    TEE_INSTANCE *my_instance = (TEE_INSTANCE *) instance;
    TEE_SESSION *my_session;
    char *remote, *userName;

    if (strcmp(my_instance->service->name, session->service->name) == 0)
    {
        MXS_ERROR("%s: Recursive use of tee filter in service.",
                  session->service->name);
        my_session = NULL;
        goto retblock;
    }

    HASHTABLE* ht = hashtable_alloc(100, hashtable_item_strhash, hashtable_item_strcmp);
    bool is_loop = detect_loops(my_instance, ht, session->service);
    hashtable_free(ht);

    if (is_loop)
    {
        MXS_ERROR("%s: Recursive use of tee filter in service.",
                  session->service->name);
        my_session = NULL;
        goto retblock;
    }

    if ((my_session = MXS_CALLOC(1, sizeof(TEE_SESSION))) != NULL)
    {
        my_session->active = 1;
        my_session->residual = 0;
        my_session->tee_replybuf = NULL;
        my_session->client_dcb = session->client_dcb;
        my_session->instance = my_instance;
        my_session->client_multistatement = false;
        my_session->queue = NULL;
        spinlock_init(&my_session->tee_lock);
        if (my_instance->source &&
            (remote = session_get_remote(session)) != NULL)
        {
            if (strcmp(remote, my_instance->source))
            {
                my_session->active = 0;

                MXS_WARNING("Tee filter is not active.");
            }
        }
        userName = session_getUser(session);

        if (my_instance->userName &&
            userName &&
            strcmp(userName, my_instance->userName))
        {
            my_session->active = 0;

            MXS_WARNING("Tee filter is not active.");
        }

        if (my_session->active)
        {
            DCB* dcb;
            SESSION* ses;
            if ((dcb = dcb_clone(session->client_dcb)) == NULL)
            {
                freeSession(instance, (void *) my_session);
                my_session = NULL;

                MXS_ERROR("Creating client DCB for Tee "
                          "filter failed. Terminating session.");

                goto retblock;
            }

            if ((ses = session_alloc(my_instance->service, dcb)) == NULL)
            {
                dcb_close(dcb);
                freeSession(instance, (void *) my_session);
                my_session = NULL;
                MXS_ERROR("Creating client session for Tee "
                          "filter failed. Terminating session.");

                goto retblock;
            }

            ss_dassert(ses->ses_is_child);

            my_session->branch_session = ses;
            my_session->branch_dcb = dcb;
        }
    }
retblock:
    return my_session;
}

/**
 * Close a session with the filter, this is the mechanism
 * by which a filter may cleanup data structure etc.
 * In the case of the tee filter we need to close down the
 * "branch" session.
 *
 * @param instance  The filter instance data
 * @param session   The session being closed
 */
static void
closeSession(FILTER *instance, void *session)
{
    TEE_SESSION *my_session = (TEE_SESSION *) session;
    ROUTER_OBJECT *router;
    void *router_instance, *rsession;
    SESSION *bsession;
#ifdef SS_DEBUG
    MXS_INFO("Tee close: %d", atomic_add(&debug_seq, 1));
#endif
    if (my_session->active)
    {

        if ((bsession = my_session->branch_session) != NULL)
        {
            CHK_SESSION(bsession);
            spinlock_acquire(&bsession->ses_lock);

            if (bsession->state != SESSION_STATE_STOPPING)
            {
                bsession->state = SESSION_STATE_STOPPING;
            }
            router = bsession->service->router;
            router_instance = bsession->service->router_instance;
            rsession = bsession->router_session;
            spinlock_release(&bsession->ses_lock);

            /** Close router session and all its connections */
            router->closeSession(router_instance, rsession);
        }
        /* No need to free the session, this is done as
         * a side effect of closing the client DCB of the
         * session.
         */

        if (my_session->waiting[PARENT])
        {
            if (my_session->command != 0x01 &&
                my_session->client_dcb &&
                my_session->client_dcb->state == DCB_STATE_POLLING)
            {
                MXS_INFO("Tee session closed mid-query.");
                GWBUF* errbuf = modutil_create_mysql_err_msg(1, 0, 1, "00000", "Session closed.");
                my_session->client_dcb->func.write(my_session->client_dcb, errbuf);
            }
        }


        my_session->active = 0;
    }
}

/**
 * Free the memory associated with the session
 *
 * @param instance  The filter instance
 * @param session   The filter session
 */
static void
freeSession(FILTER *instance, void *session)
{
    TEE_SESSION *my_session = (TEE_SESSION *) session;
    SESSION* ses = my_session->branch_session;
    session_state_t state;
#ifdef SS_DEBUG
    MXS_INFO("Tee free: %d", atomic_add(&debug_seq, 1));
#endif
    if (ses != NULL)
    {
        state = ses->state;

        if (state == SESSION_STATE_ROUTER_READY)
        {
            session_put_ref(ses);
        }
        else if (state == SESSION_STATE_TO_BE_FREED)
        {
            /** Free branch router session */
            ses->service->router->freeSession(
                ses->service->router_instance,
                ses->router_session);
            /** Free memory of branch client session */
            ses->state = SESSION_STATE_FREE;
            MXS_FREE(ses);
            /** This indicates that branch session is not available anymore */
            my_session->branch_session = NULL;
        }
        else if (state == SESSION_STATE_STOPPING)
        {
            create_orphan(ses);
        }
    }
    if (my_session->dummy_filterdef)
    {
        filter_free(my_session->dummy_filterdef);
    }
    if (my_session->tee_replybuf)
    {
        gwbuf_free(my_session->tee_replybuf);
    }
    MXS_FREE(session);

    orphan_free(NULL);

    return;
}

/**
 * Set the downstream filter or router to which queries will be
 * passed from this filter.
 *
 * @param instance  The filter instance data
 * @param session   The filter session
 * @param downstream    The downstream filter or router.
 */
static void
setDownstream(FILTER *instance, void *session, DOWNSTREAM *downstream)
{
    TEE_SESSION *my_session = (TEE_SESSION *) session;
    my_session->down = *downstream;
}

/**
 * Set the downstream filter or router to which queries will be
 * passed from this filter.
 *
 * @param instance  The filter instance data
 * @param session   The filter session
 * @param downstream    The downstream filter or router.
 */
static void
setUpstream(FILTER *instance, void *session, UPSTREAM *upstream)
{
    TEE_SESSION *my_session = (TEE_SESSION *) session;
    my_session->up = *upstream;
}

/**
 * The routeQuery entry point. This is passed the query buffer
 * to which the filter should be applied. Once applied the
 * query should normally be passed to the downstream component
 * (filter or router) in the filter chain.
 *
 * If my_session->residual is set then duplicate that many bytes
 * and send them to the branch.
 *
 * If my_session->residual is zero then this must be a new request
 * Extract the SQL text if possible, match against that text and forward
 * the request. If the requets is not contained witin the packet we have
 * then set my_session->residual to the number of outstanding bytes
 *
 * @param instance  The filter instance data
 * @param session   The filter session
 * @param queue     The query data
 */
static int
routeQuery(FILTER *instance, void *session, GWBUF *queue)
{
    TEE_INSTANCE *my_instance = (TEE_INSTANCE *) instance;
    TEE_SESSION *my_session = (TEE_SESSION *) session;
    GWBUF *clone = clone_query(my_instance, my_session, queue);

    return route_single_query(my_instance, my_session, queue, clone);
}

/**
 * The clientReply entry point. This is passed the response buffer
 * to which the filter should be applied. Once processed the
 * query is passed to the upstream component
 * (filter or router) in the filter chain.
 *
 * @param instance  The filter instance data
 * @param session   The filter session
 * @param reply     The response data
 */
static int
clientReply(FILTER* instance, void *session, GWBUF *reply)
{
    int rc = 1, branch, eof;
    TEE_SESSION *my_session = (TEE_SESSION *) session;
    
    return my_session->up.clientReply(my_session->up.instance,
                                      my_session->up.session,
                                      reply);
}

/**
 * Diagnostics routine
 *
 * If fsession is NULL then print diagnostics on the filter
 * instance as a whole, otherwise print diagnostics for the
 * particular session.
 *
 * @param   instance    The filter instance
 * @param   fsession    Filter session, may be NULL
 * @param   dcb     The DCB for diagnostic output
 */
static void
diagnostic(FILTER *instance, void *fsession, DCB *dcb)
{
    TEE_INSTANCE *my_instance = (TEE_INSTANCE *) instance;
    TEE_SESSION *my_session = (TEE_SESSION *) fsession;

    if (my_instance->source)
    {
        dcb_printf(dcb, "\t\tLimit to connections from 		%s\n",
                   my_instance->source);
    }
    dcb_printf(dcb, "\t\tDuplicate statements to service		%s\n",
               my_instance->service->name);
    if (my_instance->userName)
    {
        dcb_printf(dcb, "\t\tLimit to user			%s\n",
                   my_instance->userName);
    }
    if (my_instance->match)
    {
        dcb_printf(dcb, "\t\tInclude queries that match		%s\n",
                   my_instance->match);
    }
    if (my_instance->nomatch)
    {
        dcb_printf(dcb, "\t\tExclude queries that match		%s\n",
                   my_instance->nomatch);
    }
    if (my_session)
    {
        dcb_printf(dcb, "\t\tNo. of statements duplicated:	%d.\n",
                   my_session->n_duped);
        dcb_printf(dcb, "\t\tNo. of statements rejected:	%d.\n",
                   my_session->n_rejected);
    }
}

/**
 * Capability routine.
 *
 * @return The capabilities of the filter.
 */
static uint64_t getCapabilities(void)
{
    return RCAP_TYPE_CONTIGUOUS_INPUT;
}

/**
 * Determine if the packet is a command that must be sent to the branch
 * to maintain the session consistancy. These are COM_INIT_DB,
 * COM_CHANGE_USER and COM_QUIT packets.
 *
 * @param queue     The buffer to check
 * @return      non-zero if the packet should be sent to the branch
 */
static int
packet_is_required(GWBUF *queue)
{
    uint8_t *ptr;
    int i;

    ptr = GWBUF_DATA(queue);
    if (GWBUF_LENGTH(queue) > 4)
    {
        for (i = 0; required_packets[i]; i++)
        {
            if (ptr[4] == required_packets[i])
            {
                return 1;
            }
        }
    }
    return 0;
}

/**
 * Detects possible loops in the query cloning chain.
 */
int detect_loops(TEE_INSTANCE *instance, HASHTABLE* ht, SERVICE* service)
{
    SERVICE* svc = service;
    int i;

    if (ht == NULL)
    {
        return -1;
    }

    if (hashtable_add(ht, (void*) service->name, (void*) true) == 0)
    {
        return true;
    }

    for (i = 0; i < svc->n_filters; i++)
    {
        if (strcmp(svc->filters[i]->module, "tee") == 0)
        {
            /*
             * Found a Tee filter, recurse down its path
             * if the service name isn't already in the hashtable.
             */

            TEE_INSTANCE* ninst = (TEE_INSTANCE*) svc->filters[i]->filter;
            if (ninst == NULL)
            {
                /**
                 * This tee instance hasn't been initialized yet and full
                 * resolution of recursion cannot be done now.
                 */
                continue;
            }
            SERVICE* tgt = ninst->service;

            if (detect_loops((TEE_INSTANCE*) svc->filters[i]->filter, ht, tgt))
            {
                return true;
            }

        }
    }

    return false;
}

GWBUF* clone_query(TEE_INSTANCE* my_instance, TEE_SESSION* my_session, GWBUF* buffer)
{
    GWBUF* clone = NULL;

    if ((!my_instance->match && !my_instance->nomatch) || packet_is_required(buffer))
    {
        clone = gwbuf_clone_all(buffer);
    }
    else
    {
        char *ptr = modutil_get_SQL(buffer);

        if (ptr)
        {
            if ((my_instance->match && regexec(&my_instance->re, ptr, 0, NULL, 0) == 0) ||
                (my_instance->nomatch && regexec(&my_instance->nore, ptr, 0, NULL, 0) != 0))
            {
                clone = gwbuf_clone_all(buffer);
            }
            MXS_FREE(ptr);
        }
    }

    return clone;
}

/**
 * Route the main query downstream along the main filter chain and possibly route
 * a clone of the buffer to the branch session. If the clone buffer is NULL, nothing
 * is routed to the branch session.
 * @param my_instance Tee instance
 * @param my_session Tee session
 * @param buffer Main buffer
 * @param clone Cloned buffer
 * @return 1 on success, 0 on failure.
 */
int route_single_query(TEE_INSTANCE* my_instance, TEE_SESSION* my_session, GWBUF* buffer, GWBUF* clone)
{
    int rval = 0;

    if (my_session->active && my_session->branch_session &&
        my_session->branch_session->state == SESSION_STATE_ROUTER_READY)
    {

        rval = my_session->down.routeQuery(my_session->down.instance,
                                           my_session->down.session,
                                           buffer);
        if (clone)
        {
            my_session->n_duped++;

            if (my_session->branch_session->state == SESSION_STATE_ROUTER_READY)
            {
                SESSION_ROUTE_QUERY(my_session->branch_session, clone);
            }
            else
            {
                /** Close tee session */
                my_session->active = 0;
                rval = 0;
                MXS_INFO("Closed tee filter session: Child session in invalid state.");
                gwbuf_free(clone);
            }
        }
    }

    return rval;
}

/**
 * Reset the session's internal counters.
 * @param my_session Tee session
 * @param buffer Buffer with the query of the main branch in it
 * @return 1 on success, 0 on error
 */
int reset_session_state(TEE_SESSION* my_session, GWBUF* buffer)
{
    if (gwbuf_length(buffer) < 5)
    {
        return 0;
    }

    unsigned char command = *((unsigned char*) buffer->start + 4);

    switch (command)
    {
        case 0x1b:
            my_session->client_multistatement = *((unsigned char*) buffer->start + 5);
            MXS_INFO("tee: client %s multistatements",
                     my_session->client_multistatement ? "enabled" : "disabled");
        case 0x03:
        case 0x16:
        case 0x17:
        case 0x04:
        case 0x0a:
            memset(my_session->multipacket, (char) true, 2 * sizeof(bool));
            break;
        default:
            memset(my_session->multipacket, (char) false, 2 * sizeof(bool));
            break;
    }

    memset(my_session->replies, 0, 2 * sizeof(int));
    memset(my_session->reply_packets, 0, 2 * sizeof(int));
    memset(my_session->eof, 0, 2 * sizeof(int));
    memset(my_session->waiting, 1, 2 * sizeof(bool));
    my_session->command = command;

    return 1;
}

void create_orphan(SESSION* ses)
{
    orphan_session_t* orphan = MXS_MALLOC(sizeof(orphan_session_t));
    if (orphan)
    {
        orphan->session = ses;
        spinlock_acquire(&orphanLock);
        orphan->next = allOrphans;
        allOrphans = orphan;
        spinlock_release(&orphanLock);
    }
}