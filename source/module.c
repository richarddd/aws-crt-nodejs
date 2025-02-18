/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include "auth.h"
#include "checksums.h"
#include "crypto.h"
#include "http_connection.h"
#include "http_connection_manager.h"
#include "http_headers.h"
#include "http_message.h"
#include "http_stream.h"
#include "io.h"
#include "logger.h"
#include "mqtt_client.h"
#include "mqtt_client_connection.h"

#include <aws/cal/cal.h>

#include <aws/common/clock.h>
#include <aws/common/environment.h>
#include <aws/common/logging.h>
#include <aws/common/ref_count.h>
#include <aws/common/system_info.h>

#include <aws/io/event_loop.h>
#include <aws/io/tls_channel_handler.h>

#include <aws/http/http.h>

#include <aws/auth/auth.h>

#include <uv.h>

/* aws-crt-nodejs requires N-API version 4 or above for the threadsafe function API */
AWS_STATIC_ASSERT(NAPI_VERSION >= 4);

#define AWS_DEFINE_ERROR_INFO_CRT_NODEJS(CODE, STR) AWS_DEFINE_ERROR_INFO(CODE, STR, "aws-crt-nodejs")

/* clang-format off */
static struct aws_error_info s_errors[] = {
    AWS_DEFINE_ERROR_INFO_CRT_NODEJS(
        AWS_CRT_NODEJS_ERROR_THREADSAFE_FUNCTION_NULL_NAPI_ENV,
        "There was an attempt to execute a thread-safe napi function binding with a null napi environment.  This is usually due to the function binding being released by a shutdown/cleanup process while the execution is waiting in the queue."),
};
/* clang-format on */

static struct aws_error_info_list s_error_list = {
    .error_list = s_errors,
    .count = sizeof(s_errors) / sizeof(struct aws_error_info),
};

static struct aws_log_subject_info s_log_subject_infos[] = {
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_NODEJS_CRT_GENERAL, "node", "General Node/N-API events"),
};

static struct aws_log_subject_info_list s_log_subject_list = {
    .subject_list = s_log_subject_infos,
    .count = AWS_ARRAY_SIZE(s_log_subject_infos),
};

static uv_loop_t *s_node_uv_loop = NULL;
static struct aws_event_loop *s_node_uv_event_loop = NULL;
static struct aws_event_loop_group *s_node_uv_elg = NULL;
static struct aws_host_resolver *s_default_host_resolver = NULL;
static struct aws_client_bootstrap *s_default_client_bootstrap = NULL;

napi_status aws_byte_buf_init_from_napi(struct aws_byte_buf *buf, napi_env env, napi_value node_str) {

    AWS_ASSERT(buf);

    napi_valuetype type = napi_undefined;
    AWS_NAPI_CALL(env, napi_typeof(env, node_str, &type), { return status; });

    if (type == napi_string) {

        size_t length = 0;
        AWS_NAPI_CALL(env, napi_get_value_string_utf8(env, node_str, NULL, 0, &length), { return status; });

        /* Node requires that the null terminator be written */
        if (aws_byte_buf_init(buf, aws_napi_get_allocator(), length + 1)) {
            return napi_generic_failure;
        }

        AWS_NAPI_CALL(env, napi_get_value_string_utf8(env, node_str, (char *)buf->buffer, buf->capacity, &buf->len), {
            return status;
        });
        AWS_ASSERT(length == buf->len);
        return napi_ok;

    } else if (type == napi_object) {

        bool is_expected = false;

        /* Try ArrayBuffer */
        AWS_NAPI_CALL(env, napi_is_arraybuffer(env, node_str, &is_expected), { return status; });
        if (is_expected) {
            napi_status status = napi_get_arraybuffer_info(env, node_str, (void **)&buf->buffer, &buf->len);
            buf->capacity = buf->len;
            return status;
        }

        /* Try DataView */
        AWS_NAPI_CALL(env, napi_is_dataview(env, node_str, &is_expected), { return status; });
        if (is_expected) {
            napi_status status = napi_get_dataview_info(env, node_str, &buf->len, (void **)&buf->buffer, NULL, NULL);
            buf->capacity = buf->len;
            return status;
        }

        /* Try TypedArray */
        AWS_NAPI_CALL(env, napi_is_typedarray(env, node_str, &is_expected), { return status; });
        if (is_expected) {
            napi_typedarray_type array_type = napi_uint8_array;
            size_t length = 0;
            AWS_NAPI_CALL(
                env, napi_get_typedarray_info(env, node_str, &array_type, &length, (void **)&buf->buffer, NULL, NULL), {
                    return status;
                });

            size_t element_size = 0;

            /* whoever added napi_bigint64_array to the node api deserves a good thrashing!!!! */
            int type_hack = array_type;
            switch (type_hack) {
                case napi_int8_array:
                case napi_uint8_array:
                case napi_uint8_clamped_array:
                    element_size = 1;
                    break;

                case napi_int16_array:
                case napi_uint16_array:
                    element_size = 2;
                    break;

                case napi_int32_array:
                case napi_uint32_array:
                case napi_float32_array:
                    element_size = 4;
                    break;

                case napi_float64_array:
                case 9:  /*napi_bigint64_array */
                case 10: /*napi_biguint64_array*/
                    element_size = 8;
                    break;
            }
            buf->len = length * element_size;
            buf->capacity = buf->len;

            return napi_ok;
        }
    }

    return napi_invalid_arg;
}

struct aws_string *aws_string_new_from_napi(napi_env env, napi_value node_str) {

    struct aws_byte_buf temp_buf;
    if (aws_byte_buf_init_from_napi(&temp_buf, env, node_str)) {
        return NULL;
    }

    struct aws_string *string = aws_string_new_from_array(aws_napi_get_allocator(), temp_buf.buffer, temp_buf.len);
    aws_byte_buf_clean_up(&temp_buf);
    return string;
}

napi_status aws_napi_create_dataview_from_byte_cursor(
    napi_env env,
    const struct aws_byte_cursor *cur,
    napi_value *result) {

    void *data = NULL;
    napi_value arraybuffer;
    AWS_NAPI_CALL(env, napi_create_arraybuffer(env, cur->len, &data, &arraybuffer), { return status; });

    struct aws_byte_buf arraybuffer_buf = aws_byte_buf_from_empty_array(data, cur->len);
    struct aws_byte_cursor input = *cur;
    if (!aws_byte_buf_write_from_whole_cursor(&arraybuffer_buf, input)) {
        return napi_generic_failure;
    }

    AWS_NAPI_CALL(env, napi_create_dataview(env, cur->len, arraybuffer, 0, result), { return status; });

    return napi_ok;
}

bool aws_napi_is_null_or_undefined(napi_env env, napi_value value) {

    napi_valuetype type = napi_undefined;
    if (napi_ok != napi_typeof(env, value, &type)) {
        return true;
    }

    return type == napi_null || type == napi_undefined;
}

void aws_napi_throw_last_error(napi_env env) {
    const int error_code = aws_last_error();
    napi_throw_error(env, aws_error_str(error_code), aws_error_debug_str(error_code));
}

struct uv_loop_s *aws_napi_get_node_uv_loop(void) {
    return s_node_uv_loop;
}

struct aws_event_loop *aws_napi_get_node_event_loop(void) {
    return s_node_uv_event_loop;
}

struct aws_event_loop_group *aws_napi_get_node_elg(void) {
    return s_node_uv_elg;
}

struct aws_client_bootstrap *aws_napi_get_default_client_bootstrap(void) {
    return s_default_client_bootstrap;
}

/* The napi_status enum has grown, and is not bound by N-API versioning */
#if defined(__clang__) || defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wswitch"
#endif

const char *aws_napi_status_to_str(napi_status status) {
    const char *reason = "UNKNOWN";
    switch (status) {
        case napi_ok:
            reason = "OK";
            break;
        case napi_invalid_arg:
            reason = "napi_invalid_arg: an invalid argument was supplied";
            break;
        case napi_object_expected:
            reason = "napi_object_expected";
            break;
        case napi_string_expected:
            reason = "napi_name_expected";
            break;
        case napi_name_expected:
            reason = "napi_name_expected";
            break;
        case napi_function_expected:
            reason = "napi_function_expected";
            break;
        case napi_number_expected:
            reason = "napi_number_expected";
            break;
        case napi_boolean_expected:
            reason = "napi_boolean_expected";
            break;
        case napi_array_expected:
            reason = "napi_array_expected";
            break;
        case napi_generic_failure:
            reason = "napi_generic_failure";
            break;
        case napi_pending_exception:
            reason = "napi_pending_exception";
            break;
        case napi_cancelled:
            reason = "napi_cancelled";
            break;
        case napi_escape_called_twice:
            reason = "napi_escape_called_twice";
            break;
        case napi_handle_scope_mismatch:
            reason = "napi_handle_scope_mismatch";
            break;
        case napi_callback_scope_mismatch:
            reason = "napi_callback_scope_mismatch";
            break;
#if NAPI_VERSION >= 3
        case napi_queue_full:
            reason = "napi_queue_full";
            break;
        case napi_closing:
            reason = "napi_closing";
            break;
        case napi_bigint_expected:
            reason = "napi_bigint_expected";
            break;
#endif
    }
    return reason;
}

#if defined(__clang__) || defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif

static void s_handle_failed_callback(napi_env env, napi_value function, napi_status reason) {
    /* Figure out if there's an exception pending, if so, no callbacks will ever succeed again until it's cleared */
    bool pending_exception = reason == napi_pending_exception;
    AWS_NAPI_ENSURE(env, napi_is_exception_pending(env, &pending_exception));
    /* if there's no pending exception, but failure occurred, log what we can find and get out */
    if (!pending_exception) {
        const napi_extended_error_info *node_error_info = NULL;
        AWS_NAPI_ENSURE(env, napi_get_last_error_info(env, &node_error_info));
        AWS_NAPI_LOGF_ERROR(
            "Extended error info: engine_error_code=%u error_code=%s error_message=%s",
            node_error_info->engine_error_code,
            aws_napi_status_to_str(node_error_info->error_code),
            node_error_info->error_message);
        return;
    }
    /* get the current exception and report it, and clear it so that execution can continue */
    napi_value node_exception = NULL;
    AWS_NAPI_ENSURE(env, napi_get_and_clear_last_exception(env, &node_exception));

    /* figure out what the exception is */
    bool is_error = false;
    AWS_NAPI_ENSURE(env, napi_is_error(env, node_exception, &is_error));

    /*
     * Convert the function to a string. If it's a lambda, this will produce the source of the lambda, if
     * it's a class function or free function, it will produce the name
     */
    napi_value node_function_str = NULL;
    AWS_NAPI_ENSURE(env, napi_coerce_to_string(env, function, &node_function_str));
    struct aws_string *function_str = aws_string_new_from_napi(env, node_function_str);
    if (function_str) {
        AWS_NAPI_LOGF_ERROR("Calling %s", aws_string_c_str(function_str));
    }

    /* If it's an Error, extract info from it and log it */
    if (is_error) {
        /* get the Error.message field */
        napi_value node_message = NULL;
        AWS_NAPI_ENSURE(env, napi_get_named_property(env, node_exception, "message", &node_message));

        /* extract and log the message */
        struct aws_string *message = aws_string_new_from_napi(env, node_message);
        if (message) {
            AWS_NAPI_LOGF_ERROR("Error: %s", aws_string_bytes(message));
            aws_string_destroy(message);
        } else {
            AWS_NAPI_LOGF_ERROR("aws_string_new_from_napi(exception.message) failed");
            return;
        }

        /* get the Error.stack field */
        napi_value node_stack = NULL;
        AWS_NAPI_ENSURE(env, napi_get_named_property(env, node_exception, "stack", &node_stack));

        /* extract and log the stack trace */
        struct aws_string *stacktrace = aws_string_new_from_napi(env, node_stack);
        if (stacktrace) {
            AWS_NAPI_LOGF_ERROR("Stack:\n%s", aws_string_bytes(stacktrace));
            aws_string_destroy(stacktrace);
        } else {
            AWS_NAPI_LOGF_ERROR("aws_string_new_from_napi(exception.stack) failed");
            return;
        }

        /* the Error has been reported and cleared, that's all we can do */
        return;
    }

    /* The last thing thrown was some other sort of object/primitive, so convert it to a string and log it */
    napi_value node_error_str = NULL;
    AWS_NAPI_ENSURE(env, napi_coerce_to_string(env, node_exception, &node_error_str));

    struct aws_string *error_str = aws_string_new_from_napi(env, node_error_str);
    if (error_str) {
        AWS_NAPI_LOGF_ERROR("Error: %s", aws_string_bytes(error_str));
    } else {
        AWS_NAPI_LOGF_ERROR("aws_string_new_from_napi(ToString(exception)) failed");
        return;
    }
}

napi_status aws_napi_dispatch_threadsafe_function(
    napi_env env,
    napi_threadsafe_function tsfn,
    napi_value this_ptr,
    napi_value function,
    size_t argc,
    napi_value *argv) {

    napi_status call_status = napi_ok;
    if (!this_ptr) {
        AWS_NAPI_ENSURE(env, napi_get_undefined(env, &this_ptr));
    }
    AWS_NAPI_CALL(env, napi_call_function(env, this_ptr, function, argc, argv, NULL), {
        call_status = status;
        s_handle_failed_callback(env, function, status);
    });
    /* Must always decrement the ref count, or the function will be pinned */
    napi_status release_status = napi_release_threadsafe_function(tsfn, napi_tsfn_release);
    return (call_status != napi_ok) ? call_status : release_status;
}

napi_status aws_napi_create_threadsafe_function(
    napi_env env,
    napi_value function,
    const char *name,
    napi_threadsafe_function_call_js call_js,
    void *context,
    napi_threadsafe_function *result) {

    napi_value resource_name = NULL;
    AWS_NAPI_ENSURE(env, napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &resource_name));

    return napi_create_threadsafe_function(
        env, function, NULL, resource_name, 0, 1, NULL, NULL, context, call_js, result);
}

napi_status aws_napi_release_threadsafe_function(
    napi_threadsafe_function function,
    napi_threadsafe_function_release_mode mode) {
    if (function) {
        return napi_release_threadsafe_function(function, mode);
    }
    return napi_ok;
}

napi_status aws_napi_acquire_threadsafe_function(napi_threadsafe_function function) {
    if (function) {
        return napi_acquire_threadsafe_function(function);
    }
    return napi_ok;
}

napi_status aws_napi_unref_threadsafe_function(napi_env env, napi_threadsafe_function function) {
    if (function) {
        return napi_unref_threadsafe_function(env, function);
    }
    return napi_ok;
}

napi_status aws_napi_queue_threadsafe_function(napi_threadsafe_function function, void *user_data) {
    /* increase the ref count, gets decreased when the call completes */
    AWS_NAPI_ENSURE(NULL, napi_acquire_threadsafe_function(function));
    return napi_call_threadsafe_function(function, user_data, napi_tsfn_nonblocking);
}

AWS_STATIC_STRING_FROM_LITERAL(s_mem_tracing_env_var, "AWS_CRT_MEMORY_TRACING");
static struct aws_allocator *s_allocator = NULL;
struct aws_allocator *aws_napi_get_allocator() {
    if (AWS_UNLIKELY(s_allocator == NULL)) {
        struct aws_string *value = NULL;
        if (aws_get_environment_value(aws_default_allocator(), s_mem_tracing_env_var, &value) || value == NULL) {
            return s_allocator = aws_default_allocator();
        }

        int level = atoi(aws_string_c_str(value));
        if (level < AWS_MEMTRACE_NONE || level > AWS_MEMTRACE_STACKS) {
            /* this can't go through logging, because it happens before logging is set up */
            fprintf(
                stderr,
                "AWS_CRT_MEMORY_TRACING is set to invalid value: %s, must be 0 (none), 1 (bytes), or 2 (stacks)",
                aws_string_bytes(value));
            level = AWS_MEMTRACE_NONE;
        }
        s_allocator = aws_mem_tracer_new(aws_default_allocator(), NULL, level, 16);
    }
    return s_allocator;
}

napi_value aws_napi_native_memory(napi_env env, napi_callback_info info) {
    (void)info;
    napi_value node_allocated = NULL;
    size_t allocated = 0;
    if (aws_napi_get_allocator() != aws_default_allocator()) {
        allocated = aws_mem_tracer_bytes(aws_napi_get_allocator());
    }
    AWS_NAPI_CALL(env, napi_create_int64(env, allocated, &node_allocated), { return NULL; });
    return node_allocated;
}

napi_value aws_napi_native_memory_dump(napi_env env, napi_callback_info info) {
    (void)info;
    (void)env;
    if (aws_napi_get_allocator() != aws_default_allocator()) {
        aws_mem_tracer_dump(aws_napi_get_allocator());
    }
    return NULL;
}

#if defined(_WIN32)
#    include <windows.h>
static LONG WINAPI s_print_stack_trace(struct _EXCEPTION_POINTERS *exception_pointers) {
    aws_backtrace_print(stderr, exception_pointers);
    return EXCEPTION_EXECUTE_HANDLER;
}
#elif defined(AWS_HAVE_EXECINFO)
#    include <signal.h>
static void s_print_stack_trace(int sig, siginfo_t *sig_info, void *user_data) {
    (void)sig;
    (void)sig_info;
    (void)user_data;
    aws_backtrace_print(stderr, sig_info);
    exit(-1);
}
#endif

static void s_install_crash_handler(void) {
#if defined(_WIN32)
    SetUnhandledExceptionFilter(s_print_stack_trace);
#elif defined(AWS_HAVE_EXECINFO)
    struct sigaction sa;
    memset(&sa, 0, sizeof(struct sigaction));
    sigemptyset(&sa.sa_mask);

    sa.sa_flags = SA_NODEFER;
    sa.sa_sigaction = s_print_stack_trace;

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
#endif
}

static void s_napi_context_finalize(napi_env env, void *user_data, void *finalize_hint) {
    (void)env;
    (void)finalize_hint;
    aws_client_bootstrap_release(s_default_client_bootstrap);
    aws_host_resolver_release(s_default_host_resolver);
    aws_event_loop_group_release(s_node_uv_elg);

    aws_thread_join_all_managed();

    aws_unregister_log_subject_info_list(&s_log_subject_list);
    aws_unregister_error_info(&s_error_list);
    aws_auth_library_clean_up();
    aws_mqtt_library_clean_up();

    struct aws_napi_context *ctx = user_data;
    aws_napi_logger_destroy(ctx->logger);
    aws_mem_release(ctx->allocator, ctx);

    if (ctx->allocator != aws_default_allocator()) {
        aws_mem_tracer_destroy(ctx->allocator);
        if (s_allocator == ctx->allocator) {
            s_allocator = NULL;
        }
    }
}

static struct aws_napi_context *s_napi_context_new(struct aws_allocator *allocator, napi_env env, napi_value exports) {
    struct aws_napi_context *ctx = aws_mem_calloc(allocator, 1, sizeof(struct aws_napi_context));
    AWS_FATAL_ASSERT(ctx && "Failed to initialize napi context");
    ctx->allocator = allocator;

    /* bind the context to exports, thus binding its lifetime to that object */
    AWS_NAPI_ENSURE(env, napi_wrap(env, exports, ctx, s_napi_context_finalize, NULL, NULL));

    ctx->logger = aws_napi_logger_new(allocator, env);

    return ctx;
}

/** Helper for creating and registering a function */
static bool s_create_and_register_function(
    napi_env env,
    napi_value exports,
    napi_callback fn,
    const char *fn_name,
    size_t fn_name_len) {

    napi_value napi_fn;
    AWS_NAPI_CALL(env, napi_create_function(env, fn_name, fn_name_len, fn, NULL, &napi_fn), {
        napi_throw_error(env, NULL, "Unable to wrap native function");
        return false;
    });

    AWS_NAPI_CALL(env, napi_set_named_property(env, exports, fn_name, napi_fn), {
        napi_throw_error(env, NULL, "Unable to populate exports");
        return false;
    });

    return true;
}

/* napi_value */ NAPI_MODULE_INIT() /* (napi_env env, napi_value exports) */ {
    s_install_crash_handler();

    struct aws_allocator *allocator = aws_napi_get_allocator();
    /* context is bound to exports, will be cleaned up by finalizer */
    s_napi_context_new(allocator, env, exports);

    aws_cal_library_init(allocator);
    aws_http_library_init(allocator);
    aws_mqtt_library_init(allocator);
    aws_auth_library_init(allocator);
    aws_register_error_info(&s_error_list);
    aws_register_log_subject_info_list(&s_log_subject_list);

    /* Initialize the event loop group */
    /*
     * We don't currently support multi-init of the module, but we should.
     * Things that would need to be solved:
     *    (1) global objects (event loop group, logger, allocator, more)
     *    (2) multi-init/multi-cleanup of aws-c-*
     *    (3) allocator cross-talk/lifetimes
     */
    AWS_FATAL_ASSERT(s_node_uv_elg == NULL);
    s_node_uv_elg = aws_event_loop_group_new_default(allocator, 1, NULL);
    AWS_FATAL_ASSERT(s_node_uv_elg != NULL);

    /*
     * Default host resolver and client bootstrap to use if none specific at the javascript level.  In most
     * cases the user doesn't even need to know about these, so let's let them leave it out completely.
     */
    AWS_FATAL_ASSERT(s_default_host_resolver == NULL);

    struct aws_host_resolver_default_options resolver_options = {
        .max_entries = 64,
        .el_group = s_node_uv_elg,
    };
    s_default_host_resolver = aws_host_resolver_new_default(allocator, &resolver_options);
    AWS_FATAL_ASSERT(s_default_host_resolver != NULL);

    AWS_FATAL_ASSERT(s_default_client_bootstrap == NULL);

    struct aws_client_bootstrap_options bootstrap_options = {
        .event_loop_group = s_node_uv_elg,
        .host_resolver = s_default_host_resolver,
    };

    s_default_client_bootstrap = aws_client_bootstrap_new(allocator, &bootstrap_options);

    AWS_FATAL_ASSERT(s_default_client_bootstrap != NULL);

    napi_value null;
    napi_get_null(env, &null);

#define CREATE_AND_REGISTER_FN(fn)                                                                                     \
    if (!s_create_and_register_function(env, exports, aws_napi_##fn, #fn, sizeof(#fn))) {                              \
        return null;                                                                                                   \
    }

    /* Common */
    CREATE_AND_REGISTER_FN(native_memory)
    CREATE_AND_REGISTER_FN(native_memory_dump)
    CREATE_AND_REGISTER_FN(error_code_to_string)
    CREATE_AND_REGISTER_FN(error_code_to_name)

    /* IO */
    CREATE_AND_REGISTER_FN(io_logging_enable)
    CREATE_AND_REGISTER_FN(is_alpn_available)
    CREATE_AND_REGISTER_FN(io_client_bootstrap_new)
    CREATE_AND_REGISTER_FN(io_tls_ctx_new)
    CREATE_AND_REGISTER_FN(io_tls_connection_options_new);
    CREATE_AND_REGISTER_FN(io_socket_options_new)
    CREATE_AND_REGISTER_FN(io_input_stream_new)
    CREATE_AND_REGISTER_FN(io_input_stream_append)

    /* MQTT Client */
    CREATE_AND_REGISTER_FN(mqtt_client_new)

    /* MQTT Client Connection */
    CREATE_AND_REGISTER_FN(mqtt_client_connection_new)
    CREATE_AND_REGISTER_FN(mqtt_client_connection_connect)
    CREATE_AND_REGISTER_FN(mqtt_client_connection_reconnect)
    CREATE_AND_REGISTER_FN(mqtt_client_connection_publish)
    CREATE_AND_REGISTER_FN(mqtt_client_connection_subscribe)
    CREATE_AND_REGISTER_FN(mqtt_client_connection_on_message)
    CREATE_AND_REGISTER_FN(mqtt_client_connection_unsubscribe)
    CREATE_AND_REGISTER_FN(mqtt_client_connection_disconnect)
    CREATE_AND_REGISTER_FN(mqtt_client_connection_close)

    /* Crypto */
    CREATE_AND_REGISTER_FN(hash_md5_new)
    CREATE_AND_REGISTER_FN(hash_sha1_new)
    CREATE_AND_REGISTER_FN(hash_sha256_new)
    CREATE_AND_REGISTER_FN(hash_update)
    CREATE_AND_REGISTER_FN(hash_digest)
    CREATE_AND_REGISTER_FN(hash_md5_compute)
    CREATE_AND_REGISTER_FN(hash_sha1_compute)
    CREATE_AND_REGISTER_FN(hash_sha256_compute)
    CREATE_AND_REGISTER_FN(hmac_sha256_new)
    CREATE_AND_REGISTER_FN(hmac_update)
    CREATE_AND_REGISTER_FN(hmac_digest)
    CREATE_AND_REGISTER_FN(hmac_sha256_compute)

    /* Checksums */
    CREATE_AND_REGISTER_FN(checksums_crc32)
    CREATE_AND_REGISTER_FN(checksums_crc32c)

    /* HTTP */
    CREATE_AND_REGISTER_FN(http_proxy_options_new)
    CREATE_AND_REGISTER_FN(http_connection_new)
    CREATE_AND_REGISTER_FN(http_connection_close)
    CREATE_AND_REGISTER_FN(http_stream_new)
    CREATE_AND_REGISTER_FN(http_stream_activate)
    CREATE_AND_REGISTER_FN(http_stream_close)
    CREATE_AND_REGISTER_FN(http_connection_manager_new)
    CREATE_AND_REGISTER_FN(http_connection_manager_close)
    CREATE_AND_REGISTER_FN(http_connection_manager_acquire)
    CREATE_AND_REGISTER_FN(http_connection_manager_release)

#undef CREATE_AND_REGISTER_FN

    AWS_NAPI_ENSURE(env, aws_napi_http_headers_bind(env, exports));
    AWS_NAPI_ENSURE(env, aws_napi_http_message_bind(env, exports));
    AWS_NAPI_ENSURE(env, aws_napi_auth_bind(env, exports));

    return exports;
}
