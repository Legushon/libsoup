#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libsoup/soup.h"
#include "libsoup/soup-auth.h"
#include "libsoup/soup-private.h"
#include "libsoup/soup-message-private.h"
#include "libsoup/soup-session.h"

int errors = 0;

typedef struct {
	const char *explanation;
	const char *url;
	const char *expected;
	gboolean success;
} SoupAuthTest;

SoupAuthTest tests[] = {
	{ "No auth available, should fail",
	  "http://primates.ximian.com/~danw/soup-test/Basic/realm1/index.txt",
	  "0", FALSE },

	{ "Should fail with no auth, fail again with bad password, and give up",
	  "http://user4:realm4@primates.ximian.com/~danw/soup-test/Basic/realm2/index.txt",
	  "04", FALSE },

	{ "Known realm, auth provided, so should succeed immediately",
	  "http://user1:realm1@primates.ximian.com/~danw/soup-test/Basic/realm1/index.txt",
	  "1", TRUE },

	{ "Now should automatically reuse previous auth",
	  "http://primates.ximian.com/~danw/soup-test/Basic/realm1/index.txt",
	  "1", TRUE },

	{ "Subdir should also automatically reuse auth",
	  "http://primates.ximian.com/~danw/soup-test/Basic/realm1/subdir/index.txt",
	  "1", TRUE },

	{ "Subdir should retry last auth, but will fail this time",
	  "http://primates.ximian.com/~danw/soup-test/Basic/realm1/realm2/index.txt",
	  "1", FALSE },

	{ "Now should use provided auth on first try",
	  "http://user2:realm2@primates.ximian.com/~danw/soup-test/Basic/realm1/realm2/index.txt",
	  "2", TRUE },

	{ "Reusing last auth. Should succeed on first try",
	  "http://primates.ximian.com/~danw/soup-test/Basic/realm1/realm2/index.txt",
	  "2", TRUE },

	{ "Reuse will fail, but 2nd try will succeed because it's a known realm",
	  "http://primates.ximian.com/~danw/soup-test/Basic/realm1/realm2/realm1/index.txt",
	  "21", TRUE },

	{ "Should succeed on first try. (Known realm with cached password)",
	  "http://primates.ximian.com/~danw/soup-test/Basic/realm2/index.txt",
	  "2", TRUE },

	{ "Fail once, then use password",
	  "http://user3:realm3@primates.ximian.com/~danw/soup-test/Basic/realm3/index.txt",
	  "03", TRUE },


	{ "No auth available, should fail",
	  "http://primates.ximian.com/~danw/soup-test/Digest/realm1/index.txt",
	  "0", FALSE },

	{ "Should fail with no auth, fail again with bad password, and give up",
	  "http://user4:realm4@primates.ximian.com/~danw/soup-test/Digest/realm2/index.txt",
	  "04", FALSE },

	{ "Known realm, auth provided, so should succeed immediately",
	  "http://user1:realm1@primates.ximian.com/~danw/soup-test/Digest/realm1/index.txt",
	  "1", TRUE },

	{ "Now should automatically reuse previous auth",
	  "http://primates.ximian.com/~danw/soup-test/Digest/realm1/index.txt",
	  "1", TRUE },

	{ "Subdir should also automatically reuse auth",
	  "http://primates.ximian.com/~danw/soup-test/Digest/realm1/subdir/index.txt",
	  "1", TRUE },

	{ "Subdir should retry last auth, but will fail this time",
	  "http://primates.ximian.com/~danw/soup-test/Digest/realm1/realm2/index.txt",
	  "1", FALSE },

	{ "Now should use provided auth on first try",
	  "http://user2:realm2@primates.ximian.com/~danw/soup-test/Digest/realm1/realm2/index.txt",
	  "2", TRUE },

	{ "Reusing last auth. Should succeed on first try",
	  "http://primates.ximian.com/~danw/soup-test/Digest/realm1/realm2/index.txt",
	  "2", TRUE },

	{ "Should succeed on first try because of earlier domain directive",
	  "http://primates.ximian.com/~danw/soup-test/Digest/realm1/realm2/realm1/index.txt",
	  "1", TRUE },

	{ "Should succeed on first try. (Known realm with cached password)",
	  "http://primates.ximian.com/~danw/soup-test/Digest/realm2/index.txt",
	  "2", TRUE },

	{ "Fail once, then use password",
	  "http://user3:realm3@primates.ximian.com/~danw/soup-test/Digest/realm3/index.txt",
	  "03", TRUE },


	{ "Make sure we haven't forgotten anything",
	  "http://primates.ximian.com/~danw/soup-test/Basic/realm1/index.txt",
	  "1", TRUE },

	{ "Make sure we haven't forgotten anything",
	  "http://primates.ximian.com/~danw/soup-test/Basic/realm1/realm2/index.txt",
	  "2", TRUE },

	{ "Make sure we haven't forgotten anything",
	  "http://primates.ximian.com/~danw/soup-test/Basic/realm1/realm2/realm1/index.txt",
	  "1", TRUE },

	{ "Make sure we haven't forgotten anything",
	  "http://primates.ximian.com/~danw/soup-test/Basic/realm2/index.txt",
	  "2", TRUE },

	{ "Make sure we haven't forgotten anything",
	  "http://primates.ximian.com/~danw/soup-test/Basic/realm3/index.txt",
	  "3", TRUE },


	{ "Make sure we haven't forgotten anything",
	  "http://primates.ximian.com/~danw/soup-test/Digest/realm1/index.txt",
	  "1", TRUE },

	{ "Make sure we haven't forgotten anything",
	  "http://primates.ximian.com/~danw/soup-test/Digest/realm1/realm2/index.txt",
	  "2", TRUE },

	{ "Make sure we haven't forgotten anything",
	  "http://primates.ximian.com/~danw/soup-test/Digest/realm1/realm2/realm1/index.txt",
	  "1", TRUE },

	{ "Make sure we haven't forgotten anything",
	  "http://primates.ximian.com/~danw/soup-test/Digest/realm2/index.txt",
	  "2", TRUE },

	{ "Make sure we haven't forgotten anything",
	  "http://primates.ximian.com/~danw/soup-test/Digest/realm3/index.txt",
	  "3", TRUE }
};
int ntests = sizeof (tests) / sizeof (tests[0]);

static const char *auths[] = {
	"no password", "password 1",
	"password 2", "password 3",
	"intentionally wrong password",
};

static int
identify_auth (SoupMessage *msg)
{
	const char *header;
	int num;

	header = soup_message_get_header (msg->request_headers,
					  "Authorization");
	if (!header)
		return 0;

	if (!g_ascii_strncasecmp (header, "Basic ", 6)) {
		char *token;
		int len;

		token = soup_base64_decode (header + 6, &len);
		num = token[len - 1] - '0';
		g_free (token);
	} else {
		const char *user;

		user = strstr (header, "username=\"user");
		if (user)
			num = user[14] - '0';
		else
			num = 0;
	}

	return num;
}

static void
handler (SoupMessage *msg, gpointer data)
{
	char *expected = data;
	int auth, exp;

	auth = identify_auth (msg);

	printf ("  %d %s (using %s)\n", msg->status_code, msg->reason_phrase,
		auths[auth]);

	if (*expected) {
		exp = *expected - '0';
		if (auth != exp) {
			printf ("    expected %s!\n", auths[exp]);
			errors++;
		}
		memmove (expected, expected + 1, strlen (expected));
	} else {
		printf ("    expected to be finished\n");
		errors++;
	}
}

int
main (int argc, char **argv)
{
	SoupSession *session;
	SoupMessage *msg;
	char *expected;
	int i;

	g_type_init ();
	session = soup_session_new_default ();

	for (i = 0; i < ntests; i++) {
		printf ("Test %d: %s\n", i + 1, tests[i].explanation);

		printf ("  GET %s\n", tests[i].url);

		msg = soup_message_new (SOUP_METHOD_GET, tests[i].url);
		if (!msg) {
			fprintf (stderr, "auth-test: Could not parse URI\n");
			exit (1);
		}

		expected = g_strdup (tests[i].expected);
		soup_message_add_status_code_handler (
			msg, SOUP_STATUS_UNAUTHORIZED,
			SOUP_HANDLER_PRE_BODY, handler, expected);
		soup_message_add_status_code_handler (
			msg, SOUP_STATUS_OK, SOUP_HANDLER_PRE_BODY,
			handler, expected);
		soup_session_send_message (session, msg);
		if (msg->status_code != SOUP_STATUS_UNAUTHORIZED &&
		    msg->status_code != SOUP_STATUS_OK) {
			printf ("  %d %s !\n", msg->status_code,
				msg->reason_phrase);
		}
		if (*expected) {
			printf ("  expected %d more round(s)\n",
				strlen (expected));
			errors++;
		}
		g_free (expected);

		if (SOUP_STATUS_IS_SUCCESSFUL (msg->status_code) !=
		    tests[i].success) {
			printf ("  expected %s\n",
				tests[i].success ? "success" : "failure");
			errors++;
		}

		printf ("\n");

		g_object_unref (msg);
	}

	g_object_unref (session);

	printf ("\nauth-test: %d errors\n", errors);
	return errors;
}
