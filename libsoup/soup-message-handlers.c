/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * soup-message-handlers.c: HTTP response handlers
 *
 * Copyright (C) 2000-2003, Ximian, Inc.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "soup-message.h"
#include "soup-message-private.h"
#include "soup-misc.h"
#include "soup-private.h"

typedef enum {
	SOUP_HANDLER_HEADER = 1,
	SOUP_HANDLER_ERROR_CODE,
	SOUP_HANDLER_ERROR_CLASS
} SoupHandlerKind;

typedef struct {
	SoupHandlerPhase  phase;
	SoupCallbackFn    handler_cb;
	gpointer          user_data;

	SoupHandlerKind   kind;
	union {
		guint           errorcode;
		SoupErrorClass  errorclass;
		const char     *header;
	} data;
} SoupHandlerData;

static inline void
run_handler (SoupMessage     *msg,
	     SoupHandlerPhase invoke_phase,
	     SoupHandlerData *data)
{
	if (data->phase != invoke_phase)
		return;

	switch (data->kind) {
	case SOUP_HANDLER_HEADER:
		if (!soup_message_get_header (msg->response_headers,
					      data->data.header))
			return;
		break;
	case SOUP_HANDLER_ERROR_CODE:
		if (msg->errorcode != data->data.errorcode)
			return;
		break;
	case SOUP_HANDLER_ERROR_CLASS:
		if (msg->errorclass != data->data.errorclass)
			return;
		break;
	default:
		break;
	}

	(*data->handler_cb) (msg, data->user_data);
}

/*
 * Run each handler with matching criteria. If a handler requeues a
 * message, we stop processing and terminate the current request.
 *
 * After running all handlers, if there is an error set or the invoke
 * phase was post_body, issue the final callback.
 *
 * FIXME: If the errorcode is changed by a handler, we should restart
 * the processing.
 */
void
soup_message_run_handlers (SoupMessage *msg, SoupHandlerPhase invoke_phase)
{
	GSList *list;

	g_return_if_fail (SOUP_IS_MESSAGE (msg));

	for (list = msg->priv->content_handlers; list; list = list->next) {
		run_handler (msg, invoke_phase, list->data);

		if (SOUP_MESSAGE_IS_STARTING (msg))
			return;
	}
}

static void
add_handler (SoupMessage      *msg,
	     SoupHandlerPhase  phase,
	     SoupCallbackFn    handler_cb,
	     gpointer          user_data,
	     SoupHandlerKind   kind,
	     const char       *header,
	     guint             errorcode,
	     guint             errorclass)
{
	SoupHandlerData *data;

	data = g_new0 (SoupHandlerData, 1);
	data->phase = phase;
	data->handler_cb = handler_cb;
	data->user_data = user_data;
	data->kind = kind;

	switch (kind) {
	case SOUP_HANDLER_HEADER:
		data->data.header = header;
		break;
	case SOUP_HANDLER_ERROR_CODE:
		data->data.errorcode = errorcode;
		break;
	case SOUP_HANDLER_ERROR_CLASS:
		data->data.errorclass = errorclass;
		break;
	default:
		break;
	}

	msg->priv->content_handlers =
		g_slist_append (msg->priv->content_handlers, data);
}

void
soup_message_add_header_handler (SoupMessage      *msg,
				 const char       *header,
				 SoupHandlerPhase  phase,
				 SoupCallbackFn    handler_cb,
				 gpointer          user_data)
{
	g_return_if_fail (SOUP_IS_MESSAGE (msg));
	g_return_if_fail (header != NULL);
	g_return_if_fail (handler_cb != NULL);

	add_handler (msg, phase, handler_cb, user_data,
		     SOUP_HANDLER_HEADER,
		     header, 0, 0);
}

void
soup_message_add_error_code_handler (SoupMessage      *msg,
				     guint             errorcode,
				     SoupHandlerPhase  phase,
				     SoupCallbackFn    handler_cb,
				     gpointer          user_data)
{
	g_return_if_fail (SOUP_IS_MESSAGE (msg));
	g_return_if_fail (errorcode != 0);
	g_return_if_fail (handler_cb != NULL);

	add_handler (msg, phase, handler_cb, user_data,
		     SOUP_HANDLER_ERROR_CODE,
		     NULL, errorcode, 0);
}

void
soup_message_add_error_class_handler (SoupMessage      *msg,
				      SoupErrorClass    errorclass,
				      SoupHandlerPhase  phase,
				      SoupCallbackFn    handler_cb,
				      gpointer          user_data)
{
	g_return_if_fail (SOUP_IS_MESSAGE (msg));
	g_return_if_fail (errorclass != 0);
	g_return_if_fail (handler_cb != NULL);

	add_handler (msg, phase, handler_cb, user_data,
		     SOUP_HANDLER_ERROR_CLASS,
		     NULL, 0, errorclass);
}

void
soup_message_add_handler (SoupMessage      *msg,
			  SoupHandlerPhase  phase,
			  SoupCallbackFn    handler_cb,
			  gpointer          user_data)
{
	g_return_if_fail (SOUP_IS_MESSAGE (msg));
	g_return_if_fail (handler_cb != NULL);

	add_handler (msg, phase, handler_cb, user_data, 0, NULL, 0, 0);
}

void
soup_message_remove_handler (SoupMessage     *msg,
			     SoupHandlerPhase phase,
			     SoupCallbackFn   handler_cb,
			     gpointer         user_data)
{
	GSList *iter = msg->priv->content_handlers;

	while (iter) {
		SoupHandlerData *data = iter->data;

		if (data->handler_cb == handler_cb &&
		    data->user_data == user_data &&
		    data->phase == phase) {
			msg->priv->content_handlers =
				g_slist_remove (msg->priv->content_handlers,
						data);
			g_free (data);
			break;
		}

		iter = iter->next;
	}
}