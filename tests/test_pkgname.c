/* Unit tests for the package-name injection guard and blocklist. */
#include "pkgname.h"
#include "test.h"

static void test_valid_accepts(void) {
    CHECK(pkg_valid("com.foo.bar"));
    CHECK(pkg_valid("com.foo_bar.v2"));
    CHECK(pkg_valid("a"));
    CHECK(pkg_valid("A.B.C_1.9"));
    CHECK(pkg_valid("org.chromium.chrome"));
}

static void test_valid_rejects(void) {
    CHECK(!pkg_valid(NULL));
    CHECK(!pkg_valid(""));
    CHECK(!pkg_valid("com.foo;rm -rf /")); /* command separator */
    CHECK(!pkg_valid("com.foo bar"));      /* space */
    CHECK(!pkg_valid("com.foo$(id)"));     /* command substitution */
    CHECK(!pkg_valid("com.foo`id`"));      /* backticks */
    CHECK(!pkg_valid("com.foo|bar"));      /* pipe */
    CHECK(!pkg_valid("com.foo&bar"));      /* background */
    CHECK(!pkg_valid("com.foo>out"));      /* redirect */
    CHECK(!pkg_valid("com.foo\nbar"));     /* newline */
    CHECK(!pkg_valid("com.foo'bar"));      /* quote */
    CHECK(!pkg_valid("com.foo\"bar"));     /* dquote */
    CHECK(!pkg_valid("com/foo"));          /* slash */
    CHECK(!pkg_valid("com-foo"));          /* hyphen is not valid in an app id */
    CHECK(!pkg_valid("café.app"));         /* non-ASCII */
}

static void test_blocked(void) {
    CHECK(pkg_blocked("android"));
    CHECK(pkg_blocked("com.android.systemui"));
    CHECK(pkg_blocked("com.android.settings"));
    CHECK(pkg_blocked("com.android.launcher3"));
    CHECK(pkg_blocked("com.google.android.apps.nexuslauncher"));
    CHECK(pkg_blocked("com.google.android.gms"));
    CHECK(pkg_blocked("com.android.phone"));
    CHECK(pkg_blocked("com.android.shell"));
    /* root-manager apps are NOT hard-blocked anymore: hiding them is a legitimate
       anti-detection feature, gated by an explicit confirm in the WebUI instead */
    CHECK(!pkg_blocked("com.topjohnwu.magisk"));
    CHECK(!pkg_blocked("me.weishu.kernelsu"));
    CHECK(!pkg_blocked("me.bmax.apatch"));
    /* ordinary third-party apps are not blocked */
    CHECK(!pkg_blocked("com.foo.bar"));
    CHECK(!pkg_blocked("io.github.a13e300.ksuwebui")); /* WebUI host stays lockable */
    CHECK(!pkg_blocked("com.android"));                /* prefix, not an exact match */
    CHECK(!pkg_blocked("com.android.systemui2"));
    CHECK(!pkg_blocked(""));
}

int main(void) {
    RUN(test_valid_accepts);
    RUN(test_valid_rejects);
    RUN(test_blocked);
    TEST_SUMMARY();
    return TEST_EXIT();
}
