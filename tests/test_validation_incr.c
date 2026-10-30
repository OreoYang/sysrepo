/**
 * @file test_validation_incr.c
 * @brief end to end test of edits over augmented modules, meant to be run with the incremental validator on (the
 * default) and off (cmake -DENABLE_VALIDATE_INCREMENTAL=OFF) so that both validators must give the same answers
 *
 * Module incr-b augments incr-a and carries a rule that reads incr-a, incr-c reads both, so an edit of one module
 * can be rejected by a rule of another one.
 *
 *  * Copyright (c) 2026 Vecima Networks Inc.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE

#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>
#include <libyang/libyang.h>

#include "sysrepo.h"
#include "tests/tcommon.h"

struct state {
    sr_conn_ctx_t *conn;
    sr_session_ctx_t *sess;
};

static int
setup_f(void **state)
{
    struct state *st;
    const char *schema_paths[] = {
        TESTS_SRC_DIR "/files/incr-a.yang",
        TESTS_SRC_DIR "/files/incr-b.yang",
        TESTS_SRC_DIR "/files/incr-c.yang",
        TESTS_SRC_DIR "/files/dl.yang",
        TESTS_SRC_DIR "/files/dz.yang",
        TESTS_SRC_DIR "/files/dm.yang",
        NULL
    };

    st = calloc(1, sizeof *st);
    *state = st;

    if (sr_connect(0, &st->conn) != SR_ERR_OK) {
        return 1;
    }
    if (sr_install_modules(st->conn, schema_paths, TESTS_SRC_DIR "/files", NULL) != SR_ERR_OK) {
        return 1;
    }
    if (sr_session_start(st->conn, SR_DS_RUNNING, &st->sess) != SR_ERR_OK) {
        return 1;
    }

    return 0;
}

static int
teardown_f(void **state)
{
    struct state *st = (struct state *)*state;
    const char *module_names[] = {"dz", "dm", "dl", "incr-c", "incr-b", "incr-a", NULL};

    sr_remove_modules(st->conn, module_names, 0);
    sr_disconnect(st->conn);
    free(st);
    return 0;
}

static int
clear_data(void **state)
{
    struct state *st = (struct state *)*state;

    sr_discard_changes(st->sess);
    sr_delete_item(st->sess, "/incr-c:c-refs", 0);
    sr_delete_item(st->sess, "/incr-b:b-top", 0);
    sr_delete_item(st->sess, "/incr-a:ifs", 0);
    sr_apply_changes(st->sess, 0);

    return 0;
}

static void
set_item(struct state *st, const char *path, const char *value)
{
    assert_int_equal(SR_ERR_OK, sr_set_item_str(st->sess, path, value, NULL, 0));
}

/**
 * @brief Whether @p path exists in the running datastore, the pending edit is not looked at.
 */
static int
exists(struct state *st, const char *path)
{
    sr_session_ctx_t *sess;
    sr_val_t *val = NULL;
    int ret;

    assert_int_equal(SR_ERR_OK, sr_session_start(st->conn, SR_DS_RUNNING, &sess));
    ret = sr_get_item(sess, path, 0, &val);
    if (ret == SR_ERR_OK) {
        sr_free_val(val);
    } else {
        assert_int_equal(SR_ERR_NOT_FOUND, ret);
    }
    sr_session_stop(sess);

    return ret == SR_ERR_OK;
}

/**
 * @brief Valid starting data: e1 is an 'eth' with an explicit b-cfg and one address, x1 is a plain 'eth' read by the
 * rule of b-chk, and c-ref r1 reads the speed and the address of e1.
 */
static void
load_base(struct state *st)
{
    set_item(st, "/incr-a:ifs/if[name='e1']/type", "incr-a:eth");
    set_item(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/speed", "1000");
    set_item(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/mtu", "1500");
    set_item(st, "/incr-a:ifs/if[name='e1']/incr-b:b-addr[id='1']/ip", "10.0.0.1");
    set_item(st, "/incr-a:ifs/if[name='x1']/type", "incr-a:eth");
    set_item(st, "/incr-b:b-top/b-chk[name='k2']/ifname", "x1");
    set_item(st, "/incr-b:b-top/b-chk[name='k2']/need", "y");
    set_item(st, "/incr-c:c-refs/c-ref[name='r1']/ifname", "e1");
    set_item(st, "/incr-c:c-refs/c-ref[name='r1']/addr-id", "1");
    set_item(st, "/incr-c:c-refs/c-ref[name='r1']/fast", "y");
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));
}

/**
 * @brief Apply the pending edit and require it to be rejected without touching the datastore.
 */
static void
apply_rejected(struct state *st)
{
    assert_int_equal(SR_ERR_VALIDATION_FAILED, sr_apply_changes(st->sess, 0));
    assert_int_equal(SR_ERR_OK, sr_discard_changes(st->sess));
}

static void
test_add(void **state)
{
    struct state *st = (struct state *)*state;

    load_base(st);

    /* a valid augmented list entry and a valid new target instance */
    set_item(st, "/incr-a:ifs/if[name='e1']/incr-b:b-addr[id='2']/ip", "10.0.0.2");
    set_item(st, "/incr-a:ifs/if[name='e2']/type", "incr-a:eth");
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));
    assert_true(exists(st, "/incr-a:ifs/if[name='e1']/incr-b:b-addr[id='2']/ip"));
    assert_true(exists(st, "/incr-a:ifs/if[name='e2']/type"));

    /* a duplicate ip in the same list breaks the unique */
    set_item(st, "/incr-a:ifs/if[name='e1']/incr-b:b-addr[id='3']/ip", "10.0.0.1");
    apply_rejected(st);
    assert_false(exists(st, "/incr-a:ifs/if[name='e1']/incr-b:b-addr[id='3']/ip"));

    /* the same ip under another interface does not */
    set_item(st, "/incr-a:ifs/if[name='x1']/incr-b:b-addr[id='1']/ip", "10.0.0.1");
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));

    /* the must of the augment reads the sibling speed, from both sides, and the must of c-ref reads it as well */
    set_item(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/mtu", "500");
    apply_rejected(st);
    set_item(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/speed", "2000");
    apply_rejected(st);
    set_item(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/speed", "900");
    apply_rejected(st);
    set_item(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/speed", "1200");
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));
    assert_true(exists(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/speed"));
}

static void
test_delete(void **state)
{
    struct state *st = (struct state *)*state;

    load_base(st);

    /* the referenced address and the referenced interface cannot go */
    assert_int_equal(SR_ERR_OK, sr_delete_item(st->sess, "/incr-a:ifs/if[name='e1']/incr-b:b-addr[id='1']", 0));
    apply_rejected(st);
    assert_true(exists(st, "/incr-a:ifs/if[name='e1']/incr-b:b-addr[id='1']/ip"));

    assert_int_equal(SR_ERR_OK, sr_delete_item(st->sess, "/incr-a:ifs/if[name='e1']", 0));
    apply_rejected(st);
    assert_true(exists(st, "/incr-a:ifs/if[name='e1']/type"));

    /* the rule of b-chk only needs x1 to exist and to be an 'eth' */
    assert_int_equal(SR_ERR_OK, sr_delete_item(st->sess, "/incr-a:ifs/if[name='x1']", 0));
    apply_rejected(st);
    assert_true(exists(st, "/incr-a:ifs/if[name='x1']/type"));

    /* the explicit container goes and a default one comes back, which is too slow for c-ref/fast */
    assert_int_equal(SR_ERR_OK, sr_delete_item(st->sess, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg", 0));
    apply_rejected(st);
    assert_true(exists(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/speed"));

    /* with the referrer deleted in the same edit nothing dangles any more */
    assert_int_equal(SR_ERR_OK, sr_delete_item(st->sess, "/incr-a:ifs/if[name='x1']", 0));
    assert_int_equal(SR_ERR_OK, sr_delete_item(st->sess, "/incr-b:b-top/b-chk[name='k2']", 0));
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));
    assert_false(exists(st, "/incr-a:ifs/if[name='x1']/type"));
    assert_false(exists(st, "/incr-b:b-top/b-chk[name='k2']/ifname"));
    assert_true(exists(st, "/incr-a:ifs/if[name='e1']/type"));
    assert_true(exists(st, "/incr-a:ifs/if[name='e1']/incr-b:b-addr[id='1']/ip"));
    assert_true(exists(st, "/incr-c:c-refs/c-ref[name='r1']/fast"));
}

static void
test_cross_module(void **state)
{
    struct state *st = (struct state *)*state;

    load_base(st);

    /* only incr-a is edited, the rule that breaks is in incr-b */
    set_item(st, "/incr-a:ifs/if[name='x1']/type", "incr-a:ani");
    apply_rejected(st);
    assert_true(exists(st, "/incr-b:b-top/b-chk[name='k2']/need"));

    /* same flip once the rule of b-chk is gone: the b-cfg that is only a default simply disappears */
    assert_int_equal(SR_ERR_OK, sr_delete_item(st->sess, "/incr-b:b-top/b-chk[name='k2']", 0));
    set_item(st, "/incr-a:ifs/if[name='x1']/type", "incr-a:ani");
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));

    /* only incr-b and incr-c are edited, they read incr-a */
    set_item(st, "/incr-c:c-refs/c-ref[name='r2']/ifname", "x1");
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));
    set_item(st, "/incr-c:c-refs/c-ref[name='r1']/addr-id", "9");
    apply_rejected(st);
}

/**
 * @brief Flipping the type removes the explicit b-cfg through its when, the edit does not mention it, and
 * c-ref/fast reads its speed. Full validation rejects the edit.
 *
 * The same scenario at libyang level (test_aug_when_cascade in test_validation_incr.c there) is accepted by the
 * incremental validator, here it is rejected in both modes, so this stays as a guard for the sysrepo flow.
 */
static void
test_when_cascade(void **state)
{
    struct state *st = (struct state *)*state;

    load_base(st);
    assert_int_equal(SR_ERR_OK, sr_delete_item(st->sess, "/incr-b:b-top/b-chk[name='k2']", 0));
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));

    set_item(st, "/incr-a:ifs/if[name='e1']/type", "incr-a:ani");
    apply_rejected(st);
    assert_true(exists(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/speed"));
}

/**
 * @brief The same cascade caught by a leafref: x1 points at the speed of e1, the flip of e1 removes it.
 *
 * Leafrefs are resolved in the first validation pass, before sysrepo merges the validation diff, so only the
 * fallback of the incremental validator on deletions made by the validation itself catches this.
 */
static void
test_when_cascade_leafref(void **state)
{
    struct state *st = (struct state *)*state;

    load_base(st);
    assert_int_equal(SR_ERR_OK, sr_delete_item(st->sess, "/incr-c:c-refs/c-ref[name='r1']", 0));
    set_item(st, "/incr-a:ifs/if[name='x1']/incr-b:peer-speed", "1000");
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));

    set_item(st, "/incr-a:ifs/if[name='e1']/type", "incr-a:ani");
    apply_rejected(st);
    assert_true(exists(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/speed"));
    assert_true(exists(st, "/incr-a:ifs/if[name='x1']/incr-b:peer-speed"));
}

/**
 * @brief Creating the m2 case replaces the default m1 case, and c-ref/m1need reads m1-val.
 *
 * On x1 the whole b-cfg only exists as a default. The libyang-level test (test_aug_dflt_case_in_dflt) shows the
 * incremental validator accepting this when the edit diff calls that b-cfg created; the sysrepo edit diff matches the
 * existing default container instead, so here both modes reject. On e1 b-cfg is explicit.
 */
static void
test_dflt_case_replace(void **state)
{
    struct state *st = (struct state *)*state;

    load_base(st);
    set_item(st, "/incr-c:c-refs/c-ref[name='r4']/ifname", "x1");
    set_item(st, "/incr-c:c-refs/c-ref[name='r4']/m1need", "y");
    set_item(st, "/incr-c:c-refs/c-ref[name='r5']/ifname", "e1");
    set_item(st, "/incr-c:c-refs/c-ref[name='r5']/m1need", "y");
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));

    set_item(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/m2-val", "5");
    apply_rejected(st);
    assert_false(exists(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/m2-val"));

    set_item(st, "/incr-a:ifs/if[name='x1']/incr-b:b-cfg/m2-val", "5");
    apply_rejected(st);
    assert_false(exists(st, "/incr-a:ifs/if[name='x1']/incr-b:b-cfg/m2-val"));
}

/*
 * Delete matrix. The same cases as the libyang test test_del_cases, which has the full validation as its oracle: every
 * reader a deletion can break (leafref, must, when, in this and in another module), every rule of the structure that
 * stays (min-elements, mandatory, choice, defaults, emptied containers), and edits that delete several things or
 * delete and create at once. Each case only needs the verdict, the same one in both validators, and a rejected edit
 * has to leave the datastore as it was.
 */

#define DL_T(x) "<top xmlns=\"urn:tests:dl\">" x "</top>"
#define DL_C(c, x) "<" c " xmlns=\"urn:tests:dl\">" x "</" c ">"
#define DL_I(n, v) "<item><name>" n "</name><val>" v "</val></item>"
#define DL_R(n, x) "<r><name>" n "</name>" x "</r>"
#define DL_GATE "<item><name>gate</name></item>"
#define DM_M(x) "<mtop xmlns=\"urn:tests:dm\"><mr><name>m1</name>" x "</mr></mtop>"
#define DZ_Z "<zt xmlns=\"urn:tests:dz\"><zr><name>z</name></zr></zt>"
#define DL_G1 "<grp><name>g1</name><want>w1</want><other>g2</other>"
#define DL_LONE DL_C("lone", "<li><name>l1</name></li>")


/**
 * @brief One delete edit with the verdict it must get.
 *
 * @p edits is in the format of aug_apply(): a path starting with '-' deletes it, any other path is set to the
 * value next to it, and a NULL path ends the list.
 */
struct dl_case {
    const char *name;
    const char *xml;
    const char *edits[9];
    LY_ERR expected;
};

#define DL_P "/dl:top/"
#define E_BAD LY_EVALID
#define E_OK LY_SUCCESS

static const struct dl_case dl_cases[] = {
    /* leafrefs: the one the deleted instance is the target of, and one it is not */
    {"leafref target", DL_T(DL_I("i1", "a") DL_I("i2", "b") DL_R("r", "<lref>i1</lref>")),
        {"-" DL_P "item[name='i1']"}, E_BAD},
    {"leafref other instance", DL_T(DL_I("i1", "a") DL_I("i2", "b") DL_R("r", "<lref>i1</lref>")),
        {"-" DL_P "item[name='i2']"}, E_OK},
    {"leafref to a leaf-list instance", DL_T("<tag>t1</tag><tag>t2</tag>" DL_R("r", "<tref>t1</tref>")),
        {"-" DL_P "tag[.='t1']"}, E_BAD},
    {"leafref to another leaf-list instance", DL_T("<tag>t1</tag><tag>t2</tag>" DL_R("r", "<tref>t1</tref>")),
        {"-" DL_P "tag[.='t2']"}, E_OK},
    {"union leafref member", DL_T(DL_I("i1", "a") DL_I("i2", "b") DL_R("r", "<uref>i1</uref>")),
        {"-" DL_P "item[name='i1']"}, E_BAD},
    {"union enum member", DL_T(DL_I("i1", "a") DL_I("i2", "b") DL_R("r", "<uref>none</uref>")),
        {"-" DL_P "item[name='i1']"}, E_OK},
    {"key leafref", DL_T(DL_I("i1", "a") DL_I("i2", "b") "<kr><k>i1</k></kr>"),
        {"-" DL_P "item[name='i1']"}, E_BAD},
    {"leaf-list of leafrefs, one target", DL_T(DL_I("i1", "a") DL_I("i2", "b") DL_R("r", "<lrefs>i1</lrefs><lrefs>i2</lrefs>")),
        {"-" DL_P "item[name='i2']"}, E_BAD},
    {"leaf-list of leafrefs, not a target", DL_T(DL_I("i1", "a") DL_I("i2", "b") DL_I("i3", "c")
        DL_R("r", "<lrefs>i1</lrefs><lrefs>i2</lrefs>")), {"-" DL_P "item[name='i3']"}, E_OK},
    {"leafref picked by current(), the instance", DL_T(DL_I("i1", "a") DL_I("i2", "good")
        DL_R("r", "<sel>i2</sel><dref>good</dref>")), {"-" DL_P "item[name='i2']"}, E_BAD},
    {"leafref picked by current(), the target leaf", DL_T(DL_I("i1", "a") DL_I("i2", "good")
        DL_R("r", "<sel>i2</sel><dref>good</dref>")), {"-" DL_P "item[name='i2']/val"}, E_BAD},
    {"leafref picked by current(), another instance", DL_T(DL_I("i1", "a") DL_I("i2", "good")
        DL_R("r", "<sel>i2</sel><dref>good</dref>")), {"-" DL_P "item[name='i1']"}, E_OK},

    /* must: existence, value, counts, the first node, positions */
    {"must needs the instance", DL_T(DL_I("i1", "a") DL_I("i3", "c") DL_R("r", "<sel>i3</sel><m-exist>y</m-exist>")),
        {"-" DL_P "item[name='i3']"}, E_BAD},
    {"must needs the instance, another one goes", DL_T(DL_I("i1", "a") DL_I("i3", "c")
        DL_R("r", "<sel>i3</sel><m-exist>y</m-exist>")), {"-" DL_P "item[name='i1']"}, E_OK},
    {"must compares the value", DL_T(DL_I("i2", "good") DL_R("r", "<sel>i2</sel><m-val>y</m-val>") "<tag>t</tag>"),
        {"-" DL_P "item[name='i2']"}, E_BAD},
    {"count falls below the bound", DL_T(DL_I("i1", "a") DL_I("i2", "b") DL_R("r", "<m-count>y</m-count>")),
        {"-" DL_P "item[name='i1']"}, E_BAD},
    {"count stays above the bound", DL_T(DL_I("i1", "a") DL_I("i2", "b") DL_I("i3", "c") DL_R("r", "<m-count>y</m-count>")),
        {"-" DL_P "item[name='i1']"}, E_OK},
    {"sum falls below the bound", DL_T("<num>3</num><num>4</num>" DL_R("r", "<m-sum>y</m-sum>")),
        {"-" DL_P "num[.='3']"}, E_BAD},
    {"sum stays above the bound", DL_T("<num>3</num><num>4</num><num>5</num>" DL_R("r", "<m-sum>y</m-sum>")),
        {"-" DL_P "num[.='5']"}, E_OK},
    {"first node changes the string value", DL_T(DL_I("i1", "k1") DL_I("i2", "zz") DL_R("r", "<m-first>y</m-first>")),
        {"-" DL_P "item[name='i1']"}, E_BAD},
    {"first node changes but the value holds", DL_T(DL_I("i1", "k1") DL_I("i2", "k2") DL_R("r", "<m-first>y</m-first>")),
        {"-" DL_P "item[name='i1']"}, E_OK},
    {"a later node goes, the first one stays", DL_T(DL_I("i1", "k1") DL_I("i2", "zz") DL_R("r", "<m-first>y</m-first>")),
        {"-" DL_P "item[name='i2']"}, E_OK},
    {"first of an ordered-by-user list goes", DL_T("<ou><name>first</name></ou><ou><name>mid</name></ou>"
        "<ou><name>tail</name></ou>" DL_R("r", "<m-ou1>y</m-ou1>")), {"-" DL_P "ou[name='first']"}, E_BAD},
    {"middle of an ordered-by-user list goes", DL_T("<ou><name>first</name></ou><ou><name>mid</name></ou>"
        "<ou><name>tail</name></ou>" DL_R("r", "<m-ou1>y</m-ou1>")), {"-" DL_P "ou[name='mid']"}, E_OK},
    {"last of an ordered-by-user list goes", DL_T("<ou><name>first</name></ou><ou><name>mid</name></ou>"
        "<ou><name>tail</name></ou>" DL_R("r", "<m-oul>y</m-oul>")), {"-" DL_P "ou[name='tail']"}, E_BAD},
    {"middle goes, the last one stays", DL_T("<ou><name>first</name></ou><ou><name>mid</name></ou>"
        "<ou><name>tail</name></ou>" DL_R("r", "<m-oul>y</m-oul>")), {"-" DL_P "ou[name='mid']"}, E_OK},

    /* when: becoming false takes the node away, becoming true brings a default in */
    {"when turns true to false", DL_T(DL_GATE DL_R("r", "<w-on>v1</w-on>")), {"-" DL_P "item[name='gate']"}, E_OK},
    {"when turns false, a leafref reads the dropped node", DL_T(DL_GATE DL_R("r", "<w-on>v1</w-on>")
        "<wr><name>w1</name><wref>v1</wref></wr>"), {"-" DL_P "item[name='gate']"}, E_BAD},
    {"when turns false to true, a default appears", DL_T(DL_GATE DL_R("r", "<sel>x</sel>")) DZ_Z,
        {"-" DL_P "item[name='gate']"}, E_OK},
    {"default node whose when reads the deleted instance", DL_T(DL_GATE DL_R("r", "<sel>x</sel>")) DZ_Z,
        {"-" DL_P "item[name='gate']"}, E_OK},
    {"default node whose when reads a node the validation drops", DL_T(DL_GATE DL_R("r", "<w-on>v1</w-on>")) DZ_Z,
        {"-" DL_P "item[name='gate']"}, E_OK},
    {"when of a case turns false", DL_T(DL_GATE "<wc-v>cv</wc-v>") DL_LONE, {"-" DL_P "item[name='gate']"}, E_OK},
    {"when of a case turns false, a leafref reads it", DL_T(DL_GATE "<wc-v>cv</wc-v><wcr>cv</wcr>"),
        {"-" DL_P "item[name='gate']"}, E_BAD},

    /* the reader is in another module, or the data is */
    {"other module leafref", DL_T(DL_I("i1", "a") DL_I("i2", "b")) DM_M("<iref>i1</iref>"),
        {"-" DL_P "item[name='i1']"}, E_BAD},
    {"other module leafref, another instance", DL_T(DL_I("i1", "a") DL_I("i2", "b")) DM_M("<iref>i1</iref>"),
        {"-" DL_P "item[name='i2']"}, E_OK},
    {"other module leafref to a top-level list", DL_LONE DL_T(DL_I("i1", "a")) DM_M("<lone-ref>l1</lone-ref>"),
        {"-/dl:lone/li[name='l1']"}, E_BAD},
    {"other module leafref to a whole top-level container", DL_LONE DL_T(DL_I("i1", "a")) DM_M("<lone-ref>l1</lone-ref>"),
        {"-/dl:lone"}, E_BAD},
    {"a top-level container nobody reads", DL_LONE DL_T(DL_I("i1", "a")), {"-/dl:lone"}, E_OK},
    {"other module count", DL_T("<tag>t1</tag>") DM_M("<mcnt>y</mcnt>"), {"-" DL_P "tag[.='t1']"}, E_BAD},
    {"other module count, one left", DL_T("<tag>t1</tag><tag>t2</tag>") DM_M("<mcnt>y</mcnt>"),
        {"-" DL_P "tag[.='t1']"}, E_OK},
    {"augmented leaf reads the deleted instance", DL_T("<tag>t1</tag><tag>t2</tag><item><name>i1</name>"
        "<aug-ref xmlns=\"urn:tests:dm\">t1</aug-ref></item>"), {"-" DL_P "tag[.='t1']"}, E_BAD},
    {"augmented list instance is the one deleted", DL_T("<tag>t1</tag><item><name>i1</name>"
        "<aug-ref xmlns=\"urn:tests:dm\">t1</aug-ref></item>" DL_I("i2", "b")), {"-" DL_P "item[name='i1']"}, E_OK},
    {"a whole top-level container read from outside", DL_LONE DL_T(DL_I("i1", "a")) DM_M("<iref>i1</iref>"),
        {"-/dl:top"}, E_BAD},
    {"the reader of that container goes with it", DL_LONE DL_T(DL_I("i1", "a")) DM_M("<iref>i1</iref>"),
        {"-/dl:top", NULL, "-/dm:mtop"}, E_OK},
    {"state data reads the deleted instance", DL_T(DL_I("i1", "a") DL_I("i2", "b"))
        DL_C("st", "<s><name>n1</name><sref>i2</sref></s>"), {"-" DL_P "item[name='i2']"}, E_BAD},

    /* what the surviving structure demands */
    {"min-elements of a list", DL_C("c-mn", "<keep>k</keep><mn><name>m1</name></mn><mn><name>m2</name></mn>"),
        {"-/dl:c-mn/mn[name='m1']"}, E_BAD},
    {"min-elements of a list, enough left", DL_C("c-mn", "<keep>k</keep><mn><name>m1</name></mn><mn><name>m2</name></mn>"
        "<mn><name>m3</name></mn>"), {"-/dl:c-mn/mn[name='m1']"}, E_OK},
    {"min-elements of a leaf-list", DL_C("c-mt", "<keep>k</keep><mt>t</mt>"), {"-/dl:c-mt/mt[.='t']"}, E_BAD},
    {"min-elements of a leaf-list, enough left", DL_C("c-mt", "<keep>k</keep><mt>t</mt><mt>u</mt>"),
        {"-/dl:c-mt/mt[.='t']"}, E_OK},
    {"mandatory leaf", DL_C("c-mp", "<m>x</m><l><name>a</name></l>"), {"-/dl:c-mp/m"}, E_BAD},
    {"list next to a mandatory leaf", DL_C("c-mp", "<m>x</m><l><name>a</name></l>"), {"-/dl:c-mp/l[name='a']"}, E_OK},
    {"mandatory choice loses its only node", DL_C("c-mc", "<keep>k</keep><la><name>a1</name></la>"),
        {"-/dl:c-mc/la[name='a1']"}, E_BAD},
    {"mandatory choice keeps a node", DL_C("c-mc", "<keep>k</keep><la><name>a1</name></la><la><name>a2</name></la>"),
        {"-/dl:c-mc/la[name='a1']"}, E_OK},
    {"default case comes back", DL_C("c-dc", "<keep>k</keep><rule><id>1</id></rule>"), {"-/dl:c-dc/rule[id='1']"}, E_OK},
    {"default case does not come back", DL_C("c-dc", "<keep>k</keep><rule><id>1</id></rule><rule><id>2</id></rule>"),
        {"-/dl:c-dc/rule[id='1']"}, E_OK},
    {"both rules of the other case go", DL_C("c-dc", "<keep>k</keep><rule><id>1</id></rule><rule><id>2</id></rule>"),
        {"-/dl:c-dc/rule[id='1']", NULL, "-/dl:c-dc/rule[id='2']"}, E_OK},
    {"presence container is emptied", DL_C("c-pc", "<l><name>a</name></l>") DL_LONE, {"-/dl:c-pc/l[name='a']"}, E_OK},
    {"non-presence container is emptied and a rule reads it", DL_C("c-np", "<l><name>a</name></l>")
        DL_T(DL_R("r", "<m-np>y</m-np>")), {"-/dl:c-np/l[name='a']"}, E_BAD},
    {"non-presence container is emptied, nothing reads it", DL_C("c-np", "<l><name>a</name></l>") DL_LONE,
        {"-/dl:c-np/l[name='a']"}, E_OK},
    {"non-presence container keeps one", DL_C("c-np", "<l><name>a</name></l><l><name>b</name></l>")
        DL_T(DL_R("r", "<m-np>y</m-np>")), {"-/dl:c-np/l[name='a']"}, E_OK},
    {"explicit value goes back to its default", DL_T("<item><name>i1</name><dd>z</dd></item>"
        DL_R("r", "<m-dd>y</m-dd>")), {"-" DL_P "item[name='i1']/dd"}, E_BAD},
    {"explicit value goes back to its default, nobody reads it", DL_T("<item><name>i1</name><dd>z</dd></item>"),
        {"-" DL_P "item[name='i1']/dd"}, E_OK},
    {"count of one value breaks its partner", DL_C("c-vk", "<vk><name>v1</name><grp>g1</grp></vk>"
        "<vk><name>v2</name><grp>g1</grp></vk>"), {"-/dl:c-vk/vk[name='v1']"}, E_BAD},
    {"count of one value keeps enough", DL_C("c-vk", "<vk><name>v1</name><grp>g1</grp></vk>"
        "<vk><name>v2</name><grp>g1</grp></vk><vk><name>v3</name><grp>g1</grp></vk>"), {"-/dl:c-vk/vk[name='v1']"}, E_OK},
    {"unique and max-elements never break", DL_C("c-neg", "<un><name>a</name><u>u1</u></un><un><name>b</name><u>u2</u></un>"
        "<mx>x</mx><mx>y</mx>"), {"-/dl:c-neg/un[name='a']"}, E_OK},
    {"max-elements never breaks", DL_C("c-neg", "<un><name>a</name><u>u1</u></un><mx>x</mx><mx>y</mx>"),
        {"-/dl:c-neg/mx[.='x']"}, E_OK},

    /* the shape of the edit */
    {"reader and target go together", DL_T(DL_I("i1", "a") DL_I("i2", "b") DL_R("r", "<lref>i1</lref>")),
        {"-" DL_P "item[name='i1']", NULL, "-" DL_P "r[name='r']"}, E_OK},
    {"target and an unrelated reader go", DL_T(DL_I("i1", "a") DL_I("i2", "b") DL_R("r1", "<lref>i1</lref>")
        DL_R("r2", "<lref>i2</lref>")), {"-" DL_P "item[name='i1']", NULL, "-" DL_P "r[name='r2']"}, E_BAD},
    {"target goes and the reader is repointed", DL_T(DL_I("i1", "a") DL_I("i2", "b") DL_R("r", "<lref>i1</lref>")),
        {"-" DL_P "item[name='i1']", NULL, DL_P "r[name='r']/lref", "i2"}, E_OK},
    {"target goes and the reader is repointed to a new one", DL_T(DL_I("i1", "a") DL_I("i2", "b")
        DL_R("r", "<lref>i1</lref>")), {"-" DL_P "item[name='i1']", NULL, DL_P "item[name='i9']", NULL,
        DL_P "r[name='r']/lref", "i9"}, E_OK},
    {"one of two targets goes, the reader stays", DL_T(DL_I("i1", "a") DL_I("i2", "b") DL_R("r", "<lrefs>i1</lrefs>")),
        {"-" DL_P "item[name='i2']"}, E_OK},
    {"two targets go, one is read", DL_T(DL_I("i1", "a") DL_I("i2", "b") DL_I("i3", "c") DL_R("r", "<lrefs>i1</lrefs>")),
        {"-" DL_P "item[name='i2']", NULL, "-" DL_P "item[name='i1']"}, E_BAD},
    {"a whole subtree goes, only a descendant is read", DL_T("<inner><a>a</a><x><name>x1</name></x><x><name>x2</name></x></inner>"
        DL_R("r", "<m-anc2>y</m-anc2>")), {"-" DL_P "inner"}, E_BAD},
    {"a leaf reader is deleted", DL_T(DL_I("i1", "a") DL_R("r", "<lref>i1</lref>")), {"-" DL_P "r[name='r']/lref"}, E_OK},
};

static const char *const dl_roots[] = {
    "/dz:zt", "/dm:mtop", "/dl:top", "/dl:lone", "/dl:c-mn", "/dl:c-mt", "/dl:c-mp", "/dl:c-mc", "/dl:c-dc", "/dl:c-pc",
    "/dl:c-np", "/dl:c-vk", "/dl:c-neg", "/dl:pr", NULL
};

static void
dl_clear(struct state *st)
{
    uint32_t i;

    sr_discard_changes(st->sess);
    for (i = 0; dl_roots[i]; ++i) {
        sr_delete_item(st->sess, dl_roots[i], 0);
    }
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));
}

/**
 * @brief Store the starting data of a case, which must be valid.
 */
static void
dl_load(struct state *st, const char *xml)
{
    const struct ly_ctx *ctx;
    struct lyd_node *tree = NULL;

    ctx = sr_acquire_context(st->conn);
    assert_non_null(ctx);
    assert_int_equal(LY_SUCCESS, lyd_parse_data_mem(ctx, xml, LYD_XML, LYD_PARSE_ONLY | LYD_PARSE_STRICT, 0, &tree));
    assert_int_equal(SR_ERR_OK, sr_edit_batch(st->sess, tree, "merge"));
    lyd_free_all(tree);
    sr_release_context(st->conn);
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));
}

static void
dl_run(struct state *st, const struct dl_case *cases, uint32_t count)
{
    const struct dl_case *c;
    uint32_t i, e;
    int rc;

    for (i = 0; i < count; ++i) {
        c = &cases[i];
        if (!strncmp(c->name, "state data", 10)) {
            /* the running datastore holds no state data */
            continue;
        }
        print_message("[  CASE    ] %s\n", c->name);

        dl_clear(st);
        dl_load(st, c->xml);

        for (e = 0; c->edits[e]; e += 2) {
            if (c->edits[e][0] == '-') {
                assert_int_equal(SR_ERR_OK, sr_delete_item(st->sess, c->edits[e] + 1, 0));
            } else {
                assert_int_equal(SR_ERR_OK, sr_set_item_str(st->sess, c->edits[e], c->edits[e + 1], NULL, 0));
            }
        }

        rc = sr_apply_changes(st->sess, 0);
        if (rc != ((c->expected == E_OK) ? SR_ERR_OK : SR_ERR_VALIDATION_FAILED)) {
            print_message("[  MSG     ] case \"%s\": expected %s, got %s\n", c->name,
                    (c->expected == E_OK) ? "accepted" : "rejected", sr_strerror(rc));
        }
        if (c->expected == E_OK) {
            assert_int_equal(SR_ERR_OK, rc);
        } else {
            assert_int_equal(SR_ERR_VALIDATION_FAILED, rc);
            assert_int_equal(SR_ERR_OK, sr_discard_changes(st->sess));
            /* the first deleted path is still there */
            if (c->edits[0][0] == '-') {
                assert_true(exists(st, c->edits[0] + 1));
            }
        }
    }
    dl_clear(st);
}

static void
test_delete_matrix(void **state)
{
    dl_run((struct state *)*state, dl_cases, sizeof dl_cases / sizeof *dl_cases);
}

/*
 * A rule that reads the text of a container while the edit deletes or changes a descendant of it, see the libyang
 * test of the same name.
 */
static const struct dl_case dl_ancestor_cases[] = {
    {"must counts the children of a container", DL_T("<inner><a>a</a><x><name>x1</name></x><x><name>x2</name></x></inner>"
        DL_R("r", "<m-anc2>y</m-anc2>")), {"-" DL_P "inner/x[name='x1']"}, E_BAD},
    {"must reads the string value of a container", DL_T("<inner><a>a</a><x><name>x1</name></x><x><name>x2</name></x></inner>"
        DL_R("r", "<m-anc>y</m-anc>")), {"-" DL_P "inner/x[name='x1']"}, E_BAD},
    {"rule of another module reads the string value of a container", DL_T("<inner><a>a</a><x><name>x1</name></x>"
        "<x><name>x2</name></x></inner>") DM_M("<manc>y</manc>"), {"-" DL_P "inner/x[name='x1']"}, E_BAD},
    {"a leaf below a container changes its string value", DL_T("<inner><a>a</a><x><name>x1</name></x></inner>"
        DL_R("r", "<m-anc3>y</m-anc3>")), {DL_P "inner/a", "bad"}, E_BAD},
    {"a leaf below a container changes the string value another module reads", DL_T("<inner><a>a</a>"
        "<x><name>x1</name></x></inner>") DM_M("<manc2>y</manc2>"), {DL_P "inner/a", "bad"}, E_BAD},
};

/*
 * Constructs of the YANG language that no other case reads, each one a delete that full validation rejects.
 * The deref and instance-identifier cases of the libyang test end in an error code other than a validation failure
 * and are left there.
 */
static const struct dl_case dl_probe_cases[] = {
    {"descendant axis counts the nodes below a container", DL_T("<inner><x><name>x1</name></x><x><name>x2</name></x></inner>"
        DL_R("r", "<m-desc>y</m-desc>")), {"-" DL_P "inner/x[name='x1']"}, E_BAD},
    {"a default leaf-list value appears when the explicit one goes", DL_T("<dtag>x</dtag>" DL_R("r", "<m-dflt>y</m-dflt>")),
        {"-" DL_P "dtag[.='x']"}, E_BAD},
    {"later sibling of the same list is read", DL_T(DL_R("r1", "<m-fs>y</m-fs>") DL_R("r2", "")),
        {"-" DL_P "r[name='r2']"}, E_BAD},
    {"earlier sibling is read by the preceding axis", DL_T("<pos><name>a</name></pos><pos><name>b</name><pp>y</pp></pos>"),
        {"-" DL_P "pos[name='a']"}, E_BAD},
    {"reader in the same list instance, member of a nested list", DL_T("<grp><name>g1</name><mem><name>a</name></mem>"
        "<mem><name>b</name></mem><need>y</need></grp>"), {"-" DL_P "grp[name='g1']/mem[name='a']"}, E_BAD},
    {"reader in another instance, member of a nested list", DL_T("<grp><name>g1</name><mem><name>a</name></mem>"
        "<mem><name>b</name></mem></grp><grp><name>g2</name><mem><name>c</name></mem><xr>y</xr></grp>"),
        {"-" DL_P "grp[name='g1']/mem[name='a']"}, E_BAD},
    {"reader inside a sibling list instance reads the list", DL_T("<inner><x><name>x1</name></x><x><name>x2</name>"
        "<chk>y</chk></x></inner>"), {"-" DL_P "inner/x[name='x1']"}, E_BAD},
    {"count of one leaf-list value", DL_T("<tag>t1</tag><tag>t2</tag>" DL_R("r", "<m-ttag>y</m-ttag>")),
        {"-" DL_P "tag[.='t1']"}, E_BAD},
    {"count of every child of the container", DL_T("<tag>t1</tag><tag>t2</tag><num>1</num><num>2</num><inner/>"
        DL_R("r", "<m-alln>y</m-alln>")), {"-" DL_P "tag[.='t1']"}, E_BAD},
};

/*
 * Adds and modifies that full validation rejects, through a rule that reads another instance of the same list by a
 * relative path, see the libyang test of the same name.
 */
static const struct dl_case dl_add_cases[] = {
    /* LOCAL */
    {"add: LOCAL leafref without target", DL_T(DL_I("i1", "a")), {DL_P "wcr", "x"}, E_BAD},
    {"add: LOCAL leafref with target", DL_T(DL_GATE "<wc-v>x</wc-v>"), {DL_P "wcr", "x"}, E_OK},
    /* LIST_SCOPE */
    {"add: LIST_SCOPE must counts the children, too few", DL_T("<grp><name>g1</name><mem><name>a</name></mem></grp>"),
        {DL_P "grp[name='g1']/need", "y"}, E_BAD},
    {"add: LIST_SCOPE must counts the children, enough", DL_T("<grp><name>g1</name><mem><name>a</name></mem>"
        "<mem><name>b</name></mem></grp>"), {DL_P "grp[name='g1']/need", "y"}, E_OK},
    {"add: LIST_SCOPE reader of a template, component missing",
        DL_T("<tpl><name>t1</name><comp><name>c1</name><cls>ok</cls></comp></tpl>"),
        {DL_P "tpl[name='t1']/ifc[name='i1']/hw", "c9"}, E_BAD},
    {"add: LIST_SCOPE reader of a template, component there",
        DL_T("<tpl><name>t1</name><comp><name>c1</name><cls>ok</cls></comp></tpl>"),
        {DL_P "tpl[name='t1']/ifc[name='i1']/hw", "c1"}, E_OK},
    /* BOUNDED */
    {"add: BOUNDED not() over a relative path finds what was added", DL_T(DL_G1 "<nx>y</nx></grp>"
        "<grp><name>g2</name><mem><name>a</name></mem></grp>"), {DL_P "grp[name='g2']/mem[name='w1']", NULL}, E_BAD},
    {"add: BOUNDED count() over a relative path finds what was added", DL_T(DL_G1 "<nc>y</nc></grp>"
        "<grp><name>g2</name><mem><name>a</name></mem></grp>"), {DL_P "grp[name='g2']/mem[name='w1']", NULL}, E_BAD},
    {"add: BOUNDED count() climbs on after a predicate, finds what was added", DL_T(DL_G1 "<mem><name>x</name></mem>"
        "<pc><np>y</np></pc></grp><grp><name>g2</name><mem><name>a</name></mem></grp>"),
        {DL_P "grp[name='g2']/mem[name='w1']", NULL}, E_BAD},
    {"add: BOUNDED creation-safe reader, an unrelated instance is added",
        DL_T(DL_I("i1", "good") DL_R("r", "<sel>i1</sel><m-val>y</m-val>")),
        {DL_P "item[name='i2']/val", "bad"}, E_OK},
    {"add: BOUNDED reader added, its target is missing", DL_T(DL_I("i1", "good")),
        {DL_P "r[name='r2']/sel", "nope", DL_P "r[name='r2']/m-exist", "y"}, E_BAD},
    /* UNBOUNDED */
    {"add: UNBOUNDED upper bound count over an absolute path, one too many",
        DL_T(DL_I("i1", "a") DL_I("i2", "a") DL_I("i3", "a") DL_R("r", "<m-cap>y</m-cap>")),
        {DL_P "item[name='i4']/val", "a"}, E_BAD},
    {"add: UNBOUNDED upper bound count over an absolute path, at the limit",
        DL_T(DL_I("i1", "a") DL_I("i2", "a") DL_R("r", "<m-cap>y</m-cap>")), {DL_P "item[name='i3']/val", "a"}, E_OK},
    {"add: UNBOUNDED lower bound count, an instance is added", DL_T(DL_I("i1", "a") DL_I("i2", "a")
        DL_R("r", "<m-count>y</m-count>")), {DL_P "item[name='i3']/val", "a"}, E_OK},
    {"add: UNBOUNDED reader added, leafref without target", DL_T(DL_I("i1", "a")), {DL_P "r[name='n']/lref", "nope"}, E_BAD},
    {"add: UNBOUNDED when over another instance, the gate is missing", DL_T(DL_I("i1", "a") DL_R("r", "")),
        {DL_P "r[name='r']/w-on", "v"}, E_BAD},
    {"add: UNBOUNDED when over another instance, the gate is there", DL_T(DL_GATE DL_R("r", "")),
        {DL_P "r[name='r']/w-on", "v"}, E_OK},
    {"add: UNBOUNDED must counts the sibling instances, alone", DL_T("<inner><a>a</a></inner>"),
        {DL_P "inner/x[name='x1']/chk", "y"}, E_BAD},
    {"add: UNBOUNDED must counts the earlier siblings, first one", DL_T(DL_I("i1", "a")),
        {DL_P "pos[name='a']/pp", "y"}, E_BAD},
    {"add: structure, max-elements of a list", DL_C("c-neg", "<un><name>a</name></un><un><name>b</name></un>"
        "<un><name>c</name></un>"), {"/dl:c-neg/un[name='d']", NULL}, E_BAD},
    {"add: structure, unique value twice", DL_C("c-neg", "<un><name>a</name><u>1</u></un>"),
        {"/dl:c-neg/un[name='b']/u", "1"}, E_BAD},
    /* CASCADE */
    {"add: CASCADE the when turns false, the node goes", DL_T(DL_G1 "<wn>y</wn></grp>"
        "<grp><name>g2</name><mem><name>a</name></mem></grp>"), {DL_P "grp[name='g2']/mem[name='w1']", NULL}, E_OK},
    {"add: CASCADE a default appears and another goes", DL_T(DL_I("i1", "a")) DZ_Z,
        {DL_P "item[name='gate']/val", "a"}, E_OK},
    /* the first adds, kept */
    {"add a leaf in front of the one a rule reads", DL_T("<grp><name>g1</name><nf>y</nf></grp>"
        "<grp><name>g2</name><lv>ok</lv></grp>"), {DL_P "grp[name='g2']/lv", "no"}, E_BAD},
};

static const struct dl_case dl_modify_cases[] = {
    /* LOCAL */
    {"modify: LOCAL leafref target changes", DL_T(DL_GATE "<wc-v>a</wc-v><wcr>a</wcr>"), {DL_P "wc-v", "b"}, E_BAD},
    {"modify: LOCAL leafref and target change together", DL_T(DL_GATE "<wc-v>a</wc-v><wcr>a</wcr>"),
        {DL_P "wc-v", "b", DL_P "wcr", "b"}, E_OK},
    /* LIST_SCOPE */
    {"modify: LIST_SCOPE component class changes, an interface uses it",
        DL_T("<tpl><name>t1</name><comp><name>c1</name><cls>ok</cls></comp>"
        "<ifc><name>i1</name><hw>c1</hw></ifc></tpl>"), {DL_P "tpl[name='t1']/comp[name='c1']/cls", "no"}, E_BAD},
    {"modify: LIST_SCOPE component class changes, nothing uses it",
        DL_T("<tpl><name>t1</name><comp><name>c1</name><cls>ok</cls></comp><comp><name>c2</name><cls>ok</cls></comp>"
        "<ifc><name>i1</name><hw>c1</hw></ifc></tpl>"), {DL_P "tpl[name='t1']/comp[name='c2']/cls", "no"}, E_OK},
    /* BOUNDED */
    {"modify: BOUNDED must reads the value of the instance it picks",
        DL_T(DL_I("i1", "good") DL_R("r", "<sel>i1</sel><m-val>y</m-val>")), {DL_P "item[name='i1']/val", "bad"}, E_BAD},
    {"modify: BOUNDED must, an unrelated instance changes",
        DL_T(DL_I("i1", "good") DL_I("i2", "x") DL_R("r", "<sel>i1</sel><m-val>y</m-val>")),
        {DL_P "item[name='i2']/val", "bad"}, E_OK},
    {"modify: BOUNDED leafref to the value of the instance it picks",
        DL_T(DL_I("i1", "v1") DL_R("r", "<sel>i1</sel><dref>v1</dref>")), {DL_P "item[name='i1']/val", "v2"}, E_BAD},
    {"modify: BOUNDED not() over a relative path, the leaf it names changes",
        DL_T(DL_G1 "<nx>y</nx></grp><grp><name>g2</name><mem><name>a</name></mem></grp>"),
        {DL_P "grp[name='g1']/want", "a"}, E_BAD},
    /* UNBOUNDED */
    {"modify: UNBOUNDED must reads a leaf of a named instance",
        DL_T("<item><name>i1</name><dd>z</dd></item>" DL_R("r", "<m-dd>y</m-dd>")), {DL_P "item[name='i1']/dd", "y"}, E_BAD},
    {"modify: UNBOUNDED must, another instance changes",
        DL_T("<item><name>i1</name><dd>z</dd></item>" DL_I("i2", "a") DL_R("r", "<m-dd>y</m-dd>")),
        {DL_P "item[name='i2']/dd", "y"}, E_OK},
    {"modify: UNBOUNDED must reads a leaf of another list instance",
        DL_T("<grp><name>g1</name><mv>y</mv></grp><grp><name>g2</name><lv>ok</lv></grp>"),
        {DL_P "grp[name='g2']/lv", "no"}, E_BAD},
    {"modify: UNBOUNDED must reads a leaf of another list instance, a different instance changes",
        DL_T("<grp><name>g1</name><mv>y</mv><lv>a</lv></grp><grp><name>g2</name><lv>ok</lv></grp>"),
        {DL_P "grp[name='g1']/lv", "b"}, E_OK},
    {"modify: UNBOUNDED must reads the text of a container",
        DL_T("<inner><a>a</a><x><name>x1</name></x></inner>" DL_R("r", "<m-anc3>y</m-anc3>")),
        {DL_P "inner/a", "bad"}, E_BAD},
    {"modify: UNBOUNDED value keyed count, a value leaves its group",
        DL_C("c-vk", "<vk><name>a</name><grp>g</grp></vk><vk><name>b</name><grp>g</grp></vk>"
        "<vk><name>c</name><grp>g</grp></vk>"), {"/dl:c-vk/vk[name='c']/grp", "h"}, E_BAD},
    /* CASCADE */
    {"modify: CASCADE the when turns false, the node is dropped, a reader needs it",
        DL_T(DL_R("r", "<sw>on</sw><dep>x</dep><m-dep>y</m-dep>")), {DL_P "r[name='r']/sw", "off"}, E_BAD},
    {"modify: CASCADE the when turns false, the node is dropped, nothing reads it",
        DL_T(DL_R("r", "<sw>on</sw><dep>x</dep>")), {DL_P "r[name='r']/sw", "off"}, E_OK},
};

static void
test_add_relative_readers(void **state)
{
    dl_run((struct state *)*state, dl_add_cases, sizeof dl_add_cases / sizeof *dl_add_cases);
}

static void
test_modify_rules(void **state)
{
    dl_run((struct state *)*state, dl_modify_cases, sizeof dl_modify_cases / sizeof *dl_modify_cases);
}

/*
 * The rules of the product schema, one shape each. The names are not the product names, the expressions are. Every
 * case is an add or a modify, and the expectation is what full validation says.
 */
#define PR_P "/dl:pr/"
#define PR_T(x) DL_C("pr", x)
#define PR_SH "<shaper><name>s1</name><single>y</single></shaper><shaper><name>s2</name><two>y</two></shaper>"
#define PR_Q(d, sh) "<qprof><name>q1</name><queues><queue><name>a</name>" d "<shaper-name>" sh "</shaper-name></queue></queues></qprof>"
#define PR_TM "<tmpl><name>t1</name><ifc><name>i1</name><v4>y</v4></ifc></tmpl><tmpl><name>t2</name><ifc><name>i1</name><v4>y</v4></ifc>" \
        "<ifc><name>i2</name><v4>y</v4></ifc></tmpl><tmpl><name>t3</name><ifc><name>i9</name><v4>y</v4></ifc></tmpl>"
#define PR_ONU(x) "<onu><name>o1</name><tref>t1</tref><term><name>x</name>" x "</term></onu>"
#define PR_REV(c, r) "<comp><name>c1</name><revs>" c r "</revs></comp>"
#define PR_TP(x) "<tp><name>p1</name><tcs>" x "</tcs></tp>"
#define PR_TC(n, d) "<tc><position>" n "</position><duration>" d "</duration></tc>"
#define PR_TS "<tsb><name>b1</name><pk><name>k1</name><fmt xmlns:dl=\"urn:tests:dl\">dl:fmt-ssh</fmt></pk></tsb><tsb><name>b2</name><pk><name>k2</name><fmt xmlns:dl=\"urn:tests:dl\">dl:fmt-other</fmt></pk></tsb>"
static const struct dl_case dl_prod_cases[] = {
    /* the queue shaper must: not(boolean(dual)) and boolean(shaper/single), or the same with dual and two */
    {"product QoS: dual added, the shaper has only a single bucket", PR_T(PR_SH PR_Q("", "s1")),
        {PR_P "qprof[name='q1']/queues/queue[name='a']/dual", "y"}, E_BAD},
    {"product QoS: dual added and the shaper moved to the two bucket one", PR_T(PR_SH PR_Q("", "s1")),
        {PR_P "qprof[name='q1']/queues/queue[name='a']/dual", "y", PR_P "qprof[name='q1']/queues/queue[name='a']/shaper-name", "s2"}, E_OK},
    {"product QoS: dual removed, the shaper has only a two bucket", PR_T(PR_SH PR_Q("<dual>y</dual>", "s2")),
        {"-" PR_P "qprof[name='q1']/queues/queue[name='a']/dual"}, E_BAD},
    {"product QoS: single bucket removed from the shaper in use", PR_T(PR_SH PR_Q("", "s1")),
        {"-" PR_P "shaper[name='s1']/single"}, E_BAD},
    {"product QoS: two bucket added to the shaper in use, the single one stays", PR_T(PR_SH PR_Q("", "s1")),
        {PR_P "shaper[name='s1']/two", "y"}, E_OK},
    {"product QoS: shaper moved to one with only a two bucket", PR_T(PR_SH PR_Q("", "s1")),
        {PR_P "qprof[name='q1']/queues/queue[name='a']/shaper-name", "s2"}, E_BAD},
    {"product QoS: a shaper nobody uses gets a bucket", PR_T(PR_SH "<shaper><name>s3</name></shaper>" PR_Q("", "s1")),
        {PR_P "shaper[name='s3']/two", "y"}, E_OK},
    /* when over an absolute path, bounded and creation-safe */
    {"product QoS: BOUNDED when, burst added, the shaper has a single bucket",
        PR_T(PR_SH "<bq><name>b</name><sref>s1</sref></bq>"), {PR_P "bq[name='b']/burst", "y"}, E_OK},
    {"product QoS: BOUNDED when, burst added, the shaper has no single bucket",
        PR_T(PR_SH "<bq><name>b</name><sref>s2</sref></bq>"), {PR_P "bq[name='b']/burst", "y"}, E_BAD},
    {"product QoS: CASCADE BOUNDED when, single bucket removed under a burst, the node is dropped",
        PR_T(PR_SH "<bq><name>b</name><sref>s1</sref><burst>y</burst></bq>"), {"-" PR_P "shaper[name='s1']/single"}, E_OK},
    {"product QoS: CASCADE BOUNDED when, sref moved under a burst, the node is dropped",
        PR_T(PR_SH "<bq><name>b</name><sref>s1</sref><burst>y</burst></bq>"), {PR_P "bq[name='b']/sref", "s2"}, E_OK},
    /* ancestor:: axis inside a predicate */
    {"product ONU: interface not in the template of the ONU", PR_T(PR_TM PR_ONU("")),
        {PR_P "onu[name='o1']/term[name='x']/ipif", "i2"}, E_BAD},
    {"product ONU: interface in the template of the ONU", PR_T(PR_TM PR_ONU("")),
        {PR_P "onu[name='o1']/term[name='x']/ipif", "i1"}, E_OK},
    {"product ONU: the address of the interface is removed from the template", PR_T(PR_TM PR_ONU("<ipif>i1</ipif>")),
        {"-" PR_P "tmpl[name='t1']/ifc[name='i1']/v4"}, E_BAD},
    {"product ONU: the v4 address is removed, a v6 one stays", PR_T(PR_TM PR_ONU("<ipif>i1</ipif>")),
        {PR_P "tmpl[name='t1']/ifc[name='i1']/v6", "y", "-" PR_P "tmpl[name='t1']/ifc[name='i1']/v4"}, E_OK},
    {"product ONU: the ONU moves to a template without the interface", PR_T(PR_TM PR_ONU("<ipif>i1</ipif>")),
        {PR_P "onu[name='o1']/tref", "t3"}, E_BAD},
    {"product ONU: the ONU moves to a template with the interface", PR_T(PR_TM PR_ONU("<ipif>i1</ipif>")),
        {PR_P "onu[name='o1']/tref", "t2"}, E_OK},
    {"product ONU: LOCAL leafref to a sibling instance, missing", PR_T(PR_TM PR_ONU("")),
        {PR_P "onu[name='o1']/term[name='y']/peer", "nope"}, E_BAD},
    {"product ONU: LOCAL leafref to a sibling instance, there", PR_T(PR_TM PR_ONU("")),
        {PR_P "onu[name='o1']/term[name='y']/peer", "x"}, E_OK},
    /* when with boolean(), derived-from() and count() over the siblings */
    {"product SWM: when boolean(capability=x), the leaf is added and the capability fits",
        PR_T(PR_REV("<cap>alias</cap>", "<rev><name>r1</name><state>available</state></rev>")),
        {PR_P "comp[name='c1']/revs/rev[name='r1']/alias", "y"}, E_OK},
    {"product SWM: when boolean(capability=x), the leaf is added and the capability does not fit",
        PR_T(PR_REV("<cap>other</cap>", "<rev><name>r1</name><state>available</state></rev>")),
        {PR_P "comp[name='c1']/revs/rev[name='r1']/alias", "y"}, E_BAD},
    {"product SWM: CASCADE when boolean(capability=x), the capability changes under the leaf, the node is dropped",
        PR_T(PR_REV("<cap>alias</cap>", "<rev><name>r1</name><state>available</state><alias>y</alias></rev>")),
        {PR_P "comp[name='c1']/revs/cap", "other"}, E_OK},
    {"product SWM: when with a state test, the state fits",
        PR_T(PR_REV("<cap>act</cap>", "<rev><name>r1</name><state>in-use</state></rev>")),
        {PR_P "comp[name='c1']/revs/rev[name='r1']/act", "y"}, E_OK},
    {"product SWM: when with a state test, the state does not fit",
        PR_T(PR_REV("<cap>act</cap>", "<rev><name>r1</name><state>new</state></rev>")),
        {PR_P "comp[name='c1']/revs/rev[name='r1']/act", "y"}, E_BAD},
    {"product SWM: CASCADE when with a state test, the state changes under the leaf, the node is dropped",
        PR_T(PR_REV("<cap>act</cap>", "<rev><name>r1</name><state>available</state><act>y</act></rev>")),
        {PR_P "comp[name='c1']/revs/rev[name='r1']/state", "new"}, E_OK},
    {"product SWM: CASCADE plain derived-from(), capability changes under the leaf, the node is dropped",
        PR_T(PR_REV("<dcap xmlns:dl=\"urn:tests:dl\">dl:cap-sub</dcap>", "<rev><name>r1</name><state>available</state><fin>y</fin></rev>")),
        {PR_P "comp[name='c1']/revs/dcap", "cap-other"}, E_OK},
    {"product SWM: derived-from() and count()=0, download added, nothing downloads",
        PR_T(PR_REV("<dcap xmlns:dl=\"urn:tests:dl\">dl:cap-sub</dcap>", "<rev><name>r1</name><state>available</state></rev>")),
        {PR_P "comp[name='c1']/revs/download", "y"}, E_OK},
    {"product SWM: derived-from() and count()=0, download added while a revision downloads",
        PR_T(PR_REV("<dcap xmlns:dl=\"urn:tests:dl\">dl:cap-sub</dcap>", "<rev><name>r1</name><state>downloading</state></rev>")),
        {PR_P "comp[name='c1']/revs/download", "y"}, E_BAD},
    {"product SWM: CASCADE count()=0, a revision starts to download under the leaf, the node is dropped",
        PR_T(PR_REV("<dcap xmlns:dl=\"urn:tests:dl\">dl:cap-sub</dcap>", "<rev><name>r1</name><state>available</state></rev><download>y</download>")),
        {PR_P "comp[name='c1']/revs/rev[name='r1']/state", "downloading"}, E_OK},
    {"product SWM: CASCADE count()=0, a revision that downloads is added under the leaf, the node is dropped",
        PR_T(PR_REV("<dcap xmlns:dl=\"urn:tests:dl\">dl:cap-sub</dcap>", "<rev><name>r1</name><state>available</state></rev><download>y</download>")),
        {PR_P "comp[name='c1']/revs/rev[name='r2']/state", "downloading"}, E_OK},
    {"product SWM: count()=0, a revision that does not download is added under the leaf",
        PR_T(PR_REV("<dcap xmlns:dl=\"urn:tests:dl\">dl:cap-sub</dcap>", "<rev><name>r1</name><state>available</state></rev><download>y</download>")),
        {PR_P "comp[name='c1']/revs/rev[name='r2']/state", "available"}, E_OK},
    {"product SWM: CASCADE derived-from(), capability changes under the download leaf, the node is dropped",
        PR_T(PR_REV("<dcap xmlns:dl=\"urn:tests:dl\">dl:cap-sub</dcap>", "<rev><name>r1</name><state>available</state></rev><download>y</download>")),
        {PR_P "comp[name='c1']/revs/dcap", "cap-other"}, E_OK},
    /* a list keyed by position, and rules that read the leaf named position */
    {"product tone: the next position is added", PR_T(PR_TP(PR_TC("1", "a") PR_TC("2", "b"))),
        {PR_P "tp[name='p1']/tcs/tc[position='3']/duration", "c"}, E_OK},
    {"product tone: a position is added after a gap", PR_T(PR_TP(PR_TC("1", "a") PR_TC("2", "b"))),
        {PR_P "tp[name='p1']/tcs/tc[position='5']/duration", "c"}, E_BAD},
    {"product tone: repetition added, the last component is not continuous", PR_T(PR_TP(PR_TC("1", "a") PR_TC("2", "b"))),
        {PR_P "tp[name='p1']/tcs/rb", "y"}, E_OK},
    {"product tone: repetition added, the last component is continuous", PR_T(PR_TP(PR_TC("1", "a") PR_TC("2", "continuous"))),
        {PR_P "tp[name='p1']/tcs/rb", "y"}, E_BAD},
    {"product tone: repetition added, only one component", PR_T(PR_TP(PR_TC("1", "a"))),
        {PR_P "tp[name='p1']/tcs/rb", "y"}, E_BAD},
    {"product tone: CASCADE the last component becomes continuous under the repetition, the node is dropped",
        PR_T(PR_TP(PR_TC("1", "a") PR_TC("2", "b") "<rb>y</rb>")),
        {PR_P "tp[name='p1']/tcs/tc[position='2']/duration", "continuous"}, E_OK},
    {"product tone: CASCADE a continuous component is added last under the repetition, the node is dropped",
        PR_T(PR_TP(PR_TC("1", "a") PR_TC("2", "b") "<rb>y</rb>")),
        {PR_P "tp[name='p1']/tcs/tc[position='3']/duration", "continuous"}, E_OK},
    /* deref() with a predicate that has not(derived-from-or-self()) inside */
    {"product truststore: the format of a key in use changes", PR_T(PR_TS "<usr><name>u1</name><ref>b1</ref></usr>"),
        {PR_P "tsb[name='b1']/pk[name='k1']/fmt", "fmt-other"}, E_BAD},
    {"product truststore: a key with another format is added to the bag in use", PR_T(PR_TS "<usr><name>u1</name><ref>b1</ref></usr>"),
        {PR_P "tsb[name='b1']/pk[name='k3']/fmt", "fmt-other"}, E_BAD},
    {"product truststore: a key with the right format is added to the bag in use", PR_T(PR_TS "<usr><name>u1</name><ref>b1</ref></usr>"),
        {PR_P "tsb[name='b1']/pk[name='k3']/fmt", "fmt-ssh"}, E_OK},
    {"product truststore: a bag with another format is not used, its key changes", PR_T(PR_TS "<usr><name>u1</name><ref>b1</ref></usr>"),
        {PR_P "tsb[name='b2']/pk[name='k2']/fmt", "fmt-ssh"}, E_OK},
    {"product truststore: a user is added on the good bag", PR_T(PR_TS "<usr><name>u1</name><ref>b1</ref></usr>"),
        {PR_P "usr[name='u2']/ref", "b1"}, E_OK},
    {"product truststore: a user is added on the bag with another format", PR_T(PR_TS "<usr><name>u1</name><ref>b1</ref></usr>"),
        {PR_P "usr[name='u2']/ref", "b2"}, E_BAD},
    {"product truststore: a user moves to the bag with another format", PR_T(PR_TS "<usr><name>u1</name><ref>b1</ref></usr>"),
        {PR_P "usr[name='u1']/ref", "b2"}, E_BAD},
};

static void
test_product_shapes(void **state)
{
    dl_run((struct state *)*state, dl_prod_cases, sizeof dl_prod_cases / sizeof *dl_prod_cases);
}

static void
test_delete_yang_probe(void **state)
{
    dl_run((struct state *)*state, dl_probe_cases, sizeof dl_probe_cases / sizeof *dl_probe_cases);
}

static void
test_delete_ancestor_reader(void **state)
{
    dl_run((struct state *)*state, dl_ancestor_cases, sizeof dl_ancestor_cases / sizeof *dl_ancestor_cases);
}

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_teardown(test_add, clear_data),
        cmocka_unit_test_teardown(test_delete, clear_data),
        cmocka_unit_test_teardown(test_cross_module, clear_data),
        cmocka_unit_test_teardown(test_when_cascade, clear_data),
        cmocka_unit_test_teardown(test_when_cascade_leafref, clear_data),
        cmocka_unit_test_teardown(test_dflt_case_replace, clear_data),
        cmocka_unit_test_teardown(test_delete_matrix, clear_data),
        cmocka_unit_test_teardown(test_delete_ancestor_reader, clear_data),
        cmocka_unit_test_teardown(test_delete_yang_probe, clear_data),
        cmocka_unit_test_teardown(test_add_relative_readers, clear_data),
        cmocka_unit_test_teardown(test_modify_rules, clear_data),
        cmocka_unit_test_teardown(test_product_shapes, clear_data),
    };

    setenv("CMOCKA_TEST_ABORT", "1", 1);
    test_log_init();
    return cmocka_run_group_tests(tests, setup_f, teardown_f);
}
