/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * soup-queue.c: Asyncronous Callback-based HTTP Request Queue.
 *
 * Authors:
 *      Alex Graveley (alex@ximian.com)
 *
 * Copyright (C) 2000-2002, Ximian, Inc.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ctype.h>
#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>

#include "soup-queue.h"
#include "soup-auth.h"
#include "soup-message.h"
#include "soup-message-private.h"
#include "soup-context.h"
#include "soup-headers.h"
#include "soup-misc.h"
#include "soup-private.h"
#include "soup-ssl.h"

static GSList *soup_active_requests = NULL, *soup_active_request_next = NULL;

static guint soup_queue_idle_tag = 0;

static void
soup_debug_print_a_header (gchar *key, gchar *val, gpointer not_used)
{
	g_print ("\tKEY: \"%s\", VALUE: \"%s\"\n", key, val);
}

void 
soup_debug_print_headers (SoupMessage *req)
{
	g_print ("Request Headers:\n");
	soup_message_foreach_header (req->request_headers,
				     (GHFunc) soup_debug_print_a_header,
				     NULL);

	g_print ("Response Headers:\n");
	soup_message_foreach_header (req->response_headers,
				     (GHFunc) soup_debug_print_a_header,
				     NULL);
}

static void 
soup_queue_error_cb (SoupMessage *req, gpointer user_data)
{
	SoupConnection *conn = soup_message_get_connection (req);
	const SoupUri *uri;
	gboolean conn_is_new;

	conn_is_new = soup_connection_is_new (conn);
	soup_message_disconnect (req);

	switch (req->priv->status) {
	case SOUP_MESSAGE_STATUS_IDLE:
	case SOUP_MESSAGE_STATUS_QUEUED:
	case SOUP_MESSAGE_STATUS_FINISHED:
		break;

	case SOUP_MESSAGE_STATUS_CONNECTING:
		soup_message_set_error (req, SOUP_ERROR_CANT_CONNECT);
		soup_message_issue_callback (req);
		break;

	case SOUP_MESSAGE_STATUS_WRITING_HEADERS:
	case SOUP_MESSAGE_STATUS_READING_HEADERS:
		uri = soup_message_get_uri (req);

		if (uri->protocol == SOUP_PROTOCOL_HTTPS) {
			/* FIXME: what does this really do? */

			/*
			 * This can happen if the SSL handshake fails
			 * for some reason (untrustable signatures,
			 * etc.)
			 */
			if (req->priv->retries >= 3) {
				soup_message_set_error (req, SOUP_ERROR_SSL_FAILED);
				soup_message_issue_callback (req);
			} else {
				req->priv->retries++;
				soup_message_requeue (req);
			}
		} else if (conn_is_new) {
			soup_message_set_error (req, SOUP_ERROR_CANT_CONNECT);
			soup_message_issue_callback (req);
		} else {
			/* Must have timed out. Try a new connection */
			soup_message_requeue (req);
		}
		break;

	default:
		soup_message_set_error (req, SOUP_ERROR_IO);
		soup_message_issue_callback (req);
		break;
	}
}

static SoupKnownErrorCode
soup_queue_parse_headers_cb (SoupMessage *req,
			     char *headers, guint headers_len,
			     SoupTransferEncoding *encoding,
			     guint *content_len,
			     gpointer user_data)
{
	const char *length, *enc;
	SoupHttpVersion version;
	GHashTable *resp_hdrs;
	SoupMethodId meth_id;

	if (!soup_headers_parse_response (headers, headers_len,
					  req->response_headers,
					  &version,
					  &req->errorcode,
					  (char **) &req->errorphrase))
		return SOUP_ERROR_MALFORMED;

	meth_id   = soup_method_get_id (req->method);
	resp_hdrs = req->response_headers;

	req->errorclass = soup_error_get_class (req->errorcode);

	/* 
	 * Special case zero body handling for:
	 *   - HEAD requests (where content-length must be ignored) 
	 *   - CONNECT requests (no body expected) 
	 *   - No Content (204) responses (no message-body allowed)
	 *   - Reset Content (205) responses (no entity allowed)
	 *   - Not Modified (304) responses (no message-body allowed)
	 *   - 1xx Informational responses (where no body is allowed)
	 */
	if (meth_id == SOUP_METHOD_ID_HEAD ||
	    meth_id == SOUP_METHOD_ID_CONNECT ||
	    req->errorcode  == SOUP_ERROR_NO_CONTENT || 
	    req->errorcode  == SOUP_ERROR_RESET_CONTENT || 
	    req->errorcode  == SOUP_ERROR_NOT_MODIFIED || 
	    req->errorclass == SOUP_ERROR_CLASS_INFORMATIONAL) {
		*encoding = SOUP_TRANSFER_CONTENT_LENGTH;
		*content_len = 0;
		return SOUP_ERROR_OK;
	}

	/* 
	 * Handle Chunked encoding.  Prefer Chunked over a Content-Length to
	 * support broken Traffic-Server proxies that supply both.  
	 */
	enc = soup_message_get_header (resp_hdrs, "Transfer-Encoding");
	if (enc) {
		if (g_strcasecmp (enc, "chunked") == 0) {
			*encoding = SOUP_TRANSFER_CHUNKED;
			return SOUP_ERROR_OK;
		} else
			return SOUP_ERROR_MALFORMED;
	}

	/* 
	 * Handle Content-Length encoding 
	 */
	length = soup_message_get_header (resp_hdrs, "Content-Length");
	if (length) {
		int len;

		*encoding = SOUP_TRANSFER_CONTENT_LENGTH;
		len = atoi (length);
		if (len < 0)
			return SOUP_ERROR_MALFORMED;
		else
			*content_len = len;
	}

	return SOUP_ERROR_OK;
}

static void
soup_queue_read_headers_cb (SoupMessage *req, gpointer user_data)
{
	soup_message_run_handlers (req, SOUP_HANDLER_PRE_BODY);
}

static void
soup_queue_read_chunk_cb (SoupMessage *req, SoupDataBuffer *chunk,
			  gpointer user_data)
{
	/* FIXME? */
	memcpy (&req->response, chunk, sizeof (req->response));

	soup_message_run_handlers (req, SOUP_HANDLER_BODY_CHUNK);
}

static void
soup_queue_read_done_cb (SoupMessage *req, gpointer user_data)
{
	SoupConnection *conn = soup_message_get_connection (req);

	if (soup_message_is_keepalive (req) && conn)
		soup_connection_mark_old (conn);
	else
		soup_message_disconnect (req);

	if (req->errorclass == SOUP_ERROR_CLASS_INFORMATIONAL) {
		soup_message_read (req, &req->response,
				   soup_queue_parse_headers_cb,
				   soup_queue_read_headers_cb,
				   soup_queue_read_chunk_cb,
				   soup_queue_read_done_cb,
				   soup_queue_error_cb,
				   NULL);
	} else
		req->priv->status = SOUP_MESSAGE_STATUS_FINISHED;

	soup_message_run_handlers (req, SOUP_HANDLER_POST_BODY);
}

static void
soup_encode_http_auth (SoupMessage *msg, GString *header, gboolean proxy_auth)
{
	SoupAuth *auth;
	SoupContext *ctx;
	char *token;

	ctx = proxy_auth ? soup_get_proxy () : msg->priv->context;

	auth = soup_context_lookup_auth (ctx, msg);
	if (!auth)
		return;
	if (!soup_auth_is_authenticated (auth) &&
	    !soup_context_authenticate_auth (ctx, auth))
		return;

	token = soup_auth_get_authorization (auth, msg);
	if (token) {
		g_string_sprintfa (header, "%s: %s\r\n",
				   proxy_auth ? 
					"Proxy-Authorization" : 
					"Authorization",
				   token);
		g_free (token);
	}
}

struct SoupUsedHeaders {
	gboolean host;
	gboolean user_agent;
	gboolean content_type;
	gboolean connection;
	gboolean proxy_auth;
	gboolean auth;

	GString *out;
};

static void 
soup_check_used_headers (gchar  *key, 
			 GSList *vals, 
			 struct SoupUsedHeaders *hdrs)
{
	switch (toupper (key [0])) {
	case 'H':
		if (!g_strcasecmp (key+1, "ost")) 
			hdrs->host = TRUE;
		break;
	case 'U':
		if (!g_strcasecmp (key+1, "ser-Agent")) 
			hdrs->user_agent = TRUE;
		break;
	case 'A':
		if (!g_strcasecmp (key+1, "uthorization")) 
			hdrs->auth = TRUE;
		break;
	case 'P':
		if (!g_strcasecmp (key+1, "roxy-Authorization")) 
			hdrs->proxy_auth = TRUE;
		break;
	case 'C':
		if (!g_strcasecmp (key+1, "onnection")) 
			hdrs->connection = TRUE;
		else if (!g_strcasecmp (key+1, "ontent-Type"))
			hdrs->content_type = TRUE;
		else if (!g_strcasecmp (key+1, "ontent-Length")) {
			g_warning ("Content-Length set as custom request "
				   "header is not allowed.");
			return;
		}
		break;
	}

	while (vals) {
		g_string_sprintfa (hdrs->out, 
				   "%s: %s\r\n", 
				   key, 
				   (gchar *) vals->data);
		vals = vals->next;
	}
}

static void
soup_queue_get_request_header_cb (SoupMessage *req, GString *header,
				  gpointer user_data)
{
	char *uri;
	SoupContext *proxy;
	const SoupUri *suri;
	struct SoupUsedHeaders hdrs = {
		FALSE, 
		FALSE, 
		FALSE, 
		FALSE, 
		FALSE, 
		FALSE, 
		NULL
	};

	hdrs.out = header;
	proxy = soup_get_proxy ();
	suri = soup_message_get_uri (req);

	if (!g_strcasecmp (req->method, "CONNECT")) {
		/* CONNECT URI is hostname:port for tunnel destination */
		uri = g_strdup_printf ("%s:%d", suri->host, suri->port);
	} else {
		/* Proxy expects full URI to destination. Otherwise
		 * just the path.
		 */
		uri = soup_uri_to_string (suri, !proxy);
	}

	g_string_sprintfa (header,
			   req->priv->http_version == SOUP_HTTP_1_1 ? 
			           "%s %s HTTP/1.1\r\n" : 
			           "%s %s HTTP/1.0\r\n",
			   req->method,
			   uri);
	g_free (uri);

	/*
	 * FIXME: Add a 411 "Length Required" response code handler here?
	 */
	if (req->request.length > 0) {
		g_string_sprintfa (header,
				   "Content-Length: %d\r\n",
				   req->request.length);
	}

	g_hash_table_foreach (req->request_headers, 
			      (GHFunc) soup_check_used_headers,
			      &hdrs);

	/* 
	 * If we specify an absoluteURI in the request line, the Host header
	 * MUST be ignored by the proxy.  
	 */
	g_string_sprintfa (header, 
			   "%s%s%s%s%s%s%s",
			   hdrs.host ? "" : "Host: ",
			   hdrs.host ? "" : suri->host,
			   hdrs.host ? "" : "\r\n",
			   hdrs.content_type ? "" : "Content-Type: text/xml; ",
			   hdrs.content_type ? "" : "charset=utf-8\r\n",
			   hdrs.connection ? "" : "Connection: keep-alive\r\n",
			   hdrs.user_agent ? 
			           "" : 
			           "User-Agent: Soup/" VERSION "\r\n");

	/* 
	 * Proxy-Authorization from the proxy Uri 
	 */
	if (!hdrs.proxy_auth && proxy && soup_context_get_uri (proxy)->user)
		soup_encode_http_auth (req, header, TRUE);

	/* 
	 * Authorization from the context Uri 
	 */
	if (!hdrs.auth)
		soup_encode_http_auth (req, header, FALSE);

	g_string_append (header, "\r\n");
}

static void 
soup_queue_write_done_cb (SoupMessage *req, gpointer user_data)
{
	soup_message_read (req, &req->response,
			   soup_queue_parse_headers_cb,
			   soup_queue_read_headers_cb,
			   soup_queue_read_chunk_cb,
			   soup_queue_read_done_cb,
			   soup_queue_error_cb,
			   NULL);
}

static void
start_request (SoupContext *ctx, SoupMessage *req)
{
	SoupSocket *sock;

	sock = soup_message_get_socket (req);
	if (!sock) {	/* FIXME */
		SoupProtocol proto;
		gchar *phrase;

		proto = soup_context_get_uri (ctx)->protocol;

		if (proto == SOUP_PROTOCOL_HTTPS)
			phrase = "Unable to create secure data channel";
		else
			phrase = "Unable to create data channel";

		if (ctx != req->priv->context)
			soup_message_set_error_full (
				req, 
				SOUP_ERROR_CANT_CONNECT_PROXY,
				phrase);
		else 
			soup_message_set_error_full (
				req, 
				SOUP_ERROR_CANT_CONNECT,
				phrase);

		soup_message_issue_callback (req);
		return;
	}

	soup_message_write_simple (req, &req->request,
				   soup_queue_get_request_header_cb,
				   soup_queue_write_done_cb,
				   soup_queue_error_cb,
				   NULL);
}

static void
proxy_https_connect_cb (SoupMessage *msg, gpointer user_data)
{
	gboolean *ret = user_data;

	if (!SOUP_MESSAGE_IS_ERROR (msg)) {
		soup_socket_start_ssl (soup_message_get_socket (msg));
		*ret = TRUE;
	}
}

static gboolean
proxy_https_connect (SoupContext    *proxy, 
		     SoupConnection *conn, 
		     SoupContext    *dest_ctx)
{
	SoupProtocol proxy_proto;
	SoupMessage *connect_msg;
	gboolean ret = FALSE;

	proxy_proto = soup_context_get_uri (proxy)->protocol;

	if (proxy_proto != SOUP_PROTOCOL_HTTP && 
	    proxy_proto != SOUP_PROTOCOL_HTTPS) 
		return FALSE;

	connect_msg = soup_message_new_from_uri (SOUP_METHOD_CONNECT,
						 soup_context_get_uri (dest_ctx));
	soup_message_set_connection (connect_msg, conn);
	soup_message_add_handler (connect_msg, 
				  SOUP_HANDLER_POST_BODY,
				  proxy_https_connect_cb,
				  &ret);
	soup_message_send (connect_msg);
	g_object_unref (connect_msg);

	return ret;
}

static gboolean
proxy_connect (SoupContext *ctx, SoupMessage *req, SoupConnection *conn)
{
	SoupProtocol proto, dest_proto;

	/* 
	 * Only attempt proxy connect if the connection's context is different
	 * from the requested context, and if the connection is new 
	 */
	if (ctx == req->priv->context || !soup_connection_is_new (conn))
		return FALSE;

	proto = soup_context_get_uri (ctx)->protocol;
	dest_proto = soup_context_get_uri (req->priv->context)->protocol;
	
	/* Handle HTTPS tunnel setup via proxy CONNECT request. */
	if (dest_proto == SOUP_PROTOCOL_HTTPS) {
		/* Syncronously send CONNECT request */
		if (!proxy_https_connect (ctx, conn, req->priv->context)) {
			soup_message_set_error_full (
				req, 
				SOUP_ERROR_CANT_CONNECT_PROXY,
				"Unable to create secure data "
				"tunnel through proxy");
			soup_message_issue_callback (req);
			return TRUE;
		}
	}

	return FALSE;
}

void
soup_queue_connect_cb (SoupContext          *ctx,
		       SoupKnownErrorCode    err,
		       SoupConnection       *conn,
		       gpointer              user_data)
{
	SoupMessage *req = user_data;

	req->priv->connect_tag = NULL;
	soup_message_set_connection (req, conn);

	switch (err) {
	case SOUP_ERROR_OK:
		/* 
		 * NOTE: proxy_connect will either set an error or call us 
		 * again after proxy negotiation.
		 */
		if (proxy_connect (ctx, req, conn))
			return;

		start_request (ctx, req);
		break;

	case SOUP_ERROR_CANT_RESOLVE:
		if (ctx == req->priv->context)
			soup_message_set_error (req, SOUP_ERROR_CANT_RESOLVE);
		else
			soup_message_set_error (req, SOUP_ERROR_CANT_RESOLVE_PROXY);
		soup_message_issue_callback (req);
		break;

	default:
		if (ctx == req->priv->context)
			soup_message_set_error (req, SOUP_ERROR_CANT_CONNECT);
		else
			soup_message_set_error (req, SOUP_ERROR_CANT_CONNECT_PROXY);
		soup_message_issue_callback (req);
		break;
	}

	return;
}

void
soup_queue_add_request (SoupMessage *req)
{
	soup_active_requests = g_slist_prepend (soup_active_requests, req);
}

void
soup_queue_remove_request (SoupMessage *req)
{
	if (soup_active_request_next && soup_active_request_next->data == req)
		soup_queue_next_request ();
	soup_active_requests = g_slist_remove (soup_active_requests, req);
}

SoupMessage *
soup_queue_first_request (void)
{
	if (!soup_active_requests)
		return NULL;

	soup_active_request_next = soup_active_requests->next;
	return soup_active_requests->data;
}

SoupMessage *
soup_queue_next_request (void)
{
	SoupMessage *ret;

	if (!soup_active_request_next)
		return NULL;
	ret = soup_active_request_next->data;
	soup_active_request_next = soup_active_request_next->next;
	return ret;
}

static gboolean
request_in_progress (SoupMessage *req)
{
	if (!soup_active_requests)
		return FALSE;

	return g_slist_index (soup_active_requests, req) != -1;
}

static gboolean 
soup_idle_handle_new_requests (gpointer unused)
{
	SoupMessage *req = soup_queue_first_request ();
	SoupConnection *conn;

	for (; req; req = soup_queue_next_request ()) {
		SoupContext *ctx, *proxy;

		if (req->priv->status != SOUP_MESSAGE_STATUS_QUEUED)
			continue;

		proxy = soup_get_proxy ();
		ctx = proxy ? proxy : req->priv->context;

		req->priv->status = SOUP_MESSAGE_STATUS_CONNECTING;

		conn = soup_message_get_connection (req);
		if (conn && soup_connection_is_connected (conn))
			start_request (ctx, req);
		else {
			gpointer connect_tag;

			connect_tag = 
				soup_context_get_connection (
					ctx, 
					soup_queue_connect_cb, 
					req);

			if (connect_tag && request_in_progress (req))
				req->priv->connect_tag = connect_tag;
		}
	}

	soup_queue_idle_tag = 0;
	return FALSE;
}

static void
soup_queue_initialize (void)
{
	if (!soup_initialized)
		soup_load_config (NULL);

	if (!soup_queue_idle_tag)
		soup_queue_idle_tag = 
			g_idle_add (soup_idle_handle_new_requests, NULL);
}

void 
soup_queue_message (SoupMessage *req)
{
	g_return_if_fail (SOUP_IS_MESSAGE (req));

	req->priv->status = SOUP_MESSAGE_STATUS_QUEUED;
	soup_queue_add_request (req);
	soup_queue_initialize ();
}

/**
 * soup_queue_shutdown:
 * 
 * Shut down the message queue by calling soup_message_cancel() on all
 * active requests and then closing all open connections.
 */
void 
soup_queue_shutdown (void)
{
	SoupMessage *req;

	soup_initialized = FALSE;

	if (soup_queue_idle_tag) {
		g_source_remove (soup_queue_idle_tag);
		soup_queue_idle_tag = 0;
	}

	req = soup_queue_first_request ();
	for (; req; req = soup_queue_next_request ())
		soup_message_cancel (req);

	soup_connection_purge_idle ();
}
