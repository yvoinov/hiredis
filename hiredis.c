/*
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <assert.h>
#include <errno.h>

#include "hiredis.h"
#include "anet.h"
#include "sds.h"

typedef struct redisReader {
    struct redisReplyObjectFunctions *fn;
    sds error; /* holds optional error */
    void *reply; /* holds temporary reply */

    sds buf; /* read buffer */
    unsigned int pos; /* buffer cursor */

    redisReadTask *rlist; /* list of items to process */
    unsigned int rlen; /* list length */
    unsigned int rpos; /* list cursor */
} redisReader;

static redisReply *createReplyObject(int type, sds reply);
static void *createStringObject(redisReadTask *task, char *str, size_t len);
static void *createArrayObject(redisReadTask *task, int elements);
static void *createIntegerObject(redisReadTask *task, long long value);
static void *createNilObject(redisReadTask *task);
static void redisSetReplyReaderError(redisReader *r, sds err);

/* Default set of functions to build the reply. */
static redisReplyFunctions defaultFunctions = {
    createStringObject,
    createArrayObject,
    createIntegerObject,
    createNilObject,
    freeReplyObject
};

/* We simply abort on out of memory */
static void redisOOM(void) {
    fprintf(stderr,"Out of memory in hiredis.c");
    exit(1);
}

/* Create a reply object */
static redisReply *createReplyObject(int type, sds reply) {
    redisReply *r = calloc(sizeof(*r),1);

    if (!r) redisOOM();
    r->type = type;
    r->reply = reply;
    return r;
}

/* Free a reply object */
void freeReplyObject(void *reply) {
    redisReply *r = reply;
    size_t j;

    switch(r->type) {
    case REDIS_REPLY_INTEGER:
        break; /* Nothing to free */
    case REDIS_REPLY_ARRAY:
        for (j = 0; j < r->elements; j++)
            if (r->element[j]) freeReplyObject(r->element[j]);
        free(r->element);
        break;
    default:
        if (r->reply != NULL)
            sdsfree(r->reply);
        break;
    }
    free(r);
}

static void *createStringObject(redisReadTask *task, char *str, size_t len) {
    redisReply *r = createReplyObject(task->type,sdsnewlen(str,len));
    assert(task->type == REDIS_REPLY_ERROR ||
           task->type == REDIS_REPLY_STATUS ||
           task->type == REDIS_REPLY_STRING);

    /* for API compat, set STATUS to STRING */
    if (task->type == REDIS_REPLY_STATUS)
        r->type = REDIS_REPLY_STRING;

    if (task->parent) {
        redisReply *parent = task->parent;
        assert(parent->type == REDIS_REPLY_ARRAY);
        parent->element[task->idx] = r;
    }
    return r;
}

static void *createArrayObject(redisReadTask *task, int elements) {
    redisReply *r = createReplyObject(REDIS_REPLY_ARRAY,NULL);
    r->elements = elements;
    if ((r->element = calloc(sizeof(redisReply*),elements)) == NULL)
        redisOOM();
    if (task->parent) {
        redisReply *parent = task->parent;
        assert(parent->type == REDIS_REPLY_ARRAY);
        parent->element[task->idx] = r;
    }
    return r;
}

static void *createIntegerObject(redisReadTask *task, long long value) {
    redisReply *r = createReplyObject(REDIS_REPLY_INTEGER,NULL);
    r->integer = value;
    if (task->parent) {
        redisReply *parent = task->parent;
        assert(parent->type == REDIS_REPLY_ARRAY);
        parent->element[task->idx] = r;
    }
    return r;
}

static void *createNilObject(redisReadTask *task) {
    redisReply *r = createReplyObject(REDIS_REPLY_NIL,NULL);
    if (task->parent) {
        redisReply *parent = task->parent;
        assert(parent->type == REDIS_REPLY_ARRAY);
        parent->element[task->idx] = r;
    }
    return r;
}

static char *readBytes(redisReader *r, unsigned int bytes) {
    char *p;
    if (sdslen(r->buf)-r->pos >= bytes) {
        p = r->buf+r->pos;
        r->pos += bytes;
        return p;
    }
    return NULL;
}

static char *readLine(redisReader *r, int *_len) {
    char *p, *s = strstr(r->buf+r->pos,"\r\n");
    int len;
    if (s != NULL) {
        p = r->buf+r->pos;
        len = s-(r->buf+r->pos);
        r->pos += len+2; /* skip \r\n */
        if (_len) *_len = len;
        return p;
    }
    return NULL;
}

static int processLineItem(redisReader *r) {
    redisReadTask *cur = &(r->rlist[r->rpos]);
    void *obj;
    char *p;
    int len;

    if ((p = readLine(r,&len)) != NULL) {
        if (cur->type == REDIS_REPLY_INTEGER) {
            obj = r->fn->createInteger(cur,strtoll(p,NULL,10));
        } else {
            obj = r->fn->createString(cur,p,len);
        }

        /* If there is no root yet, register this object as root. */
        if (r->reply == NULL)
            r->reply = obj;
        r->rpos++;
        return 0;
    }
    return -1;
}

static int processBulkItem(redisReader *r) {
    redisReadTask *cur = &(r->rlist[r->rpos]);
    void *obj = NULL;
    char *p, *s;
    long len;
    unsigned long bytelen;

    p = r->buf+r->pos;
    s = strstr(p,"\r\n");
    if (s != NULL) {
        p = r->buf+r->pos;
        bytelen = s-(r->buf+r->pos)+2; /* include \r\n */
        len = strtol(p,NULL,10);

        if (len < 0) {
            /* The nil object can always be created. */
            obj = r->fn->createNil(cur);
        } else {
            /* Only continue when the buffer contains the entire bulk item. */
            bytelen += len+2; /* include \r\n */
            if (r->pos+bytelen <= sdslen(r->buf)) {
                obj = r->fn->createString(cur,s+2,len);
            }
        }

        /* Proceed when obj was created. */
        if (obj != NULL) {
            r->pos += bytelen;
            if (r->reply == NULL)
                r->reply = obj;
            r->rpos++;
            return 0;
        }
    }
    return -1;
}

static int processMultiBulkItem(redisReader *r) {
    redisReadTask *cur = &(r->rlist[r->rpos]);
    void *obj;
    char *p;
    long elements, j;

    if ((p = readLine(r,NULL)) != NULL) {
        elements = strtol(p,NULL,10);
        if (elements == -1) {
            obj = r->fn->createNil(cur);
        } else {
            obj = r->fn->createArray(cur,elements);

            /* Modify read list when there are more than 0 elements. */
            if (elements > 0) {
                /* Append elements to the read list. */
                r->rlen += elements;
                if ((r->rlist = realloc(r->rlist,sizeof(redisReadTask)*r->rlen)) == NULL)
                    redisOOM();

                /* Move existing items backwards. */
                memmove(&(r->rlist[r->rpos+1+elements]),
                        &(r->rlist[r->rpos+1]),
                        (r->rlen-(r->rpos+1+elements))*sizeof(redisReadTask));

                /* Populate new read items. */
                redisReadTask *t;
                for (j = 0; j < elements; j++) {
                    t = &(r->rlist[r->rpos+1+j]);
                    t->type = -1;
                    t->parent = obj;
                    t->idx = j;
                }
            }
        }

        if (obj != NULL) {
            if (r->reply == NULL)
                r->reply = obj;
            r->rpos++;
            return 0;
        }
    }
    return -1;
}

static int processItem(redisReader *r) {
    redisReadTask *cur = &(r->rlist[r->rpos]);
    char *p;
    sds byte;

    /* check if we need to read type */
    if (cur->type < 0) {
        if ((p = readBytes(r,1)) != NULL) {
            switch (p[0]) {
            case '-':
                cur->type = REDIS_REPLY_ERROR;
                break;
            case '+':
                cur->type = REDIS_REPLY_STATUS;
                break;
            case ':':
                cur->type = REDIS_REPLY_INTEGER;
                break;
            case '$':
                cur->type = REDIS_REPLY_STRING;
                break;
            case '*':
                cur->type = REDIS_REPLY_ARRAY;
                break;
            default:
                byte = sdscatrepr(sdsempty(),p,1);
                redisSetReplyReaderError(r,sdscatprintf(sdsempty(),
                    "protocol error, got %s as reply type byte", byte));
                sdsfree(byte);
                return -1;
            }
        } else {
            /* could not consume 1 byte */
            return -1;
        }
    }

    /* process typed item */
    switch(cur->type) {
    case REDIS_REPLY_ERROR:
    case REDIS_REPLY_STATUS:
    case REDIS_REPLY_INTEGER:
        return processLineItem(r);
    case REDIS_REPLY_STRING:
        return processBulkItem(r);
    case REDIS_REPLY_ARRAY:
        return processMultiBulkItem(r);
    default:
        redisSetReplyReaderError(r,sdscatprintf(sdsempty(),
            "unknown item type '%d'", cur->type));
        return -1;
    }
}

void *redisReplyReaderCreate(redisReplyFunctions *fn) {
    redisReader *r = calloc(sizeof(redisReader),1);
    r->error = NULL;
    r->fn = fn == NULL ? &defaultFunctions : fn;
    r->buf = sdsempty();
    r->rlist = malloc(sizeof(redisReadTask)*1);
    return r;
}

/* External libraries wrapping hiredis might need access to the temporary
 * variable while the reply is built up. When the reader contains an
 * object in between receiving some bytes to parse, this object might
 * otherwise be free'd by garbage collection. */
void *redisReplyReaderGetObject(void *reader) {
    redisReader *r = reader;
    return r->reply;
}

void redisReplyReaderFree(void *reader) {
    redisReader *r = reader;
    if (r->error != NULL)
        sdsfree(r->error);
    if (r->reply != NULL)
        r->fn->freeObject(r->reply);
    if (r->buf != NULL)
        sdsfree(r->buf);
    if (r->rlist != NULL)
        free(r->rlist);
    free(r);
}

static void redisSetReplyReaderError(redisReader *r, sds err) {
    if (r->reply != NULL)
        r->fn->freeObject(r->reply);

    /* Clear remaining buffer when we see a protocol error. */
    if (r->buf != NULL) {
        sdsfree(r->buf);
        r->buf = sdsempty();
        r->pos = 0;
    }
    r->rlen = r->rpos = 0;
    r->error = err;
}

char *redisReplyReaderGetError(void *reader) {
    redisReader *r = reader;
    return r->error;
}

void redisReplyReaderFeed(void *reader, char *buf, int len) {
    redisReader *r = reader;

    /* Copy the provided buffer. */
    if (buf != NULL && len >= 1)
        r->buf = sdscatlen(r->buf,buf,len);
}

int redisReplyReaderGetReply(void *reader, void **reply) {
    redisReader *r = reader;
    if (reply != NULL) *reply = NULL;

    /* When the buffer is empty, there will never be a reply. */
    if (sdslen(r->buf) == 0)
        return REDIS_OK;

    /* Create first item to process when the item list is empty. */
    if (r->rlen == 0) {
        r->rlist = realloc(r->rlist,sizeof(redisReadTask)*1);
        r->rlist[0].type = -1;
        r->rlist[0].parent = NULL;
        r->rlist[0].idx = -1;
        r->rlen = 1;
        r->rpos = 0;
    }

    /* Process items in reply. */
    while (r->rpos < r->rlen)
        if (processItem(r) < 0)
            break;

    /* Discard the consumed part of the buffer. */
    if (r->pos > 0) {
        if (r->pos == sdslen(r->buf)) {
            /* sdsrange has a quirck on this edge case. */
            sdsfree(r->buf);
            r->buf = sdsempty();
        } else {
            r->buf = sdsrange(r->buf,r->pos,sdslen(r->buf));
        }
        r->pos = 0;
    }

    /* Emit a reply when there is one. */
    if (r->rpos == r->rlen) {
        void *aux = r->reply;
        r->reply = NULL;

        /* Destroy the buffer when it is empty and is quite large. */
        if (sdslen(r->buf) == 0 && sdsavail(r->buf) > 16*1024) {
            sdsfree(r->buf);
            r->buf = sdsempty();
            r->pos = 0;
        }

        /* Set list of items to read to be empty. */
        r->rlen = r->rpos = 0;

        /* Check if there actually *is* a reply. */
        if (r->error != NULL) {
            return REDIS_ERR;
        } else {
            if (reply != NULL) *reply = aux;
        }
    }
    return REDIS_OK;
}

/* Helper function for redisCommand(). It's used to append the next argument
 * to the argument vector. */
static void addArgument(sds a, char ***argv, int *argc) {
    (*argc)++;
    if ((*argv = realloc(*argv, sizeof(char*)*(*argc))) == NULL) redisOOM();
    (*argv)[(*argc)-1] = a;
}

/* Execute a command. This function is printf alike:
 *
 * %s represents a C nul terminated string you want to interpolate
 * %b represents a binary safe string
 *
 * When using %b you need to provide both the pointer to the string
 * and the length in bytes. Examples:
 *
 * redisCommand("GET %s", mykey);
 * redisCommand("SET %s %b", mykey, somevalue, somevalue_len);
 *
 * RETURN VALUE:
 *
 * The returned value is a redisReply object that must be freed using the
 * redisFreeReply() function.
 *
 * given a redisReply "reply" you can test if there was an error in this way:
 *
 * if (reply->type == REDIS_REPLY_ERROR) {
 *     printf("Error in request: %s\n", reply->reply);
 * }
 *
 * The replied string itself is in reply->reply if the reply type is
 * a REDIS_REPLY_STRING. If the reply is a multi bulk reply then
 * reply->type is REDIS_REPLY_ARRAY and you can access all the elements
 * in this way:
 *
 * for (i = 0; i < reply->elements; i++)
 *     printf("%d: %s\n", i, reply->element[i]);
 *
 * Finally when type is REDIS_REPLY_INTEGER the long long integer is
 * stored at reply->integer.
 */
static sds redisFormatCommand(const char *format, va_list ap) {
    size_t size;
    const char *arg, *c = format;
    sds cmd = sdsempty();     /* whole command buffer */
    sds current = sdsempty(); /* current argument */
    char **argv = NULL;
    int argc = 0, j;

    /* Build the command string accordingly to protocol */
    while(*c != '\0') {
        if (*c != '%' || c[1] == '\0') {
            if (*c == ' ') {
                if (sdslen(current) != 0) {
                    addArgument(current, &argv, &argc);
                    current = sdsempty();
                }
            } else {
                current = sdscatlen(current,c,1);
            }
        } else {
            switch(c[1]) {
            case 's':
                arg = va_arg(ap,char*);
                current = sdscat(current,arg);
                break;
            case 'b':
                arg = va_arg(ap,char*);
                size = va_arg(ap,size_t);
                current = sdscatlen(current,arg,size);
                break;
            case '%':
                cmd = sdscat(cmd,"%");
                break;
            }
            c++;
        }
        c++;
    }

    /* Add the last argument if needed */
    if (sdslen(current) != 0)
        addArgument(current, &argv, &argc);
    else
        sdsfree(current);

    /* Build the command at protocol level */
    cmd = sdscatprintf(cmd,"*%d\r\n",argc);
    for (j = 0; j < argc; j++) {
        cmd = sdscatprintf(cmd,"$%zu\r\n",sdslen(argv[j]));
        cmd = sdscatlen(cmd,argv[j],sdslen(argv[j]));
        cmd = sdscatlen(cmd,"\r\n",2);
        sdsfree(argv[j]);
    }
    free(argv);
    return cmd;
}

static int redisContextConnect(redisContext *c, const char *ip, int port) {
    char err[ANET_ERR_LEN];
    if (c->flags & REDIS_BLOCK) {
        c->fd = anetTcpConnect(err,(char*)ip,port);
    } else {
        c->fd = anetTcpNonBlockConnect(err,(char*)ip,port);
    }

    if (c->fd == ANET_ERR) {
        c->error = sdsnew(err);
        return REDIS_ERR;
    }
    if (anetTcpNoDelay(err,c->fd) == ANET_ERR) {
        c->error = sdsnew(err);
        return REDIS_ERR;
    }
    return REDIS_OK;
}

static redisContext *redisContextInit(redisReplyFunctions *fn) {
    redisContext *c = malloc(sizeof(*c));
    c->error = NULL;
    c->obuf = sdsempty();
    c->fn = fn == NULL ? &defaultFunctions : fn;
    c->reader = redisReplyReaderCreate(c->fn);
    c->callbacks = calloc(sizeof(redisCallback),1);
    c->clen = 1;
    c->cpos = 0;
    return c;
}

/* Connect to a Redis instance. On error the field error in the returned
 * context will be set to the return value of the error function.
 * When no set of reply functions is given, the default set will be used. */
redisContext *redisConnect(const char *ip, int port, redisReplyFunctions *fn) {
    redisContext *c = redisContextInit(fn);
    c->flags |= REDIS_BLOCK;
    redisContextConnect(c,ip,port);
    return c;
}

redisContext *redisConnectNonBlock(const char *ip, int port, redisReplyFunctions *fn) {
    redisContext *c = redisContextInit(fn);
    c->flags &= ~REDIS_BLOCK;
    redisContextConnect(c,ip,port);
    return c;
}

/* Use this function to handle a read event on the descriptor. It will try
 * and read some bytes from the socket and feed them to the reply parser.
 *
 * After this function is called, you may use redisContextReadReply to
 * see if there is a reply available. */
int redisBufferRead(redisContext *c) {
    char buf[2048];
    int nread = read(c->fd,buf,sizeof(buf));
    if (nread == -1) {
        if (errno == EAGAIN) {
            /* Try again later */
        } else {
            /* Set error in context */
            c->error = sdscatprintf(sdsempty(),
                "Error reading from socket: %s", strerror(errno));
            return REDIS_ERR;
        }
    } else if (nread == 0) {
        c->error = sdscatprintf(sdsempty(),
            "Server closed the connection");
        return REDIS_ERR;
    } else {
        redisReplyReaderFeed(c->reader,buf,nread);
    }
    return REDIS_OK;
}

static void redisPopCallback(redisContext *c) {
    if (c->cpos > 1) {
        memmove(&c->callbacks[0],&c->callbacks[1],(c->cpos-1)*sizeof(redisCallback));
    }
    c->cpos--;
}

int redisGetReply(redisContext *c, void **reply) {
    redisPopCallback(c);
    if (redisReplyReaderGetReply(c->reader,reply) == REDIS_ERR) {
        /* Copy the (protocol) error from the reader to the context. */
        c->error = sdsnew(((redisReader*)c->reader)->error);
        return REDIS_ERR;
    }
    return REDIS_OK;
}

int redisProcessCallbacks(redisContext *c) {
    void *reply = NULL;
    redisCallback cb;

    do {
        cb = c->callbacks[0];
        if (redisGetReply(c,&reply) == REDIS_ERR)
            return REDIS_ERR;

        /* Fire callback when there is a reply. */
        if (reply != NULL) {
            if (cb.fn != NULL) {
                cb.fn(c,reply,cb.privdata);
            } else {
                c->fn->freeObject(reply);
            }
        }
    } while (reply != NULL);
    return REDIS_OK;
}

/* Use this function to try and write the entire output buffer to the
 * descriptor. Returns 1 when the entire buffer was written, 0 otherwise. */
int redisBufferWrite(redisContext *c, int *done) {
    int nwritten = write(c->fd,c->obuf,sdslen(c->obuf));
    if (nwritten == -1) {
        if (errno == EAGAIN) {
            /* Try again later */
        } else {
            /* Set error in context */
            c->error = sdscatprintf(sdsempty(),
                "Error writing to socket: %s", strerror(errno));
            return REDIS_ERR;
        }
    } else if (nwritten > 0) {
        if (nwritten == (signed)sdslen(c->obuf)) {
            sdsfree(c->obuf);
            c->obuf = sdsempty();
        } else {
            c->obuf = sdsrange(c->obuf,nwritten,-1);
        }
    }
    if (done != NULL) *done = (sdslen(c->obuf) == 0);
    return REDIS_OK;
}

static int redisCommandWriteBlock(redisContext *c, void **reply, char *str, size_t len) {
    int wdone = 0;
    void *aux = NULL;
    assert(c->flags & REDIS_BLOCK);
    c->obuf = sdscatlen(c->obuf,str,len);

    /* Write until done. */
    do {
        if (redisBufferWrite(c,&wdone) == REDIS_ERR)
            return REDIS_ERR;
    } while (!wdone);

    /* Read until there is a reply. */
    do {
        if (redisBufferRead(c) == REDIS_ERR)
            return REDIS_ERR;
        if (redisGetReply(c,&aux) == REDIS_ERR)
            return REDIS_ERR;
    } while (aux == NULL);

    /* Set reply object. */
    if (reply != NULL)
        *reply = aux;

    return REDIS_OK;
}

static int redisCommandWriteNonBlock(redisContext *c, redisCallback *cb, char *str, size_t len) {
    assert(!(c->flags & REDIS_BLOCK));
    c->obuf = sdscatlen(c->obuf,str,len);

    /* Make sure there is space for the callback. */
    assert(c->cpos <= c->clen);
    if (c->cpos == c->clen) {
        c->clen++;
        c->callbacks = realloc(c->callbacks,c->clen*sizeof(redisCallback));
    }

    if (cb != NULL) {
        c->callbacks[c->cpos] = *cb;
    } else {
        memset(&c->callbacks[c->cpos],0,sizeof(redisCallback));
    }
    c->cpos++;

    return REDIS_OK;
}

/* Write a formatted command to the output buffer. If the given context is
 * blocking, immediately read the reply into the "reply" pointer. When the
 * context is non-blocking, the "reply" pointer will not be used and a
 * NULL callback will be appended to the list of callbacks.
 *
 * Returns the reply when a reply was succesfully retrieved. Returns NULL
 * otherwise. When NULL is returned in a blocking context, provided that
 * the reply build functions did not return NULL when building the reply,
 * the error field in the context will be set. */
void *redisCommand(redisContext *c, const char *format, ...) {
    va_list ap;
    sds cmd;
    void *reply = NULL;
    va_start(ap,format);
    cmd = redisFormatCommand(format,ap);
    va_end(ap);

    if (c->flags & REDIS_BLOCK) {
        if (redisCommandWriteBlock(c,&reply,cmd,sdslen(cmd)) == REDIS_OK) {
            sdsfree(cmd);
            return reply;
        }
    } else {
        redisCommandWriteNonBlock(c,NULL,cmd,sdslen(cmd));
    }
    sdsfree(cmd);
    return NULL;
}

/* Write a formatted command to the output buffer. Registers the provided
 * callback function and argument in the callback list.
 *
 * Always returns NULL. In a non-blocking context this will never fail because
 * this function does not do any I/O. In a blocking context this function will
 * have no effect (a callback in a blocking context makes no sense). */
void *redisCommandWithCallback(redisContext *c, redisCallbackFn *fn, void *privdata, const char *format, ...) {
    va_list ap;
    sds cmd;
    int status;
    redisCallback cb = { fn, privdata };

    /* This function may only be used in a non-blocking context. */
    if (c->flags & REDIS_BLOCK) return NULL;

    va_start(ap,format);
    cmd = redisFormatCommand(format,ap);
    va_end(ap);

    status = redisCommandWriteNonBlock(c,&cb,cmd,sdslen(cmd));
    sdsfree(cmd);
    return NULL;
}
