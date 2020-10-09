/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2008 Red Hat, Inc.
 */

#include "test-utils.h"

#define RESPONSE_CHUNK_SIZE 1024

GBytes *full_response;
char *full_response_md5;

static void
write_next_chunk (SoupServerMessage *msg,
		  gpointer           user_data)
{
	gsize *offset = user_data;
	gsize chunk_length;
	SoupMessageBody *response_body;

	response_body = soup_server_message_get_response_body (msg);

	chunk_length = MIN (RESPONSE_CHUNK_SIZE, g_bytes_get_size (full_response) - *offset);
	if (chunk_length > 0) {
		debug_printf (2, "  writing chunk\n");
                GBytes *chunk = g_bytes_new_from_bytes (full_response, *offset, chunk_length);
                soup_message_body_append_bytes (response_body, chunk);
                g_bytes_unref (chunk);
		*offset += chunk_length;
	} else {
		debug_printf (2, "  done\n");
		/* This is only actually needed in the chunked and eof
		 * cases, but it's harmless in the content-length
		 * case.
		 */
		soup_message_body_complete (response_body);
	}
}

static void
free_offset (SoupServerMessage *msg,
	     gpointer           offset)
{
	g_free (offset);
}

static void
server_callback (SoupServer        *server,
		 SoupServerMessage *msg,
		 const char        *path,
		 GHashTable        *query,
		 gpointer           data)
{
	gsize *offset;
	SoupMessageHeaders *response_headers;

	response_headers = soup_server_message_get_response_headers (msg);
	if (!strcmp (path, "/chunked")) {
		soup_message_headers_set_encoding (response_headers,
						   SOUP_ENCODING_CHUNKED);
	} else if (!strcmp (path, "/content-length")) {
		soup_message_headers_set_encoding (response_headers,
						   SOUP_ENCODING_CONTENT_LENGTH);
		soup_message_headers_set_content_length (response_headers,
							 g_bytes_get_size (full_response));
	} else if (!strcmp (path, "/eof")) {
		soup_message_headers_set_encoding (response_headers,
						   SOUP_ENCODING_EOF);
	} else {
		soup_server_message_set_status (msg, SOUP_STATUS_NOT_FOUND, NULL);
		return;
	}
	soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);

	offset = g_new0 (gsize, 1);
	g_signal_connect (msg, "wrote-headers",
			  G_CALLBACK (write_next_chunk), offset);
	g_signal_connect (msg, "wrote-chunk",
			  G_CALLBACK (write_next_chunk), offset);
	g_signal_connect (msg, "finished",
			  G_CALLBACK (free_offset), offset);
}

static void
do_request (SoupSession *session, SoupURI *base_uri, char *path)
{
	SoupURI *uri;
	SoupMessage *msg;
	GBytes *body;
	char *md5;

	uri = soup_uri_new_with_base (base_uri, path);
	msg = soup_message_new_from_uri ("GET", uri);
	soup_uri_free (uri);

	body = soup_test_session_async_send (session, msg);

	soup_test_assert_message_status (msg, SOUP_STATUS_OK);
	g_assert_cmpint (g_bytes_get_size (body), ==, g_bytes_get_size (full_response));

	md5 = g_compute_checksum_for_data (G_CHECKSUM_MD5,
					   (guchar *)g_bytes_get_data (body, NULL),
					   g_bytes_get_size (body));
	g_assert_cmpstr (md5, ==, full_response_md5);
	g_free (md5);

	g_bytes_unref (body);
	g_object_unref (msg);
}

static void
do_chunked_test (gconstpointer data)
{
	SoupURI *base_uri = (SoupURI *)data;
	SoupSession *session;

	session = soup_test_session_new (SOUP_TYPE_SESSION, NULL);
	do_request (session, base_uri, "chunked");
	soup_test_session_abort_unref (session);
}

static void
do_content_length_test (gconstpointer data)
{
	SoupURI *base_uri = (SoupURI *)data;
	SoupSession *session;

	session = soup_test_session_new (SOUP_TYPE_SESSION, NULL);
	do_request (session, base_uri, "content-length");
	soup_test_session_abort_unref (session);
}

static void
do_eof_test (gconstpointer data)
{
	SoupURI *base_uri = (SoupURI *)data;
	SoupSession *session;

	g_test_bug ("572153");

	session = soup_test_session_new (SOUP_TYPE_SESSION, NULL);
	do_request (session, base_uri, "eof");
	soup_test_session_abort_unref (session);
}

int
main (int argc, char **argv)
{
	GMainLoop *loop;
	SoupServer *server;
	SoupURI *base_uri;
	int ret;

	test_init (argc, argv, NULL);

	full_response = soup_test_get_index ();
	full_response_md5 = g_compute_checksum_for_bytes (G_CHECKSUM_MD5, full_response);

	server = soup_test_server_new (SOUP_TEST_SERVER_DEFAULT);
	soup_server_add_handler (server, NULL,
				 server_callback, NULL, NULL);

	loop = g_main_loop_new (NULL, TRUE);

	base_uri = soup_test_server_get_uri (server, "http", NULL);

	g_test_add_data_func ("/streaming/chunked", base_uri, do_chunked_test);
	g_test_add_data_func ("/streaming/content-length", base_uri, do_content_length_test);
	g_test_add_data_func ("/streaming/eof", base_uri, do_eof_test);

	ret = g_test_run ();

	soup_uri_free (base_uri);
	g_main_loop_unref (loop);

	g_free (full_response_md5);
	soup_test_server_quit_unref (server);
	test_cleanup ();

	return ret;
}
