/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

#include "test-utils.h"

static struct {
	const char *one, *two;
        gboolean equal;
        GUriFlags flags_one, flags_two;
} eq_tests[] = {
        // NOTE: GUri doesn't remove dot segments from absolute URIs
	// { "example://a/b/c/%7Bfoo%7D", "eXAMPLE://a/./b/../b/%63/%7Bfoo%7D", "628728" },
	{ "http://example.com", "http://example.com/", TRUE,
          SOUP_HTTP_URI_FLAGS, SOUP_HTTP_URI_FLAGS },
	/* From RFC 2616 */
	{ "http://abc.com:80/~smith/home.html", "http://abc.com:80/~smith/home.html", TRUE,
          SOUP_HTTP_URI_FLAGS, SOUP_HTTP_URI_FLAGS },
	{ "http://abc.com:80/~smith/home.html", "http://ABC.com/%7Esmith/home.html", TRUE,
          SOUP_HTTP_URI_FLAGS, SOUP_HTTP_URI_FLAGS },
	{ "http://abc.com:80/~smith/home.html", "http://ABC.com:/%7esmith/home.html", TRUE,
          SOUP_HTTP_URI_FLAGS, SOUP_HTTP_URI_FLAGS },
        /* Test flags affecting comparisons */
        { "http://example.com/%2F", "http://example.com/%2F", FALSE,
          G_URI_FLAGS_ENCODED_PATH, G_URI_FLAGS_NONE },
        { "http://example.com/%2F", "http://example.com/%2F", TRUE,
          G_URI_FLAGS_PARSE_RELAXED, G_URI_FLAGS_NONE },
        { "http://example.com/%2F", "http://example.com/%2F", TRUE,
          G_URI_FLAGS_PARSE_RELAXED, G_URI_FLAGS_HAS_PASSWORD },
};
static int num_eq_tests = G_N_ELEMENTS(eq_tests);

static void
do_equality_tests (void)
{
	GUri *uri1, *uri2;
	int i;

	for (i = 0; i < num_eq_tests; i++) {
		uri1 = g_uri_parse (eq_tests[i].one, eq_tests[i].flags_one, NULL);
		uri2 = g_uri_parse (eq_tests[i].two, eq_tests[i].flags_two, NULL);

		debug_printf (1, "<%s> %c= <%s>\n", eq_tests[i].one, eq_tests[i].equal ? '=' : '!', eq_tests[i].two);
                g_assert_cmpint (soup_uri_equal (uri1, uri2), ==, eq_tests[i].equal);

		g_uri_unref (uri1);
		g_uri_unref (uri2);
	}
}

static void
do_copy_tests (void)
{
        GUri *uri;
        GUri *copy;
        char *str;

        uri = g_uri_parse ("http://127.0.0.1:1234/foo#bar", SOUP_HTTP_URI_FLAGS, NULL);

        /* Exact copy */
        copy = soup_uri_copy (uri, SOUP_URI_NONE);
        str = g_uri_to_string (copy);
        g_assert_cmpstr (str, ==, "http://127.0.0.1:1234/foo#bar");
        g_free (str);
        g_uri_unref (copy);

        /* Update the path */
        copy = soup_uri_copy (uri, SOUP_URI_PATH, "/baz", SOUP_URI_NONE);
        str = g_uri_to_string (copy);
        g_assert_cmpstr (str, ==, "http://127.0.0.1:1234/baz#bar");
        g_free (str);
        g_uri_unref (copy);

        /* Add credentials */
        copy = soup_uri_copy (uri, SOUP_URI_USER, "user", SOUP_URI_PASSWORD, "password", SOUP_URI_NONE);
        str = g_uri_to_string (copy);
        g_assert_cmpstr (str, ==, "http://user:password@127.0.0.1:1234/foo#bar");
        g_free (str);
        g_uri_unref (copy);

        /* Remove the fragment and add a query */
        copy = soup_uri_copy (uri, SOUP_URI_FRAGMENT, NULL, SOUP_URI_QUERY, "baz=1", SOUP_URI_NONE);
        str = g_uri_to_string (copy);
        g_assert_cmpstr (str, ==, "http://127.0.0.1:1234/foo?baz=1");
        g_free (str);
        g_uri_unref (copy);

        /* Update host and port */
        copy = soup_uri_copy (uri, SOUP_URI_HOST, "localhost", SOUP_URI_PORT, -1, SOUP_URI_NONE);
        str = g_uri_to_string (copy);
        g_assert_cmpstr (str, ==, "http://localhost/foo#bar");
        g_free (str);
        g_uri_unref (copy);

        /* Update everything */
        copy = soup_uri_copy (uri,
                              SOUP_URI_SCHEME, "https",
                              SOUP_URI_USER, "user",
                              SOUP_URI_PASSWORD, "password",
                              SOUP_URI_HOST, "localhost",
                              SOUP_URI_PORT, 4321,
                              SOUP_URI_PATH, "/baz",
                              SOUP_URI_FRAGMENT, "foo",
                              SOUP_URI_NONE);
        str = g_uri_to_string (copy);
        g_assert_cmpstr (str, ==, "https://user:password@localhost:4321/baz#foo");
        g_free (str);
        g_uri_unref (copy);

        /* Convert to file */
        copy = soup_uri_copy (uri, SOUP_URI_SCHEME, "file", SOUP_URI_HOST, "", SOUP_URI_PORT, -1, SOUP_URI_FRAGMENT, NULL, SOUP_URI_NONE);
        str = g_uri_to_string (copy);
        g_assert_cmpstr (str, ==, "file:///foo");
        g_free (str);
        g_uri_unref (copy);

        g_uri_unref (uri);
}

int
main (int argc, char **argv)
{
	int ret;

	test_init (argc, argv, NULL);

	g_test_add_func ("/uri/equality", do_equality_tests);
	g_test_add_func ("/uri/copy", do_copy_tests);

	ret = g_test_run ();

	test_cleanup ();
	return ret;
}
