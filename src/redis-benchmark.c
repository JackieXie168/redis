/* Redis benchmark utility. 
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fmacros.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <signal.h>
#include <assert.h>
#include <math.h>
#include <pthread.h>

#include <sds.h> /* Use hiredis sds. */
#include "ae.h"
#include "hiredis.h"
#include "adlist.h"
#include "zmalloc.h"
#include "atomicvar.h"
#include "crc16_slottable.h"

#define UNUSED(V) ((void) V)
#define RANDPTR_INITIAL_SIZE 8
#define MAX_LATENCY_PRECISION 3
#define MAX_THREADS 16
#define CLUSTER_SLOTS 16384

#define CLIENT_GET_EVENTLOOP(c) \
    (c->thread_id >= 0 ? config.threads[c->thread_id]->el : config.el)

struct benchmarkThread;
struct clusterNode;

static struct config {
    aeEventLoop *el;
    const char *hostip;
    int hostport;
    const char *hostsocket;
    int numclients;
    int liveclients;
    int requests;
    int requests_issued;
    int requests_finished;
    int keysize;
    int datasize;
    int randomkeys;
    int randomkeys_keyspacelen;
    int keepalive;
    int pipeline;
    int showerrors;
    long long start;
    long long totlatency;
    long long *latency;
    const char *title;
    list *clients;
    int quiet;
    int csv;
    int loop;
    int idlemode;
    int dbnum;
    sds dbnumstr;
    char *tests;
    char *auth;
    int precision;
    int num_threads;
    struct benchmarkThread **threads;
    int cluster_mode;
    int cluster_node_count;
    struct clusterNode **cluster_nodes;
    /* Thread mutexes to be used as fallbacks by atomicvar.h */
    pthread_mutex_t requests_issued_mutex;
    pthread_mutex_t requests_finished_mutex;
    pthread_mutex_t liveclients_mutex;
} config;

typedef struct _client {
    redisContext *context;
    sds obuf;
    char **randptr;         /* Pointers to :rand: strings inside the command buf */
    size_t randlen;         /* Number of pointers in client->randptr */
    size_t randfree;        /* Number of unused pointers in client->randptr */
    char **stagptr;         /* Pointers to slot hashtags (cluster mode only) */
    size_t staglen;         /* Number of pointers in client->stagptr */
    size_t stagfree;        /* Number of unused pointers in client->stagptr */
    size_t written;         /* Bytes of 'obuf' already written */
    long long start;        /* Start time of a request */
    long long latency;      /* Request latency */
    int pending;            /* Number of pending requests (replies to consume) */
    int prefix_pending;     /* If non-zero, number of pending prefix commands. Commands
                               such as auth and select are prefixed to the pipeline of
                               benchmark commands and discarded after the first send. */
    int prefixlen;          /* Size in bytes of the pending prefix commands */
    int thread_id;
    struct clusterNode *cluster_node;
} *client;

/* Threads. */

typedef struct benchmarkThread {
    int index;
    pthread_t thread;
    aeEventLoop *el;
} benchmarkThread;

/* Cluster. */
typedef struct clusterNode {
    char *ip;
    int port;
    sds name;
    int flags;
    sds replicate;  /* Master ID if node is a slave */
    int *slots;
    int current_slot_index;
    int slots_count;
    int replicas_count;
    sds *migrating; /* An array of sds where even strings are slots and odd
                     * strings are the destination node IDs. */
    sds *importing; /* An array of sds where even strings are slots and odd
                     * strings are the source node IDs. */
    int migrating_count; /* Length of the migrating array (migrating slots*2) */
    int importing_count; /* Length of the importing array (importing slots*2) */
} clusterNode;

/* Prototypes */
static void writeHandler(aeEventLoop *el, int fd, void *privdata, int mask);
static void createMissingClients(client c);
static benchmarkThread *createBenchmarkThread(int index);
static void freeBenchmarkThread(benchmarkThread *thread);
static void freeBenchmarkThreads();
static void *execBenchmarkThread(void *ptr);
static clusterNode *createClusterNode(char *ip, int port);
int showThroughput(struct aeEventLoop *eventLoop, long long id,
                   void *clientData);

/* Implementation */
static long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long)tv.tv_sec)*1000000;
    ust += tv.tv_usec;
    return ust;
}

static long long mstime(void) {
    struct timeval tv;
    long long mst;

    gettimeofday(&tv, NULL);
    mst = ((long long)tv.tv_sec)*1000;
    mst += tv.tv_usec/1000;
    return mst;
}

static void freeClient(client c) {
    aeEventLoop *el = CLIENT_GET_EVENTLOOP(c);
    listNode *ln;
    aeDeleteFileEvent(el,c->context->fd,AE_WRITABLE);
    aeDeleteFileEvent(el,c->context->fd,AE_READABLE);
    aeStop(el);
    redisFree(c->context);
    sdsfree(c->obuf);
    zfree(c->randptr);
    zfree(c->stagptr);
    zfree(c);
    if (config.num_threads) pthread_mutex_lock(&(config.liveclients_mutex));
    config.liveclients--;
    ln = listSearchKey(config.clients,c);
    assert(ln != NULL);
    listDelNode(config.clients,ln);
    if (config.num_threads) pthread_mutex_unlock(&(config.liveclients_mutex));
}

static void freeAllClients(void) {
    listNode *ln = config.clients->head, *next;

    while(ln) {
        next = ln->next;
        freeClient(ln->value);
        ln = next;
    }
}

static void resetClient(client c) {
    aeEventLoop *el = CLIENT_GET_EVENTLOOP(c);
    aeDeleteFileEvent(el,c->context->fd,AE_WRITABLE);
    aeDeleteFileEvent(el,c->context->fd,AE_READABLE);
    aeCreateFileEvent(el,c->context->fd,AE_WRITABLE,writeHandler,c);
    c->written = 0;
    c->pending = config.pipeline;
}

static void randomizeClientKey(client c) {
    size_t i;

    for (i = 0; i < c->randlen; i++) {
        char *p = c->randptr[i]+11;
        size_t r = random() % config.randomkeys_keyspacelen;
        size_t j;

        for (j = 0; j < 12; j++) {
            *p = '0'+r%10;
            r/=10;
            p--;
        }
    }
}

static void setClusterKeyHashTag(client c) {
    assert(c->thread_id >= 0);
    clusterNode *node = c->cluster_node;
    assert(node);
    assert(node->current_slot_index < node->slots_count);
    int slot = node->slots[node->current_slot_index];
    const char *tag = crc16_slot_table[slot];
    int taglen = strlen(tag);
    size_t i;
    for (i = 0; i < c->staglen; i++) {
        char *p = c->stagptr[i] + 1;
        p[0] = tag[0];
        p[1] = (taglen >= 2 ? tag[1] : '}');
        p[2] = (taglen == 3 ? tag[2] : '}');
    }
}

static void clientDone(client c) {
    int requests_finished = 0;
    if (!config.num_threads) requests_finished = config.requests_finished;
    else atomicGet(config.requests_finished, requests_finished);
    if (requests_finished >= config.requests) {
        freeClient(c);
        return;
    }
    if (config.keepalive) {
        resetClient(c);
    } else {
        if (config.num_threads) pthread_mutex_lock(&(config.liveclients_mutex));
        config.liveclients--;
        createMissingClients(c);
        config.liveclients++;
        if (config.num_threads)
            pthread_mutex_unlock(&(config.liveclients_mutex));
        freeClient(c);
    }
}

static void readHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    client c = privdata;
    void *reply = NULL;
    UNUSED(el);
    UNUSED(fd);
    UNUSED(mask);

    /* Calculate latency only for the first read event. This means that the
     * server already sent the reply and we need to parse it. Parsing overhead
     * is not part of the latency, so calculate it only once, here. */
    if (c->latency < 0) c->latency = ustime()-(c->start);

    if (redisBufferRead(c->context) != REDIS_OK) {
        fprintf(stderr,"Error: %s\n",c->context->errstr);
        exit(1);
    } else {
        while(c->pending) {
            if (redisGetReply(c->context,&reply) != REDIS_OK) {
                fprintf(stderr,"Error: %s\n",c->context->errstr);
                exit(1);
            }
            if (reply != NULL) {
                if (reply == (void*)REDIS_REPLY_ERROR) {
                    fprintf(stderr,"Unexpected error reply, exiting...\n");
                    exit(1);
                }
                redisReply *r = reply;
                int is_err = (r->type == REDIS_REPLY_ERROR);

                if (is_err && config.showerrors) {
                    static time_t lasterr_time = 0;
                    time_t now = time(NULL);
                    if (lasterr_time != now) {
                        lasterr_time = now;
                        if (c->cluster_node) {
                            printf("Error from server %s:%d: %s\n",
                                   c->cluster_node->ip,
                                   c->cluster_node->port,
                                   r->str);
                        } else printf("Error from server: %s\n", r->str);
                    }
                }

                if (config.cluster_mode && is_err && c->cluster_node &&
                    (!strncmp(r->str,"MOVED",5) ||
                     !strcmp(r->str,"ASK")))
                {
                    clusterNode *node = c->cluster_node;
                    assert(node);
                    if (++node->current_slot_index >= node->slots_count) {
                        fprintf(stderr,"Cluster node %s:%d has no more "
                                       "valid slots, aborting...\n", node->ip,
                                       node->port);
                        exit(1);
                    }
                }

                freeReplyObject(reply);
                /* This is an OK for prefix commands such as auth and select.*/
                if (c->prefix_pending > 0) {
                    c->prefix_pending--;
                    c->pending--;
                    /* Discard prefix commands on first response.*/
                    if (c->prefixlen > 0) {
                        size_t j;
                        sdsrange(c->obuf, c->prefixlen, -1);
                        /* We also need to fix the pointers to the strings
                        * we need to randomize. */
                        for (j = 0; j < c->randlen; j++)
                            c->randptr[j] -= c->prefixlen;
                        c->prefixlen = 0;
                    }
                    continue;
                }
                int requests_finished = 0;
                atomicGetIncr(config.requests_finished, requests_finished, 1);
                requests_finished--;
                if (requests_finished < config.requests)
                    config.latency[++requests_finished] = c->latency;
                c->pending--;
                if (c->pending == 0) {
                    clientDone(c);
                    break;
                }
            } else {
                break;
            }
        }
    }
}

static void writeHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    client c = privdata;
    UNUSED(el);
    UNUSED(fd);
    UNUSED(mask);

    /* Initialize request when nothing was written. */
    if (c->written == 0) {
        /* Enforce upper bound to number of requests. */
        int requests_issued = 0;
        if (!config.num_threads) requests_issued = config.requests_issued++;
        else atomicGetIncr(config.requests_issued, requests_issued, 1);
        if (requests_issued >= config.requests) {
            freeClient(c);
            return;
        }

        /* Really initialize: randomize keys and set start time. */
        if (config.randomkeys) randomizeClientKey(c);
        if (config.cluster_mode && c->staglen > 0) setClusterKeyHashTag(c);
        c->start = ustime();
        c->latency = -1;
    }
    if (sdslen(c->obuf) > c->written) {
        void *ptr = c->obuf+c->written;
        ssize_t nwritten = write(c->context->fd,ptr,sdslen(c->obuf)-c->written);
        if (nwritten == -1) {
            if (errno != EPIPE)
                fprintf(stderr, "Writing to socket: %s\n", strerror(errno));
            freeClient(c);
            return;
        }
        c->written += nwritten;
        if (sdslen(c->obuf) == c->written) {
            aeDeleteFileEvent(el,c->context->fd,AE_WRITABLE);
            aeCreateFileEvent(el,c->context->fd,AE_READABLE,readHandler,c);
        }
    }
}

/* Create a benchmark client, configured to send the command passed as 'cmd' of
 * 'len' bytes.
 *
 * The command is copied N times in the client output buffer (that is reused
 * again and again to send the request to the server) accordingly to the configured
 * pipeline size.
 *
 * Also an initial SELECT command is prepended in order to make sure the right
 * database is selected, if needed. The initial SELECT will be discarded as soon
 * as the first reply is received.
 *
 * To create a client from scratch, the 'from' pointer is set to NULL. If instead
 * we want to create a client using another client as reference, the 'from' pointer
 * points to the client to use as reference. In such a case the following
 * information is take from the 'from' client:
 *
 * 1) The command line to use.
 * 2) The offsets of the __rand_int__ elements inside the command line, used
 *    for arguments randomization.
 *
 * Even when cloning another client, prefix commands are applied if needed.*/
static client createClient(char *cmd, size_t len, client from, int thread_id) {
    int j;
    int is_cluster_client = (config.cluster_mode && thread_id >= 0);
    client c = zmalloc(sizeof(struct _client));

    const char *ip;
    int port;
    c->cluster_node = NULL;
    if (config.hostsocket == NULL || is_cluster_client) {
        if (!is_cluster_client) {
            ip = config.hostip;
            port = config.hostport;
        } else {
            clusterNode *node = config.cluster_nodes[thread_id];
            if (node == NULL) exit(1);
            ip = (const char *) node->ip;
            port = node->port;
            c->cluster_node = node;
        }
        c->context = redisConnectNonBlock(ip,port);
    } else {
        c->context = redisConnectUnixNonBlock(config.hostsocket);
    }
    if (c->context->err) {
        fprintf(stderr,"Could not connect to Redis at ");
        if (config.hostsocket == NULL || is_cluster_client)
            fprintf(stderr,"%s:%d: %s\n",ip,port,c->context->errstr);
        else
            fprintf(stderr,"%s: %s\n",config.hostsocket,c->context->errstr);
        exit(1);
    }
    c->thread_id = thread_id;
    /* Suppress hiredis cleanup of unused buffers for max speed. */
    c->context->reader->maxbuf = 0;

    /* Build the request buffer:
     * Queue N requests accordingly to the pipeline size, or simply clone
     * the example client buffer. */
    c->obuf = sdsempty();
    /* Prefix the request buffer with AUTH and/or SELECT commands, if applicable.
     * These commands are discarded after the first response, so if the client is
     * reused the commands will not be used again. */
    c->prefix_pending = 0;
    if (config.auth) {
        char *buf = NULL;
        int len = redisFormatCommand(&buf, "AUTH %s", config.auth);
        c->obuf = sdscatlen(c->obuf, buf, len);
        free(buf);
        c->prefix_pending++;
    }

    /* If a DB number different than zero is selected, prefix our request
     * buffer with the SELECT command, that will be discarded the first
     * time the replies are received, so if the client is reused the
     * SELECT command will not be used again. */
    if (config.dbnum != 0) {
        c->obuf = sdscatprintf(c->obuf,"*2\r\n$6\r\nSELECT\r\n$%d\r\n%s\r\n",
            (int)sdslen(config.dbnumstr),config.dbnumstr);
        c->prefix_pending++;
    }
    c->prefixlen = sdslen(c->obuf);
    /* Append the request itself. */
    if (from) {
        c->obuf = sdscatlen(c->obuf,
            from->obuf+from->prefixlen,
            sdslen(from->obuf)-from->prefixlen);
    } else {
        for (j = 0; j < config.pipeline; j++)
            c->obuf = sdscatlen(c->obuf,cmd,len);
    }

    c->written = 0;
    c->pending = config.pipeline+c->prefix_pending;
    c->randptr = NULL;
    c->randlen = 0;
    c->stagptr = NULL;
    c->staglen = 0;

    /* Find substrings in the output buffer that need to be randomized. */
    if (config.randomkeys) {
        if (from) {
            c->randlen = from->randlen;
            c->randfree = 0;
            c->randptr = zmalloc(sizeof(char*)*c->randlen);
            /* copy the offsets. */
            for (j = 0; j < (int)c->randlen; j++) {
                c->randptr[j] = c->obuf + (from->randptr[j]-from->obuf);
                /* Adjust for the different select prefix length. */
                c->randptr[j] += c->prefixlen - from->prefixlen;
            }
        } else {
            char *p = c->obuf;

            c->randlen = 0;
            c->randfree = RANDPTR_INITIAL_SIZE;
            c->randptr = zmalloc(sizeof(char*)*c->randfree);
            while ((p = strstr(p,"__rand_int__")) != NULL) {
                if (c->randfree == 0) {
                    c->randptr = zrealloc(c->randptr,sizeof(char*)*c->randlen*2);
                    c->randfree += c->randlen;
                }
                c->randptr[c->randlen++] = p;
                c->randfree--;
                p += 12; /* 12 is strlen("__rand_int__). */
            }
        }
    }
    /* If cluster mode is enabled, set slot hashtags pointers. */
    if (config.cluster_mode) {
        if (from) {
            c->staglen = from->staglen;
            c->stagfree = 0;
            c->stagptr = zmalloc(sizeof(char*)*c->staglen);
            /* copy the offsets. */
            for (j = 0; j < (int)c->staglen; j++) {
                c->stagptr[j] = c->obuf + (from->stagptr[j]-from->obuf);
                /* Adjust for the different select prefix length. */
                c->stagptr[j] += c->prefixlen - from->prefixlen;
            }
        } else {
            char *p = c->obuf;

            c->staglen = 0;
            c->stagfree = RANDPTR_INITIAL_SIZE;
            c->stagptr = zmalloc(sizeof(char*)*c->stagfree);
            while ((p = strstr(p,"{tag}")) != NULL) {
                if (c->stagfree == 0) {
                    c->stagptr = zrealloc(c->stagptr,
                                          sizeof(char*)*c->staglen*2);
                    c->stagfree += c->staglen;
                }
                c->stagptr[c->staglen++] = p;
                c->stagfree--;
                p += 5; /* 12 is strlen("{tag}"). */
            }
        }
    }
    aeEventLoop *el = NULL;
    if (thread_id < 0) el = config.el;
    else {
        benchmarkThread *thread = config.threads[thread_id];
        el = thread->el;
    }
    if (config.idlemode == 0)
        aeCreateFileEvent(el,c->context->fd,AE_WRITABLE,writeHandler,c);
    listAddNodeTail(config.clients,c);
    config.liveclients++;
    return c;
}

static void createMissingClients(client c) {
    int n = 0;
    while(config.liveclients < config.numclients) {
        int thread_id = -1;
        if (config.num_threads)
            thread_id = config.liveclients % config.num_threads;
        createClient(NULL,0,c,thread_id);

        /* Listen backlog is quite limited on most systems */
        if (++n > 64) {
            usleep(50000);
            n = 0;
        }
    }
}

static int compareLatency(const void *a, const void *b) {
    return (*(long long*)a)-(*(long long*)b);
}

static int ipow(int base, int exp) {
    int result = 1;
    while (exp) {
        if (exp & 1) result *= base;
        exp /= 2;
        base *= base;
    }
    return result;
}

static void showLatencyReport(void) {
    int i, curlat = 0;
    int usbetweenlat = ipow(10, MAX_LATENCY_PRECISION-config.precision);
    float perc, reqpersec;

    reqpersec = (float)config.requests_finished/((float)config.totlatency/1000);
    if (!config.quiet && !config.csv) {
        printf("====== %s ======\n", config.title);
        printf("  %d requests completed in %.2f seconds\n", config.requests_finished,
            (float)config.totlatency/1000);
        printf("  %d parallel clients\n", config.numclients);
        printf("  %d bytes payload\n", config.datasize);
        printf("  keep alive: %d\n", config.keepalive);
        printf("  multi-thread: %s\n", (config.num_threads ? "yes" : "no"));
        if (config.num_threads)
            printf("  threads: %d\n", config.num_threads);
        printf("\n");

        qsort(config.latency,config.requests,sizeof(long long),compareLatency);
        for (i = 0; i < config.requests; i++) {
            if (config.latency[i]/usbetweenlat != curlat ||
                i == (config.requests-1))
            {
                /* After the 2 milliseconds latency to have percentages split
                 * by decimals will just add a lot of noise to the output. */
                if (config.latency[i] >= 2000) {
                    config.precision = 0;
                    usbetweenlat = ipow(10,
                        MAX_LATENCY_PRECISION-config.precision);
                }

                curlat = config.latency[i]/usbetweenlat;
                perc = ((float)(i+1)*100)/config.requests;
                printf("%.2f%% <= %.*f milliseconds\n", perc, config.precision,
                    curlat/pow(10.0, config.precision));
            }
        }
        printf("%.2f requests per second\n\n", reqpersec);
    } else if (config.csv) {
        printf("\"%s\",\"%.2f\"\n", config.title, reqpersec);
    } else {
        printf("%s: %.2f requests per second\n", config.title, reqpersec);
    }
}

static void benchmark(char *title, char *cmd, int len) {
    int i;
    client c;

    config.title = title;
    config.requests_issued = 0;
    config.requests_finished = 0;

    if (config.num_threads) {
        if (config.threads) freeBenchmarkThreads();
        config.threads = zmalloc(config.num_threads * sizeof(benchmarkThread*));
        for (i = 0; i < config.num_threads; i++) {
            benchmarkThread *thread = createBenchmarkThread(i);
            config.threads[i] = thread;
        }
    }

    int thread_id = config.num_threads > 0 ? 0 : -1;
    c = createClient(cmd,len,NULL,thread_id);
    createMissingClients(c);

    config.start = mstime();
    if (!config.num_threads) aeMain(config.el);
    else {
        for (i = 0; i < config.num_threads; i++) {
            benchmarkThread *t = config.threads[i];
            if (pthread_create(&(t->thread), NULL, execBenchmarkThread, t)){
                fprintf(stderr, "FATAL: Failed to start thread %d.\n", i);
                exit(1);
            }
        }
        for (i = 0; i < config.num_threads; i++)
            pthread_join(config.threads[i]->thread, NULL);
    }
    config.totlatency = mstime()-config.start;

    showLatencyReport();
    freeAllClients();
    if (config.threads) freeBenchmarkThreads();
}

static benchmarkThread *createBenchmarkThread(int index) {
    benchmarkThread *thread = zmalloc(sizeof(*thread));
    if (thread == NULL) return NULL;
    thread->index = index;
    thread->el = aeCreateEventLoop(1024*10);
    aeCreateTimeEvent(thread->el,1,showThroughput,NULL,NULL);
    return thread;
}

static void freeBenchmarkThread(benchmarkThread *thread) {
    if (thread->el) aeDeleteEventLoop(thread->el);
    zfree(thread);
}

static void freeBenchmarkThreads() {
    int i = 0;
    for (; i < config.num_threads; i++) {
        benchmarkThread *thread = config.threads[i];
        if (thread) freeBenchmarkThread(thread);
    }
    zfree(config.threads);
    config.threads = NULL;
}

static void *execBenchmarkThread(void *ptr) {
    benchmarkThread *thread = (benchmarkThread *) ptr;
    aeMain(thread->el);
    return NULL;
}

static clusterNode *createClusterNode(char *ip, int port) {
    clusterNode *node = zmalloc(sizeof(*node));
    if (!node) return NULL;
    node->ip = ip;
    node->port = port;
    node->name = NULL;
    node->flags = 0;
    node->replicate = NULL;
    node->replicas_count = 0;
    node->slots = zmalloc(CLUSTER_SLOTS * sizeof(int));
    node->slots_count = 0;
    node->current_slot_index = 0;
    node->migrating = NULL;
    node->importing = NULL;
    node->migrating_count = 0;
    node->importing_count = 0;
    return node;
}

static void freeClusterNode(clusterNode *node) {
    int i;
    if (node->name) sdsfree(node->name);
    if (node->replicate) sdsfree(node->replicate);
    if (node->migrating != NULL) {
        for (i = 0; i < node->migrating_count; i++) sdsfree(node->migrating[i]);
        zfree(node->migrating);
    }
    if (node->importing != NULL) {
        for (i = 0; i < node->importing_count; i++) sdsfree(node->importing[i]);
        zfree(node->importing);
    }
    /* If the node is not the reference node, that uses the address from
     * config.hostip and config.hostport, then the node ip has been
     * allocated by fetchClusterConfiguration, so it must be freed. */
    if (node->ip && strcmp(node->ip, config.hostip) != 0) sdsfree(node->ip);
    zfree(node->slots);
    zfree(node);
}

static void freeClusterNodes() {
    int i = 0;
    for (; i < config.cluster_node_count; i++) {
        clusterNode *n = config.cluster_nodes[i];
        if (n) freeClusterNode(n);
    }
    zfree(config.cluster_nodes);
}

static clusterNode **addClusterNode(clusterNode *node) {
    int count = config.cluster_node_count + 1;
    config.cluster_nodes = zrealloc(config.cluster_nodes,
                                    count * sizeof(*node));
    if (!config.cluster_nodes) return NULL;
    config.cluster_nodes[config.cluster_node_count++] = node;
    return config.cluster_nodes;
}

static int fetchClusterConfiguration() {
    int success = 1;
    redisContext *ctx = NULL;
    redisReply *reply =  NULL;
    if (config.hostsocket == NULL)
        ctx = redisConnect(config.hostip,config.hostport);
    else
        ctx = redisConnectUnix(config.hostsocket);
    if (ctx->err) {
        fprintf(stderr,"Could not connect to Redis at ");
        if (config.hostsocket == NULL) {
            fprintf(stderr,"%s:%d: %s\n",config.hostip,config.hostport,
                    ctx->errstr);
        } else fprintf(stderr,"%s: %s\n",config.hostsocket,ctx->errstr);
        exit(1);
    }
    clusterNode *firstNode = createClusterNode((char *) config.hostip,
                                               config.hostport);
    if (!firstNode) {success = 0; goto cleanup;}
    reply = redisCommand(ctx, "CLUSTER NODES");
    success = (reply != NULL);
    if (!success) goto cleanup;
    success = (reply->type != REDIS_REPLY_ERROR);
    if (!success) {
        fprintf(stderr, "Cluster node %s:%d replied with error:\n%s\n",
                config.hostip, config.hostport, reply->str);
        goto cleanup;
    }
    char *lines = reply->str, *p, *line;
    while ((p = strstr(lines, "\n")) != NULL) {
        *p = '\0';
        line = lines;
        lines = p + 1;
        char *name = NULL, *addr = NULL, *flags = NULL, *master_id = NULL;
        int i = 0;
        while ((p = strchr(line, ' ')) != NULL) {
            *p = '\0';
            char *token = line;
            line = p + 1;
            switch(i++){
            case 0: name = token; break;
            case 1: addr = token; break;
            case 2: flags = token; break;
            case 3: master_id = token; break;
            }
            if (i == 8) break; // Slots
        }
        if (!flags) {
            fprintf(stderr, "Invalid CLUSTER NODES reply: missing flags.\n");
            success = 0;
            goto cleanup;
        }
        int myself = (strstr(flags, "myself") != NULL);
        int is_replica = (strstr(flags, "slave") != NULL ||
                         (master_id != NULL && master_id[0] != '-'));
        if (is_replica) continue;
        clusterNode *node = NULL;
        if (myself) {
            node = firstNode;
        } else {
            if (addr == NULL) {
                fprintf(stderr, "Invalid CLUSTER NODES reply: missing addr.\n");
                success = 0;
                goto cleanup;
            }
            char *paddr = strchr(addr, ':');
            if (paddr == NULL) {
                success = 0;
                goto cleanup;
            }
            *paddr = '\0';
            char *ip = addr;
            addr = paddr + 1;
            /* If internal bus is specified, then just drop it. */
            if ((paddr = strchr(addr, '@')) != NULL) *paddr = '\0';
            int port = atoi(addr);
            node = createClusterNode(sdsnew(ip), port);
        }
        if (node == NULL) {
            success = 0;
            goto cleanup;
        }
        node->name = sdsnew(name);
        if (i == 8) {
            int remaining = strlen(line);
            while (remaining > 0) {
                p = strchr(line, ' ');
                if (p == NULL) p = line + remaining;
                remaining -= (p - line);

                char *slotsdef = line;
                *p = '\0';
                if (remaining) {
                    line = p + 1;
                    remaining--;
                } else line = p;
                char *dash = NULL;
                if (slotsdef[0] == '[') {
                    slotsdef++;
                    if ((p = strstr(slotsdef, "->-"))) { // Migrating
                        *p = '\0';
                        p += 3;
                        char *closing_bracket = strchr(p, ']');
                        if (closing_bracket) *closing_bracket = '\0';
                        sds slot = sdsnew(slotsdef);
                        sds dst = sdsnew(p);
                        node->migrating_count += 2;
                        node->migrating =
                            zrealloc(node->migrating,
                                (node->migrating_count * sizeof(sds)));
                        node->migrating[node->migrating_count - 2] =
                            slot;
                        node->migrating[node->migrating_count - 1] =
                            dst;
                    }  else if ((p = strstr(slotsdef, "-<-"))) {//Importing
                        *p = '\0';
                        p += 3;
                        char *closing_bracket = strchr(p, ']');
                        if (closing_bracket) *closing_bracket = '\0';
                        sds slot = sdsnew(slotsdef);
                        sds src = sdsnew(p);
                        node->importing_count += 2;
                        node->importing = zrealloc(node->importing,
                            (node->importing_count * sizeof(sds)));
                        node->importing[node->importing_count - 2] =
                            slot;
                        node->importing[node->importing_count - 1] =
                            src;
                    }
                } else if ((dash = strchr(slotsdef, '-')) != NULL) {
                    p = dash;
                    int start, stop;
                    *p = '\0';
                    start = atoi(slotsdef);
                    stop = atoi(p + 1);
                    while (start <= stop) {
                        int slot = start++;
                        node->slots[node->slots_count++] = slot;
                    }
                } else if (p > slotsdef) {
                    int slot = atoi(slotsdef);
                    node->slots[node->slots_count++] = slot;
                }
            }
        }
        if (node->slots_count == 0) {
            printf("WARNING: master node %s:%d has no slots, skipping...\n",
                   node->ip, node->port);
            continue;
        }
        if (!addClusterNode(node)) {
            success = 0;
            goto cleanup;
        }
    }
cleanup:
    if (ctx) redisFree(ctx);
    if (!success) {
        if (config.cluster_nodes) freeClusterNodes();
    }
    if (reply) freeReplyObject(reply);
    return success;
}

/* Returns number of consumed options. */
int parseOptions(int argc, const char **argv) {
    int i;
    int lastarg;
    int exit_status = 1;

    for (i = 1; i < argc; i++) {
        lastarg = (i == (argc-1));

        if (!strcmp(argv[i],"-c")) {
            if (lastarg) goto invalid;
            config.numclients = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"-n")) {
            if (lastarg) goto invalid;
            config.requests = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"-k")) {
            if (lastarg) goto invalid;
            config.keepalive = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"-h")) {
            if (lastarg) goto invalid;
            config.hostip = strdup(argv[++i]);
        } else if (!strcmp(argv[i],"-p")) {
            if (lastarg) goto invalid;
            config.hostport = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"-s")) {
            if (lastarg) goto invalid;
            config.hostsocket = strdup(argv[++i]);
        } else if (!strcmp(argv[i],"-a") ) {
            if (lastarg) goto invalid;
            config.auth = strdup(argv[++i]);
        } else if (!strcmp(argv[i],"-d")) {
            if (lastarg) goto invalid;
            config.datasize = atoi(argv[++i]);
            if (config.datasize < 1) config.datasize=1;
            if (config.datasize > 1024*1024*1024) config.datasize = 1024*1024*1024;
        } else if (!strcmp(argv[i],"-P")) {
            if (lastarg) goto invalid;
            config.pipeline = atoi(argv[++i]);
            if (config.pipeline <= 0) config.pipeline=1;
        } else if (!strcmp(argv[i],"-r")) {
            if (lastarg) goto invalid;
            config.randomkeys = 1;
            config.randomkeys_keyspacelen = atoi(argv[++i]);
            if (config.randomkeys_keyspacelen < 0)
                config.randomkeys_keyspacelen = 0;
        } else if (!strcmp(argv[i],"-q")) {
            config.quiet = 1;
        } else if (!strcmp(argv[i],"--csv")) {
            config.csv = 1;
        } else if (!strcmp(argv[i],"-l")) {
            config.loop = 1;
        } else if (!strcmp(argv[i],"-I")) {
            config.idlemode = 1;
        } else if (!strcmp(argv[i],"-e")) {
            config.showerrors = 1;
        } else if (!strcmp(argv[i],"-t")) {
            if (lastarg) goto invalid;
            /* We get the list of tests to run as a string in the form
             * get,set,lrange,...,test_N. Then we add a comma before and
             * after the string in order to make sure that searching
             * for ",testname," will always get a match if the test is
             * enabled. */
            config.tests = sdsnew(",");
            config.tests = sdscat(config.tests,(char*)argv[++i]);
            config.tests = sdscat(config.tests,",");
            sdstolower(config.tests);
        } else if (!strcmp(argv[i],"--dbnum")) {
            if (lastarg) goto invalid;
            config.dbnum = atoi(argv[++i]);
            config.dbnumstr = sdsfromlonglong(config.dbnum);
        } else if (!strcmp(argv[i],"--precision")) {
            if (lastarg) goto invalid;
            config.precision = atoi(argv[++i]);
            if (config.precision < 0) config.precision = 0;
            if (config.precision > MAX_LATENCY_PRECISION) config.precision = MAX_LATENCY_PRECISION;
        } else if (!strcmp(argv[i],"--threads")) {
             if (lastarg) goto invalid;
             config.num_threads = atoi(argv[++i]);
             if (config.num_threads > MAX_THREADS) {
                printf("WARNING: too many threads, limiting threads to %d.\n",
                       MAX_THREADS);
                config.num_threads = MAX_THREADS;
             } else if (config.num_threads < 0) config.num_threads = 0;
        } else if (!strcmp(argv[i],"--cluster")) {
            config.cluster_mode = 1;
        } else if (!strcmp(argv[i],"--help")) {
            exit_status = 0;
            goto usage;
        } else {
            /* Assume the user meant to provide an option when the arg starts
             * with a dash. We're done otherwise and should use the remainder
             * as the command and arguments for running the benchmark. */
            if (argv[i][0] == '-') goto invalid;
            return i;
        }
    }

    return i;

invalid:
    printf("Invalid option \"%s\" or option argument missing\n\n",argv[i]);

usage:
    printf(
"Usage: redis-benchmark [-h <host>] [-p <port>] [-c <clients>] [-n <requests>] [-k <boolean>]\n\n"
" -h <hostname>      Server hostname (default 127.0.0.1)\n"
" -p <port>          Server port (default 6379)\n"
" -s <socket>        Server socket (overrides host and port)\n"
" -a <password>      Password for Redis Auth\n"
" -c <clients>       Number of parallel connections (default 50)\n"
" -n <requests>      Total number of requests (default 100000)\n"
" -d <size>          Data size of SET/GET value in bytes (default 3)\n"
" --dbnum <db>       SELECT the specified db number (default 0)\n"
" -k <boolean>       1=keep alive 0=reconnect (default 1)\n"
" -r <keyspacelen>   Use random keys for SET/GET/INCR, random values for SADD\n"
"  Using this option the benchmark will expand the string __rand_int__\n"
"  inside an argument with a 12 digits number in the specified range\n"
"  from 0 to keyspacelen-1. The substitution changes every time a command\n"
"  is executed. Default tests use this to hit random keys in the\n"
"  specified range.\n"
" -P <numreq>        Pipeline <numreq> requests. Default 1 (no pipeline).\n"
" -e                 If server replies with errors, show them on stdout.\n"
"                    (no more than 1 error per second is displayed)\n"
" -q                 Quiet. Just show query/sec values\n"
" --precision        Number of decimal places to display in latency output (default 0)\n"
" --csv              Output in CSV format\n"
" -l                 Loop. Run the tests forever\n"
" -t <tests>         Only run the comma separated list of tests. The test\n"
"                    names are the same as the ones produced as output.\n"
" -I                 Idle mode. Just open N idle connections and wait.\n\n"
"Examples:\n\n"
" Run the benchmark with the default configuration against 127.0.0.1:6379:\n"
"   $ redis-benchmark\n\n"
" Use 20 parallel clients, for a total of 100k requests, against 192.168.1.1:\n"
"   $ redis-benchmark -h 192.168.1.1 -p 6379 -n 100000 -c 20\n\n"
" Fill 127.0.0.1:6379 with about 1 million keys only using the SET test:\n"
"   $ redis-benchmark -t set -n 1000000 -r 100000000\n\n"
" Benchmark 127.0.0.1:6379 for a few commands producing CSV output:\n"
"   $ redis-benchmark -t ping,set,get -n 100000 --csv\n\n"
" Benchmark a specific command line:\n"
"   $ redis-benchmark -r 10000 -n 10000 eval 'return redis.call(\"ping\")' 0\n\n"
" Fill a list with 10000 random elements:\n"
"   $ redis-benchmark -r 10000 -n 10000 lpush mylist __rand_int__\n\n"
" On user specified command lines __rand_int__ is replaced with a random integer\n"
" with a range of values selected by the -r option.\n"
    );
    exit(exit_status);
}

int showThroughput(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);
    int liveclients = 0;
    int requests_finished = 0;
    if (!config.num_threads) {
        liveclients = config.liveclients;
        requests_finished = config.requests_finished;
    } else {
        atomicGet(config.liveclients, liveclients);
        atomicGet(config.requests_finished, requests_finished);
    }

    if (liveclients == 0 && requests_finished != config.requests) {
        fprintf(stderr,"All clients disconnected... aborting.\n");
        exit(1);
    }
    if (config.csv) return 250;
    if (config.idlemode == 1) {
        printf("clients: %d\r", config.liveclients);
        fflush(stdout);
	return 250;
    }
    float dt = (float)(mstime()-config.start)/1000.0;
    float rps = (float)requests_finished/dt;
    printf("%s: %.2f\r", config.title, rps);
    fflush(stdout);
    return 250; /* every 250ms */
}

/* Return true if the named test was selected using the -t command line
 * switch, or if all the tests are selected (no -t passed by user). */
int test_is_selected(char *name) {
    char buf[256];
    int l = strlen(name);

    if (config.tests == NULL) return 1;
    buf[0] = ',';
    memcpy(buf+1,name,l);
    buf[l+1] = ',';
    buf[l+2] = '\0';
    return strstr(config.tests,buf) != NULL;
}

int main(int argc, const char **argv) {
    int i;
    char *data, *cmd;
    int len;

    client c;

    srandom(time(NULL));
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    config.numclients = 50;
    config.requests = 100000;
    config.liveclients = 0;
    config.el = aeCreateEventLoop(1024*10);
    aeCreateTimeEvent(config.el,1,showThroughput,NULL,NULL);
    config.keepalive = 1;
    config.datasize = 3;
    config.pipeline = 1;
    config.showerrors = 0;
    config.randomkeys = 0;
    config.randomkeys_keyspacelen = 0;
    config.quiet = 0;
    config.csv = 0;
    config.loop = 0;
    config.idlemode = 0;
    config.latency = NULL;
    config.clients = listCreate();
    config.hostip = "127.0.0.1";
    config.hostport = 6379;
    config.hostsocket = NULL;
    config.tests = NULL;
    config.dbnum = 0;
    config.auth = NULL;
    config.precision = 1;
    config.num_threads = 0;
    config.threads = NULL;
    config.cluster_mode = 0;
    config.cluster_node_count = 0;
    config.cluster_nodes = NULL;

    i = parseOptions(argc,argv);
    argc -= i;
    argv += i;

    config.latency = zmalloc(sizeof(long long)*config.requests);

    if (config.cluster_mode) {
        /* Fetch cluster configuration. */
        if (!fetchClusterConfiguration() || !config.cluster_nodes) {
            if (!config.hostsocket) {
                fprintf(stderr, "Failed to fetch cluster configuration from "
                                "%s:%d\n", config.hostip, config.hostport);
            } else {
                fprintf(stderr, "Failed to fetch cluster configuration from "
                                "%s\n", config.hostsocket);
            }
            exit(1);
        }
        if (config.cluster_node_count <= 1) {
            fprintf(stderr, "Invalid cluster: %d node(s).\n",
                    config.cluster_node_count);
            exit(1);
        }
        printf("Cluster has %d master nodes:\n\n", config.cluster_node_count);
        int i = 0;
        for (; i < config.cluster_node_count; i++) {
            clusterNode *node = config.cluster_nodes[i];
            if (!node) {
                fprintf(stderr, "Invalid cluster node #%d\n", i);
                exit(1);
            }
            if (node->name) printf("%s ", node->name);
            printf("%s:%d\n", node->ip, node->port);
        }
        /* TODO: allow for more thrads per cluster node. */
        config.num_threads = config.cluster_node_count;
    }

    if (config.num_threads > 0) {
        pthread_mutex_init(&(config.requests_issued_mutex), NULL);
        pthread_mutex_init(&(config.requests_finished_mutex), NULL);
        pthread_mutex_init(&(config.liveclients_mutex), NULL);
    }

    if (config.keepalive == 0) {
        printf("WARNING: keepalive disabled, you probably need 'echo 1 > /proc/sys/net/ipv4/tcp_tw_reuse' for Linux and 'sudo sysctl -w net.inet.tcp.msl=1000' for Mac OS X in order to use a lot of clients/requests\n");
    }

    if (config.idlemode) {
        printf("Creating %d idle connections and waiting forever (Ctrl+C when done)\n", config.numclients);
        c = createClient("",0,NULL,-1); /* will never receive a reply */
        createMissingClients(c);
        aeMain(config.el);
        /* and will wait for every */
    }

    /* Run benchmark with command in the remainder of the arguments. */
    if (argc) {
        sds title = sdsnew(argv[0]);
        for (i = 1; i < argc; i++) {
            title = sdscatlen(title, " ", 1);
            title = sdscatlen(title, (char*)argv[i], strlen(argv[i]));
        }

        do {
            len = redisFormatCommandArgv(&cmd,argc,argv,NULL);
            benchmark(title,cmd,len);
            free(cmd);
        } while(config.loop);

        return 0;
    }

    /* Run default benchmark suite. */
    data = zmalloc(config.datasize+1);
    do {
        memset(data,'x',config.datasize);
        data[config.datasize] = '\0';

        if (test_is_selected("ping_inline") || test_is_selected("ping"))
            benchmark("PING_INLINE","PING\r\n",6);

        if (test_is_selected("ping_mbulk") || test_is_selected("ping")) {
            len = redisFormatCommand(&cmd,"PING");
            benchmark("PING_BULK",cmd,len);
            free(cmd);
        }

        if (test_is_selected("set")) {
            len = redisFormatCommand(&cmd,"SET key:{tag}:__rand_int__ %s",data);
            benchmark("SET",cmd,len);
            free(cmd);
        }

        if (test_is_selected("get")) {
            len = redisFormatCommand(&cmd,"GET key:{tag}:__rand_int__");
            benchmark("GET",cmd,len);
            free(cmd);
        }

        if (test_is_selected("incr")) {
            len = redisFormatCommand(&cmd,"INCR counter:{tag}:__rand_int__");
            benchmark("INCR",cmd,len);
            free(cmd);
        }

        if (test_is_selected("lpush")) {
            len = redisFormatCommand(&cmd,"LPUSH mylist %s",data);
            benchmark("LPUSH",cmd,len);
            free(cmd);
        }

        if (test_is_selected("rpush")) {
            len = redisFormatCommand(&cmd,"RPUSH mylist %s",data);
            benchmark("RPUSH",cmd,len);
            free(cmd);
        }

        if (test_is_selected("lpop")) {
            len = redisFormatCommand(&cmd,"LPOP mylist");
            benchmark("LPOP",cmd,len);
            free(cmd);
        }

        if (test_is_selected("rpop")) {
            len = redisFormatCommand(&cmd,"RPOP mylist");
            benchmark("RPOP",cmd,len);
            free(cmd);
        }

        if (test_is_selected("sadd")) {
            len = redisFormatCommand(&cmd,
                "SADD myset element:__rand_int__");
            benchmark("SADD",cmd,len);
            free(cmd);
        }

        if (test_is_selected("hset")) {
            len = redisFormatCommand(&cmd,
                "HSET myset:__rand_int__ element:__rand_int__ %s",data);
            benchmark("HSET",cmd,len);
            free(cmd);
        }

        if (test_is_selected("spop")) {
            len = redisFormatCommand(&cmd,"SPOP myset");
            benchmark("SPOP",cmd,len);
            free(cmd);
        }

        if (test_is_selected("lrange") ||
            test_is_selected("lrange_100") ||
            test_is_selected("lrange_300") ||
            test_is_selected("lrange_500") ||
            test_is_selected("lrange_600"))
        {
            len = redisFormatCommand(&cmd,"LPUSH mylist %s",data);
            benchmark("LPUSH (needed to benchmark LRANGE)",cmd,len);
            free(cmd);
        }

        if (test_is_selected("lrange") || test_is_selected("lrange_100")) {
            len = redisFormatCommand(&cmd,"LRANGE mylist 0 99");
            benchmark("LRANGE_100 (first 100 elements)",cmd,len);
            free(cmd);
        }

        if (test_is_selected("lrange") || test_is_selected("lrange_300")) {
            len = redisFormatCommand(&cmd,"LRANGE mylist 0 299");
            benchmark("LRANGE_300 (first 300 elements)",cmd,len);
            free(cmd);
        }

        if (test_is_selected("lrange") || test_is_selected("lrange_500")) {
            len = redisFormatCommand(&cmd,"LRANGE mylist 0 449");
            benchmark("LRANGE_500 (first 450 elements)",cmd,len);
            free(cmd);
        }

        if (test_is_selected("lrange") || test_is_selected("lrange_600")) {
            len = redisFormatCommand(&cmd,"LRANGE mylist 0 599");
            benchmark("LRANGE_600 (first 600 elements)",cmd,len);
            free(cmd);
        }

        if (test_is_selected("mset")) {
            const char *argv[21];
            argv[0] = "MSET";
            for (i = 1; i < 21; i += 2) {
                argv[i] = "key:__rand_int__";
                argv[i+1] = data;
            }
            len = redisFormatCommandArgv(&cmd,21,argv,NULL);
            benchmark("MSET (10 keys)",cmd,len);
            free(cmd);
        }

        if (!config.csv) printf("\n");
    } while(config.loop);

    return 0;
}
