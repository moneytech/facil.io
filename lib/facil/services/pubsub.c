/*
Copyright: Boaz segev, 2017
License: MIT

Feel free to copy, use and enjoy according to the license provided.
*/
#include "spnlock.inc"

#include "facil.h"
#include "fio_llist.h"
#include "fiobj.h"
#include "pubsub.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* used later on */
static int pubsub_glob_match(uint8_t *data, size_t data_len, uint8_t *pattern,
                             size_t pat_len);
/* *****************************************************************************
The Hash Map (macros and the include instruction for `fio_hashmap.h`)
***************************************************************************** */

/* the hash key type for string keys */
typedef struct {
  size_t hash;
  fiobj_s *obj;
} fio_hash_key_s;

/* define the macro to set the key type */
#define FIO_HASH_KEY_TYPE fio_hash_key_s
/* the macro that returns the key's hash value */
#define FIO_HASH_KEY2UINT(key) ((key).hash)
/* Compare the keys using length testing and `memcmp` */
#define FIO_HASH_COMPARE_KEYS(k1, k2)                                          \
  ((k1).obj == (k2).obj || fiobj_iseq((k1).obj, (k2).obj))
/* an "all bytes are zero" invalid key */
#define FIO_HASH_KEY_INVALID ((fio_hash_key_s){.obj = NULL})
/* tests if a key is the invalid key */
#define FIO_HASH_KEY_ISINVALID(key) ((key).obj == NULL)
/* creates a persistent copy of the key's string */
#define FIO_HASH_KEY_COPY(key)                                                 \
  ((fio_hash_key_s){.hash = (key).hash, .obj = fiobj_dup((key).obj)})
/* frees the allocated string */
#define FIO_HASH_KEY_DESTROY(key) (fiobj_free((key).obj))

#include "fio_hashmap.h"

/* *****************************************************************************
Channel and Client Data Structures
***************************************************************************** */

typedef struct {
  /* clients are nodes in a list. */
  fio_ls_embd_s node;
  /* a reference counter (how many messages pending) */
  size_t ref;
  /* a pointer to the channel data */
  void *parent;
  /** The on message callback. the `*msg` pointer is to a temporary object. */
  void (*on_message)(pubsub_message_s *msg);
  /** An optional callback for when a subscription is fully canceled. */
  void (*on_unsubscribe)(void *udata1, void *udata2);
  /** Opaque user data#1 */
  void *udata1;
  /** Opaque user data#2 .. using two allows some allocations to be avoided. */
  void *udata2;
} client_s;

typedef struct {
  /* the root for the client's list */
  fio_ls_embd_s clients;
  /** The channel name. */
  fiobj_s *name;
  /** Use pattern matching for channel subscription. */
  unsigned use_pattern : 1;
} channel_s;

static fio_hash_s patterns;
static fio_hash_s channels;
static fio_hash_s clients;
static fio_hash_s engines;
static spn_lock_i lock;

/* *****************************************************************************
Channel and Client Management
***************************************************************************** */

/* for engine thingy */
static void pubsub_on_channel_create(channel_s *ch);
/* for engine thingy */
static void pubsub_on_channel_destroy(channel_s *ch);

static inline void client_test4free(client_s *cl) {
  if (spn_sub(&cl->ref, 1)) {
    /* client is still being used. */
    return;
  }
  free(cl);
}

static void pubsub_deferred_unsub(void *cl_, void *ignr) {
  client_s *cl = cl_;
  cl->on_unsubscribe(cl->udata1, cl->udata2);
  client_test4free(cl);
  (void)ignr;
}

static inline uint64_t client_compute_hash(client_s client) {
  return (((((uint64_t)(client.on_message) *
             ((uint64_t)client.udata1 ^ 0x736f6d6570736575ULL)) >>
            5) |
           (((uint64_t)(client.on_unsubscribe) *
             ((uint64_t)client.udata1 ^ 0x736f6d6570736575ULL))
            << 47)) ^
          ((uint64_t)client.udata2 ^ 0x646f72616e646f6dULL));
}

static client_s *pubsub_client_new(client_s client, channel_s channel) {
  if (!client.on_message || !channel.name) {
    fprintf(stderr,
            "ERROR: (pubsub) subscription request failed. missing on of:\n"
            "       1. channel name.\n"
            "       2. massage handler.\n");
    if (client.on_unsubscribe)
      client.on_unsubscribe(client.udata1, client.udata2);
    return NULL;
  }
  uint64_t client_hash = client_compute_hash(client);
  spn_lock(&lock);
  /* ignore if client exists. */
  client_s *cl = fio_hash_find(
      &clients, (fio_hash_key_s){.hash = client_hash, .obj = channel.name});
  if (cl) {
    spn_unlock(&lock);
    return cl;
  }
  /* no client, we need a new client */
  cl = malloc(sizeof(*cl));
  if (!cl)
    perror("FATAL ERROR: (pubsub) client memory allocation error"), exit(errno);
  *cl = client;
  fio_hash_insert(
      &clients, (fio_hash_key_s){.hash = client_hash, .obj = channel.name}, cl);

  /* test for existing channel */
  uint64_t channel_hash = fiobj_sym_id(channel.name);
  fio_hash_s *ch_hashmap = (channel.use_pattern ? &patterns : &channels);
  channel_s *ch = fio_hash_find(
      ch_hashmap, (fio_hash_key_s){.hash = channel_hash, .obj = channel.name});
  if (!ch) {
    /* open new channel */
    ch = malloc(sizeof(*ch));
    if (!ch)
      perror("FATAL ERROR: (pubsub) channel memory allocation error"),
          exit(errno);
    *ch = channel;
    fio_hash_insert(ch_hashmap,
                    (fio_hash_key_s){.hash = client_hash, .obj = channel.name},
                    cl);
    pubsub_on_channel_create(ch);
  }
  cl->parent = ch;
  cl->ref = 1;
  fio_ls_embd_push(&ch->clients, &cl->node);
  spn_unlock(&lock);
  return cl;
}

/** Destroys a client (and empty channels as well) */
static void pubsub_client_destroy(client_s *client) {
  if (!client || !client->parent)
    return;
  channel_s *ch = client->parent;

  fio_hash_s *ch_hashmap = (ch->use_pattern ? &patterns : &channels);
  uint64_t channel_hash = fiobj_sym_id(ch->name);
  uint8_t is_ch_any;

  spn_lock(&lock);
  fio_ls_embd_remove(&client->node);
  is_ch_any = fio_ls_embd_any(&ch->clients);
  if (!is_ch_any) {
    channel_s *test = fio_hash_insert(
        ch_hashmap, (fio_hash_key_s){.hash = channel_hash, .obj = ch->name},
        NULL);
    if (test != ch)
      fprintf(stderr,
              "FATAL ERROR: (pubsub) channel database corruption detected.\n"),
          exit(-1);
    pubsub_on_channel_destroy(ch);
  }
  spn_unlock(&lock);
  if (client->on_unsubscribe) {
    spn_add(&client->ref, 1);
    defer(pubsub_deferred_unsub, client, NULL);
  }
  client_test4free(client);
  if (is_ch_any) {
    return;
  }
  free(ch);
}

/** finds a pointer to an existing client (matching registration details) */
static inline client_s *pubsub_client_find(client_s client, channel_s channel) {
  /* the logic is written twice due to locking logic (we don't want to release
   * the lock for `pubsub_client_new`)
   */
  if (!client.on_message || !channel.name) {
    return NULL;
  }
  uint64_t client_hash = client_compute_hash(client);
  spn_lock(&lock);
  client_s *cl = fio_hash_find(
      &clients, (fio_hash_key_s){.hash = client_hash, .obj = channel.name});
  spn_unlock(&lock);
  return cl;
}

/* *****************************************************************************
Subscription API
***************************************************************************** */

/**
 * Subscribes to a specific channel.
 *
 * Returns a subscription pointer or NULL (failure).
 */
#undef pubsub_subscribe
pubsub_sub_pt pubsub_subscribe(struct pubsub_subscribe_args args) {
  channel_s channel = {.name = args.channel, .use_pattern = args.use_pattern};
  client_s client = {.on_message = args.on_message,
                     .on_unsubscribe = args.on_unsubscribe,
                     .udata1 = args.udata1,
                     .udata2 = args.udata2};
  return (pubsub_sub_pt)pubsub_client_new(client, channel);
}

/**
 * This helper searches for an existing subscription.
 *
 * Use with care, NEVER call `pubsub_unsubscribe` more times than you have
 * called `pubsub_subscribe`, since the subscription handle memory is realesed
 * onnce the reference count reaches 0.
 *
 * Returns a subscription pointer or NULL (none found).
 */
#undef pubsub_find_sub
pubsub_sub_pt pubsub_find_sub(struct pubsub_subscribe_args args) {
  channel_s channel = {.name = args.channel, .use_pattern = args.use_pattern};
  client_s client = {.on_message = args.on_message,
                     .on_unsubscribe = args.on_unsubscribe,
                     .udata1 = args.udata1,
                     .udata2 = args.udata2};
  return (pubsub_sub_pt)pubsub_client_find(client, channel);
}
#define pubsub_find_sub(...)                                                   \
  pubsub_find_sub((struct pubsub_subscribe_args){__VA_ARGS__})

/**
 * Unsubscribes from a specific subscription.
 *
 * Returns 0 on success and -1 on failure.
 */
int pubsub_unsubscribe(pubsub_sub_pt subscription) {
  if (!subscription)
    return -1;
  pubsub_client_destroy((client_s *)subscription);
  return 0;
}

/**
 * Publishes a message to a channel belonging to a pub/sub service (engine).
 *
 * Returns 0 on success and -1 on failure.
 */
#undef pubsub_publish
int pubsub_publish(struct pubsub_message_s m) {
  if (!m.channel || !m.message)
    return -1;
  if (!m.engine)
    m.engine = PUBSUB_DEFAULT_ENGINE;
  if (!m.engine)
    m.engine = PUBSUB_CLUSTER_ENGINE;
  if (!m.engine)
    fprintf(stderr, "FATAL ERROR: (pubsub) engine pointer data corrupted! \n"),
        exit(-1);
  return m.engine->publish(m.engine, m.channel, m.message);
}
#define pubsub_publish(...)                                                    \
  pubsub_publish((struct pubsub_message_s){__VA_ARGS__})

/* *****************************************************************************
Engine handling and Management
***************************************************************************** */

/* runs in lock(!) let'm all know */
static void pubsub_on_channel_create(channel_s *ch) {
  FIO_HASH_FOR_LOOP(&engines, e_) {
    if (!e_)
      continue;
    pubsub_engine_s *e = e_->obj;
    e->subscribe(e, ch->name, ch->use_pattern);
  }
}

/* runs in lock(!) let'm all know */
static void pubsub_on_channel_destroy(channel_s *ch) {
  FIO_HASH_FOR_LOOP(&engines, e_) {
    if (!e_)
      continue;
    pubsub_engine_s *e = e_->obj;
    e->unsubscribe(e, ch->name, ch->use_pattern);
  }
}

/** Registers an engine, so it's callback can be called. */
void pubsub_engine_register(pubsub_engine_s *engine) {
  spn_lock(&lock);
  fio_hash_insert(&engines,
                  (fio_hash_key_s){.hash = (size_t)engine, .obj = fiobj_null()},
                  engine);
  spn_unlock(&lock);
}

/** Unregisters an engine, so it could be safely destroyed. */
void pubsub_engine_deregister(pubsub_engine_s *engine) {
  spn_lock(&lock);
  if (PUBSUB_DEFAULT_ENGINE == engine)
    PUBSUB_DEFAULT_ENGINE = PUBSUB_CLUSTER_ENGINE;
  fio_hash_insert(&engines,
                  (fio_hash_key_s){.hash = (size_t)engine, .obj = fiobj_null()},
                  NULL);
  spn_unlock(&lock);
}

/* *****************************************************************************
Single Process Engine and `pubsub_defer`
***************************************************************************** */

typedef struct {
  size_t ref;
  fiobj_s *channel;
  fiobj_s *msg;
} msg_wrapper_s;

typedef struct {
  msg_wrapper_s *wrapper;
  pubsub_message_s msg;
} msg_container_s;

static void msg_wrapper_free(msg_wrapper_s *m) {
  if (spn_sub(&m->ref, 1))
    return;
  fiobj_free(m->channel);
  fiobj_free(m->msg);
  free(m);
}

/* calls a client's `on_message` callback */
void pubsub_en_process_deferred_on_message(void *cl_, void *m_) {
  msg_wrapper_s *m = m_;
  client_s *cl = cl_;
  msg_container_s arg = {.wrapper = m,
                         .msg = {
                             .channel = m->channel,
                             .message = m->msg,
                             .subscription = (pubsub_sub_pt)cl,
                             .udata1 = cl->udata1,
                             .udata2 = cl->udata1,
                         }};
  cl->on_message(&arg.msg);
  msg_wrapper_free(m);
  client_test4free(cl_);
}

/* Must subscribe channel. Failures are ignored. */
void pubsub_en_process_subscribe(const pubsub_engine_s *eng, fiobj_s *channel,
                                 uint8_t use_pattern) {
  (void)eng;
  (void)channel;
  (void)use_pattern;
}

/* Must unsubscribe channel. Failures are ignored. */
void pubsub_en_process_unsubscribe(const pubsub_engine_s *eng, fiobj_s *channel,
                                   uint8_t use_pattern) {
  (void)eng;
  (void)channel;
  (void)use_pattern;
}
/** Should return 0 on success and -1 on failure. */
int pubsub_en_process_publish(const pubsub_engine_s *eng, fiobj_s *channel,
                              fiobj_s *msg) {
  uint64_t channel_hash = fiobj_sym_id(channel);
  msg_wrapper_s *m = malloc(sizeof(*m));
  int ret = -1;
  if (!m)
    perror("FATAL ERROR: (pubsub) couldn't allocate message wrapper"),
        exit(errno);
  *m = (msg_wrapper_s){
      .ref = 1, .channel = fiobj_dup(channel), .msg = fiobj_dup(msg)};
  spn_lock(&lock);
  {
    /* test for direct match */
    channel_s *ch = fio_hash_find(
        &channels, (fio_hash_key_s){.hash = channel_hash, .obj = channel});
    if (ch) {
      ret = 0;
      FIO_LS_EMBD_FOR(&ch->clients, cl_) {
        client_s *cl = FIO_LS_EMBD_OBJ(client_s, node, cl_);
        spn_add(&m->ref, 1);
        spn_add(&cl->ref, 1);
        defer(pubsub_en_process_deferred_on_message, cl, m);
      }
    }
  }
  /* test for pattern match */
  fio_cstr_s ch_str = fiobj_obj2cstr(channel);
  FIO_HASH_FOR_LOOP(&patterns, ch_) {
    channel_s *ch = (channel_s *)ch_->obj;
    fio_cstr_s tmp = fiobj_obj2cstr(ch->name);
    if (pubsub_glob_match(ch_str.bytes, ch_str.len, tmp.bytes, tmp.len)) {
      ret = 0;
      FIO_LS_EMBD_FOR(&ch->clients, cl_) {
        client_s *cl = FIO_LS_EMBD_OBJ(client_s, node, cl_);
        spn_add(&m->ref, 1);
        spn_add(&cl->ref, 1);
        defer(pubsub_en_process_deferred_on_message, cl, m);
      }
    }
  }
finish:
  spn_unlock(&lock);
  return ret;
  (void)eng;
  (void)channel;
  (void)msg;
  return -1;
}

const pubsub_engine_s PUBSUB_PROCESS_ENGINE_S = {
    .subscribe = pubsub_en_process_subscribe,
    .unsubscribe = pubsub_en_process_unsubscribe,
    .publish = pubsub_en_process_publish,
};

const pubsub_engine_s *PUBSUB_PROCESS_ENGINE = &PUBSUB_PROCESS_ENGINE_S;

/**
 * defers message hadling if it can't be performed (i.e., resource is busy) or
 * should be fragmented (allowing large tasks to be broken down).
 *
 * This should only be called from within the `on_message` callback.
 *
 * It's recommended that the `on_message` callback return immediately following
 * this function call, as code might run concurrently.
 *
 * Uses reference counting for zero copy.
 *
 * It's impossible to use a different `on_message` callbck without resorting to
 * memory allocations... so when in need, manage routing withing the
 * `on_message` callback.
 */
void pubsub_defer(pubsub_message_s *msg) {
  msg_container_s *arg = FIO_LS_EMBD_OBJ(msg_container_s, msg, msg);
  spn_add(&arg->wrapper->ref, 1);
  spn_add(&((client_s *)arg->msg.subscription)->ref, 1);
  defer(pubsub_en_process_deferred_on_message, arg->msg.subscription,
        arg->wrapper);
}

/* *****************************************************************************
Cluster Engine
***************************************************************************** */

/* Must subscribe channel. Failures are ignored. */
void pubsub_en_cluster_subscribe(const pubsub_engine_s *eng, fiobj_s *channel,
                                 uint8_t use_pattern) {
  (void)eng;
  (void)channel;
  (void)use_pattern;
}

/* Must unsubscribe channel. Failures are ignored. */
void pubsub_en_cluster_unsubscribe(const pubsub_engine_s *eng, fiobj_s *channel,
                                   uint8_t use_pattern) {
  (void)eng;
  (void)channel;
  (void)use_pattern;
}
/** Should return 0 on success and -1 on failure. */
int pubsub_en_cluster_publish(const pubsub_engine_s *eng, fiobj_s *channel,
                              fiobj_s *msg) {

  (void)eng;
  (void)channel;
  (void)msg;
  return -1;
}

const pubsub_engine_s PUBSUB_CLUSTER_ENGINE_S = {
    .subscribe = pubsub_en_cluster_subscribe,
    .unsubscribe = pubsub_en_cluster_unsubscribe,
    .publish = pubsub_en_cluster_publish,
};

const pubsub_engine_s *PUBSUB_CLUSTER_ENGINE = &PUBSUB_CLUSTER_ENGINE_S;

/* *****************************************************************************
Glob Matching Helper
***************************************************************************** */

/** A binary glob matching helper. Returns 1 on match, otherwise returns 0. */
static int pubsub_glob_match(uint8_t *data, size_t data_len, uint8_t *pattern,
                             size_t pat_len) {
  /* adapted and rewritten, with thankfulness, from the code at:
   * https://github.com/opnfv/kvmfornfv/blob/master/kernel/lib/glob.c
   *
   * Original version's copyright:
   * Copyright 2015 Open Platform for NFV Project, Inc. and its contributors
   * Under the MIT license.
   */

  /*
   * Backtrack to previous * on mismatch and retry starting one
   * character later in the string.  Because * matches all characters
   * (no exception for /), it can be easily proved that there's
   * never a need to backtrack multiple levels.
   */
  uint8_t *back_pat = NULL, *back_str = data;
  size_t back_pat_len = 0, back_str_len = data_len;

  /*
   * Loop over each token (character or class) in pat, matching
   * it against the remaining unmatched tail of str.  Return false
   * on mismatch, or true after matching the trailing nul bytes.
   */
  while (data_len) {
    uint8_t c = *data++;
    uint8_t d = *pattern++;
    data_len--;
    pat_len--;

    switch (d) {
    case '?': /* Wildcard: anything goes */
      break;

    case '*':       /* Any-length wildcard */
      if (!pat_len) /* Optimize trailing * case */
        return 1;
      back_pat = pattern;
      back_pat_len = pat_len;
      back_str = --data; /* Allow zero-length match */
      back_str_len = ++data_len;
      break;

    case '[': { /* Character class */
      uint8_t match = 0, inverted = (*pattern == '^');
      uint8_t *cls = pattern + inverted;
      uint8_t a = *cls++;

      /*
       * Iterate over each span in the character class.
       * A span is either a single character a, or a
       * range a-b.  The first span may begin with ']'.
       */
      do {
        uint8_t b = a;

        if (cls[0] == '-' && cls[1] != ']') {
          b = cls[1];

          cls += 2;
          if (a > b) {
            uint8_t tmp = a;
            a = b;
            b = tmp;
          }
        }
        match |= (a <= c && c <= b);
      } while ((a = *cls++) != ']');

      if (match == inverted)
        goto backtrack;
      pat_len -= cls - pattern;
      pattern = cls;

    } break;
    case '\\':
      d = *pattern++;
      pat_len--;
    /*FALLTHROUGH*/
    default: /* Literal character */
      if (c == d)
        break;
    backtrack:
      if (!back_pat)
        return 0; /* No point continuing */
      /* Try again from last *, one character later in str. */
      pattern = back_pat;
      data = ++back_str;
      data_len = --back_str_len;
      pat_len = back_pat_len;
    }
  }
  return !data_len && !pat_len;
}
