/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright 2007-2012 Red Hat, Inc.
 */

#include "test-utils.h"

#include "soup-connection.h"
#include "soup-socket.h"
#include "soup-server-message-private.h"

#include <gio/gnetworking.h>

static SoupServer *server;
static GUri *base_uri;
static GMutex server_mutex;

static void
forget_close (SoupServerMessage *msg,
	      gpointer           user_data)
{
	soup_message_headers_remove (soup_server_message_get_response_headers (msg),
				     "Connection");
}

static void
close_socket (SoupServerMessage *msg,
	      SoupSocket        *sock)
{
        GSocket *gsocket;
	int sockfd;

	/* Actually calling soup_socket_disconnect() here would cause
	 * us to leak memory, so just shutdown the socket instead.
	 */
        gsocket = soup_socket_get_gsocket (sock); 
	sockfd = g_socket_get_fd (gsocket);
#ifdef G_OS_WIN32
	shutdown (sockfd, SD_SEND);
#else
	shutdown (sockfd, SHUT_WR);
#endif

	/* Then add the missing data to the message now, so SoupServer
	 * can clean up after itself properly.
	 */
	soup_message_body_append (soup_server_message_get_response_body (msg),
				  SOUP_MEMORY_STATIC,
				  "foo", 3);
}

static gboolean
timeout_socket (GObject    *pollable,
		SoupSocket *sock)
{
	soup_socket_disconnect (sock);
	return FALSE;
}

static void
timeout_request_started (SoupServer        *server,
			 SoupServerMessage *msg,
			 gpointer           user_data)
{
	SoupSocket *sock;
	GMainContext *context = g_main_context_get_thread_default ();
	GIOStream *iostream;
	GInputStream *istream;
	GSource *source;

	g_signal_handlers_disconnect_by_func (server, timeout_request_started, NULL);

	sock = soup_server_message_get_soup_socket (msg);
	iostream = soup_socket_get_iostream (sock);
	istream = g_io_stream_get_input_stream (iostream);
	source = g_pollable_input_stream_create_source (G_POLLABLE_INPUT_STREAM (istream), NULL);
	g_source_set_callback (source, (GSourceFunc)timeout_socket, sock, NULL);
	g_source_attach (source, g_main_context_get_thread_default ());
	g_source_unref (source);

	g_mutex_unlock (&server_mutex);
	while (soup_socket_is_connected (sock))
		g_main_context_iteration (context, TRUE);
}

static void
setup_timeout_persistent (SoupServer *server, SoupSocket *sock)
{
	/* In order for the test to work correctly, we have to
	 * close the connection *after* the client side writes
	 * the request. To ensure that this happens reliably,
	 * regardless of thread scheduling, we:
	 *
	 *   1. Wait for the server to finish this request and
	 *      start reading the next one (and lock server_mutex
	 *      to interlock with the client and ensure that it
	 *      doesn't start writing its next request until
	 *      that point).
	 *   2. Block until input stream is readable, meaning the
	 *      client has written its request.
	 *   3. Close the socket.
	 */
	g_mutex_lock (&server_mutex);
	g_signal_connect (server, "request-started",
			  G_CALLBACK (timeout_request_started), NULL);
}

static void
server_callback (SoupServer        *server,
		 SoupServerMessage *msg,
		 const char        *path,
		 GHashTable        *query,
		 gpointer           data)
{
	const char *method;

	/* The way this gets used in the tests, we don't actually
	 * need to hold it through the whole function, so it's simpler
	 * to just release it right away.
	 */
	g_mutex_lock (&server_mutex);
	g_mutex_unlock (&server_mutex);

	method = soup_server_message_get_method (msg);
	if (method != SOUP_METHOD_GET && method != SOUP_METHOD_POST) {
		soup_server_message_set_status (msg, SOUP_STATUS_NOT_IMPLEMENTED, NULL);
		return;
	}

	if (g_str_has_prefix (path, "/content-length/")) {
		gboolean too_long = strcmp (path, "/content-length/long") == 0;
		gboolean no_close = strcmp (path, "/content-length/noclose") == 0;
		SoupMessageHeaders *response_headers;

		soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
		soup_server_message_set_response (msg, "text/plain",
						  SOUP_MEMORY_STATIC, "foobar", 6);

		response_headers = soup_server_message_get_response_headers (msg);
		if (too_long)
			soup_message_headers_set_content_length (response_headers, 9);
		soup_message_headers_append (response_headers,
					     "Connection", "close");

		if (too_long) {
			SoupSocket *sock;

			/* soup-message-io will wait for us to add
			 * another chunk after the first, to fill out
			 * the declared Content-Length. Instead, we
			 * forcibly close the socket at that point.
			 */
			sock = soup_server_message_get_soup_socket (msg);
			g_signal_connect (msg, "wrote-chunk",
					  G_CALLBACK (close_socket), sock);
		} else if (no_close) {
			/* Remove the 'Connection: close' after writing
			 * the headers, so that when we check it after
			 * writing the body, we'll think we aren't
			 * supposed to close it.
			 */
			g_signal_connect (msg, "wrote-headers",
					  G_CALLBACK (forget_close), NULL);
		}
		return;
	}

	if (!strcmp (path, "/timeout-persistent")) {
		SoupSocket *sock;

		sock = soup_server_message_get_soup_socket (msg);
		setup_timeout_persistent (server, sock);
	}

	soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
	soup_server_message_set_response (msg, "text/plain",
					  SOUP_MEMORY_STATIC, "index", 5);
	return;
}

static void
do_content_length_framing_test (void)
{
	SoupSession *session;
	SoupMessage *msg;
	GUri *request_uri;
	goffset declared_length;
	GBytes *body;

	g_test_bug ("611481");

	session = soup_test_session_new (NULL);

	debug_printf (1, "  Content-Length larger than message body length\n");
	request_uri = g_uri_parse_relative (base_uri, "/content-length/long", SOUP_HTTP_URI_FLAGS, NULL);
	msg = soup_message_new_from_uri ("GET", request_uri);
	body = soup_test_session_send (session, msg, NULL, NULL);

	soup_test_assert_message_status (msg, SOUP_STATUS_OK);

	declared_length = soup_message_headers_get_content_length (soup_message_get_response_headers (msg));
	debug_printf (2, "    Content-Length: %lu, body: %s\n",
		      (gulong)declared_length, (char *)g_bytes_get_data (body, NULL));
	g_assert_cmpint (g_bytes_get_size (body), <, declared_length);

	g_uri_unref (request_uri);
	g_bytes_unref (body);
	g_object_unref (msg);

	debug_printf (1, "  Server claims 'Connection: close' but doesn't\n");
	request_uri = g_uri_parse_relative (base_uri, "/content-length/noclose", SOUP_HTTP_URI_FLAGS, NULL);
	msg = soup_message_new_from_uri ("GET", request_uri);
	body = soup_test_session_send (session, msg, NULL, NULL);

	soup_test_assert_message_status (msg, SOUP_STATUS_OK);

	declared_length = soup_message_headers_get_content_length (soup_message_get_response_headers (msg));
	g_assert_cmpint (g_bytes_get_size (body), ==, declared_length);

	g_uri_unref (request_uri);
	g_bytes_unref (body);
	g_object_unref (msg);

	soup_test_session_abort_unref (session);
}

static void
message_started_socket_collector (SoupMessage *msg,
				  GSocket    **sockets)
{
        SoupConnection *conn = soup_message_get_connection (msg);
        GSocket *socket = soup_connection_get_socket (conn);
	int i;

	debug_printf (2, "      msg %p => socket %p\n", msg, socket);
	for (i = 0; i < 4; i++) {
		if (!sockets[i]) {
			/* We ref the socket to make sure that even if
			 * it gets disconnected, it doesn't get freed,
			 * since our checks would get messed up if the
			 * slice allocator reused the same address for
			 * two consecutive sockets.
			 */
			sockets[i] = g_object_ref (socket);
			break;
		}
	}

	soup_test_assert (i < 4, "socket queue overflowed");
}

static void
request_queued_socket_collector (SoupSession *session,
				 SoupMessage *msg,
				 gpointer     data)
{
	g_signal_connect (msg, "starting",
			  G_CALLBACK (message_started_socket_collector),
			  data);
}

static void
do_timeout_test_for_session (SoupSession *session)
{
	SoupMessage *msg;
	GSocket *sockets[4] = { NULL, NULL, NULL, NULL };
	GUri *timeout_uri;
	int i;
	GBytes *body;

	g_signal_connect (session, "request-queued",
			  G_CALLBACK (request_queued_socket_collector),
			  &sockets);

	debug_printf (1, "    First message\n");
	timeout_uri = g_uri_parse_relative (base_uri, "/timeout-persistent", SOUP_HTTP_URI_FLAGS, NULL);
	msg = soup_message_new_from_uri ("GET", timeout_uri);
	g_uri_unref (timeout_uri);
	body = soup_test_session_send (session, msg, NULL, NULL);
	soup_test_assert_message_status (msg, SOUP_STATUS_OK);

	if (sockets[1]) {
		soup_test_assert (sockets[1] == NULL, "Message was retried");
		sockets[1] = sockets[2] = sockets[3] = NULL;
	}
	g_bytes_unref (body);
	g_object_unref (msg);

	/* The server will grab server_mutex before returning the response,
	 * and release it when it's ready for us to send the second request.
	 */
	g_mutex_lock (&server_mutex);
	g_mutex_unlock (&server_mutex);

	debug_printf (1, "    Second message\n");
	msg = soup_message_new_from_uri ("GET", base_uri);
	body = soup_test_session_send (session, msg, NULL, NULL);
	soup_test_assert_message_status (msg, SOUP_STATUS_OK);

	soup_test_assert (sockets[1] == sockets[0],
			  "Message was not retried on existing connection");
	soup_test_assert (sockets[2] != NULL,
			  "Message was not retried after disconnect");
	soup_test_assert (sockets[2] != sockets[1],
			  "Message was retried on closed connection");
	soup_test_assert (sockets[3] == NULL,
			  "Message was retried again");
	g_bytes_unref (body);
	g_object_unref (msg);

	for (i = 0; sockets[i]; i++)
		g_object_unref (sockets[i]);
}

static void
do_persistent_connection_timeout_test (void)
{
	SoupSession *session;

	g_test_bug ("631525");

	debug_printf (1, "  Normal session, message API\n");
	session = soup_test_session_new (NULL);
	do_timeout_test_for_session (session);
	soup_test_session_abort_unref (session);
}

static void
do_persistent_connection_timeout_test_with_cancellation (void)
{
	SoupSession *session;
	SoupMessage *msg;
	GSocket *sockets[4] = { NULL, NULL, NULL, NULL };
	GUri *timeout_uri;
	GCancellable *cancellable;
	GInputStream *response;
	int i;
	char buf[8192];

	session = soup_test_session_new (NULL);

	g_signal_connect (session, "request-queued",
			  G_CALLBACK (request_queued_socket_collector),
			  &sockets);

	debug_printf (1, "    First message\n");
	timeout_uri = g_uri_parse_relative (base_uri, "/timeout-persistent", SOUP_HTTP_URI_FLAGS, NULL);
	msg = soup_message_new_from_uri ("GET", timeout_uri);
	cancellable = g_cancellable_new ();
	g_uri_unref (timeout_uri);
	response = soup_session_send (session, msg, cancellable, NULL);
	soup_test_assert_message_status (msg, SOUP_STATUS_OK);

	if (sockets[1]) {
		soup_test_assert (sockets[1] == NULL, "Message was retried");
		sockets[1] = sockets[2] = sockets[3] = NULL;
	}
	g_object_unref (msg);

	soup_test_assert (response, "No response received");

	while (g_input_stream_read (response, buf, sizeof (buf), NULL, NULL))
		debug_printf (1, "Reading response\n");

	soup_test_assert (!g_cancellable_is_cancelled (cancellable),
			  "User-supplied cancellable was cancelled");

	g_object_unref (response);

	/* The server will grab server_mutex before returning the response,
	 * and release it when it's ready for us to send the second request.
	 */
	g_mutex_lock (&server_mutex);
	g_mutex_unlock (&server_mutex);

	debug_printf (1, "    Second message\n");
	msg = soup_message_new_from_uri ("GET", base_uri);

	/* Cancel the cancellable in the signal handler, and then check that it
	 * was not reset below */
	g_signal_connect_swapped (msg, "starting",
				  G_CALLBACK (g_cancellable_cancel),
				  cancellable);

	response = soup_session_send (session, msg, cancellable, NULL);

	soup_test_assert (response == NULL, "Unexpected response");

	soup_test_assert_message_status (msg, SOUP_STATUS_NONE);

	soup_test_assert (sockets[1] == sockets[0],
			  "Message was not retried on existing connection");
	soup_test_assert (sockets[2] != sockets[1],
			  "Message was retried on closed connection");
	soup_test_assert (sockets[3] == NULL,
			  "Message was retried again");
	g_object_unref (msg);

	/* cancellable should not have been reset, it should still be in the
	 * cancelled state */
	soup_test_assert (g_cancellable_is_cancelled (cancellable),
			  "User-supplied cancellable was reset");

	for (i = 0; sockets[i]; i++)
		g_object_unref (sockets[i]);

	g_object_unref (cancellable);

	soup_test_session_abort_unref (session);
}

static GMainLoop *max_conns_loop;
static int msgs_done;
static guint quit_loop_timeout;
#define MAX_CONNS 2
#define TEST_CONNS (MAX_CONNS * 2) + 1

static gboolean
idle_start_server (gpointer data)
{
	g_mutex_unlock (&server_mutex);
	return FALSE;
}

static gboolean
quit_loop (gpointer data)
{
	quit_loop_timeout = 0;
	g_main_loop_quit (max_conns_loop);
	return FALSE;
}

static void
max_conns_message_started (SoupMessage *msg)
{
	g_signal_handlers_disconnect_by_func (msg, max_conns_message_started, NULL);

	if (++msgs_done >= MAX_CONNS) {
                if (quit_loop_timeout)
                        g_source_remove (quit_loop_timeout);
	        quit_loop_timeout = g_timeout_add (100, quit_loop, NULL);
        }
}

static void
max_conns_request_queued (SoupSession *session,
			  SoupMessage *msg,
			  gpointer     data)
{
	g_signal_connect (msg, "starting",
			  G_CALLBACK (max_conns_message_started),
			  data);
}

static void
max_conns_message_complete (SoupMessage *msg, gpointer user_data)
{
	if (++msgs_done == TEST_CONNS)
		g_main_loop_quit (max_conns_loop);
}

static void
do_max_conns_test_for_session (SoupSession *session)
{
	SoupMessage *msgs[TEST_CONNS];
	int i;
	GCancellable *cancellable;

	max_conns_loop = g_main_loop_new (NULL, TRUE);

	g_mutex_lock (&server_mutex);

	cancellable = g_cancellable_new ();
	g_signal_connect (session, "request-queued",
			  G_CALLBACK (max_conns_request_queued), NULL);
	msgs_done = 0;
	for (i = 0; i < TEST_CONNS; i++) {
		msgs[i] = soup_message_new_from_uri ("GET", base_uri);
		g_signal_connect (msgs[i], "finished",
				  G_CALLBACK (max_conns_message_complete), NULL);
		soup_session_send_async (session, msgs[i], G_PRIORITY_DEFAULT, cancellable, NULL, NULL);
	}

	g_main_loop_run (max_conns_loop);
	g_assert_cmpint (msgs_done, ==, MAX_CONNS);

	if (quit_loop_timeout)
		g_source_remove (quit_loop_timeout);
	quit_loop_timeout = g_timeout_add (1000, quit_loop, NULL);

	for (i = 0; i < TEST_CONNS; i++)
		g_signal_handlers_disconnect_by_func (msgs[i], max_conns_message_started, NULL);

	msgs_done = 0;
	g_idle_add (idle_start_server, NULL);
	if (quit_loop_timeout)
		g_source_remove (quit_loop_timeout);
	quit_loop_timeout = g_timeout_add (1000, quit_loop, NULL);
	g_main_loop_run (max_conns_loop);

	for (i = 0; i < TEST_CONNS; i++)
		soup_test_assert_message_status (msgs[i], SOUP_STATUS_OK);

	if (msgs_done != TEST_CONNS) {
		/* Clean up so we don't get a spurious "Leaked
		 * session" error.
		 */
		g_cancellable_cancel (cancellable);
		g_main_loop_run (max_conns_loop);
	}

	g_object_unref (cancellable);
	g_main_loop_unref (max_conns_loop);
	if (quit_loop_timeout) {
		g_source_remove (quit_loop_timeout);
		quit_loop_timeout = 0;
	}

	for (i = 0; i < TEST_CONNS; i++)
		g_object_unref (msgs[i]);
}

static void
do_max_conns_test (void)
{
	SoupSession *session;

	g_test_bug ("634422");

	session = soup_test_session_new ("max-conns", MAX_CONNS,
					 NULL);
	do_max_conns_test_for_session (session);
	soup_test_session_abort_unref (session);
}

static void
np_message_started (SoupMessage *msg,
		    GSocket    **save_socket)
{
        SoupConnection *conn = soup_message_get_connection (msg);
        GSocket *socket = soup_connection_get_socket (conn);

	*save_socket = g_object_ref (socket);
}

static void
np_request_queued (SoupSession *session,
		   SoupMessage *msg,
		   gpointer     data)
{
	g_signal_connect (msg, "starting",
			  G_CALLBACK (np_message_started),
			  data);
}

static void
np_request_unqueued (SoupSession *session,
		     SoupMessage *msg,
		     GSocket    **socket)
{
	g_assert_false (g_socket_is_connected (*socket));
}

static void
np_request_finished (SoupMessage *msg,
		     gpointer     user_data)
{
	GMainLoop *loop = user_data;

	g_main_loop_quit (loop);
}

static void
do_non_persistent_test_for_session (SoupSession *session)
{
	SoupMessage *msg;
	GSocket *socket = NULL;
	GMainLoop *loop;

	loop = g_main_loop_new (NULL, FALSE);

	g_signal_connect (session, "request-queued",
			  G_CALLBACK (np_request_queued),
			  &socket);
	g_signal_connect (session, "request-unqueued",
			  G_CALLBACK (np_request_unqueued),
			  &socket);

	msg = soup_message_new_from_uri ("GET", base_uri);
	soup_message_headers_append (soup_message_get_request_headers (msg), "Connection", "close");
	g_signal_connect (msg, "finished",
			  G_CALLBACK (np_request_finished), loop);
	soup_session_send_async (session, msg, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
	g_main_loop_run (loop);
	g_main_loop_unref (loop);

	soup_test_assert_message_status (msg, SOUP_STATUS_OK);

	g_object_unref (msg);
	g_object_unref (socket);
}

static void
do_non_persistent_connection_test (void)
{
	SoupSession *session;

	g_test_bug ("578990");

	session = soup_test_session_new (NULL);
	do_non_persistent_test_for_session (session);
	soup_test_session_abort_unref (session);
}

static void
do_non_idempotent_test_for_session (SoupSession *session)
{
	SoupMessage *msg;
	GSocket *sockets[4] = { NULL, NULL, NULL, NULL };
	int i;
	GBytes *body;

	g_signal_connect (session, "request-queued",
			  G_CALLBACK (request_queued_socket_collector),
			  &sockets);

	debug_printf (2, "    GET\n");
	msg = soup_message_new_from_uri ("GET", base_uri);
	body = soup_test_session_send (session, msg, NULL, NULL);
	soup_test_assert_message_status (msg, SOUP_STATUS_OK);
	if (sockets[1]) {
		soup_test_assert (sockets[1] == NULL, "Message was retried");
		sockets[1] = sockets[2] = sockets[3] = NULL;
	}
	g_bytes_unref (body);
	g_object_unref (msg);

	debug_printf (2, "    POST\n");
	msg = soup_message_new_from_uri ("POST", base_uri);
	body = soup_test_session_send (session, msg, NULL, NULL);
	soup_test_assert_message_status (msg, SOUP_STATUS_OK);
	soup_test_assert (sockets[1] != sockets[0],
			  "Message was sent on existing connection");
	soup_test_assert (sockets[2] == NULL,
			  "Too many connections used");
	g_bytes_unref (body);
	g_object_unref (msg);

	for (i = 0; sockets[i]; i++)
		g_object_unref (sockets[i]);
}

static void
do_non_idempotent_connection_test (void)
{
	SoupSession *session;

	session = soup_test_session_new (NULL);
	do_non_idempotent_test_for_session (session);
	soup_test_session_abort_unref (session);
}

#define HTTP_SERVER  "http://127.0.0.1:47524"
#define HTTPS_SERVER "https://127.0.0.1:47525"
#define HTTP_PROXY   "http://127.0.0.1:47526"

static SoupConnectionState state_transitions[] = {
	/* NEW -> */        SOUP_CONNECTION_CONNECTING,
	/* CONNECTING -> */ SOUP_CONNECTION_IN_USE,
	/* IDLE -> */       SOUP_CONNECTION_DISCONNECTED,
	/* IN_USE -> */     SOUP_CONNECTION_IDLE,

	/* REMOTE_DISCONNECTED */ -1,
	/* DISCONNECTED */        -1,
};

static const char *state_names[] = {
	"NEW", "CONNECTING", "IDLE", "IN_USE",
	"REMOTE_DISCONNECTED", "DISCONNECTED"
};

static void
connection_state_changed (SoupConnection      *conn,
			  GParamSpec          *param,
			  SoupConnectionState *state)
{
	SoupConnectionState new_state;

	g_object_get (conn, "state", &new_state, NULL);
	debug_printf (2, "      %s -> %s\n",
		      state_names[*state], state_names[new_state]);
	soup_test_assert (state_transitions[*state] == new_state,
			  "Unexpected transition: %s -> %s\n",
			  state_names[*state], state_names[new_state]);
	*state = new_state;
}

static void
message_network_event (SoupMessage         *msg,
		       GSocketClientEvent   event,
		       GIOStream           *connection,
		       SoupConnectionState *state)
{
	SoupConnection *conn;

	if (event != G_SOCKET_CLIENT_RESOLVING)
		return;

	/* This is connecting, so we know it comes from a NEW state. */
	*state = SOUP_CONNECTION_NEW;

	conn = soup_message_get_connection (msg);
	g_assert_nonnull (conn);
	connection_state_changed (conn, NULL, state);

	g_signal_connect (conn, "notify::state",
                          G_CALLBACK (connection_state_changed),
                          state);
}

static void
do_one_connection_state_test (SoupSession         *session,
			      const char          *uri,
			      SoupConnectionState *state)
{
	SoupMessage *msg;
	GBytes *body;

	msg = soup_message_new ("GET", uri);
	g_signal_connect (msg, "network-event",
			  G_CALLBACK (message_network_event),
			  state);
	body = soup_test_session_async_send (session, msg, NULL, NULL);
	soup_test_assert_message_status (msg, SOUP_STATUS_OK);
	g_bytes_unref (body);
	g_object_unref (msg);
	soup_session_abort (session);
}

static void
do_connection_state_test_for_session (SoupSession *session)
{
	SoupConnectionState state;
	GProxyResolver *resolver;

	debug_printf (1, "    http\n");
	do_one_connection_state_test (session, HTTP_SERVER, &state);

	if (tls_available) {
		debug_printf (1, "    https\n");
		do_one_connection_state_test (session, HTTPS_SERVER, &state);
	} else
		debug_printf (1, "    https -- SKIPPING\n");

	resolver = g_simple_proxy_resolver_new (HTTP_PROXY, NULL);
	g_object_set (G_OBJECT (session),
		      "proxy-resolver", resolver,
		      NULL);
	g_object_unref (resolver);

	debug_printf (1, "    http with proxy\n");
	do_one_connection_state_test (session, HTTP_SERVER, &state);

	if (tls_available) {
		debug_printf (1, "    https with proxy\n");
		do_one_connection_state_test (session, HTTPS_SERVER, &state);
	} else
		debug_printf (1, "    https with proxy -- SKIPPING\n");
}

static void
do_connection_state_test (void)
{
	SoupSession *session;

	SOUP_TEST_SKIP_IF_NO_APACHE;

	session = soup_test_session_new (NULL);
	do_connection_state_test_for_session (session);
	soup_test_session_abort_unref (session);
}


static const char *event_names[] = {
	"RESOLVING", "RESOLVED", "CONNECTING", "CONNECTED",
	"PROXY_NEGOTIATING", "PROXY_NEGOTIATED",
	"TLS_HANDSHAKING", "TLS_HANDSHAKED", "COMPLETE"
};

static const char event_abbrevs[] = {
	'r', 'R', 'c', 'C', 'p', 'P', 't', 'T', 'x', '\0'
};

static const char *
event_name_from_abbrev (char abbrev)
{
	int evt;

	for (evt = 0; event_abbrevs[evt]; evt++) {
		if (event_abbrevs[evt] == abbrev)
			return event_names[evt];
	}
	return "???";
}

static void
network_event (SoupMessage *msg, GSocketClientEvent event,
	       GIOStream *connection, gpointer user_data)
{
	const char **events = user_data;

	debug_printf (2, "      %s\n", event_names[event]);
	soup_test_assert (**events == event_abbrevs[event],
			  "Unexpected event: %s (expected %s)",
			  event_names[event],
			  event_name_from_abbrev (**events));

	if (**events == event_abbrevs[event]) {
		if (event == G_SOCKET_CLIENT_RESOLVING ||
		    event == G_SOCKET_CLIENT_RESOLVED) {
			soup_test_assert (connection == NULL,
					  "Unexpectedly got connection (%s) with '%s' event",
					  G_OBJECT_TYPE_NAME (connection),
					  event_names[event]);
		} else if (event < G_SOCKET_CLIENT_TLS_HANDSHAKING) {
			soup_test_assert (G_IS_SOCKET_CONNECTION (connection),
					  "Unexpectedly got %s with '%s' event",
					  G_OBJECT_TYPE_NAME (connection),
					  event_names[event]);
		} else if (event == G_SOCKET_CLIENT_TLS_HANDSHAKING ||
			   event == G_SOCKET_CLIENT_TLS_HANDSHAKED) {
			soup_test_assert (G_IS_TLS_CLIENT_CONNECTION (connection),
					  "Unexpectedly got %s with '%s' event",
					  G_OBJECT_TYPE_NAME (connection),
					  event_names[event]);
		} else if (event == G_SOCKET_CLIENT_COMPLETE) {
			/* See if the previous expected event was TLS_HANDSHAKED */
			if ((*events)[-1] == 'T') {
				soup_test_assert (G_IS_TLS_CLIENT_CONNECTION (connection),
						  "Unexpectedly got %s with '%s' event",
						  G_OBJECT_TYPE_NAME (connection),
						  event_names[event]);
			} else {
				soup_test_assert (G_IS_SOCKET_CONNECTION (connection),
						  "Unexpectedly got %s with '%s' event",
						  G_OBJECT_TYPE_NAME (connection),
						  event_names[event]);
			}
		}
	}

	*events = *events + 1;
}

static void
do_one_connection_event_test (SoupSession *session, const char *uri,
			      const char *events)
{
	SoupMessage *msg;
	GBytes *body;

	msg = soup_message_new ("GET", uri);
	g_signal_connect (msg, "network-event",
			  G_CALLBACK (network_event),
			  &events);
	body = soup_test_session_send (session, msg, NULL, NULL);
	soup_test_assert_message_status (msg, SOUP_STATUS_OK);
	while (*events) {
		soup_test_assert (!*events,
				  "Expected %s",
				  event_name_from_abbrev (*events));
		events++;
	}

	g_bytes_unref (body);
	g_object_unref (msg);
	soup_session_abort (session);
}

static void
do_connection_event_test_for_session (SoupSession *session)
{
	GProxyResolver *resolver;

	debug_printf (1, "    http\n");
	do_one_connection_event_test (session, HTTP_SERVER, "rRcCx");

	if (tls_available) {
		debug_printf (1, "    https\n");
		do_one_connection_event_test (session, HTTPS_SERVER, "rRcCtTx");
	} else
		debug_printf (1, "    https -- SKIPPING\n");

	resolver = g_simple_proxy_resolver_new (HTTP_PROXY, NULL);
	g_object_set (G_OBJECT (session),
		      "proxy-resolver", resolver,
		      NULL);
	g_object_unref (resolver);

	debug_printf (1, "    http with proxy\n");
	do_one_connection_event_test (session, HTTP_SERVER, "rRcCx");

	if (tls_available) {
		debug_printf (1, "    https with proxy\n");
		do_one_connection_event_test (session, HTTPS_SERVER, "rRcCpPtTx");
	} else
		debug_printf (1, "    https with proxy -- SKIPPING\n");
}

static void
do_connection_event_test (void)
{
	SoupSession *session;

	SOUP_TEST_SKIP_IF_NO_APACHE;

	session = soup_test_session_new (NULL);
	do_connection_event_test_for_session (session);
	soup_test_session_abort_unref (session);
}

int
main (int argc, char **argv)
{
	int ret;

	test_init (argc, argv, NULL);
	apache_init ();

	server = soup_test_server_new (SOUP_TEST_SERVER_IN_THREAD);
	soup_server_add_handler (server, NULL, server_callback, "http", NULL);
	base_uri = soup_test_server_get_uri (server, "http", NULL);

	g_test_add_func ("/connection/content-length-framing", do_content_length_framing_test);
	g_test_add_func ("/connection/persistent-connection-timeout", do_persistent_connection_timeout_test);
	g_test_add_func ("/connection/persistent-connection-timeout-with-cancellable",
			 do_persistent_connection_timeout_test_with_cancellation);
	g_test_add_func ("/connection/max-conns", do_max_conns_test);
	g_test_add_func ("/connection/non-persistent", do_non_persistent_connection_test);
	g_test_add_func ("/connection/non-idempotent", do_non_idempotent_connection_test);
	g_test_add_func ("/connection/state", do_connection_state_test);
	g_test_add_func ("/connection/event", do_connection_event_test);

	ret = g_test_run ();

	g_uri_unref (base_uri);
	soup_test_server_quit_unref (server);

	test_cleanup ();
	return ret;
}
