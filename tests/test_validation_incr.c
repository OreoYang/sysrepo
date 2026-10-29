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
    const char *module_names[] = {"dm", "dl", "incr-c", "incr-b", "incr-a", NULL};

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
    {"when turns false to true, a default appears", DL_T(DL_GATE DL_R("r", "<sel>x</sel>")),
        {"-" DL_P "item[name='gate']"}, E_OK},
    {"default node whose when reads the deleted instance", DL_T(DL_GATE DL_R("r", "<sel>x</sel>")),
        {"-" DL_P "item[name='gate']"}, E_OK},
    {"default node whose when reads a node the validation drops", DL_T(DL_GATE DL_R("r", "<w-on>v1</w-on>")),
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
    "/dm:mtop", "/dl:top", "/dl:lone", "/dl:c-mn", "/dl:c-mt", "/dl:c-mp", "/dl:c-mc", "/dl:c-dc", "/dl:c-pc",
    "/dl:c-np", "/dl:c-vk", "/dl:c-neg", NULL
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
    };

    setenv("CMOCKA_TEST_ABORT", "1", 1);
    test_log_init();
    return cmocka_run_group_tests(tests, setup_f, teardown_f);
}
