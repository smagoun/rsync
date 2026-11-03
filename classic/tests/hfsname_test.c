/*
 * Unit tests for openrsync/hfsname.c (UTF-8 <-> HFS names).
 * cc -I../../openrsync hfsname_test.c ../../openrsync/hfsname.c
 */
#include <stdio.h>
#include <string.h>

#include "hfsname.h"

static int fails;

#define CHECK(what, cond) do {						\
	if (cond)							\
		printf("ok   - %s\n", what);				\
	else {								\
		printf("FAIL - %s\n", what);				\
		fails++;						\
	}								\
} while (0)

static int
is(const unsigned char *p, const char *mr)
{
	return p[0] == strlen(mr) && memcmp(p + 1, mr, p[0]) == 0;
}

static unsigned char *
P(const char *s)
{
	static unsigned char buf[8][HFS_NAMEMAX + 2];
	static int k;
	unsigned char *b = buf[k++ & 7];

	b[0] = (unsigned char)strlen(s);
	memcpy(b + 1, s, b[0]);
	return b;
}

int
main(void)
{
	unsigned char h[HFS_NAMEMAX + 1], h2[HFS_NAMEMAX + 1];
	char u[256];
	int rc;

	CHECK("ASCII unchanged", hfs_encode_name("main.c", h) == 0 &&
	    is(h, "main.c"));
	CHECK("trade mark sign -> 0xAA",
	    hfs_encode_name("Civilization\xe2\x84\xa2 1.1.2", h) == 0 &&
	    is(h, "Civilization\xaa 1.1.2"));
	CHECK("decomposed accent composed (e + U+0301 -> 0x8E)",
	    hfs_encode_name("Cafe\xcc\x81.c", h) == 0 && is(h, "Caf\x8e.c"));
	CHECK("precomposed accent", hfs_encode_name("Caf\xc3\xa9.c", h) == 0 &&
	    is(h, "Caf\x8e.c"));
	CHECK("colon becomes slash", hfs_encode_name("a:b", h) == 0 &&
	    is(h, "a/b"));
	hfs_decode_name((const unsigned char *)"a/b", 3, u, sizeof(u));
	CHECK("slash decodes to colon", strcmp(u, "a:b") == 0);
	hfs_decode_name((const unsigned char *)"Caf\x8e \xaa", 6, u, sizeof(u));
	CHECK("MacRoman decodes to UTF-8",
	    strcmp(u, "Caf\xc3\xa9 \xe2\x84\xa2") == 0);
	CHECK("31-byte name kept",
	    hfs_encode_name("abcdefghijklmnopqrstuvwxyz01234", h) == 0 &&
	    h[0] == 31);

	rc = hfs_encode_name("ThisIsAVeryLongSourceFileName_Helpers.c", h);
	printf("     long name -> %.*s\n", h[0], h + 1);
	CHECK("long name mangled to 31 bytes, extension kept", rc == 1 &&
	    h[0] == 31 && memcmp(h + 30, ".c", 2) == 0 &&
	    h[25] == '#' && memcmp(h + 1, "ThisIsAVeryLongSourceF", 22) == 0);
	hfs_encode_name("ThisIsAVeryLongSourceFileName_Helpers.c", h2);
	CHECK("mangling is deterministic", memcmp(h, h2, h[0] + 1) == 0);
	hfs_encode_name("ThisIsAVeryLongSourceFileName_Helpers.h", h2);
	CHECK("different names mangle differently",
	    memcmp(h, h2, h[0] + 1) != 0);

	rc = hfs_encode_name("\xe6\x97\xa5\xe6\x9c\xac.txt", h);
	printf("     CJK name -> %.*s\n", h[0], h + 1);
	CHECK("unrepresentable characters mangled, .txt kept", rc == 1 &&
	    h[0] == 11 && memcmp(h + 1, "__#", 3) == 0 &&
	    memcmp(h + 8, ".txt", 4) == 0);
	rc = hfs_encode_name("averyveryveryveryveryverylongname.longext", h);
	CHECK("over-long extension not kept", rc == 1 && h[0] == 31 &&
	    h[27] == '#');

	CHECK("case-insensitive compare", hfs_name_equal(P("Foo.C"), P("foo.c")));
	CHECK("accented case fold (0x83 E-acute vs 0x8E e-acute)",
	    hfs_name_equal(P("\x83t\x8e"), P("\x8et\x83")));
	CHECK("diacritic-sensitive", !hfs_name_equal(P("e"), P("\x8e")));

	{
		const char *list[] = {
			"Foo.c", "foo.c", "src", "src/Foo.c",
			"src/Cafe\xcc\x81.c",
			"src/ThisIsAVeryLongSourceFileName_Helpers.c",
		};
		hfs_names_register(list, 6);
	}
	hfs_names_forward("Foo.c", h);
	CHECK("first of a case collision keeps its name", is(h, "Foo.c"));
	hfs_names_forward("foo.c", h);
	printf("     collision -> %.*s\n", h[0], h + 1);
	CHECK("second of a case collision is mangled",
	    h[0] == 10 && memcmp(h + 1, "foo#", 4) == 0);
	hfs_names_reverse("", h, u, sizeof(u));
	CHECK("mangled collision maps back", strcmp(u, "foo.c") == 0);
	hfs_names_reverse("", P("FOO.C"), u, sizeof(u));
	CHECK("case variant maps back to registered spelling",
	    strcmp(u, "Foo.c") == 0);
	hfs_names_forward("src/Foo.c", h);
	CHECK("same name in another folder is not a collision",
	    is(h, "Foo.c"));
	hfs_names_reverse("src", P("Caf\x8e.c"), u, sizeof(u));
	CHECK("reverse returns the server's decomposed spelling",
	    strcmp(u, "Cafe\xcc\x81.c") == 0);
	hfs_names_forward("src/ThisIsAVeryLongSourceFileName_Helpers.c", h);
	hfs_names_reverse("src", h, u, sizeof(u));
	CHECK("long mangled name maps back",
	    strcmp(u, "ThisIsAVeryLongSourceFileName_Helpers.c") == 0);
	hfs_names_reverse("src", P("Stale#1234.c"), u, sizeof(u));
	CHECK("unknown local name decodes as-is (so --delete sees it)",
	    strcmp(u, "Stale#1234.c") == 0);
	hfs_names_forward("src/new.c", h);
	CHECK("unregistered name uses the plain rule", is(h, "new.c"));
	hfs_names_reset();

	if (fails)
		printf("%d test(s) failed\n", fails);
	else
		printf("all tests passed\n");
	return fails != 0;
}
