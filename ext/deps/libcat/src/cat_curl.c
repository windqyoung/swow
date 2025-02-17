/*
  +--------------------------------------------------------------------------+
  | libcat                                                                   |
  +--------------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0 (the "License");          |
  | you may not use this file except in compliance with the License.         |
  | You may obtain a copy of the License at                                  |
  | http://www.apache.org/licenses/LICENSE-2.0                               |
  | Unless required by applicable law or agreed to in writing, software      |
  | distributed under the License is distributed on an "AS IS" BASIS,        |
  | WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. |
  | See the License for the specific language governing permissions and      |
  | limitations under the License. See accompanying LICENSE file.            |
  +--------------------------------------------------------------------------+
  | Author: Twosee <twosee@php.net>                                          |
  +--------------------------------------------------------------------------+
 */

#include "cat_curl.h"
#include "cat_coroutine.h"
#include "cat_event.h"
#include "cat_poll.h"
#include "cat_queue.h"
#include "cat_time.h"

#ifdef CAT_CURL

typedef struct cat_curl_easy_context_s {
    CURLM *multi;
    cat_coroutine_t *coroutine;
    curl_socket_t sockfd;
    cat_pollfd_events_t events;
    long timeout;
} cat_curl_easy_context_t;

typedef struct cat_curl_multi_context_s {
    cat_queue_node_t node;
    CURLM *multi;
    cat_coroutine_t *coroutine;
    cat_queue_t fds;
    cat_nfds_t nfds;
    long timeout;
} cat_curl_multi_context_t;

typedef struct cat_curl_pollfd_s {
    cat_queue_node_t node;
    curl_socket_t sockfd;
    int action;
} cat_curl_pollfd_t;

CAT_GLOBALS_STRUCT_BEGIN(cat_curl) {
    cat_queue_t multi_map;
} CAT_GLOBALS_STRUCT_END(cat_curl);

CAT_GLOBALS_DECLARE(cat_curl);

#define CAT_CURL_G(x) CAT_GLOBALS_GET(cat_curl, x)

/* common */

static cat_always_inline void cat_curl_multi_configure(CURLM *multi, void *socket_function, void *timer_function, void *context)
{
    curl_multi_setopt(multi, CURLMOPT_SOCKETFUNCTION, socket_function);
    curl_multi_setopt(multi, CURLMOPT_SOCKETDATA, context);
    curl_multi_setopt(multi, CURLMOPT_TIMERFUNCTION, timer_function);
    curl_multi_setopt(multi, CURLMOPT_TIMERDATA, context);
}

static cat_always_inline int cat_curl_translate_poll_flags_from_sys(int events, int revents)
{
    int action = CURL_POLL_NONE;

    if (revents & POLLIN) {
        action |= CURL_CSELECT_IN;
    }
    if (revents & POLLOUT) {
        action |= CURL_CSELECT_OUT;
    }
    if (revents & POLLERR) {
        action |= CURL_CSELECT_ERR;
    }
    if ((revents &~ (POLLIN | POLLOUT | POLLERR)) != 0) {
        if (events & POLLIN) {
            action |= CURL_CSELECT_IN;
        } else if (events & POLLOUT) {
            action |= CURL_CSELECT_OUT;
        } else if (events & POLLERR) {
            action |= CURL_CSELECT_ERR;
        }
    }

    return action;
}

static cat_always_inline cat_pollfd_events_t cat_curl_translate_poll_flags_to_sys(int action)
{
    cat_pollfd_events_t events = POLLNONE;

    if (action != CURL_POLL_REMOVE) {
        if (action != CURL_POLL_IN) {
            events |= POLLOUT;
        }
        if (action != CURL_POLL_OUT) {
            events |= POLLIN;
        }
    }

    return events;
}

#ifdef CAT_DEBUG
static const char *cat_curl_translate_action_name(int action)
{
#define CAT_CURL_ACTION_MAP(XX) \
    XX(NONE) \
    XX(IN) \
    XX(OUT) \
    XX(INOUT) \
    XX(REMOVE)
#define CAT_CURL_ACTION_NAME_GEN(name) case CURL_POLL_##name: return "CURL_POLL_" #name;
    switch (action) {
        CAT_CURL_ACTION_MAP(CAT_CURL_ACTION_NAME_GEN);
        default: CAT_NEVER_HERE("Non-exist");
    }
#undef CAT_CURL_ACTION_NAME_GEN
#undef CAT_CURL_ACTION_MAP
}
#endif

static cat_always_inline cat_timeout_t cat_curl_timeout_min(cat_timeout_t timeout1, cat_timeout_t timeout2)
{
    if (timeout1 < timeout2 && timeout1 >= 0) {
        return timeout1;
    }
    return timeout2;
}

/* easy */

static int cat_curl_easy_socket_function(CURL *ch, curl_socket_t sockfd, int action, cat_curl_easy_context_t *context, void *unused)
{
    (void) ch;
    (void) unused;
    CAT_LOG_DEBUG(CURL, "curl_easy_socket_function(multi: %p, sockfd: %d, action=%s), timeout=%ld",
        context->multi, sockfd, cat_curl_translate_action_name(action), context->timeout);

    /* make sure only 1 sockfd will be added */
    CAT_ASSERT(context->sockfd == CURL_SOCKET_BAD || context->sockfd == sockfd);

    context->sockfd = action != CURL_POLL_REMOVE ? sockfd : CURL_SOCKET_BAD;
    context->events = cat_curl_translate_poll_flags_to_sys(action);

    return 0;
}

static int cat_curl_easy_timeout_function(CURLM *multi, long timeout, cat_curl_easy_context_t *context)
{
    (void) multi;
    CAT_LOG_DEBUG(CURL, "curl_easy_timeout_function(multi: %p, timeout=%ld)", multi, timeout);

    context->timeout = timeout;

    return 0;
}

static CURLcode cat_curl_easy_perform_impl(CURL *ch)
{
    cat_curl_easy_context_t context;
    CURLMsg *message = NULL;
    CURLMcode mcode = CURLM_INTERNAL_ERROR;
    CURLcode code = CURLE_RECV_ERROR;
    int running_handles;

    context.multi = curl_multi_init();
    if (unlikely(context.multi == NULL)) {
        return CURLE_OUT_OF_MEMORY;
    }
    context.coroutine = CAT_COROUTINE_G(current);
    context.sockfd = CURL_SOCKET_BAD;
    context.timeout = -1;
    context.events = POLLNONE;
    cat_curl_multi_configure(
        context.multi,
        (void *) cat_curl_easy_socket_function,
        (void *) cat_curl_easy_timeout_function,
        &context
    );
    mcode = curl_multi_add_handle(context.multi, ch);
    if (unlikely(mcode != CURLM_OK)) {
#if LIBCURL_VERSION_NUM >= 0x072001 /* Available since 7.32.1 */
/* See: https://github.com/curl/curl/commit/19122c07682c268c2383218f62e09c3d24a41e76 */
        if (mcode == CURLM_ADDED_ALREADY) {
            /* cURL is busy with IO,
             * and can not find appropriate error code. */
            code = CURLE_AGAIN;
        }
#endif
        goto _add_failed;
    }

    while (1) {
        /* workaround for cURL with OpenSSL 3.0 bug... */
        mcode = curl_multi_perform(context.multi, &running_handles);
        CAT_LOG_DEBUG(CURL, "curl_multi_perform(ch: %p, running_handles: %d) = %d (%s) workaround",
            ch, running_handles, mcode, curl_multi_strerror(mcode));
        if (unlikely(mcode != CURLM_OK)) {
            goto _error;
        }
        if (running_handles == 0) {
            break;
        }
        if (context.sockfd == CURL_SOCKET_BAD) {
            CAT_LOG_DEBUG(CURL, "curl_time_delay(ch: %p, timeout: %ld) when sockfd is BAD", ch, context.timeout);
            cat_ret_t ret = cat_time_delay(context.timeout);
            if (unlikely(ret != CAT_RET_OK)) {
                goto _error;
            }
            mcode = curl_multi_socket_action(context.multi, CURL_SOCKET_TIMEOUT, 0, &running_handles);
            CAT_LOG_DEBUG(CURL, "curl_multi_socket_action(ch: %p, CURL_SOCKET_TIMEOUT) = %d (%s) after delay",
                ch, mcode, curl_multi_strerror(mcode));
            if (running_handles == 0) {
                break;
            }
        } else {
            cat_pollfd_events_t revents;
            int action;
            cat_ret_t ret;
            CAT_LOG_DEBUG(CURL, "poll_one() for ch<%p>", ch);
            ret = cat_poll_one(context.sockfd, context.events, &revents, context.timeout);
            if (unlikely(ret == CAT_RET_ERROR)) {
                goto _error;
            }
            action = cat_curl_translate_poll_flags_from_sys(context.events, revents);
            if (action == CURL_POLL_NONE) {
                continue;
            }
            mcode = curl_multi_socket_action(
                context.multi,
                context.sockfd,
                action,
                &running_handles
            );
            if (running_handles == 0) {
                break;
            }
        }
        if (unlikely(mcode != CURLM_OK)) {
            goto _error;
        }
    }
    CAT_ASSERT(running_handles == 0);
    message = curl_multi_info_read(context.multi, &running_handles);
    CAT_LOG_DEBUG(CURL, "curl_multi_info_read(ch: %p) = %p", ch, message);
    if (message != NULL) {
        CAT_ASSERT(message->msg == CURLMSG_DONE);
        CAT_ASSERT(running_handles == 0);
        CAT_LOG_DEBUG_VA(CURL, {
            char *done_url;
            curl_easy_getinfo(message->easy_handle, CURLINFO_EFFECTIVE_URL, &done_url);
            CAT_LOG_DEBUG(CURL, "curl_easy_getinfo(ch: %p, CURLINFO_EFFECTIVE_URL, url=\"%s\")", message->easy_handle, done_url);
        });
        code = message->data.result;
    }

    _error:
    curl_multi_remove_handle(context.multi, ch);
    _add_failed:
    curl_multi_cleanup(context.multi);

    return code;
}

CAT_API CURLcode cat_curl_easy_perform(CURL *ch)
{
    CAT_LOG_DEBUG(CURL, "easy_perform(ch: %p) = " CAT_LOG_UNFINISHED_STR, ch);

    CURLcode code = cat_curl_easy_perform_impl(ch);

    CAT_LOG_DEBUG(CURL, "easy_perform(ch: %p) = %d (%s)", ch, code, curl_easy_strerror(code));

    return code;
}

/* multi */

static int cat_curl_multi_socket_function(
    CURL *ch, curl_socket_t sockfd, int action,
    cat_curl_multi_context_t *context, cat_curl_pollfd_t *fd)
{
    (void) ch;
    CURLM *multi = context->multi;

    CAT_LOG_DEBUG(CURL, "curl_multi_socket_function(multi: %p, sockfd: %d, action=%s), nfds=%zu, timeout=%ld",
        multi, sockfd, cat_curl_translate_action_name(action), (size_t) context->nfds, context->timeout);

    if (action != CURL_POLL_REMOVE) {
        if (fd == NULL) {
            fd = (cat_curl_pollfd_t *) cat_malloc(sizeof(*fd));
#if CAT_ALLOC_HANDLE_ERRORS
            if (unlikely(fd == NULL)) {
                return CURLM_OUT_OF_MEMORY;
            }
#endif
            cat_queue_push_back(&context->fds, &fd->node);
            context->nfds++;
            fd->sockfd = sockfd;
            curl_multi_assign(multi, sockfd, fd);
        }
        fd->action = action;
    } else {
        cat_queue_remove(&fd->node);
        cat_free(fd);
        context->nfds--;
        curl_multi_assign(multi, sockfd, NULL);
    }

    return CURLM_OK;
}

static int cat_curl_multi_timeout_function(CURLM *multi, long timeout, cat_curl_multi_context_t *context)
{
    (void) multi;
    CAT_LOG_DEBUG(CURL, "curl_multi_timeout_function(multi: %p, timeout=%ld)", multi, timeout);

    context->timeout = timeout;

    return 0;
}

static cat_curl_multi_context_t *cat_curl_multi_create_context(CURLM *multi)
{
    cat_curl_multi_context_t *context;

    CAT_LOG_DEBUG(CURL, "curl_multi_context_create(multi: %p)", multi);

    context = (cat_curl_multi_context_t *) cat_malloc(sizeof(*context));
#if CAT_ALLOC_HANDLE_ERRORS
    if (unlikely(context == NULL)) {
        return NULL;
    }
#endif

    context->multi = multi;
    context->coroutine = NULL;
    cat_queue_init(&context->fds);
    context->nfds = 0;
    context->timeout = -1;
    /* latest multi has higher priority
     * (previous may leak and they would be free'd in shutdown) */
    cat_queue_push_front(&CAT_CURL_G(multi_map), &context->node);

    cat_curl_multi_configure(
        multi,
        (void *) cat_curl_multi_socket_function,
        (void *) cat_curl_multi_timeout_function,
        context
    );

    return context;
}

static cat_curl_multi_context_t *cat_curl_multi_get_context(CURLM *multi)
{
    CAT_QUEUE_FOREACH_DATA_START(&CAT_CURL_G(multi_map), cat_curl_multi_context_t, node, context) {
        if (context->multi == NULL) {
            return NULL; // eof
        }
        if (context->multi == multi) {
            return context; // hit
        }
    } CAT_QUEUE_FOREACH_DATA_END();

    return NULL;
}

static void cat_curl_multi_context_close(cat_curl_multi_context_t *context)
{
#ifdef CAT_DONT_OPTIMIZE /* fds should have been free'd in curl_multi_socket_function() */
    cat_curl_pollfd_t *fd;
    while ((fd = cat_queue_front_data(&context->fds, cat_curl_pollfd_t, node))) {
        cat_queue_remove(&fd->node);
        cat_free(fd);
        context->nfds--;
    }
#endif
    CAT_ASSERT(context->nfds == 0);
    cat_queue_remove(&context->node);
    cat_free(context);
}

static void cat_curl_multi_close_context(CURLM *multi)
{
    cat_curl_multi_context_t *context;

    context = cat_curl_multi_get_context(multi);
    CAT_ASSERT(context != NULL);
    cat_curl_multi_context_close(context);
}

CAT_API CURLM *cat_curl_multi_init(void)
{
    CURLM *multi = curl_multi_init();
    cat_curl_multi_context_t *context;

    CAT_LOG_DEBUG(CURL, "multi_init(multi: %p)", multi);

    if (unlikely(multi == NULL)) {
        return NULL;
    }

    context = cat_curl_multi_create_context(multi);
#ifdef CAT_ALLOC_NEVER_RETURNS_NULL
    (void) context;
#else
    if (unlikely(context == NULL)) {
        (void) curl_multi_cleanup(multi);
        return NULL;
    }
#endif

    return multi;
}

CAT_API CURLMcode cat_curl_multi_cleanup(CURLM *multi)
{
    CURLMcode mcode;

    mcode = curl_multi_cleanup(multi);
    /* we do not know whether libcurl would do something during cleanup,
     * so we close the context later */
    cat_curl_multi_close_context(multi);

    CAT_LOG_DEBUG(CURL, "multi_cleanup(multi: %p) = %d (%s)", multi, mcode, curl_multi_strerror(mcode));

    return mcode;
}

static CURLMcode cat_curl_multi_wait_impl(
    CURLM *multi,
    struct curl_waitfd *extra_fds, unsigned int extra_nfds,
    int timeout_ms, int *numfds, int *running_handles
)
{
    cat_curl_multi_context_t *context;
    cat_pollfd_t *fds = NULL;
    CURLMcode mcode = CURLM_OK;
    cat_msec_t start_line = cat_time_msec_cached();
    cat_timeout_t timeout = timeout_ms; /* maybe reduced in the loop */
    int ret = 0;

    /* TODO: Support it? */
    CAT_ASSERT(extra_fds == NULL && "Not support yet");
    CAT_ASSERT(extra_nfds == 0 && "Not support yet");
    CAT_ASSERT(running_handles != NULL);

    context = cat_curl_multi_get_context(multi);

    CAT_ASSERT(context != NULL);

    while (1) {
        /* workaround for alpine bug <hyperf-dockerfile:8.1-alpine-3.17-swow-0.3.2-alpha>
         * (version_number: 481024, version: 7.87.0, host: x86_64-alpine-linux-musl, with OpenSSL/3.0.7)
         * TODO: optimize it, do not always do that... */
        mcode = curl_multi_perform(multi, running_handles);
        CAT_LOG_DEBUG(CURL, "curl_multi_perform(multi: %p, running_handles: %d) = %d (%s) (workaround)",
            multi, *running_handles, mcode, curl_multi_strerror(mcode));
        if (unlikely(mcode != CURLM_OK) || *running_handles == 0) {
            goto _out;
        }
        if (context->nfds == 0) {
            cat_timeout_t op_timeout = cat_curl_timeout_min(context->timeout, timeout);
            cat_ret_t ret;
            CAT_LOG_DEBUG(CURL, "curl_time_delay(multi: %p, timeout: " CAT_TIMEOUT_FMT ") when nfds is 0", multi, op_timeout);
            ret = cat_time_delay(op_timeout);
            if (unlikely(ret != CAT_RET_OK)) {
                goto _out;
            }
            mcode = curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT, 0, running_handles);
            CAT_LOG_DEBUG(CURL, "curl_multi_socket_action(multi: %p, CURL_SOCKET_TIMEOUT) = %d (%s) after delay",
                multi, mcode, curl_multi_strerror(mcode));
            if (unlikely(mcode != CURLM_OK) || *running_handles == 0) {
                goto _out;
            }
        } else {
            cat_nfds_t i;
            fds = (cat_pollfd_t *) cat_malloc(sizeof(*fds) * context->nfds);
#if CAT_ALLOC_HANDLE_ERRORS
            if (unlikely(fds == NULL)) {
                mcode = CURLM_OUT_OF_MEMORY;
                goto _out;
            }
#endif
            i = 0;
            CAT_QUEUE_FOREACH_DATA_START(&context->fds, cat_curl_pollfd_t, node, curl_fd) {
                cat_pollfd_t *fd = &fds[i];
                fd->fd = curl_fd->sockfd;
                fd->events = cat_curl_translate_poll_flags_to_sys(curl_fd->action);
                i++;
            } CAT_QUEUE_FOREACH_DATA_END();
            CAT_LOG_DEBUG(CURL, "poll() for multi<%p>", multi);
            ret = cat_poll(fds, context->nfds, cat_curl_timeout_min(context->timeout, timeout));
            if (unlikely(ret == CAT_RET_ERROR)) {
                mcode = CURLM_OUT_OF_MEMORY; // or internal error?
                goto _out;
            }
            if (ret != 0) {
                cat_bool_t hit = cat_false;
                for (i = 0; i < context->nfds; i++) {
                    cat_pollfd_t *fd = &fds[i];
                    int action = cat_curl_translate_poll_flags_from_sys(fd->events, fd->revents);
                    if (action == CURL_POLL_NONE) {
                        continue;
                    }
                    hit = cat_true;
                    mcode = curl_multi_socket_action(multi, fd->fd, action, running_handles);
                    CAT_LOG_DEBUG(CURL, "curl_multi_socket_action(multi: %p, fd: %d, %s) = %d (%s) after poll",
                        multi, fd->fd, cat_curl_translate_action_name(action), mcode, curl_multi_strerror(mcode));
                    if (unlikely(mcode != CURLM_OK)) {
                        continue; // shall we handle it?
                    }
                    if (*running_handles == 0) {
                        goto _out;
                    }
                }
                if (unlikely(!hit)) {
                    goto _action_timeout;
                }
            } else {
                _action_timeout:
                mcode = curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT, 0, running_handles);
                CAT_LOG_DEBUG(CURL, "curl_multi_socket_action(multi: %p, CURL_SOCKET_TIMEOUT) = %d (%s) after poll return 0",
                    multi, mcode, curl_multi_strerror(mcode));
                if (unlikely(mcode != CURLM_OK) || *running_handles == 0) {
                    goto _out;
                }
            }
            goto _out;
        }
        do {
            cat_msec_t new_start_line = cat_time_msec_cached();
            timeout -= (new_start_line - start_line);
            if (timeout <= 0) {
                /* timeout */
                goto _out;
            }
            start_line = new_start_line;
        } while (0);
    }

    _out:
    if (fds != NULL) {
        cat_free(fds);
    }
    if (numfds != NULL) {
        *numfds = ret;
    }
    return mcode;
}

CAT_API CURLMcode cat_curl_multi_perform(CURLM *multi, int *running_handles)
{
    int _running_handles = -1;
    if (running_handles == NULL) {
        running_handles = &_running_handles;
    }

    CAT_LOG_DEBUG(CURL, "multi_perform(multi: %p, running_handles: %d) = " CAT_LOG_UNFINISHED_STR, multi, *running_handles);

    /* this way even can solve the problem of CPU 100% if we perform in while loop */
    CURLMcode code = cat_curl_multi_wait_impl(multi, NULL, 0, 0, NULL, running_handles);

    CAT_LOG_DEBUG(CURL, "multi_perform(multi: %p, running_handles: %d) = %d (%s)", multi, *running_handles, code, curl_multi_strerror(code));

    return code;
}

CAT_API CURLMcode cat_curl_multi_wait(
    CURLM *multi,
    struct curl_waitfd *extra_fds, unsigned int extra_nfds,
    int timeout_ms, int *numfds
)
{
    int _numfds = -1;
    int _running_handles = -1;
    int *running_handles = &_running_handles;
    if (numfds == NULL) {
        numfds = &_numfds;
    }

    CAT_LOG_DEBUG(CURL, "multi_wait(multi: %p, timeout_ms: %d, numfds: %d) = " CAT_LOG_UNFINISHED_STR, multi, timeout_ms, *numfds);

    CURLMcode mcode = cat_curl_multi_wait_impl(multi, extra_fds, extra_nfds, timeout_ms, numfds, running_handles);

    CAT_LOG_DEBUG(CURL, "multi_wait(multi: %p, timeout_ms: %d, numfds: %d, running_handles: %d) = %d (%s)", multi, timeout_ms, *numfds, *running_handles, mcode, curl_multi_strerror(mcode));

    return mcode;
}

/* module/runtime */

CAT_API cat_bool_t cat_curl_module_init(void)
{
    CAT_GLOBALS_REGISTER(cat_curl);

#ifdef CAT_DEBUG
    curl_version_info_data *curl_vid = curl_version_info(CURLVERSION_NOW);
    if (curl_vid->version_num != LIBCURL_VERSION_NUM) {
        CAT_CORE_ERROR(CURL, "Curl version mismatch, built with \"%s\", but running with \"%s\"",
            curl_vid->version, curl_version());
    }
#endif

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        CAT_CORE_ERROR(CURL, "Curl init failed");
    }

    return cat_true;
}

CAT_API cat_bool_t cat_curl_module_shutdown(void)
{
    curl_global_cleanup();

    CAT_GLOBALS_UNREGISTER(cat_curl);

    return cat_true;
}

CAT_API cat_bool_t cat_curl_runtime_init(void)
{
    cat_queue_init(&CAT_CURL_G(multi_map));

    return cat_true;
}

CAT_API cat_bool_t cat_curl_runtime_close(void)
{
    CAT_ASSERT(cat_queue_empty(&CAT_CURL_G(multi_map)));

    return cat_true;
}

#endif /* CAT_CURL */
