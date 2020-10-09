/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2007 Red Hat, Inc.
 * Copyright (C) 2011 Igalia, S.L.
 */

#include "test-utils.h"

SoupServer *server;
SoupURI *base_uri;

static void
server_callback (SoupServer        *server,
		 SoupServerMessage *msg,
		 const char        *path,
		 GHashTable        *query,
		 gpointer           data)
{
	const char *accept_encoding, *options;
	GSList *codings;
	GBytes *response = NULL;
	SoupMessageHeaders *request_headers;
	SoupMessageHeaders *response_headers;
	SoupMessageBody *response_body;

	request_headers = soup_server_message_get_request_headers (msg);
	options = soup_message_headers_get_one (request_headers,
						"X-Test-Options");
	if (!options)
		options = "";

	accept_encoding = soup_message_headers_get_list (request_headers,
							 "Accept-Encoding");
	if (accept_encoding && !soup_header_contains (options, "force-encode"))
		codings = soup_header_parse_quality_list (accept_encoding, NULL);
	else
		codings = NULL;

	response_headers = soup_server_message_get_response_headers (msg);

	if (codings) {
		gboolean claim_deflate, claim_gzip;
		const char *extension = NULL, *encoding = NULL;

		claim_deflate = g_slist_find_custom (codings, "deflate", (GCompareFunc)g_ascii_strcasecmp) != NULL;
		claim_gzip = g_slist_find_custom (codings, "gzip", (GCompareFunc)g_ascii_strcasecmp) != NULL;

		if (claim_gzip && (!claim_deflate ||
				   (!soup_header_contains (options, "prefer-deflate-zlib") &&
				    !soup_header_contains (options, "prefer-deflate-raw")))) {
			extension = "gz";
			encoding = "gzip";
		} else if (claim_deflate) {
			if (soup_header_contains (options, "prefer-deflate-raw")) {
				extension = "raw";
				encoding = "deflate";
			} else {
				extension = "zlib";
				encoding = "deflate";
			}
		}
		if (extension && encoding) {
			char *resource;

			resource = g_strdup_printf ("%s.%s", path, extension);
			response = soup_test_load_resource (resource, NULL);

			if (response) {
				soup_message_headers_append (response_headers,
							     "Content-Encoding",
							     encoding);
			}
			g_free (resource);
		}
	}

	soup_header_free_list (codings);

	if (!response)
		response = soup_test_load_resource (path, NULL);
	if (!response) {
		/* If path.gz exists but can't be read, we'll send back
		 * the error with "Content-Encoding: gzip" but there's
		 * no body, so, eh.
		 */
		soup_server_message_set_status (msg, SOUP_STATUS_NOT_FOUND, NULL);
		return;
	}

	if (soup_header_contains (options, "force-encode")) {
		const gchar *encoding = "gzip";

		if (soup_header_contains (options, "prefer-deflate-zlib") ||
		    soup_header_contains (options, "prefer-deflate-raw"))
			encoding = "deflate";

		soup_message_headers_replace (response_headers,
					      "Content-Encoding",
					      encoding);
	}

	/* Content-Type matches the "real" format, not the sent format */
	if (g_str_has_suffix (path, ".gz")) {
		soup_message_headers_append (response_headers,
					     "Content-Type",
					     "application/gzip");
	} else {
		soup_message_headers_append (response_headers,
					     "Content-Type",
					     "text/plain");
	}

	soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
	soup_message_headers_set_encoding (response_headers, SOUP_ENCODING_CHUNKED);

	response_body = soup_server_message_get_response_body (msg);
	if (!soup_header_contains (options, "empty"))
		soup_message_body_append_bytes (response_body, response);
	g_bytes_unref (response);

	if (soup_header_contains (options, "trailing-junk")) {
		soup_message_body_append (response_body, SOUP_MEMORY_COPY,
					  options, strlen (options));
	}
	soup_message_body_complete (response_body);
}

typedef struct {
	SoupSession *session;
	SoupMessage *msg;
	SoupRequest *req;
	GBytes *response;
} CodingTestData;

typedef enum {
	CODING_TEST_DEFAULT     = 0,
	CODING_TEST_NO_DECODER  = (1 << 0),
	CODING_TEST_REQUEST_API = (1 << 1),
	CODING_TEST_EMPTY       = (1 << 2)
} CodingTestType;

typedef enum {
	NO_CHECK,
	EXPECT_DECODED,
	EXPECT_NOT_DECODED
} MessageContentStatus;

static void
check_response (CodingTestData *data,
		const char *expected_encoding,
		const char *expected_content_type,
		MessageContentStatus status,
		GBytes *body)
{
	const char *coding, *type;

	soup_test_assert_message_status (data->msg, SOUP_STATUS_OK);

	coding = soup_message_headers_get_one (data->msg->response_headers, "Content-Encoding");
	g_assert_cmpstr (coding, ==, expected_encoding);

	if (status != NO_CHECK) {
		if (status == EXPECT_DECODED)
			g_assert_true (soup_message_get_flags (data->msg) & SOUP_MESSAGE_CONTENT_DECODED);
		else
			g_assert_false (soup_message_get_flags (data->msg) & SOUP_MESSAGE_CONTENT_DECODED);
	}

	type = soup_message_headers_get_one (data->msg->response_headers, "Content-Type");
	g_assert_cmpstr (type, ==, expected_content_type);

	g_assert_true (g_bytes_equal (body, data->response));
}

static void
setup_coding_test (CodingTestData *data, gconstpointer test_data)
{
	CodingTestType test_type = GPOINTER_TO_INT (test_data);
	SoupMessage *msg;
	SoupURI *uri;

	data->session = soup_test_session_new (SOUP_TYPE_SESSION,
					       NULL);

	uri = soup_uri_new_with_base (base_uri, "/mbox");

	if (test_type & CODING_TEST_EMPTY)
		data->response = g_bytes_new_static (NULL, 0);
	else {
		msg = soup_message_new_from_uri ("GET", uri);
		data->response = soup_test_session_send (data->session, msg, NULL, NULL);
		g_object_unref (msg);
	}

	if (test_type & CODING_TEST_REQUEST_API) {
		SoupRequestHTTP *reqh;

		reqh = soup_session_request_http_uri (data->session, "GET", uri, NULL);
		data->req = SOUP_REQUEST (reqh);
		data->msg = soup_request_http_get_message (reqh);
	} else
		data->msg = soup_message_new_from_uri ("GET", uri);
	soup_uri_free (uri);

	if (test_type & CODING_TEST_NO_DECODER)
		soup_session_remove_feature_by_type (data->session, SOUP_TYPE_CONTENT_DECODER);
}

static void
teardown_coding_test (CodingTestData *data, gconstpointer test_data)
{
	g_bytes_unref (data->response);

	g_clear_object (&data->req);
	g_object_unref (data->msg);

	soup_test_session_abort_unref (data->session);
}

static void
do_coding_test_plain (CodingTestData *data, gconstpointer test_data)
{
	GBytes *body;

	body = soup_test_session_send (data->session, data->msg, NULL, NULL);
	check_response (data, NULL, "text/plain", EXPECT_NOT_DECODED, body);
	g_bytes_unref (body);
}

static void
do_coding_test_gzip (CodingTestData *data, gconstpointer test_data)
{
	GBytes *body;

	body = soup_test_session_send (data->session, data->msg, NULL, NULL);
	check_response (data, "gzip", "text/plain", EXPECT_DECODED, body);
	g_bytes_unref (body);
}

static void
do_coding_test_gzip_with_junk (CodingTestData *data, gconstpointer test_data)
{
	GBytes *body;

	g_test_bug ("606352");
	g_test_bug ("676477");

	soup_message_headers_append (data->msg->request_headers,
				     "X-Test-Options", "trailing-junk");

	body = soup_test_session_send (data->session, data->msg, NULL, NULL);
	check_response (data, "gzip", "text/plain", EXPECT_DECODED, body);
	g_bytes_unref (body);
}

static void
do_coding_test_gzip_bad_server (CodingTestData *data, gconstpointer test_data)
{
	GBytes *body;

	g_test_bug ("613361");

	soup_message_headers_append (data->msg->request_headers,
				     "X-Test-Options", "force-encode");

	body = soup_test_session_send (data->session, data->msg, NULL, NULL);

	/* Failed content-decoding should have left the body untouched
	 * from what the server sent... which happens to be the
	 * uncompressed data.
	 */
	check_response (data, "gzip", "text/plain", EXPECT_NOT_DECODED, body);
	g_bytes_unref (body);
}

static void
do_coding_test_deflate (CodingTestData *data, gconstpointer test_data)
{
	GBytes *body;

	soup_message_headers_append (data->msg->request_headers,
				     "X-Test-Options", "prefer-deflate-zlib");
	body = soup_test_session_send (data->session, data->msg, NULL, NULL);
	check_response (data, "deflate", "text/plain", EXPECT_DECODED, body);
	g_bytes_unref (body);
}

static void
do_coding_test_deflate_with_junk (CodingTestData *data, gconstpointer test_data)
{
	GBytes *body;

	g_test_bug ("606352");
	g_test_bug ("676477");

	soup_message_headers_append (data->msg->request_headers,
				     "X-Test-Options", "prefer-deflate-zlib, trailing-junk");
	body = soup_test_session_send (data->session, data->msg, NULL, NULL);
	check_response (data, "deflate", "text/plain", EXPECT_DECODED, body);
	g_bytes_unref (body);
}

static void
do_coding_test_deflate_bad_server (CodingTestData *data, gconstpointer test_data)
{
	GBytes *body;

	g_test_bug ("613361");

	soup_message_headers_append (data->msg->request_headers,
				     "X-Test-Options", "force-encode, prefer-deflate-zlib");
	body = soup_test_session_send (data->session, data->msg, NULL, NULL);
	check_response (data, "deflate", "text/plain", EXPECT_NOT_DECODED, body);
	g_bytes_unref (body);
}

static void
do_coding_test_deflate_raw (CodingTestData *data, gconstpointer test_data)
{
	GBytes *body;

	soup_message_headers_append (data->msg->request_headers,
				     "X-Test-Options", "prefer-deflate-raw");
	body = soup_test_session_send (data->session, data->msg, NULL, NULL);
	check_response (data, "deflate", "text/plain", EXPECT_DECODED, body);
	g_bytes_unref (body);
}

static void
do_coding_test_deflate_raw_bad_server (CodingTestData *data, gconstpointer test_data)
{
	GBytes *body;

	g_test_bug ("613361");

	soup_message_headers_append (data->msg->request_headers,
				     "X-Test-Options", "force-encode, prefer-deflate-raw");
	body = soup_test_session_send (data->session, data->msg, NULL, NULL);
	check_response (data, "deflate", "text/plain", EXPECT_NOT_DECODED, body);
	g_bytes_unref (body);
}

static void
read_finished (GObject *stream, GAsyncResult *result, gpointer user_data)
{
	gssize *nread = user_data;
	GError *error = NULL;

	*nread = g_input_stream_read_finish (G_INPUT_STREAM (stream),
					     result, &error);
	g_assert_no_error (error);
	g_clear_error (&error);
}

static void
do_single_coding_req_test (CodingTestData *data,
			   const char *expected_encoding,
			   const char *expected_content_type,
			   MessageContentStatus status)
{
	GInputStream *stream;
	GByteArray *body;
        GBytes *body_bytes;
	guchar buf[1024];
	gssize nread;
	GError *error = NULL;

	body = g_byte_array_new ();

	stream = soup_test_request_send (data->req, NULL, 0, &error);
	if (!stream) {
		g_assert_no_error (error);
		g_error_free (error);
		return;
	}

	do {
		nread = -2;
		g_input_stream_read_async (stream, buf, sizeof (buf),
					   G_PRIORITY_DEFAULT,
					   NULL, read_finished, &nread);
		while (nread == -2)
			g_main_context_iteration (NULL, TRUE);

		if (nread > 0)
			g_byte_array_append (body, buf, nread);
	} while (nread > 0);

	soup_test_request_close_stream (data->req, stream, NULL, &error);
	g_assert_no_error (error);
	g_clear_error (&error);
	g_object_unref (stream);

        body_bytes = g_byte_array_free_to_bytes (body);
	check_response (data, expected_encoding, expected_content_type, status, body_bytes);
	g_bytes_unref (body_bytes);
}

static void
do_coding_req_test_plain (CodingTestData *data, gconstpointer test_data)
{
	do_single_coding_req_test (data, NULL, "text/plain", EXPECT_NOT_DECODED);
}

static void
do_coding_req_test_gzip (CodingTestData *data, gconstpointer test_data)
{
	do_single_coding_req_test (data, "gzip", "text/plain", EXPECT_DECODED);
}

static void
do_coding_req_test_gzip_with_junk (CodingTestData *data, gconstpointer test_data)
{
	g_test_bug ("606352");
	g_test_bug ("676477");

	soup_message_headers_append (data->msg->request_headers,
				     "X-Test-Options", "trailing-junk");

	do_single_coding_req_test (data, "gzip", "text/plain", EXPECT_DECODED);
}

static void
do_coding_req_test_gzip_bad_server (CodingTestData *data, gconstpointer test_data)
{
	g_test_bug ("613361");

	soup_message_headers_append (data->msg->request_headers,
				     "X-Test-Options", "force-encode");
	do_single_coding_req_test (data, "gzip", "text/plain", EXPECT_NOT_DECODED);
}

static void
do_coding_req_test_deflate (CodingTestData *data, gconstpointer test_data)
{
	soup_message_headers_append (data->msg->request_headers,
				     "X-Test-Options", "prefer-deflate-zlib");
	do_single_coding_req_test (data, "deflate", "text/plain", EXPECT_DECODED);
}

static void
do_coding_req_test_deflate_with_junk (CodingTestData *data, gconstpointer test_data)
{
	g_test_bug ("606352");
	g_test_bug ("676477");

	soup_message_headers_append (data->msg->request_headers,
				     "X-Test-Options", "prefer-deflate-zlib, trailing-junk");
	do_single_coding_req_test (data, "deflate", "text/plain", EXPECT_DECODED);
}

static void
do_coding_req_test_deflate_bad_server (CodingTestData *data, gconstpointer test_data)
{
	g_test_bug ("613361");

	soup_message_headers_append (data->msg->request_headers,
				     "X-Test-Options", "force-encode, prefer-deflate-zlib");
	do_single_coding_req_test (data, "deflate", "text/plain", EXPECT_NOT_DECODED);
}

static void
do_coding_req_test_deflate_raw (CodingTestData *data, gconstpointer test_data)
{
	soup_message_headers_append (data->msg->request_headers,
				     "X-Test-Options", "prefer-deflate-raw");
	do_single_coding_req_test (data, "deflate", "text/plain", EXPECT_DECODED);
}

static void
do_coding_req_test_deflate_raw_bad_server (CodingTestData *data, gconstpointer test_data)
{
	g_test_bug ("613361");

	soup_message_headers_append (data->msg->request_headers,
				     "X-Test-Options", "force-encode, prefer-deflate-raw");
	do_single_coding_req_test (data, "deflate", "text/plain", EXPECT_NOT_DECODED);
}

static void
do_coding_msg_empty_test (CodingTestData *data, gconstpointer test_data)
{
	GBytes *body;

	g_test_bug ("697527");

	soup_message_headers_append (data->msg->request_headers,
				     "X-Test-Options", "empty");
	body = soup_test_session_send (data->session, data->msg, NULL, NULL);
	check_response (data, "gzip", "text/plain", EXPECT_NOT_DECODED, body);
	g_bytes_unref (body);
}

static void
do_coding_req_empty_test (CodingTestData *data, gconstpointer test_data)
{
	g_test_bug ("697527");

	soup_message_headers_append (data->msg->request_headers,
				     "X-Test-Options", "empty");
	do_single_coding_req_test (data, "gzip", "text/plain", EXPECT_NOT_DECODED);
}

int
main (int argc, char **argv)
{
	int ret;

	test_init (argc, argv, NULL);

	server = soup_test_server_new (SOUP_TEST_SERVER_IN_THREAD);
	soup_server_add_handler (server, NULL, server_callback, NULL, NULL);
	base_uri = soup_test_server_get_uri (server, "http", NULL);

	g_test_add ("/coding/message/plain", CodingTestData,
		    GINT_TO_POINTER (CODING_TEST_NO_DECODER),
		    setup_coding_test, do_coding_test_plain, teardown_coding_test);
	g_test_add ("/coding/message/gzip", CodingTestData,
		    GINT_TO_POINTER (CODING_TEST_DEFAULT),
		    setup_coding_test, do_coding_test_gzip, teardown_coding_test);
	g_test_add ("/coding/message/gzip/with-junk", CodingTestData,
		    GINT_TO_POINTER (CODING_TEST_DEFAULT),
		    setup_coding_test, do_coding_test_gzip_with_junk, teardown_coding_test);
	g_test_add ("/coding/message/gzip/bad-server", CodingTestData,
		    GINT_TO_POINTER (CODING_TEST_DEFAULT),
		    setup_coding_test, do_coding_test_gzip_bad_server, teardown_coding_test);
	g_test_add ("/coding/message/deflate", CodingTestData,
		    GINT_TO_POINTER (CODING_TEST_DEFAULT),
		    setup_coding_test, do_coding_test_deflate, teardown_coding_test);
	g_test_add ("/coding/message/deflate/with-junk", CodingTestData,
		    GINT_TO_POINTER (CODING_TEST_DEFAULT),
		    setup_coding_test, do_coding_test_deflate_with_junk, teardown_coding_test);
	g_test_add ("/coding/message/deflate/bad-server", CodingTestData,
		    GINT_TO_POINTER (CODING_TEST_DEFAULT),
		    setup_coding_test, do_coding_test_deflate_bad_server, teardown_coding_test);
	g_test_add ("/coding/message/deflate-raw", CodingTestData,
		    GINT_TO_POINTER (CODING_TEST_DEFAULT),
		    setup_coding_test, do_coding_test_deflate_raw, teardown_coding_test);
	g_test_add ("/coding/message/deflate-raw/bad-server", CodingTestData,
		    GINT_TO_POINTER (CODING_TEST_DEFAULT),
		    setup_coding_test, do_coding_test_deflate_raw_bad_server, teardown_coding_test);

	g_test_add ("/coding/request/plain", CodingTestData,
		    GINT_TO_POINTER (CODING_TEST_NO_DECODER | CODING_TEST_REQUEST_API),
		    setup_coding_test, do_coding_req_test_plain, teardown_coding_test);
	g_test_add ("/coding/request/gzip", CodingTestData,
		    GINT_TO_POINTER (CODING_TEST_REQUEST_API),
		    setup_coding_test, do_coding_req_test_gzip, teardown_coding_test);
	g_test_add ("/coding/request/gzip/with-junk", CodingTestData,
		    GINT_TO_POINTER (CODING_TEST_REQUEST_API),
		    setup_coding_test, do_coding_req_test_gzip_with_junk, teardown_coding_test);
	g_test_add ("/coding/request/gzip/bad-server", CodingTestData,
		    GINT_TO_POINTER (CODING_TEST_REQUEST_API),
		    setup_coding_test, do_coding_req_test_gzip_bad_server, teardown_coding_test);
	g_test_add ("/coding/request/deflate", CodingTestData,
		    GINT_TO_POINTER (CODING_TEST_REQUEST_API),
		    setup_coding_test, do_coding_req_test_deflate, teardown_coding_test);
	g_test_add ("/coding/request/deflate/with-junk", CodingTestData,
		    GINT_TO_POINTER (CODING_TEST_REQUEST_API),
		    setup_coding_test, do_coding_req_test_deflate_with_junk, teardown_coding_test);
	g_test_add ("/coding/request/deflate/bad-server", CodingTestData,
		    GINT_TO_POINTER (CODING_TEST_REQUEST_API),
		    setup_coding_test, do_coding_req_test_deflate_bad_server, teardown_coding_test);
	g_test_add ("/coding/request/deflate-raw", CodingTestData,
		    GINT_TO_POINTER (CODING_TEST_REQUEST_API),
		    setup_coding_test, do_coding_req_test_deflate_raw, teardown_coding_test);
	g_test_add ("/coding/request/deflate-raw/bad-server", CodingTestData,
		    GINT_TO_POINTER (CODING_TEST_REQUEST_API),
		    setup_coding_test, do_coding_req_test_deflate_raw_bad_server, teardown_coding_test);

	g_test_add ("/coding/message/empty", CodingTestData,
		    GINT_TO_POINTER (CODING_TEST_EMPTY),
		    setup_coding_test, do_coding_msg_empty_test, teardown_coding_test);
	g_test_add ("/coding/request/empty", CodingTestData,
		    GINT_TO_POINTER (CODING_TEST_REQUEST_API | CODING_TEST_EMPTY),
		    setup_coding_test, do_coding_req_empty_test, teardown_coding_test);

	ret = g_test_run ();

	soup_uri_free (base_uri);
	soup_test_server_quit_unref (server);

	test_cleanup ();
	return ret;
}
