/*
 * Minimal Zephyr Redfish Implementation
 * Implements: ComputerSystem.Reset (On/ForceOff/PowerCycle/ForceRestart)
 *
 * redfishtool Systems -r 192.168.2.55 -vvv -I system
 * redfishtool Systems -r 192.168.2.55 -vvv reset On
 * redfishtool Systems -r 192.168.2.55 -vvv reset ForceOff
 * redfishtool Systems -r 192.168.2.55 -vvv reset PowerCycle
 * redfishtool Systems -r 192.168.2.55 -vvv reset ForceRestart
 *
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/data/json.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/base64.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "http.h"
#include "power.h"
#include "time.h"
#include "sensors.h"
#include "vpd.h"
#include "git_sha.h"
#include "https_cert.h"

LOG_MODULE_REGISTER(redfish_app, CONFIG_LOG_DEFAULT_LEVEL);

/*** Redfish HTTP handlers ***/

/* "Basic" (not session based) authentication, uses HTTP Authorization header */
HTTP_SERVER_REGISTER_HEADER_CAPTURE(capture_authorization, "authorization");
HTTP_SERVER_REGISTER_HEADER_CAPTURE(capture_x_auth_code, "x-auth-code");

#define CREDENTIALS_MAX_LEN 64
static int validate_auth(struct http_client_ctx *client)
{
	size_t header_count = client->header_capture_ctx.count;
	const struct http_header *headers = client->header_capture_ctx.headers;
	const char *auth_header = NULL;
	const char *prefix = "Basic ";

	for (unsigned int i = 0; i < header_count; i++) {
		if (!strcmp(headers[i].name, "authorization") ||
		    !strcmp(headers[i].name, "Authorization")) {
			auth_header = headers[i].value;
			break;
		}
	}

	if (auth_header == NULL) {
		LOG_WRN("No auth header");
		return -1;
	}

	if (strncmp(auth_header, prefix, strlen(prefix)) != 0) {
		LOG_WRN("Not basic auth");
		return -1; // Not Basic auth
	}

	// Extract the Base64 token part
	const char *b64_token = auth_header + strlen(prefix);
	size_t token_len = strlen(b64_token);

	uint8_t decoded_buf[CREDENTIALS_MAX_LEN];
	size_t decoded_len = 0;

	int ret = base64_decode(decoded_buf, sizeof(decoded_buf) - 1, &decoded_len,
				b64_token, token_len);
	if (ret < 0) {
		LOG_WRN("BASE64 decode failed");
		return ret;
	}

	decoded_buf[decoded_len] = '\0';

	uint8_t expected[CREDENTIALS_MAX_LEN];
	snprintf(expected, sizeof(expected), "%s:%s", "admin", config_bmc_admin_password());

	if (strcmp((char *)decoded_buf, expected) == 0)
		return 0; // Success!

	LOG_WRN("Authentication did not match");

	return -1;
}

static int validate_and_set_headers(struct http_client_ctx *client,
				struct http_response_ctx *ctx,
				uint32_t supported_methods)
{
	if (!(BIT(client->method) & supported_methods)) {
		ctx->status = HTTP_405_METHOD_NOT_ALLOWED;
		if (supported_methods == (BIT(HTTP_GET))) {
			static const struct http_header headers[] = {
				{ .name = "allow", .value = "GET" },
			};
			ctx->headers = headers;
			ctx->header_count = ARRAY_SIZE(headers);
		} else if (supported_methods == (BIT(HTTP_POST))) {
			static const struct http_header headers[] = {
				{ .name = "allow", .value = "POST" },
			};
			ctx->headers = headers;
			ctx->header_count = ARRAY_SIZE(headers);
		} else if (supported_methods == (BIT(HTTP_GET) | BIT(HTTP_PATCH))) {
			static const struct http_header headers[] = {
				{ .name = "allow", .value = "GET, PATCH" },
			};
			ctx->headers = headers;
			ctx->header_count = ARRAY_SIZE(headers);
		} else if (supported_methods == (BIT(HTTP_GET) | BIT(HTTP_DELETE))) {
			static const struct http_header headers[] = {
				{ .name = "allow", .value = "GET, DELETE" },
			};
			ctx->headers = headers;
			ctx->header_count = ARRAY_SIZE(headers);
		} else {
			LOG_ERR("Unsupported supported methods!");
		}
		ctx->final_chunk = true;
		ctx->body = NULL;
		ctx->body_len = 0;
		return -1;
	}

	if (client->method == HTTP_GET) {
		static const struct http_header headers[] = {
			{ .name = "content-type", .value = "application/json" },
			{ .name = "cache-control", .value = "no-cache" },
		};
		ctx->headers = headers;
		ctx->header_count = ARRAY_SIZE(headers);
	}

	return 0;
}

/*
 * XXX: Zephyr could provide a send_http1_401 response to send a
 * canonical 401 header, but this still seems to work.
 */
static void set_unauth_response(struct http_client_ctx *client,
				struct http_response_ctx *ctx)
{
	/* XXX: should this also include allow header? */
	static struct http_header extra_headers[] = {
		{ .name = "www-authenticate", .value = "Basic realm=\"BMC\"" },
	};
	size_t header_count = client->header_capture_ctx.count;
	const struct http_header *headers = client->header_capture_ctx.headers;
	bool webui = false;

	for (unsigned int i = 0; i < header_count; i++) {
		if (!strcmp(headers[i].name, "x-auth-code") &&
		    !strcmp(headers[i].value, "webui")) {
			webui = true;
			break;
		}
	}

	ctx->status = HTTP_401_UNAUTHORIZED;
	ctx->final_chunk = true;
	if (!webui) {
		/*
		 * Special case hack for the JS request coming from the WebUI.
		 * If we return a www-authenticate header here, the browser
		 * intercepts it and pops up a standard HTTP login box, which
		 * is not what we want (the JS interprets the request and
		 * displays an authentication error).
		 */
		ctx->headers = extra_headers;
		ctx->header_count = ARRAY_SIZE(extra_headers);
	}
	ctx->body = NULL;
	ctx->body_len = 0;
}

struct http_resource_user_data {
	bool started;
	/* A POST handler put a response body in data_buffer */
	bool respond;
	size_t size;
	size_t data_len;
	uint8_t *data_buffer;
	/* Buffer sizes for the resource */
	size_t get_size;
	size_t post_size;
};

#define USER_DATA_BUFFER_GET_SIZE	1024
#define USER_DATA_BUFFER_POST_SIZE	512

#if defined(CONFIG_APP_HTTPS_PUBLIC_KEY)
/*
 * A certificate chain for ReplaceCertificate, as escaped JSON. The request
 * arrives in one TLS record, so it can't be bigger than one.
 */
#define USER_DATA_BUFFER_CERT_POST_SIZE	CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN
/*
 * The largest certificate that can be installed, plus its escaped line
 * breaks and the rest of the Certificate resource.
 */
#define USER_DATA_BUFFER_CERT_GET_SIZE	(HTTPS_CERT_PEM_SIZE(HTTPS_CERT_LEAF_DER_MAX) + 1536)
#define USER_DATA_HEAP_SIZE		(2048 + USER_DATA_BUFFER_CERT_POST_SIZE + \
					 USER_DATA_BUFFER_CERT_GET_SIZE)
#else
#define USER_DATA_HEAP_SIZE		2048
#endif

static K_HEAP_DEFINE(heap_data_buffer, USER_DATA_HEAP_SIZE);

static bool alloc_user_data(struct http_resource_user_data *user_data, size_t size)
{
	/*
	 * Don't wait, this runs on the HTTP server thread, which is the only
	 * thread that frees.
	 */
	user_data->data_buffer = k_heap_alloc(&heap_data_buffer, size, K_NO_WAIT);
	if (!user_data->data_buffer) {
		LOG_ERR("No memory for %zu byte request buffer", size);
		return false;
	}

	user_data->started = true;
	user_data->respond = false;
	user_data->size = size;
	user_data->data_len = 0;
	return true;
}

static void free_user_data(struct http_resource_user_data *user_data)
{
	if (!user_data->started)
		return;

	user_data->started = false;
	user_data->respond = false;
	user_data->size = 0;
	user_data->data_len = 0;
	k_heap_free(&heap_data_buffer, user_data->data_buffer);
	user_data->data_buffer = NULL;
}

static bool append_user_data(struct http_resource_user_data *user_data, const void *src, size_t size)
{
	if (user_data->data_len + size > user_data->size) {
		LOG_WRN("HTTP user_data buffer out of space size=%zu < %zu",
			user_data->size, user_data->data_len + size);
		return false;
	}

	memcpy(user_data->data_buffer + user_data->data_len, src, size);
	user_data->data_len += size;

	return true;
}

static int user_data_json_append(const char *bytes, size_t len, void *data)
{
	struct http_resource_user_data *user_data = data;

	if (!append_user_data(user_data, bytes, len))
		return -ENOSPC;
	return 0;
}

/* Error response body */
struct redfish_message {
	const char *message_id;
	const char *message;
};
static const struct json_obj_descr message_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_message, "MessageId", message_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_message, "Message", message, JSON_TOK_STRING),
};

struct redfish_error_body {
	const char *code;
	const char *message;
	struct redfish_message extended_info[1];
	size_t extended_info_len;
};
static const struct json_obj_descr error_body_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct redfish_error_body, code, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct redfish_error_body, message, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJ_ARRAY_NAMED(struct redfish_error_body, "@Message.ExtendedInfo",
				       extended_info, 1, extended_info_len,
				       message_descr, ARRAY_SIZE(message_descr)),
};

struct redfish_error {
	struct redfish_error_body error;
};
static const struct json_obj_descr error_descr[] = {
	JSON_OBJ_DESCR_OBJECT(struct redfish_error, error, error_body_descr),
};

/*
 * Make a POST handler respond with status and a Redfish error body,
 * replacing the request in user_data. Returns status.
 */
static __maybe_unused int redfish_error(struct http_resource_user_data *user_data, int status,
					const char *message_id, const char *message)
{
	const struct redfish_error error = {
		.error = {
			.code = message_id,
			.message = message,
			.extended_info = { { .message_id = message_id, .message = message } },
			.extended_info_len = 1,
		},
	};

	LOG_WRN("%s", message);

	user_data->data_len = 0;
	user_data->respond = json_obj_encode(error_descr, ARRAY_SIZE(error_descr), &error,
					     user_data_json_append, user_data) == 0;
	return status;
}

static int redfish_handler(struct http_client_ctx *client,
			   enum http_transaction_status status,
			   const struct http_request_ctx *request_ctx,
			   struct http_response_ctx *response_ctx,
			   struct http_resource_user_data *user_data,
			   bool require_auth,
			   int (*get_fn)(struct http_resource_user_data *user_data),
			   int (*patch_fn)(struct http_resource_user_data *user_data),
			   int (*post_fn)(struct http_resource_user_data *user_data),
			   int (*delete_fn)(struct http_resource_user_data *user_data))
{
	uint32_t allow_methods = 0;
	int ret;

	if (status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		if (client->method == HTTP_GET || user_data->respond) {
			free_user_data(user_data);
		} else {
			if (user_data->started) {
				LOG_ERR("user_data not cleared before transaction complete");
				free_user_data(user_data);
			}
		}
		return 0;
	}

	if (status == HTTP_SERVER_TRANSACTION_ABORTED) {
		free_user_data(user_data);
		return 0;
	}

	if (get_fn)
		allow_methods |= BIT(HTTP_GET);
	if (patch_fn)
		allow_methods |= BIT(HTTP_PATCH);
	if (post_fn)
		allow_methods |= BIT(HTTP_POST);
	if (delete_fn)
		allow_methods |= BIT(HTTP_DELETE);

	if (validate_and_set_headers(client, response_ctx, allow_methods) < 0)
		return 0;

	if (require_auth && validate_auth(client) < 0) {
		LOG_ERR("Failed to authenticate");
		set_unauth_response(client, response_ctx);
		return 0;
	}

	if (client->method == HTTP_PATCH || client->method == HTTP_POST ||
	    client->method == HTTP_DELETE) {
		if (!user_data->started && !alloc_user_data(user_data, user_data->post_size)) {
			response_ctx->status = HTTP_503_SERVICE_UNAVAILABLE;
			response_ctx->final_chunk = true;
			return 0;
		}

		/* Accumulate requests into the user_buffer, until the final request. */
		if (request_ctx->data && request_ctx->data_len > 0) {
			if (!append_user_data(user_data, request_ctx->data, request_ctx->data_len)) {
				LOG_ERR("Payload too large");
				free_user_data(user_data);
				response_ctx->status = HTTP_400_BAD_REQUEST;
				response_ctx->final_chunk = true;
				return 0;
			}
		}

		if (status != HTTP_SERVER_REQUEST_DATA_FINAL)
			return 0;

		if (client->method == HTTP_PATCH)
			ret = patch_fn(user_data);
		else if (client->method == HTTP_POST)
			ret = post_fn(user_data);
		else /* client->method == HTTP_DELETE, any body is ignored */
			ret = delete_fn(user_data);

		if (user_data->respond) {
			static const struct http_header headers[] = {
				{ .name = "content-type", .value = "application/json" },
				{ .name = "cache-control", .value = "no-cache" },
			};

			/* Freed when the transaction completes */
			response_ctx->headers = headers;
			response_ctx->header_count = ARRAY_SIZE(headers);
			response_ctx->body = user_data->data_buffer;
			response_ctx->body_len = user_data->data_len;
			response_ctx->status = ret ? ret : HTTP_200_OK;
		} else {
			free_user_data(user_data);
			if (ret)
				response_ctx->status = ret;
			else
				response_ctx->status = HTTP_204_NO_CONTENT; /* 204 is success */
		}

	} else if (client->method == HTTP_GET && get_fn) {
		if (user_data->started) {
			LOG_ERR("HTTP_GET has started user data");
			free_user_data(user_data);
		}

		if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
			/* No support for accumulating GET requests (they should be small) */
			response_ctx->status = HTTP_400_BAD_REQUEST;
			response_ctx->final_chunk = true;
			return 0;
		}

		if (!alloc_user_data(user_data, user_data->get_size)) {
			response_ctx->status = HTTP_503_SERVICE_UNAVAILABLE;
			response_ctx->final_chunk = true;
			return 0;
		}

		ret = get_fn(user_data);
		if (ret) {
			response_ctx->status = ret;
		} else {
			response_ctx->body = user_data->data_buffer;
			response_ctx->body_len = user_data->data_len;
			response_ctx->status = HTTP_200_OK;
		}
	} else {
		/* Should not get here (validate should have caught it) */
		response_ctx->status = HTTP_500_INTERNAL_SERVER_ERROR;
	}

	response_ctx->final_chunk = true;
	return 0;
}

#define REDFISH_HANDLER(name, url, require_auth, get_handler, patch_handler, post_handler) \
	REDFISH_HANDLER_SIZED(name, url, require_auth, get_handler, patch_handler,	\
			      post_handler, NULL, USER_DATA_BUFFER_GET_SIZE,		\
			      USER_DATA_BUFFER_POST_SIZE)

#define REDFISH_HANDLER_SIZED(name, url, require_auth, get_handler, patch_handler,	\
			      post_handler, delete_handler, get_size_, post_size_)	\
/* Zephyr HTTP API serialises requests by resource so static data can be used here. */	\
static struct http_resource_user_data name##_user_data = {				\
	.get_size = get_size_,								\
	.post_size = post_size_,							\
};											\
static int name##_handler(struct http_client_ctx *client,				\
			  enum http_transaction_status status,				\
			  const struct http_request_ctx *request_ctx,			\
			  struct http_response_ctx *response_ctx,			\
			  void *user_data)						\
{											\
	return redfish_handler(client, status, request_ctx, response_ctx,		\
				user_data,						\
				require_auth,						\
				get_handler,						\
				patch_handler,						\
				post_handler,						\
				delete_handler);					\
}											\
static struct http_resource_detail_dynamic name##_detail = {				\
	.common = {									\
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,					\
		.bitmask_of_supported_http_methods = -1U,				\
	},										\
	.cb = name##_handler,								\
	.user_data = &name##_user_data,							\
};											\
ALL_HTTP_RESOURCE_DEFINE(name, url, &name##_detail);

/*** Core Redfish types/containers ***/

/* Collection: Member nested object */
struct redfish_member {
	const char *odata_id;
};
static const struct json_obj_descr member_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_member, "@odata.id",
				  odata_id, JSON_TOK_STRING),
};

#define REDFISH_COLLECTION_MEMBERS_MAX 1

/* Collection of members */
struct redfish_collection {
	const char *odata_id;
	const char *odata_type;
	const char *name;
	int32_t members_count;
	size_t members_len;
	struct redfish_member members[REDFISH_COLLECTION_MEMBERS_MAX];
};
static const struct json_obj_descr collection_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_collection, "@odata.id",
				  odata_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_collection, "@odata.type",
				  odata_type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_collection, "Name",
				  name, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_collection,
				  "Members@odata.count", members_count,
				  JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_OBJ_ARRAY_NAMED(struct redfish_collection,
				       "Members", members,
				       REDFISH_COLLECTION_MEMBERS_MAX, members_len,
				       member_descr, ARRAY_SIZE(member_descr)),
};

/* Link (nested by others) */
struct redfish_link {
	const char *odata_id;
};
static const struct json_obj_descr link_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_link, "@odata.id",
				  odata_id, JSON_TOK_STRING),
};


/*** /redfish/ ***/
struct redfish_version {
	const char *v1;
};
static const struct json_obj_descr version_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_version, "v1",
				  v1, JSON_TOK_STRING),
};

/* GET /redfish/ */
static int redfish_version_get_handler(struct http_resource_user_data *user_data)
{
	const struct redfish_version version = {
		.v1 = "/redfish/v1/"
	};
	int ret;

	ret = json_obj_encode(version_descr, ARRAY_SIZE(version_descr),
			      &version, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode redfish/v1/: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(redfish_version, "/redfish/",
		false, /* do not require auth */
		redfish_version_get_handler, NULL, NULL);
ALL_HTTP_RESOURCE_DEFINE(redfish_version_no_slash, "/redfish", &redfish_version_detail);

/*** /redfish/v1/ ***/
/* ServiceRoot Links (Sessions is mandatory per the ServiceRoot schema) */
struct redfish_service_root_links {
	struct redfish_link sessions;
};
static const struct json_obj_descr service_root_links_descr[] = {
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_service_root_links, "Sessions",
				    sessions, link_descr),
};

struct redfish_service_root {
	const char *odata_type;
	const char *odata_id;
	const char *id;
	const char *name;
	const char *redfish_version;
	const char *uuid;
	struct redfish_link account_service;
	struct redfish_link session_service;
	struct redfish_link systems;
	struct redfish_link managers;
	struct redfish_link chassis;
#if defined(CONFIG_APP_HTTPS_PUBLIC_KEY)
	struct redfish_link certificate_service;
#endif
	struct redfish_service_root_links links;
};
static const struct json_obj_descr service_root_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_service_root, "@odata.type",
				  odata_type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_service_root, "@odata.id",
				  odata_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_service_root, "Id",
				  id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_service_root, "Name",
				  name, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_service_root, "RedfishVersion",
				  redfish_version, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_service_root, "UUID",
				  uuid, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_service_root, "AccountService",
				    account_service, link_descr),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_service_root, "SessionService",
				    session_service, link_descr),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_service_root, "Managers",
				    managers, link_descr),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_service_root, "Systems",
				    systems, link_descr),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_service_root, "Chassis",
				    chassis, link_descr),
#if defined(CONFIG_APP_HTTPS_PUBLIC_KEY)
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_service_root, "CertificateService",
				    certificate_service, link_descr),
#endif
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_service_root, "Links",
				    links, service_root_links_descr),
};

/* GET /redfish/v1/ */
static int service_root_get_handler(struct http_resource_user_data *user_data)
{
	const struct redfish_service_root service_root = {
		.odata_type = "#ServiceRoot.v1_16_1.ServiceRoot",
		.odata_id = "/redfish/v1/",
		.id = "RootService",
		.name = "Root Service",
		.redfish_version = "1.15.0",
		.uuid = "92384634-2938-2342-8820-489239905423",
		.account_service = {
			.odata_id = "/redfish/v1/AccountService"
		},
		.session_service = {
			.odata_id = "/redfish/v1/SessionService"
		},
		.managers = {
			.odata_id = "/redfish/v1/Managers"
		},
		.systems = {
			.odata_id = "/redfish/v1/Systems"
		},
		.chassis = {
			.odata_id = "/redfish/v1/Chassis"
		},
#if defined(CONFIG_APP_HTTPS_PUBLIC_KEY)
		.certificate_service = {
			.odata_id = "/redfish/v1/CertificateService"
		},
#endif
		.links = {
			.sessions = {
				.odata_id = "/redfish/v1/SessionService/Sessions"
			},
		},
	};
	int ret;

	ret = json_obj_encode(service_root_descr, ARRAY_SIZE(service_root_descr),
				&service_root, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode service root: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(service_root, "/redfish/v1/",
		false, /* do not require auth */
		service_root_get_handler, NULL, NULL);
ALL_HTTP_RESOURCE_DEFINE(service_root_no_slash, "/redfish/v1", &service_root_detail);

/*** /redfish/v1/SessionService ***/
struct redfish_session_service {
	const char *odata_id;
	const char *odata_type;
	const char *id;
	const char *name;
	struct redfish_link sessions;
};
static const struct json_obj_descr session_service_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_session_service, "@odata.id", odata_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_session_service, "@odata.type", odata_type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_session_service, "Id", id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_session_service, "Name", name, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_session_service, "Sessions", sessions, link_descr),
};

/* GET /redfish/v1/SessionService */
static int session_service_get_handler(struct http_resource_user_data *user_data)
{
	const struct redfish_session_service session_service = {
		.odata_id = "/redfish/v1/SessionService",
		.odata_type = "#SessionService.v1_1_9.SessionService",
		.id = "SessionService",
		.name = "Session Service",
		.sessions = {
			.odata_id = "/redfish/v1/SessionService/Sessions",
		},
	};
	int ret;

	ret = json_obj_encode(session_service_descr, ARRAY_SIZE(session_service_descr),
				&session_service, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode session service: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(session_service, "/redfish/v1/SessionService",
		false, /* do not require auth */
		session_service_get_handler, NULL, NULL);

/*** /redfish/v1/SessionService/Sessions ***/
/*
 * WallaBMC uses HTTP Basic auth and has no active sessions, so this is an
 * empty (but schema-valid) SessionCollection.
 */
/* GET /redfish/v1/SessionService/Sessions */
static int sessions_collection_get_handler(struct http_resource_user_data *user_data)
{
	const struct redfish_collection sessions_collection = {
		.odata_id = "/redfish/v1/SessionService/Sessions",
		.odata_type = "#SessionCollection.SessionCollection",
		.name = "Session Collection",
		.members_count = 0,
		.members_len = 0,
	};
	int ret;

	ret = json_obj_encode(collection_descr, ARRAY_SIZE(collection_descr),
				&sessions_collection, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode sessions collection: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(sessions_collection, "/redfish/v1/SessionService/Sessions",
		true, /* require auth */
		sessions_collection_get_handler, NULL, NULL);


/*** /redfish/v1/odata ***/
struct redfish_odata_value {
	const char *name;
	const char *kind;
	const char *url;
};
static const struct json_obj_descr odata_value_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_odata_value, "name",
				  name, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_odata_value, "kind",
				  kind, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_odata_value, "url",
				  url, JSON_TOK_STRING),
};

#define REDFISH_ODATA_VALUES_MAX 5

struct redfish_odata {
	const char *odata_context;
	size_t value_count;
	struct redfish_odata_value value[REDFISH_ODATA_VALUES_MAX];
};
static const struct json_obj_descr odata_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_odata, "@odata.context",
				  odata_context, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJ_ARRAY_NAMED(struct redfish_odata, "value", value,
				       REDFISH_ODATA_VALUES_MAX, value_count,
				       odata_value_descr, ARRAY_SIZE(odata_value_descr)),
};

/* GET /redfish/v1/odata */
static int odata_get_handler(struct http_resource_user_data *user_data)
{
	const struct redfish_odata odata = {
		.odata_context = "/redfish/v1/$metadata",
		.value_count = REDFISH_ODATA_VALUES_MAX,
		.value = {
			{ .name = "Service", .kind = "Singleton", .url = "/redfish/v1/", },
			{ .name = "Systems", .kind = "Singleton", .url = "/redfish/v1/Systems", },
			{ .name = "Managers", .kind = "Singleton", .url = "/redfish/v1/Managers", },
			{ .name = "AccountService", .kind = "Singleton", .url = "/redfish/v1/AccountService", },
			{ .name = "Chassis", .kind = "Singleton", .url = "/redfish/v1/Chassis", },
		},
	};
	int ret;

	ret = json_obj_encode(odata_descr, ARRAY_SIZE(odata_descr),
				&odata, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode odata: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(odata, "/redfish/v1/odata",
		false, /* do not require auth */
		odata_get_handler, NULL, NULL);

/*** /redfish/v1/AccountService ***/
struct redfish_account_service {
	const char *odata_id;
	const char *odata_type;
	const char *id;
	const char *name;
	struct redfish_link accounts;
};
static const struct json_obj_descr account_service_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_account_service, "@odata.id", odata_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_account_service, "@odata.type", odata_type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_account_service, "Id", id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_account_service, "Name", name, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_account_service, "Accounts", accounts, link_descr),
};

/* GET /redfish/v1/AccountService */
static int account_service_get_handler(struct http_resource_user_data *user_data)
{
	const struct redfish_account_service account_service = {
		.odata_id = "/redfish/v1/AccountService",
		.odata_type = "#AccountService.v1_10_0.AccountService",
		.id = "AccountService",
		.name = "Account Service",
		.accounts = {
			.odata_id = "/redfish/v1/AccountService/Accounts",
		},
	};
	int ret;

	ret = json_obj_encode(account_service_descr, ARRAY_SIZE(account_service_descr),
				&account_service, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode account service: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(account_service, "/redfish/v1/AccountService",
		false, /* do not require auth */
		account_service_get_handler, NULL, NULL);

/* GET /redfish/v1/AccountService/Accounts */
static int accounts_collection_get_handler(struct http_resource_user_data *user_data)
{
	const struct redfish_collection accounts_collection = {
		.odata_id = "/redfish/v1/AccountService/Accounts",
		.odata_type = "#ManagerAccountCollection.ManagerAccountCollection",
		.name = "Accounts Collection",
		.members_count = 1,
		.members = {
			{
				.odata_id = "/redfish/v1/AccountService/Accounts/1",
			},
		},
		.members_len = 1,
	};
	int ret;

	ret = json_obj_encode(collection_descr, ARRAY_SIZE(collection_descr),
				&accounts_collection, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode manager: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(accounts_collection, "/redfish/v1/AccountService/Accounts",
		true, /* require auth */
		accounts_collection_get_handler, NULL, NULL);

/*** /redfish/v1/AccountService/Account/1 ***/
struct redfish_account {
	const char *odata_id;
	const char *odata_type;
	const char *id;
	const char *role_id;
	const char *name;
	const char *user_name;
	const char *password; // Write-only
	const char *account_types[1];
	size_t account_types_count;
};
static const struct json_obj_descr account_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_account, "@odata.id", odata_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_account, "@odata.type", odata_type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_account, "Id", id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_account, "RoleId", role_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_account, "Name", name, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_account, "UserName", user_name, JSON_TOK_STRING),
	// Password is usually not returned in GET, but we need descriptor for PATCH parsing
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_account, "Password", password, JSON_TOK_STRING),
	JSON_OBJ_DESCR_ARRAY_NAMED(struct redfish_account, "AccountTypes", account_types,
				   1, account_types_count, JSON_TOK_STRING),
};

/* PATCH /redfish/v1/AccountService/Account/1 */
static int account_patch_handler(struct http_resource_user_data *user_data)
{
	struct redfish_account payload;
	int ret;

	memset(&payload, 0, sizeof(payload));
	ret = json_obj_parse(user_data->data_buffer, user_data->data_len,
			     account_descr, ARRAY_SIZE(account_descr), &payload);
	if (ret < 0) {
		LOG_ERR("Account: Bad JSON (err=%d)", ret);
		return HTTP_400_BAD_REQUEST;
	}

	if (payload.password) {
		config_bmc_password_set(payload.password);
		LOG_INF("Admin Password Updated");
	}

	return 0;
}

/* GET /redfish/v1/AccountService/Account/1 */
static int account_get_handler(struct http_resource_user_data *user_data)
{
	const struct redfish_account account = {
		.odata_id = "/redfish/v1/AccountService/Accounts/1",
		.odata_type = "#ManagerAccount.v1_9_0.ManagerAccount",
		.id = "1",
		.name = "Administrator",
		.role_id = "Administrator",
		.user_name = "admin",
		.password = NULL,
		/*
		 * Zephyr's JSON parser turns NULL password into an empty string
		 * ("") instead of null. That causes redfish validation to complain,
		 * but it shouldn't be a big deal.
		 */
		.account_types = { "Redfish" },
		.account_types_count = 1,
	};
	int ret;

	ret = json_obj_encode(account_descr, ARRAY_SIZE(account_descr),
				&account, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode account: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(account, "/redfish/v1/AccountService/Accounts/1",
		true, /* require auth */
		account_get_handler, account_patch_handler, NULL);

/*** /redfish/v1/Managers ***/
/* GET /redfish/v1/Managers */
static int managers_collection_get_handler(struct http_resource_user_data *user_data)
{
	const struct redfish_collection managers_collection = {
		.odata_id = "/redfish/v1/Managers",
		.odata_type = "#ManagerCollection.ManagerCollection",
		.name = "Manager Collection",
		.members_count = 1,
		.members = {
			{
				.odata_id = "/redfish/v1/Managers/bmc"
			}
		},
		.members_len = 1
	};
	int ret;

	ret = json_obj_encode(collection_descr, ARRAY_SIZE(collection_descr),
				&managers_collection, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode managers collection: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(managers_collection, "/redfish/v1/Managers",
		true, /* require auth */
		managers_collection_get_handler, NULL, NULL);

/*** /redfish/v1/Managers/bmc ***/
struct redfish_manager_oem_wallabmc {
	const char *odata_type;
	const char *zephyr_version;
};
static const struct json_obj_descr manager_oem_wallabmc_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_manager_oem_wallabmc,
				   "@odata.type", odata_type,
				   JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_manager_oem_wallabmc,
				   "ZephyrVersion", zephyr_version,
				   JSON_TOK_STRING),
};
struct redfish_manager_oem {
	struct redfish_manager_oem_wallabmc wallabmc;
};
static const struct json_obj_descr manager_oem_descr[] = {
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_manager_oem, "WallaBMC",
				     wallabmc, manager_oem_wallabmc_descr),
};
struct redfish_manager {
	const char *odata_id;
	const char *odata_type;
	const char *id;
	const char *name;
	const char *uuid;
	const char *date_time;
	const char *firmware_version;
	struct redfish_manager_oem oem;
	struct redfish_link ethernet_interfaces;
};
static const struct json_obj_descr manager_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_manager, "@odata.id", odata_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_manager, "@odata.type", odata_type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_manager, "Id", id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_manager, "Name", name, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_manager, "UUID", uuid, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_manager, "DateTime", date_time, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_manager, "FirmwareVersion", firmware_version, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_manager, "Oem", oem, manager_oem_descr),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_manager, "EthernetInterfaces", ethernet_interfaces, link_descr),
};

static const char *get_iso_time(void)
{
	static char str[30];
	struct timespec ts;
	struct tm *tm_info;

	clock_gettime(CLOCK_REALTIME, &ts);
	tm_info = gmtime(&ts.tv_sec);
	strftime(str, sizeof(str), "%Y-%m-%dT%H:%M:%SZ", tm_info);

	return str;
}

/* PATCH /redfish/v1/Managers/bmc */
static int manager_patch_handler(struct http_resource_user_data *user_data)
{
	struct redfish_manager payload;
	int ret;

	ret = json_obj_parse(user_data->data_buffer, user_data->data_len,
			     manager_descr, ARRAY_SIZE(manager_descr), &payload);
	if (ret < 0) {
		LOG_ERR("Manager: Bad JSON (err=%d)", ret);
		return HTTP_400_BAD_REQUEST;
	}

	if (payload.date_time) {
		ret = time_set_from_iso_str(payload.date_time);
		if (ret) {
			LOG_ERR("Failed to set date/time (err=%d)", ret);
			return HTTP_500_INTERNAL_SERVER_ERROR;
		}
	}

	return 0;
}

/* GET /redfish/v1/Managers/bmc */
static int manager_get_handler(struct http_resource_user_data *user_data)
{
	struct redfish_manager manager = {
		.odata_id = "/redfish/v1/Managers/bmc",
		.odata_type = "#Manager.v1_11_0.Manager",
		.id = "bmc",
		.uuid = NULL,
		.name = "WallaBMC",
		.date_time = get_iso_time(),
		.firmware_version = PROJECT_GIT_SHA,
		.oem = { .wallabmc = {
			.odata_type = "#WallaBMC.v1_0_0.WallaBMC",
			.zephyr_version = BANNER_VERSION,
		} },
		.ethernet_interfaces = {
			.odata_id = "/redfish/v1/Managers/bmc/EthernetInterfaces",
		},
	};
	int ret;

	ret = get_bmc_uuid_string(&manager.uuid);
	if (ret < 0) {
		LOG_ERR("Failed to get BMC UUID: %d", ret);
	}

	ret = json_obj_encode(manager_descr, ARRAY_SIZE(manager_descr),
				&manager, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode manager: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(manager, "/redfish/v1/Managers/bmc",
		true, /* require auth */
		manager_get_handler, manager_patch_handler, NULL);

/*** /redfish/v1/Managers/bmc/EthernetInterfaces ***/
/* GET /redfish/v1/Managers/bmc/EthernetInterfaces */
static int ethernet_collection_get_handler(struct http_resource_user_data *user_data)
{
	const struct redfish_collection ethernet_collection = {
		.odata_id = "/redfish/v1/Managers/bmc/EthernetInterfaces",
		.odata_type = "#EthernetInterfaceCollection.EthernetInterfaceCollection",
		.name = "Ethernet Interface Collection",
		.members_count = 1,
		.members = {
			{
				.odata_id = "/redfish/v1/Managers/bmc/EthernetInterfaces/eth0"
			}
		},
		.members_len = 1
	};
	int ret;

	ret = json_obj_encode(collection_descr, ARRAY_SIZE(collection_descr),
				&ethernet_collection, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode ethernet interfaces collection: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(ethernet_interfaces, "/redfish/v1/Managers/bmc/EthernetInterfaces",
		true, /* require auth */
		ethernet_collection_get_handler, NULL, NULL);

/*** /redfish/v1/Managers/bmc/EthernetInterfaces/eth0 ***/
/* DHCPv4 */
struct redfish_dhcp_v4 {
	uint8_t dhcp_enabled;
};
static const struct json_obj_descr dhcp_v4_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_dhcp_v4, "DHCPEnabled", dhcp_enabled, JSON_TOK_TRUE),
};

/* IPv4Address */
struct redfish_ipv4_addr {
	const char *address;
	const char *subnet_mask;
	const char *gateway;
	const char *address_origin;
};
static const struct json_obj_descr ipv4_addr_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_ipv4_addr, "Address", address, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_ipv4_addr, "SubnetMask", subnet_mask, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_ipv4_addr, "Gateway", gateway, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_ipv4_addr, "AddressOrigin", address_origin, JSON_TOK_STRING),
};

/* EthInterface */
struct redfish_ethernet_interface {
	const char *odata_id;
	const char *odata_type;
	const char *id;
	const char *name;
	const char *host_name;
	struct redfish_dhcp_v4 dhcp_v4;
	struct redfish_ipv4_addr ipv4_addresses[1];
	size_t ipv4_count;
	struct redfish_ipv4_addr ipv4_static_addresses[1];
	size_t ipv4_static_count;
};
static const struct json_obj_descr ethernet_interface_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_ethernet_interface, "@odata.id", odata_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_ethernet_interface, "@odata.type", odata_type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_ethernet_interface, "Id", id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_ethernet_interface, "Name", name, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_ethernet_interface, "HostName", host_name, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_ethernet_interface, "DHCPv4", dhcp_v4, dhcp_v4_descr),
	JSON_OBJ_DESCR_OBJ_ARRAY_NAMED(struct redfish_ethernet_interface, "IPv4Addresses", ipv4_addresses,
		       1, ipv4_count, ipv4_addr_descr, ARRAY_SIZE(ipv4_addr_descr)),
	JSON_OBJ_DESCR_OBJ_ARRAY_NAMED(struct redfish_ethernet_interface, "IPv4StaticAddresses", ipv4_static_addresses,
		       1, ipv4_static_count, ipv4_addr_descr, ARRAY_SIZE(ipv4_addr_descr)),
};

/* PATCH /redfish/v1/Managers/bmc/EthernetInterfaces/eth0 */
static int ethernet_patch_handler(struct http_resource_user_data *user_data)
{
	struct redfish_ethernet_interface payload;
	int ret;

	memset(&payload, 0, sizeof(payload));
	payload.dhcp_v4.dhcp_enabled = 0xff; /* sentinel */
	payload.ipv4_static_count = -1;
	ret = json_obj_parse(user_data->data_buffer, user_data->data_len,
			     ethernet_interface_descr,
			     ARRAY_SIZE(ethernet_interface_descr), &payload);
	if (ret < 0) {
		LOG_ERR("eth0: Bad JSON (err=%d)", ret);
		return HTTP_400_BAD_REQUEST;
	}

	if (payload.host_name) {
		ret = config_bmc_hostname_set(payload.host_name);
		if (ret) {
			LOG_ERR("Failed to set hostname (err=%d)", ret);
			return ret == -EINVAL ? HTTP_400_BAD_REQUEST :
					       HTTP_500_INTERNAL_SERVER_ERROR;
		}
	}

	if (payload.dhcp_v4.dhcp_enabled != 0xff)
		config_bmc_use_dhcp4_set(payload.dhcp_v4.dhcp_enabled);

	if (payload.ipv4_static_count != -1) {
		struct redfish_ipv4_addr *redfish_static_addr = &payload.ipv4_static_addresses[0];

		/* If count is 0 then the addresses will be 0 (which clears the static IP) */
		ret = config_bmc_default_ip4_set(redfish_static_addr->address);
		if (ret) {
			LOG_ERR("Failed to set static IP address (err=%d)", ret);
			return HTTP_500_INTERNAL_SERVER_ERROR;
		}

		ret = config_bmc_default_ip4_nm_set(redfish_static_addr->subnet_mask);
		if (ret) {
			LOG_ERR("Failed to set static IP subnet mask address (err=%d)", ret);
			return HTTP_500_INTERNAL_SERVER_ERROR;
		}

		ret = config_bmc_default_ip4_gw_set(redfish_static_addr->gateway);
		if (ret) {
			LOG_ERR("Failed to set static IP gateway address (err=%d)", ret);
			return HTTP_500_INTERNAL_SERVER_ERROR;
		}
	}

	return 0;
}

/* GET /redfish/v1/Managers/bmc/EthernetInterfaces/eth0 */
static int ethernet_get_handler(struct http_resource_user_data *user_data)
{
	struct redfish_ethernet_interface ethernet_interface = {
		.odata_id = "/redfish/v1/Managers/bmc/EthernetInterfaces/eth0",
		.odata_type = "#EthernetInterface.v1_5_0.EthernetInterface",
		.id = "eth0",
		.name = "BMC Ethernet Interface",
		.host_name = net_hostname_get(),
		.dhcp_v4 = { .dhcp_enabled = config_bmc_use_dhcp4(), },
		.ipv4_count = 0,
		.ipv4_static_count = 0,
		.ipv4_static_addresses = {
			{
				.address = "0.0.0.0",
				.subnet_mask = "0.0.0.0",
				.gateway = "0.0.0.0",
			},
		},
	};
	int ret;

	struct net_if *iface = net_if_get_default();
	char ip_str[NET_IPV4_ADDR_LEN];
	char nm_str[NET_IPV4_ADDR_LEN];
	char gw_str[NET_IPV4_ADDR_LEN];
	if (iface) {
		struct redfish_ipv4_addr *redfish_addr = &ethernet_interface.ipv4_addresses[0];
		const struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;

		for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
			const struct net_if_addr_ipv4 *addr = &ipv4->unicast[i];

			if (addr->ipv4.is_used) {
				ethernet_interface.ipv4_count = 1;
				net_addr_ntop(AF_INET, &addr->ipv4.address.in_addr, ip_str, sizeof(ip_str));
				redfish_addr->address = ip_str;
				net_addr_ntop(AF_INET, &addr->netmask, nm_str, sizeof(nm_str));
				redfish_addr->subnet_mask = nm_str;
				if (addr->ipv4.addr_type == NET_ADDR_DHCP)
					redfish_addr->address_origin = "DHCP";
				else if (addr->ipv4.addr_type == NET_ADDR_MANUAL ||
					 addr->ipv4.addr_type == NET_ADDR_OVERRIDABLE)
					redfish_addr->address_origin = "Static";
				else
					redfish_addr->address_origin = "Unknown";

				if (addr->ipv4.addr_state == NET_ADDR_PREFERRED)
					break;
			}
		}

		if (ethernet_interface.ipv4_count == 1) {
			const struct net_in_addr gw_addr = net_if_ipv4_get_gw(iface);

			if (gw_addr.s_addr) {
				net_addr_ntop(AF_INET, &gw_addr, gw_str, sizeof(gw_str));
				redfish_addr->gateway = gw_str;
			} else {
				redfish_addr->gateway = "0.0.0.0";
			}
		}
	}

	if (config_bmc_default_ip4()) {
		struct redfish_ipv4_addr *redfish_static_addr = &ethernet_interface.ipv4_static_addresses[0];
		char static_ip_str[NET_IPV4_ADDR_LEN];
		char static_nm_str[NET_IPV4_ADDR_LEN];
		char static_gw_str[NET_IPV4_ADDR_LEN];
		uint32_t addr;

		ethernet_interface.ipv4_static_count = 1,

		addr = config_bmc_default_ip4();
		net_addr_ntop(AF_INET, &addr, static_ip_str, sizeof(static_ip_str));
		redfish_static_addr->address = static_ip_str;

		addr = config_bmc_default_ip4_nm();
		net_addr_ntop(AF_INET, &addr, static_nm_str, sizeof(static_nm_str));
		redfish_static_addr->subnet_mask = static_nm_str;

		addr = config_bmc_default_ip4_gw();
		net_addr_ntop(AF_INET, &addr, static_gw_str, sizeof(static_gw_str));
		redfish_static_addr->gateway = static_gw_str;
	}

	ret = json_obj_encode(ethernet_interface_descr, ARRAY_SIZE(ethernet_interface_descr),
				&ethernet_interface, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode ethernet interface: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(ethernet, "/redfish/v1/Managers/bmc/EthernetInterfaces/eth0",
		true, /* require auth */
		ethernet_get_handler, ethernet_patch_handler, NULL);

/*** /redfish/v1/Managers/bmc/NetworkProtocol ***/
struct redfish_ntp {
	uint8_t protocol_enabled;
	const char *ntp_servers[1];
	size_t ntp_servers_count;
};
static const struct json_obj_descr ntp_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_ntp , "ProtocolEnabled", protocol_enabled, JSON_TOK_TRUE),
	JSON_OBJ_DESCR_ARRAY_NAMED(struct redfish_ntp , "NTPServers", ntp_servers, 1, ntp_servers_count, JSON_TOK_STRING),
};

#if defined(CONFIG_APP_HTTPS_PUBLIC_KEY)
struct redfish_https {
	uint8_t protocol_enabled;
	int32_t port;
	struct redfish_link certificates;
};
static const struct json_obj_descr https_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_https, "ProtocolEnabled", protocol_enabled, JSON_TOK_TRUE),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_https, "Port", port, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_https, "Certificates", certificates, link_descr),
};
#endif

/* NetworkProtocol */
struct redfish_network_protocol {
	const char *odata_type;
	const char *id;
	struct redfish_ntp ntp;
#if defined(CONFIG_APP_HTTPS_PUBLIC_KEY)
	struct redfish_https https;
#endif
};
static const struct json_obj_descr network_protocol_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_network_protocol, "@odata.type", odata_type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_network_protocol, "Id", id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_network_protocol, "NTP", ntp, ntp_descr),
#if defined(CONFIG_APP_HTTPS_PUBLIC_KEY)
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_network_protocol, "HTTPS", https, https_descr),
#endif
};

/* PATCH /redfish/v1/Managers/bmc/NetworkProtocol */
static int network_protocol_patch_handler(struct http_resource_user_data *user_data)
{
	struct redfish_network_protocol payload;
	int ret;

	memset(&payload, 0, sizeof(payload));
	payload.ntp.protocol_enabled = 0xff; /* sentinel */
	payload.ntp.ntp_servers_count = -1;

	ret = json_obj_parse(user_data->data_buffer, user_data->data_len,
			     network_protocol_descr,
			     ARRAY_SIZE(network_protocol_descr), &payload);
	if (ret < 0) {
		LOG_ERR("NetworkProtocol: Bad JSON (err=%d)", ret);
		return HTTP_400_BAD_REQUEST;
	}

	if (payload.ntp.ntp_servers_count != -1) {
		/* If count is 0 then the address will be 0 (which clears the static IP) */
		ret = config_bmc_ntp_server_set(payload.ntp.ntp_servers[0]);
		if (ret) {
			LOG_ERR("Failed to set NTP server address (err=%d)", ret);
			return HTTP_500_INTERNAL_SERVER_ERROR;
		}
	}

	if (payload.ntp.protocol_enabled != 0xff)
		config_bmc_use_ntp_set(payload.ntp.protocol_enabled);

	return 0;
}

/* GET /redfish/v1/Managers/bmc/NetworkProtocol */
static int network_protocol_get_handler(struct http_resource_user_data *user_data)
{
	struct redfish_network_protocol network_protocol = {
		.odata_type = "#ManagerNetworkProtocol.v1_9_0.ManagerNetworkProtocol",
		.id = "NetworkProtocol",
		.ntp = {
			.protocol_enabled = config_bmc_use_ntp(),
		},
#if defined(CONFIG_APP_HTTPS_PUBLIC_KEY)
		.https = {
			.protocol_enabled = true,
			.port = 443,
			.certificates = {
				.odata_id = "/redfish/v1/Managers/bmc/NetworkProtocol/HTTPS/Certificates",
			},
		},
#endif
	};
	int ret;

	if (config_bmc_ntp_server()) {
		network_protocol.ntp.ntp_servers[0] = config_bmc_ntp_server();
		network_protocol.ntp.ntp_servers_count = 1;
	}

	ret = json_obj_encode(network_protocol_descr, ARRAY_SIZE(network_protocol_descr),
				&network_protocol, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode network protocol: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(network_protocol, "/redfish/v1/Managers/bmc/NetworkProtocol",
		true, /* require auth */
		network_protocol_get_handler, network_protocol_patch_handler, NULL);

/*** /redfish/v1/Systems ***/
/* GET /redfish/v1/Systems */
static int systems_collection_get_handler(struct http_resource_user_data *user_data)
{
	const struct redfish_collection systems_collection = {
		.odata_id = "/redfish/v1/Systems",
		.odata_type = "#ComputerSystemCollection.ComputerSystemCollection",
		.name = "Computer System Collection",
		.members_count = 1,
		.members = {
			{
				.odata_id = "/redfish/v1/Systems/system"
			}
		},
		.members_len = 1
	};
	int ret;

	ret = json_obj_encode(collection_descr, ARRAY_SIZE(collection_descr),
				&systems_collection, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode systems collection: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(systems_collection, "/redfish/v1/Systems",
		true, /* require auth */
		systems_collection_get_handler, NULL, NULL);

/*** /redfish/v1/Systems/system ***/
static char serial_number[] = "12345";

/*
 * PowerState is encoded through JSON_TOK_ENCODED_OBJ so it can be null:
 * Redfish's PowerState enum has no "Unknown" value and DSP0266 says a
 * property whose value the service cannot determine is null. The
 * encoder appends the string raw, hence the quotes.
 */
static const char *power_state_json(void)
{
	int state = host_power_state();

	if (state < 0)
		return "null";

	return state ? "\"On\"" : "\"Off\"";
}

/* System Info: ResetType array */
struct redfish_reset_action {
	const char *target;
	const char *reset_type_values[4];
	size_t reset_type_values_len;
};
static const struct json_obj_descr reset_action_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct redfish_reset_action, target, JSON_TOK_STRING),
	JSON_OBJ_DESCR_ARRAY_NAMED(struct redfish_reset_action,
				   "ResetType@Redfish.AllowableValues",
				   reset_type_values, 4, reset_type_values_len,
				   JSON_TOK_STRING),
};

/* System Info: Actions nested object */
struct redfish_actions {
	struct redfish_reset_action reset_action;
};
static const struct json_obj_descr actions_descr[] = {
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_actions, "#ComputerSystem.Reset",
				    reset_action, reset_action_descr),
};

/* System Info: ProcessorSummary nested object */
struct redfish_processor_summary {
	const char *odata_type;
	int32_t count;
	const char *model;
};
static const struct json_obj_descr processor_summary_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_processor_summary, "Count",
				  count, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_processor_summary, "Model",
				  model, JSON_TOK_STRING),
};

/* System Info: MemorySummary nested object */
#define REDFISH_SYSTEM_CHASSIS_MAX	1
#define REDFISH_SYSTEM_MANAGERS_MAX	1

struct redfish_system_links {
	size_t chassis_len;
	struct redfish_link chassis[REDFISH_SYSTEM_CHASSIS_MAX];
	size_t managed_by_len;
	struct redfish_link managed_by[REDFISH_SYSTEM_MANAGERS_MAX];
};
static const struct json_obj_descr system_links_descr[] = {
	JSON_OBJ_DESCR_OBJ_ARRAY_NAMED(struct redfish_system_links,
				       "Chassis", chassis,
				       REDFISH_SYSTEM_CHASSIS_MAX, chassis_len,
				       link_descr, ARRAY_SIZE(link_descr)),
	JSON_OBJ_DESCR_OBJ_ARRAY_NAMED(struct redfish_system_links,
				       "ManagedBy", managed_by,
				       REDFISH_SYSTEM_MANAGERS_MAX, managed_by_len,
				       link_descr, ARRAY_SIZE(link_descr)),
};

struct redfish_memory_summary {
	int32_t total_system_GiB;
};
static const struct json_obj_descr memory_summary_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_memory_summary, "TotalSystemMemoryGiB",
				  total_system_GiB, JSON_TOK_NUMBER),
};

struct redfish_computer_system {
	const char *odata_id;
	const char *odata_type;
	const char *id;
	const char *uuid;
	const char *name;
	const char *system_type;
	const char *manufacturer;
	const char *model;
	const char *serial_number;
	const char *host_name;
	const char *power_restore_policy;
	const char *power_state;
	struct redfish_processor_summary processor_summary;
	struct redfish_memory_summary memory_summary;
	struct redfish_actions actions;
	struct redfish_system_links links;
};
static const struct json_obj_descr computer_system_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_computer_system, "@odata.id",
				  odata_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_computer_system, "@odata.type",
				  odata_type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_computer_system, "Id",
				  id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_computer_system, "UUID",
				  uuid, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_computer_system, "Name",
				  name, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_computer_system, "SystemType",
				  system_type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_computer_system, "Manufacturer",
				  manufacturer, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_computer_system, "Model",
				  model, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_computer_system, "SerialNumber",
				  serial_number, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_computer_system, "PowerRestorePolicy",
				  power_restore_policy, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_computer_system, "PowerState",
				  power_state, JSON_TOK_ENCODED_OBJ),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_computer_system, "ProcessorSummary",
				    processor_summary, processor_summary_descr),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_computer_system, "MemorySummary",
				    memory_summary, memory_summary_descr),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_computer_system, "Links",
				    links, system_links_descr),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_computer_system, "Actions",
				    actions, actions_descr),
};

/*
 * PATCH parses with its own descriptor: PowerRestorePolicy is the only
 * writable property, and json_obj_parse() cannot decode the ENCODED_OBJ
 * PowerState above, so a PATCH that also carries the read-only
 * PowerState string would otherwise get 400. Other properties are
 * skipped, except that the parser rejects any top-level null value, so
 * a body echoing a null PowerState still gets 400, as before.
 */
static const struct json_obj_descr computer_system_patch_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_computer_system, "PowerRestorePolicy",
				  power_restore_policy, JSON_TOK_STRING),
};

/* PATCH /redfish/v1/Systems/system */
static int system_patch_handler(struct http_resource_user_data *user_data)
{
	struct redfish_computer_system payload;
	int ret;

	memset(&payload, 0, sizeof(payload));
	ret = json_obj_parse(user_data->data_buffer, user_data->data_len,
			     computer_system_patch_descr,
			     ARRAY_SIZE(computer_system_patch_descr), &payload);
	if (ret < 0) {
		LOG_ERR("System: Bad JSON (err=%d)", ret);
		return HTTP_400_BAD_REQUEST;
	}

	if (payload.power_restore_policy) {
		if (!strcmp(payload.power_restore_policy, "AlwaysOn")) {
			ret = config_host_auto_poweron_set(true);
		} else if (!strcmp(payload.power_restore_policy, "AlwaysOff")) {
			ret = config_host_auto_poweron_set(false);
		} else {
			LOG_ERR("System: Bad power restore value");
			return HTTP_400_BAD_REQUEST;
		}
		if (ret) {
			LOG_ERR("Failed to set host auto-poweron(err=%d)", ret);
			return HTTP_500_INTERNAL_SERVER_ERROR;
		}
	}

	return 0;
}

/* GET /redfish/v1/Systems/system */
static int system_get_handler(struct http_resource_user_data *user_data)
{
	const struct redfish_computer_system computer_system = {
		.odata_id = "/redfish/v1/Systems/system",
		.odata_type = "#ComputerSystem.v1_22_0.ComputerSystem",
		.id = "system",
		.uuid = "38947555-7742-3448-3784-823347823834",
		.name = CONFIG_REDFISH_SYSTEM_PRODUCT_NAME,
		.system_type = "Physical",
		.manufacturer = CONFIG_REDFISH_SYSTEM_MANUFACTURER,
		.model = CONFIG_REDFISH_SYSTEM_MODEL,
		.serial_number = serial_number,
		.processor_summary = {
			.odata_type = "#ProcessorSummary.v1_4_0.ProcessorSummary",
			.count = CONFIG_REDFISH_SYSTEM_PROCESSOR_COUNT,
			.model = CONFIG_REDFISH_SYSTEM_PROCESSOR_MODEL,
		},
		.memory_summary = {
			.total_system_GiB = CONFIG_REDFISH_SYSTEM_MEMORY_GIB,
		},
		.power_restore_policy = config_host_auto_poweron() ? "AlwaysOn" : "AlwaysOff",
		.power_state = power_state_json(),
		.links = {
			.chassis_len = 1,
			.chassis = {
				{
					.odata_id = "/redfish/v1/Chassis/1"
				},
			},
			.managed_by_len = 1,
			.managed_by = {
				{
					.odata_id = "/redfish/v1/Managers/bmc"
				},
			},
		},
		.actions = {
			.reset_action = {
				.target = "/redfish/v1/Systems/system/Actions/ComputerSystem.Reset",
				.reset_type_values = {
					"On",
					"ForceOff",
					"PowerCycle",
					"ForceRestart"
				},
				.reset_type_values_len = 4
			}
		}
	};
	int ret;

	ret = json_obj_encode(computer_system_descr, ARRAY_SIZE(computer_system_descr),
				&computer_system, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode computer system: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(system, "/redfish/v1/Systems/system",
		true, /* require auth */
		system_get_handler, system_patch_handler, NULL);

struct redfish_reset_payload {
	const char *reset_type;
};
static const struct json_obj_descr reset_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_reset_payload, "ResetType",
				  reset_type, JSON_TOK_STRING)
};

/* POST /redfish/v1/Systems/system/Actions/ComputerSystem.Reset */
static int system_reset_post_handler(struct http_resource_user_data *user_data)
{
	struct redfish_reset_payload payload;
	int ret;

	memset(&payload, 0, sizeof(payload));
	ret = json_obj_parse(user_data->data_buffer, user_data->data_len,
			     reset_descr, ARRAY_SIZE(reset_descr), &payload);
	if (ret < 0) {
		LOG_ERR("ComputerSystem.Reset: Bad JSON (err=%d)", ret);
		return HTTP_400_BAD_REQUEST;
	}

	LOG_INF("Reset Action: %s", payload.reset_type);

	if (strcmp(payload.reset_type, "On") == 0) {
		ret = power_set_state(true);
	} else if (strcmp(payload.reset_type, "ForceOff") == 0) {
		ret = power_force_off();
	} else if (strcmp(payload.reset_type, "PowerCycle") == 0) {
		ret = power_cycle();
	} else if (strcmp(payload.reset_type, "ForceRestart") == 0) {
		ret = power_reset();
		if (ret == -ENOTSUP) {
			/*
			 * ForceRestart can be implemented as PowerCycle. Redfish
			 * does not specify exactly how all reset types are
			 * implemented.
			 */
			LOG_INF("ForceRestart: no reset support, falling back to PowerCycle");
			ret = power_cycle();
		}
	} else {
		LOG_ERR("ComputerSystem.Reset: Bad reset type");
		return HTTP_400_BAD_REQUEST;
	}

	if (ret == -EBUSY) {
		/* Another host power operation holds the button; the client can retry */
		LOG_WRN("ComputerSystem.Reset: type %s rejected, host power operation in progress",
			payload.reset_type);
		return HTTP_503_SERVICE_UNAVAILABLE;
	}

	if (ret < 0) {
		LOG_ERR("ComputerSystem.Reset: type %s operation failed",
				payload.reset_type);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(system_reset, "/redfish/v1/Systems/system/Actions/ComputerSystem.Reset",
		true, /* require auth */
		NULL, NULL, system_reset_post_handler);

/*** /redfish/v1/Chassis ***/
/* GET /redfish/v1/Chassis */
static int chassis_collection_get_handler(struct http_resource_user_data *user_data)
{
	const struct redfish_collection chassis_collection = {
		.odata_id = "/redfish/v1/Chassis",
		.odata_type = "#ChassisCollection.ChassisCollection",
		.name = "Chassis Collection",
		.members_count = 1,
		.members = {
			{
				.odata_id = "/redfish/v1/Chassis/1"
			}
		},
		.members_len = 1
	};
	int ret;

	ret = json_obj_encode(collection_descr, ARRAY_SIZE(collection_descr),
			      &chassis_collection, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode chassis collection: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(chassis_collection, "/redfish/v1/Chassis",
		true, /* require auth */
		chassis_collection_get_handler, NULL, NULL);

/*** /redfish/v1/Chassis/1 ***/
#define REDFISH_CHASSIS_COMPUTER_SYSTEMS_MAX	1

struct redfish_chassis_links {
	size_t computer_systems_len;
	struct redfish_link computer_systems[REDFISH_CHASSIS_COMPUTER_SYSTEMS_MAX];
	size_t managed_by_len;
	struct redfish_link managed_by[REDFISH_SYSTEM_MANAGERS_MAX];
};
static const struct json_obj_descr chassis_links_descr[] = {
	JSON_OBJ_DESCR_OBJ_ARRAY_NAMED(struct redfish_chassis_links,
				       "ComputerSystems", computer_systems,
				       REDFISH_CHASSIS_COMPUTER_SYSTEMS_MAX, computer_systems_len,
				       link_descr, ARRAY_SIZE(link_descr)),
	JSON_OBJ_DESCR_OBJ_ARRAY_NAMED(struct redfish_chassis_links,
				       "ManagedBy", managed_by,
				       REDFISH_SYSTEM_MANAGERS_MAX, managed_by_len,
				       link_descr, ARRAY_SIZE(link_descr)),
};

struct redfish_chassis {
	const char *odata_id;
	const char *odata_type;
	const char *id;
	const char *name;
	const char *chassis_type;
	const char *manufacturer;
	const char *model;
	const char *serial_number;
	const char *power_state;
	struct redfish_link sensors;
	struct redfish_chassis_links links;
};
static const struct json_obj_descr chassis_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_chassis, "@odata.id",
				  odata_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_chassis, "@odata.type",
				  odata_type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_chassis, "Id",
				  id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_chassis, "Name",
				  name, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_chassis, "ChassisType",
				  chassis_type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_chassis, "Manufacturer",
				  manufacturer, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_chassis, "Model",
				  model, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_chassis, "SerialNumber",
				  serial_number, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_chassis, "PowerState",
				  power_state, JSON_TOK_ENCODED_OBJ),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_chassis, "Sensors",
				    sensors, link_descr),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_chassis, "Links",
				    links, chassis_links_descr),
};

/* GET /redfish/v1/Chassis/1 */
static int chassis_get_handler(struct http_resource_user_data *user_data)
{
	const struct redfish_chassis chassis = {
		.odata_id = "/redfish/v1/Chassis/1",
		.odata_type = "#Chassis.v1_22_0.Chassis",
		.id = "1",
		.name = "Chassis",
		.chassis_type = "StandAlone",
		.manufacturer = CONFIG_REDFISH_SYSTEM_MANUFACTURER,
		.model = CONFIG_REDFISH_SYSTEM_MODEL,
		.serial_number = serial_number,
		.power_state = power_state_json(),
		.sensors = {
			.odata_id = "/redfish/v1/Chassis/1/Sensors"
		},
		.links = {
			.computer_systems_len = 1,
			.computer_systems = {
				{
					.odata_id = "/redfish/v1/Systems/system"
				},
			},
			.managed_by_len = 1,
			.managed_by = {
				{
					.odata_id = "/redfish/v1/Managers/bmc"
				},
			},
		},
	};
	int ret;

	ret = json_obj_encode(chassis_descr, ARRAY_SIZE(chassis_descr),
			      &chassis, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode chassis: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(chassis, "/redfish/v1/Chassis/1",
		true, /* require auth */
		chassis_get_handler, NULL, NULL);

/*** /redfish/v1/Chassis/1/Sensors ***/

#ifdef CONFIG_APP_SENSORS
#define REDFISH_SENSORS_MEMBERS_MAX	1
#else
#define REDFISH_SENSORS_MEMBERS_MAX	0
#endif

/* GET /redfish/v1/Chassis/1/Sensors */
static int sensors_collection_get_handler(struct http_resource_user_data *user_data)
{
	const struct redfish_collection sensors_collection = {
		.odata_id = "/redfish/v1/Chassis/1/Sensors",
		.odata_type = "#SensorCollection.SensorCollection",
		.name = "Chassis Sensor Collection",
		.members_count = REDFISH_SENSORS_MEMBERS_MAX,
#ifdef CONFIG_APP_SENSORS
		.members = {
			{
				.odata_id = "/redfish/v1/Chassis/1/Sensors/TempBmc"
			}
		},
		.members_len = 1,
#else
		.members_len = 0,
#endif
	};
	int ret;

	ret = json_obj_encode(collection_descr, ARRAY_SIZE(collection_descr),
			      &sensors_collection, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode sensors collection: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(sensors_collection, "/redfish/v1/Chassis/1/Sensors",
		true, /* require auth */
		sensors_collection_get_handler, NULL, NULL);

/*** /redfish/v1/Chassis/1/Sensors/TempBmc ***/

#ifdef CONFIG_APP_SENSORS
struct redfish_sensor {
	const char *odata_id;
	const char *odata_type;
	const char *id;
	const char *name;
	int32_t reading;
	const char *reading_type;
	const char *reading_units;
	const char *physical_context;
};
static const struct json_obj_descr sensor_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_sensor, "@odata.id",
				  odata_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_sensor, "@odata.type",
				  odata_type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_sensor, "Id",
				  id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_sensor, "Name",
				  name, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_sensor, "Reading",
				  reading, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_sensor, "ReadingType",
				  reading_type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_sensor, "ReadingUnits",
				  reading_units, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_sensor, "PhysicalContext",
				  physical_context, JSON_TOK_STRING),
};

/* GET /redfish/v1/Chassis/1/Sensors/TempBmc */
static int sensor_temp_bmc_get_handler(struct http_resource_user_data *user_data)
{
	struct redfish_sensor sensor_temp_bmc = {
		.odata_id = "/redfish/v1/Chassis/1/Sensors/TempBmc",
		.odata_type = "#Sensor.v1_2_0.Sensor",
		.id = "TempBmc",
		.name = "BMC SoC Temperature",
		.reading = 40,
		.reading_type = "Temperature",
		.reading_units = "Cel",
		.physical_context = "Manager",
	};
	struct sensor_value val;
	int ret;

	ret = read_die_temperature(&val);
	if (ret < 0) {
		/* XXX: mark sensor as not okay? */
		sensor_temp_bmc.reading = -1;
	} else {
		/* XXX: how to do decimals without FP */
		sensor_temp_bmc.reading = val.val1;
	}

	ret = json_obj_encode(sensor_descr, ARRAY_SIZE(sensor_descr),
			      &sensor_temp_bmc, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode Sensor TempBmc: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(sensor_temp_bmc, "/redfish/v1/Chassis/1/Sensors/TempBmc",
		true, /* require auth */
		sensor_temp_bmc_get_handler, NULL, NULL);
#endif /* CONFIG_APP_SENSORS */

#if defined(CONFIG_APP_HTTPS_PUBLIC_KEY)
/*** Certificates: /redfish/v1/CertificateService and the HTTPS certificate ***/
#define CERT_SERVICE_URL	"/redfish/v1/CertificateService"
#define HTTPS_CERTS_URL		"/redfish/v1/Managers/bmc/NetworkProtocol/HTTPS/Certificates"
#define HTTPS_CERT_URL		HTTPS_CERTS_URL "/1"

/* CertificateService */
struct redfish_action_target {
	const char *target;
};
static const struct json_obj_descr action_target_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct redfish_action_target, target, JSON_TOK_STRING),
};

struct redfish_cert_service_actions {
	struct redfish_action_target generate_csr;
	struct redfish_action_target replace_certificate;
};
static const struct json_obj_descr cert_service_actions_descr[] = {
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_cert_service_actions,
				    "#CertificateService.GenerateCSR",
				    generate_csr, action_target_descr),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_cert_service_actions,
				    "#CertificateService.ReplaceCertificate",
				    replace_certificate, action_target_descr),
};

struct redfish_cert_service {
	const char *odata_id;
	const char *odata_type;
	const char *id;
	const char *name;
	struct redfish_cert_service_actions actions;
	struct redfish_link certificate_locations;
};
static const struct json_obj_descr cert_service_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert_service, "@odata.id", odata_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert_service, "@odata.type", odata_type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert_service, "Id", id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert_service, "Name", name, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_cert_service, "Actions", actions,
				    cert_service_actions_descr),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_cert_service, "CertificateLocations",
				    certificate_locations, link_descr),
};

/* GET /redfish/v1/CertificateService */
static int cert_service_get_handler(struct http_resource_user_data *user_data)
{
	const struct redfish_cert_service cert_service = {
		.odata_id = CERT_SERVICE_URL,
		.odata_type = "#CertificateService.v1_0_4.CertificateService",
		.id = "CertificateService",
		.name = "Certificate Service",
		.actions = {
			.generate_csr = {
				.target = CERT_SERVICE_URL "/Actions/CertificateService.GenerateCSR",
			},
			.replace_certificate = {
				.target = CERT_SERVICE_URL "/Actions/CertificateService.ReplaceCertificate",
			},
		},
		.certificate_locations = {
			.odata_id = CERT_SERVICE_URL "/CertificateLocations",
		},
	};
	int ret;

	ret = json_obj_encode(cert_service_descr, ARRAY_SIZE(cert_service_descr),
			      &cert_service, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode certificate service: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(cert_service, CERT_SERVICE_URL,
		true, /* require auth */
		cert_service_get_handler, NULL, NULL);

/* CertificateLocations */
struct redfish_cert_locations_links {
	struct redfish_member certificates[1];
	size_t certificates_len;
	int32_t certificates_count;
};
static const struct json_obj_descr cert_locations_links_descr[] = {
	JSON_OBJ_DESCR_OBJ_ARRAY_NAMED(struct redfish_cert_locations_links, "Certificates",
				       certificates, 1, certificates_len,
				       member_descr, ARRAY_SIZE(member_descr)),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert_locations_links, "Certificates@odata.count",
				  certificates_count, JSON_TOK_NUMBER),
};

struct redfish_cert_locations {
	const char *odata_id;
	const char *odata_type;
	const char *id;
	const char *name;
	struct redfish_cert_locations_links links;
};
static const struct json_obj_descr cert_locations_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert_locations, "@odata.id", odata_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert_locations, "@odata.type", odata_type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert_locations, "Id", id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert_locations, "Name", name, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_cert_locations, "Links", links,
				    cert_locations_links_descr),
};

/* GET /redfish/v1/CertificateService/CertificateLocations */
static int cert_locations_get_handler(struct http_resource_user_data *user_data)
{
	const struct redfish_cert_locations cert_locations = {
		.odata_id = CERT_SERVICE_URL "/CertificateLocations",
		.odata_type = "#CertificateLocations.v1_0_2.CertificateLocations",
		.id = "CertificateLocations",
		.name = "Certificate Locations",
		.links = {
			.certificates = { { .odata_id = HTTPS_CERT_URL } },
			.certificates_len = 1,
			.certificates_count = 1,
		},
	};
	int ret;

	ret = json_obj_encode(cert_locations_descr, ARRAY_SIZE(cert_locations_descr),
			      &cert_locations, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode certificate locations: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(cert_locations, CERT_SERVICE_URL "/CertificateLocations",
		true, /* require auth */
		cert_locations_get_handler, NULL, NULL);

/* GET /redfish/v1/Managers/bmc/NetworkProtocol/HTTPS/Certificates */
static int https_certs_get_handler(struct http_resource_user_data *user_data)
{
	const struct redfish_collection collection = {
		.odata_id = HTTPS_CERTS_URL,
		.odata_type = "#CertificateCollection.CertificateCollection",
		.name = "HTTPS Certificate Collection",
		.members_count = 1,
		.members = { { .odata_id = HTTPS_CERT_URL } },
		.members_len = 1,
	};
	int ret;

	ret = json_obj_encode(collection_descr, ARRAY_SIZE(collection_descr),
			      &collection, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode HTTPS certificate collection: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

REDFISH_HANDLER(https_certs, HTTPS_CERTS_URL,
		true, /* require auth */
		https_certs_get_handler, NULL, NULL);

/* Certificate */
struct redfish_cert_dn {
	const char *cn;
	const char *o;
	const char *ou;
	const char *l;
	const char *st;
	const char *c;
	const char *email;
};
static const struct json_obj_descr cert_dn_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert_dn, "CommonName", cn, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert_dn, "Organization", o, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert_dn, "OrganizationalUnit", ou, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert_dn, "City", l, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert_dn, "State", st, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert_dn, "Country", c, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert_dn, "Email", email, JSON_TOK_STRING),
};

struct redfish_cert {
	const char *odata_id;
	const char *odata_type;
	const char *id;
	const char *name;
	const char *certificate_string;
	const char *certificate_type;
	struct redfish_cert_dn subject;
	struct redfish_cert_dn issuer;
	const char *valid_not_before;
	const char *valid_not_after;
	const char *key_usage[4];
	size_t key_usage_len;
	const char *fingerprint;
	const char *fingerprint_hash_algorithm;
	const char *serial_number;
};

/* Subject and Issuer are filled in by cert_get_handler() */
#define CERT_DESCR_SUBJECT	6
#define CERT_DESCR_ISSUER	7
static const struct json_obj_descr cert_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert, "@odata.id", odata_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert, "@odata.type", odata_type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert, "Id", id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert, "Name", name, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert, "CertificateString",
				  certificate_string, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert, "CertificateType",
				  certificate_type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_cert, "Subject", subject, cert_dn_descr),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_cert, "Issuer", issuer, cert_dn_descr),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert, "ValidNotBefore",
				  valid_not_before, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert, "ValidNotAfter",
				  valid_not_after, JSON_TOK_STRING),
	JSON_OBJ_DESCR_ARRAY_NAMED(struct redfish_cert, "KeyUsage", key_usage, 4,
				   key_usage_len, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert, "Fingerprint", fingerprint, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert, "FingerprintHashAlgorithm",
				  fingerprint_hash_algorithm, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_cert, "SerialNumber",
				  serial_number, JSON_TOK_STRING),
};

/*
 * Only the attributes that are present are included, so make descriptors
 * for those. dn_descr must have room for ARRAY_SIZE(cert_dn_descr).
 */
static size_t cert_dn_fill(const struct https_cert_dn *dn, struct redfish_cert_dn *out,
			   struct json_obj_descr *dn_descr)
{
	const char *values[] = { dn->cn, dn->o, dn->ou, dn->l, dn->st, dn->c, dn->email };
	const char **fields = (const char **)out;
	size_t n = 0;

	BUILD_ASSERT(ARRAY_SIZE(values) == ARRAY_SIZE(cert_dn_descr));
	BUILD_ASSERT(sizeof(struct redfish_cert_dn) == sizeof(values));

	for (size_t i = 0; i < ARRAY_SIZE(values); i++) {
		fields[i] = values[i];
		if (values[i][0])
			dn_descr[n++] = cert_dn_descr[i];
	}

	return n;
}

/* GET /redfish/v1/Managers/bmc/NetworkProtocol/HTTPS/Certificates/1 */
static int cert_get_handler(struct http_resource_user_data *user_data)
{
	/* The handler isn't reentered, keep these off the stack */
	static struct https_cert_details details;
	static char pem[HTTPS_CERT_PEM_SIZE(HTTPS_CERT_LEAF_DER_MAX)];
	static struct json_obj_descr descr[ARRAY_SIZE(cert_descr)];
	static struct json_obj_descr subject_descr[ARRAY_SIZE(cert_dn_descr)];
	static struct json_obj_descr issuer_descr[ARRAY_SIZE(cert_dn_descr)];
	struct redfish_cert cert = {
		.odata_id = HTTPS_CERT_URL,
		.odata_type = "#Certificate.v1_8_0.Certificate",
		.id = "1",
		.name = "HTTPS Certificate",
		.certificate_string = pem,
		.certificate_type = "PEM",
		.valid_not_before = details.not_before,
		.valid_not_after = details.not_after,
		.fingerprint = details.fingerprint,
		.fingerprint_hash_algorithm = "TPM_ALG_SHA256",
		.serial_number = details.serial,
	};
	int ret;

	ret = https_cert_details_get(&details);
	if (!ret)
		ret = https_cert_pem_get(pem, sizeof(pem));
	if (ret)
		return ret == -ENOENT ? HTTP_404_NOT_FOUND : HTTP_500_INTERNAL_SERVER_ERROR;

	if (details.digital_signature)
		cert.key_usage[cert.key_usage_len++] = "DigitalSignature";
	if (details.key_agreement)
		cert.key_usage[cert.key_usage_len++] = "KeyAgreement";
	if (details.server_auth)
		cert.key_usage[cert.key_usage_len++] = "ServerAuthentication";
	if (details.client_auth)
		cert.key_usage[cert.key_usage_len++] = "ClientAuthentication";

	memcpy(descr, cert_descr, sizeof(descr));
	descr[CERT_DESCR_SUBJECT].object.sub_descr = subject_descr;
	descr[CERT_DESCR_SUBJECT].object.sub_descr_len =
		cert_dn_fill(&details.subject, &cert.subject, subject_descr);
	descr[CERT_DESCR_ISSUER].object.sub_descr = issuer_descr;
	descr[CERT_DESCR_ISSUER].object.sub_descr_len =
		cert_dn_fill(&details.issuer, &cert.issuer, issuer_descr);

	ret = json_obj_encode(descr, ARRAY_SIZE(descr), &cert, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode HTTPS certificate: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	return 0;
}

/*
 * DELETE /redfish/v1/Managers/bmc/NetworkProtocol/HTTPS/Certificates/1
 *
 * There must always be a certificate, so replace it with a new self-signed
 * one for the same key, like HPE iLO does.
 */
static int cert_delete_handler(struct http_resource_user_data *user_data)
{
	int ret;

	ret = https_cert_self_signed();
	if (ret)
		return redfish_error(user_data, HTTP_500_INTERNAL_SERVER_ERROR,
				     "Base.1.8.GeneralError",
				     "Could not issue a self-signed certificate");

	return 0;
}

REDFISH_HANDLER_SIZED(https_cert, HTTPS_CERT_URL,
		      true, /* require auth */
		      cert_get_handler, NULL, NULL, cert_delete_handler,
		      USER_DATA_BUFFER_CERT_GET_SIZE, USER_DATA_BUFFER_POST_SIZE);

/* GenerateCSR */
#define CSR_ALT_NAMES_MAX	8

struct redfish_generate_csr {
	struct redfish_link certificate_collection;
	const char *cn;
	const char *o;
	const char *ou;
	const char *l;
	const char *st;
	const char *c;
	const char *email;
	const char *alt_names[CSR_ALT_NAMES_MAX];
	size_t alt_names_len;
	const char *key_pair_algorithm;
	const char *key_curve_id;
	int32_t key_bit_length;
};
static const struct json_obj_descr generate_csr_descr[] = {
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_generate_csr, "CertificateCollection",
				    certificate_collection, link_descr),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_generate_csr, "CommonName", cn, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_generate_csr, "Organization", o, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_generate_csr, "OrganizationalUnit", ou,
				  JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_generate_csr, "City", l, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_generate_csr, "State", st, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_generate_csr, "Country", c, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_generate_csr, "Email", email, JSON_TOK_STRING),
	JSON_OBJ_DESCR_ARRAY_NAMED(struct redfish_generate_csr, "AlternativeNames", alt_names,
				   CSR_ALT_NAMES_MAX, alt_names_len, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_generate_csr, "KeyPairAlgorithm",
				  key_pair_algorithm, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_generate_csr, "KeyCurveId",
				  key_curve_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_generate_csr, "KeyBitLength",
				  key_bit_length, JSON_TOK_NUMBER),
};

struct redfish_generate_csr_response {
	const char *csr_string;
	struct redfish_link certificate_collection;
};
static const struct json_obj_descr generate_csr_response_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_generate_csr_response, "CSRString",
				  csr_string, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_generate_csr_response,
				    "CertificateCollection", certificate_collection, link_descr),
};

static bool str_in(const char *s, const char *const *list, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (!strcasecmp(s, list[i]))
			return true;
	}
	return false;
}

/* POST /redfish/v1/CertificateService/Actions/CertificateService.GenerateCSR */
static int generate_csr_post_handler(struct http_resource_user_data *user_data)
{
	static const char *const algorithms[] = { "TPM_ALG_ECDSA", "EC", "ECDSA" };
	static const char *const curves[] = { "TPM_ECC_NIST_P256", "prime256v1", "secp256r1",
					      "P-256" };
	static char csr[1536];
	struct redfish_generate_csr payload;
	struct redfish_generate_csr_response response = {
		.csr_string = csr,
		.certificate_collection = { .odata_id = HTTPS_CERTS_URL },
	};
	struct https_cert_csr_params params;
	const char *err;
	int ret;

	memset(&payload, 0, sizeof(payload));
	payload.key_bit_length = -1;
	ret = json_obj_parse(user_data->data_buffer, user_data->data_len, generate_csr_descr,
			     ARRAY_SIZE(generate_csr_descr), &payload);
	if (ret < 0)
		return redfish_error(user_data, HTTP_400_BAD_REQUEST, "Base.1.8.MalformedJSON",
				     "The request body is not valid JSON");

	if (!payload.certificate_collection.odata_id ||
	    strcmp(payload.certificate_collection.odata_id, HTTPS_CERTS_URL))
		return redfish_error(user_data, HTTP_400_BAD_REQUEST,
				     "Base.1.8.ActionParameterValueError",
				     "CertificateCollection must be " HTTPS_CERTS_URL);

	/* Only the existing P-256 key can be used */
	if ((payload.key_pair_algorithm &&
	     !str_in(payload.key_pair_algorithm, algorithms, ARRAY_SIZE(algorithms))) ||
	    (payload.key_curve_id &&
	     !str_in(payload.key_curve_id, curves, ARRAY_SIZE(curves))) ||
	    (payload.key_bit_length != -1 && payload.key_bit_length != 256))
		return redfish_error(user_data, HTTP_400_BAD_REQUEST,
				     "Base.1.8.ActionParameterValueError",
				     "Only ECDSA P-256 keys are supported: KeyPairAlgorithm "
				     "TPM_ALG_ECDSA, KeyCurveId TPM_ECC_NIST_P256, "
				     "KeyBitLength 256");

	params = (struct https_cert_csr_params) {
		.cn = payload.cn,
		.o = payload.o,
		.ou = payload.ou,
		.l = payload.l,
		.st = payload.st,
		.c = payload.c,
		.email = payload.email,
		.alt_names = payload.alt_names,
		.alt_count = payload.alt_names_len,
	};

	ret = https_cert_csr_generate(&params, csr, sizeof(csr), &err);
	if (ret)
		return redfish_error(user_data, ret == -EINVAL || ret == -E2BIG ?
				     HTTP_400_BAD_REQUEST : HTTP_500_INTERNAL_SERVER_ERROR,
				     ret == -EINVAL || ret == -E2BIG ?
				     "Base.1.8.ActionParameterValueError" : "Base.1.8.GeneralError",
				     err);

	/* The request isn't needed any more, write the response over it */
	user_data->data_len = 0;
	ret = json_obj_encode(generate_csr_response_descr,
			      ARRAY_SIZE(generate_csr_response_descr),
			      &response, user_data_json_append, user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode GenerateCSR response: %d", ret);
		return HTTP_500_INTERNAL_SERVER_ERROR;
	}

	user_data->respond = true;
	return 0;
}

REDFISH_HANDLER_SIZED(generate_csr,
		      CERT_SERVICE_URL "/Actions/CertificateService.GenerateCSR",
		      true, /* require auth */
		      NULL, NULL, generate_csr_post_handler, NULL,
		      USER_DATA_BUFFER_GET_SIZE, 2048);

/* ReplaceCertificate */
struct redfish_replace_certificate {
	const char *certificate_string;
	const char *certificate_type;
	struct redfish_link certificate_uri;
};
static const struct json_obj_descr replace_certificate_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_replace_certificate, "CertificateString",
				  certificate_string, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct redfish_replace_certificate, "CertificateType",
				  certificate_type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct redfish_replace_certificate, "CertificateUri",
				    certificate_uri, link_descr),
};

/*
 * Undo JSON string escapes in place. Only what can appear in PEM is
 * expected, other escapes are left as they are, which makes the PEM fail
 * to parse.
 */
static void json_unescape_pem(char *s)
{
	char *w = s;

	for (; *s; s++) {
		if (*s == '\\' && s[1]) {
			s++;
			switch (*s) {
			case 'n': *w++ = '\n'; continue;
			case 'r': *w++ = '\r'; continue;
			case 't': *w++ = '\t'; continue;
			case '/': *w++ = '/'; continue;
			case '\\': *w++ = '\\'; continue;
			case '"': *w++ = '"'; continue;
			default: *w++ = '\\'; break;
			}
		}
		*w++ = *s;
	}
	*w = '\0';
}

/* POST /redfish/v1/CertificateService/Actions/CertificateService.ReplaceCertificate */
static int replace_certificate_post_handler(struct http_resource_user_data *user_data)
{
	struct redfish_replace_certificate payload;
	const char *err;
	int ret;

	memset(&payload, 0, sizeof(payload));
	ret = json_obj_parse(user_data->data_buffer, user_data->data_len,
			     replace_certificate_descr, ARRAY_SIZE(replace_certificate_descr),
			     &payload);
	if (ret < 0)
		return redfish_error(user_data, HTTP_400_BAD_REQUEST, "Base.1.8.MalformedJSON",
				     "The request body is not valid JSON");

	if (!payload.certificate_uri.odata_id ||
	    strcmp(payload.certificate_uri.odata_id, HTTPS_CERT_URL))
		return redfish_error(user_data, HTTP_400_BAD_REQUEST,
				     "Base.1.8.ActionParameterValueError",
				     "CertificateUri must be " HTTPS_CERT_URL);

	if (payload.certificate_type && strcmp(payload.certificate_type, "PEM"))
		return redfish_error(user_data, HTTP_400_BAD_REQUEST,
				     "Base.1.8.ActionParameterValueError",
				     "CertificateType must be PEM");

	if (!payload.certificate_string)
		return redfish_error(user_data, HTTP_400_BAD_REQUEST,
				     "Base.1.8.ActionParameterMissing",
				     "CertificateString is required");

	json_unescape_pem((char *)payload.certificate_string);

	ret = https_cert_install((char *)payload.certificate_string, &err);
	if (ret)
		return redfish_error(user_data, ret == -EINVAL || ret == -E2BIG ?
				     HTTP_400_BAD_REQUEST : HTTP_500_INTERNAL_SERVER_ERROR,
				     ret == -EINVAL || ret == -E2BIG ?
				     "Base.1.8.ActionParameterValueError" : "Base.1.8.GeneralError",
				     err);

	return 0;
}

REDFISH_HANDLER_SIZED(replace_certificate,
		      CERT_SERVICE_URL "/Actions/CertificateService.ReplaceCertificate",
		      true, /* require auth */
		      NULL, NULL, replace_certificate_post_handler, NULL,
		      USER_DATA_BUFFER_GET_SIZE, USER_DATA_BUFFER_CERT_POST_SIZE);
#endif /* CONFIG_APP_HTTPS_PUBLIC_KEY */

/*** /redfish/v1/$metadata ***/
static const uint8_t redfish_metadata_xml_gz[] = {
#include "redfish_metadata.xml.gz.inc"
};

static struct http_resource_detail_static redfish_metadata_xml_gz_resource_detail = {
	.common = {
			.type = HTTP_RESOURCE_TYPE_STATIC,
			.bitmask_of_supported_http_methods = BIT(HTTP_GET),
			.content_encoding = "gzip",
			.content_type = "application/xml",
		},
	.static_data = redfish_metadata_xml_gz,
	.static_data_len = sizeof(redfish_metadata_xml_gz),
};

ALL_HTTP_RESOURCE_DEFINE(redfish_metadata_xml_gz_resource, "/redfish/v1/$metadata",
			 &redfish_metadata_xml_gz_resource_detail);
