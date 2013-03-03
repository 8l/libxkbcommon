/************************************************************
 * Copyright (c) 1994 by Silicon Graphics Computer Systems, Inc.
 *
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting
 * documentation, and that the name of Silicon Graphics not be
 * used in advertising or publicity pertaining to distribution
 * of the software without specific prior written permission.
 * Silicon Graphics makes no representation about the suitability
 * of this software for any purpose. It is provided "as is"
 * without any express or implied warranty.
 *
 * SILICON GRAPHICS DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT SHALL SILICON
 * GRAPHICS BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION  WITH
 * THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 ********************************************************/

#include "xkbcomp-priv.h"
#include "text.h"
#include "expr.h"
#include "keycodes.h"
#include "include.h"

/*
 * The xkb_keycodes section
 * ========================
 *
 * This is the simplest section type, and is the first one to be
 * compiled. The purpose of this is mostly to map between the
 * hardware/evdev scancodes and xkb keycodes. Each key is given a name
 * by which it can be referred to later, e.g. in the symbols section.
 *
 * Keycode statements
 * ------------------
 * Statements of the form:
 *      <TLDE> = 49;
 *      <AE01> = 10;
 *
 * The above would let 49 and 10 be valid keycodes in the keymap, and
 * assign them the names TLDE and AE01 respectively. The format <WXYZ> is
 * always used to refer to a key by name.
 *
 * [ The naming convention <AE01> just denoted the position of the key
 * in the main alphanumric section of the keyboard, with the two letters
 * specifying the row and the two digits specifying the column, from
 * the bottom left.]
 *
 * In the common case this just maps to the evdev scancodes from
 * /usr/include/linux/input.h, e.g. the following definitions:
 *      #define KEY_GRAVE            41
 *      #define KEY_1                2
 * Similar definitions appear in the xf86-input-keyboard driver. Note
 * that in all current keymaps there's a constant offset of 8 (for
 * historical reasons).
 *
 * If there's a conflict, like the same name given to different keycodes,
 * or same keycode given different names, it is resolved according to the
 * merge mode which applies to the definitions.
 *
 * Alias statements
 * ----------------
 * Statements of the form:
 *      alias <MENU> = <COMP>;
 *
 * Allows to refer to a previously defined key (here <COMP>) by another
 * name (here <MENU>). Conflicts are handled similarly.
 *
 * LED name statements
 * -------------------------
 * Statements of the form:
 *      indicator 1 = "Caps Lock";
 *      indicator 2 = "Num Lock";
 *      indicator 3 = "Scroll Lock";
 *
 * Assigns a name to the keyboard LED (a.k.a indicator) with the given index.
 * The led may be referred by this name later in the compat section
 * and by the user.
 *
 * Effect on the keymap
 * --------------------
 * After all of the xkb_keycodes sections have been compiled, the
 * following members of struct xkb_keymap are finalized:
 *      xkb_keycode_t min_key_code;
 *      xkb_keycode_t max_key_code;
 *      darray(struct xkb_key_alias) key_aliases;
 *      char *keycodes_section_name;
 * The 'name' field of leds declared in xkb_keycodes:
 *      darray(struct xkb_led) leds;
 * Further, the array of keys:
 *      struct xkb_key *keys;
 * had been resized to its final size (i.e. all of the xkb_key objects are
 * referable by their keycode). However the objects themselves do not
 * contain any useful information besides the key name at this point.
 */

typedef struct {
    enum merge_mode merge;

    xkb_atom_t alias;
    xkb_atom_t real;
} AliasInfo;

typedef struct {
    unsigned file_id;

    xkb_atom_t name;
} KeyNameInfo;

typedef struct {
    enum merge_mode merge;
    unsigned file_id;

    xkb_atom_t name;
} LedNameInfo;

typedef struct {
    char *name;
    int errorCount;
    enum merge_mode merge;
    unsigned file_id;

    xkb_keycode_t min_key_code;
    xkb_keycode_t max_key_code;
    darray(KeyNameInfo) key_names;
    darray(LedNameInfo) led_names;
    darray(AliasInfo) aliases;

    struct xkb_context *ctx;
} KeyNamesInfo;

/***====================================================================***/

static void
InitAliasInfo(AliasInfo *info, enum merge_mode merge,
              xkb_atom_t alias, xkb_atom_t real)
{
    memset(info, 0, sizeof(*info));
    info->merge = merge;
    info->alias = alias;
    info->real = real;
}

static LedNameInfo *
FindLedByName(KeyNamesInfo *info, xkb_atom_t name,
              xkb_led_index_t *idx_out)
{
    LedNameInfo *ledi;
    xkb_led_index_t idx;

    darray_enumerate(idx, ledi, info->led_names) {
        if (ledi->name == name) {
            *idx_out = idx;
            return ledi;
        }
    }

    return NULL;
}

static bool
AddLedName(KeyNamesInfo *info, enum merge_mode merge,
           LedNameInfo *new, xkb_led_index_t new_idx)
{
    xkb_led_index_t old_idx;
    LedNameInfo *old;
    const int verbosity = xkb_context_get_log_verbosity(info->ctx);
    const bool replace = (merge == MERGE_REPLACE || merge == MERGE_OVERRIDE);

    /* Inidicator with the same name already exists. */
    old = FindLedByName(info, new->name, &old_idx);
    if (old) {
        const bool report = ((old->file_id == new->file_id && verbosity > 0) ||
                             verbosity > 9);

        if (old_idx == new_idx) {
            log_warn(info->ctx,
                     "Multiple indicators named \"%s\"; "
                     "Identical definitions ignored\n",
                     xkb_atom_text(info->ctx, new->name));
            return true;
        }

        if (report) {
            xkb_led_index_t use = (replace ? new_idx + 1 : old_idx + 1);
            xkb_led_index_t ignore = (replace ? old_idx + 1 : new_idx + 1);
            log_warn(info->ctx,
                     "Multiple indicators named %s; Using %d, ignoring %d\n",
                     xkb_atom_text(info->ctx, new->name), use, ignore);
        }

        if (replace)
            *old = *new;

        return true;
    }

    if (new_idx >= darray_size(info->led_names))
        darray_resize0(info->led_names, new_idx + 1);

    /* Inidicator with the same index already exists. */
    old = &darray_item(info->led_names, new_idx);
    if (old->name != XKB_ATOM_NONE) {
        const bool report = ((old->file_id == new->file_id && verbosity > 0) ||
                             verbosity > 9);

        /* Same name case already handled above. */

        if (report) {
            const xkb_atom_t use = (replace ? new->name : old->name);
            const xkb_atom_t ignore = (replace ? old->name : new->name);
            log_warn(info->ctx, "Multiple names for indicator %d; "
                     "Using %s, ignoring %s\n", new_idx + 1,
                     xkb_atom_text(info->ctx, use),
                     xkb_atom_text(info->ctx, ignore));
        }

        if (replace)
            *old = *new;

        return true;
    }

    darray_item(info->led_names, new_idx) = *new;
    return true;
}

static void
ClearKeyNamesInfo(KeyNamesInfo *info)
{
    free(info->name);
    darray_free(info->key_names);
    darray_free(info->aliases);
    darray_free(info->led_names);
}

static void
InitKeyNamesInfo(KeyNamesInfo *info, struct xkb_context *ctx,
                 unsigned file_id)
{
    memset(info, 0, sizeof(*info));
    info->ctx = ctx;
    info->merge = MERGE_DEFAULT;
    info->file_id = file_id;
    info->min_key_code = XKB_KEYCODE_MAX;
}

static xkb_keycode_t
FindKeyByName(KeyNamesInfo * info, xkb_atom_t name)
{
    xkb_keycode_t i;

    for (i = info->min_key_code; i <= info->max_key_code; i++)
        if (darray_item(info->key_names, i).name == name)
            return i;

    return XKB_KEYCODE_INVALID;
}

static bool
AddKeyName(KeyNamesInfo *info, xkb_keycode_t kc, xkb_atom_t name,
           enum merge_mode merge, unsigned file_id, bool report)
{
    KeyNameInfo *namei;
    xkb_keycode_t old;
    const int verbosity = xkb_context_get_log_verbosity(info->ctx);

    if (kc >= darray_size(info->key_names))
        darray_resize0(info->key_names, kc + 1);

    info->min_key_code = MIN(info->min_key_code, kc);
    info->max_key_code = MAX(info->max_key_code, kc);

    namei = &darray_item(info->key_names, kc);

    report = report && ((verbosity > 0 && file_id == namei->file_id) ||
                        verbosity > 7);

    if (namei->name != 0) {
        const char *lname = KeyNameText(info->ctx, namei->name);
        const char *kname = KeyNameText(info->ctx, name);

        if (namei->name == name) {
            if (report)
                log_warn(info->ctx,
                         "Multiple identical key name definitions; "
                         "Later occurrences of \"%s = %d\" ignored\n",
                         lname, kc);
            return true;
        }
        else if (merge == MERGE_AUGMENT) {
            if (report)
                log_warn(info->ctx,
                         "Multiple names for keycode %d; "
                         "Using %s, ignoring %s\n", kc, lname, kname);
            return true;
        }
        else {
            if (report)
                log_warn(info->ctx,
                         "Multiple names for keycode %d; "
                         "Using %s, ignoring %s\n", kc, kname, lname);
            namei->name = 0;
            namei->file_id = 0;
        }
    }

    old = FindKeyByName(info, name);
    if (old != XKB_KEYCODE_INVALID && old != kc) {
        const char *kname = KeyNameText(info->ctx, name);

        if (merge == MERGE_OVERRIDE) {
            darray_item(info->key_names, old).name = 0;
            darray_item(info->key_names, old).file_id = 0;
            if (report)
                log_warn(info->ctx,
                         "Key name %s assigned to multiple keys; "
                         "Using %d, ignoring %d\n", kname, kc, old);
        }
        else {
            if (report)
                log_vrb(info->ctx, 3,
                        "Key name %s assigned to multiple keys; "
                        "Using %d, ignoring %d\n", kname, old, kc);
            return true;
        }
    }

    namei->name = name;
    namei->file_id = file_id;
    return true;
}

/***====================================================================***/

static int
HandleAliasDef(KeyNamesInfo *info, KeyAliasDef *def, enum merge_mode merge);

static bool
MergeAliases(KeyNamesInfo *into, KeyNamesInfo *from, enum merge_mode merge)
{
    AliasInfo *alias;
    KeyAliasDef def;

    if (darray_empty(from->aliases))
        return true;

    if (darray_empty(into->aliases)) {
        into->aliases = from->aliases;
        darray_init(from->aliases);
        return true;
    }

    memset(&def, 0, sizeof(def));

    darray_foreach(alias, from->aliases) {
        def.merge = (merge == MERGE_DEFAULT) ? alias->merge : merge;
        def.alias = alias->alias;
        def.real = alias->real;

        if (!HandleAliasDef(into, &def, def.merge))
            return false;
    }

    return true;
}

static void
MergeIncludedKeycodes(KeyNamesInfo *into, KeyNamesInfo *from,
                      enum merge_mode merge)
{
    xkb_keycode_t i;
    xkb_led_index_t idx;
    LedNameInfo *ledi;

    if (from->errorCount > 0) {
        into->errorCount += from->errorCount;
        return;
    }

    if (into->name == NULL) {
        into->name = from->name;
        from->name = NULL;
    }

    if (darray_size(into->key_names) < darray_size(from->key_names))
        darray_resize0(into->key_names, darray_size(from->key_names));

    for (i = from->min_key_code; i <= from->max_key_code; i++) {
        xkb_atom_t name = darray_item(from->key_names, i).name;
        if (name == XKB_ATOM_NONE)
            continue;

        if (!AddKeyName(into, i, name, merge, from->file_id, false))
            into->errorCount++;
    }

    darray_enumerate(idx, ledi, from->led_names) {
        if (ledi->name == XKB_ATOM_NONE)
            continue;

        ledi->merge = (merge == MERGE_DEFAULT ? ledi->merge : merge);
        if (!AddLedName(into, ledi->merge, ledi, idx))
            into->errorCount++;
    }

    if (!MergeAliases(into, from, merge))
        into->errorCount++;
}

static void
HandleKeycodesFile(KeyNamesInfo *info, XkbFile *file, enum merge_mode merge);

static bool
HandleIncludeKeycodes(KeyNamesInfo *info, IncludeStmt *include)
{
    KeyNamesInfo included;

    InitKeyNamesInfo(&included, info->ctx, info->file_id);
    included.name = include->stmt;
    include->stmt = NULL;

    for (IncludeStmt *stmt = include; stmt; stmt = stmt->next_incl) {
        KeyNamesInfo next_incl;
        XkbFile *file;

        file = ProcessIncludeFile(info->ctx, stmt, FILE_TYPE_KEYCODES);
        if (!file) {
            info->errorCount += 10;
            ClearKeyNamesInfo(&included);
            return false;
        }

        InitKeyNamesInfo(&next_incl, info->ctx, file->id);

        HandleKeycodesFile(&next_incl, file, MERGE_OVERRIDE);

        MergeIncludedKeycodes(&included, &next_incl, stmt->merge);

        ClearKeyNamesInfo(&next_incl);
        FreeXkbFile(file);
    }

    MergeIncludedKeycodes(info, &included, include->merge);
    ClearKeyNamesInfo(&included);

    return (info->errorCount == 0);
}

static bool
HandleKeycodeDef(KeyNamesInfo *info, KeycodeDef *stmt, enum merge_mode merge)
{
    if (stmt->merge != MERGE_DEFAULT) {
        if (stmt->merge == MERGE_REPLACE)
            merge = MERGE_OVERRIDE;
        else
            merge = stmt->merge;
    }

    if (stmt->value < 0 || stmt->value > XKB_KEYCODE_MAX) {
        log_err(info->ctx,
                "Illegal keycode %lld: must be between 0..%u; "
                "Key ignored\n", (long long) stmt->value, XKB_KEYCODE_MAX);
        return false;
    }

    return AddKeyName(info, stmt->value, stmt->name, merge,
                      info->file_id, true);
}

static int
HandleAliasDef(KeyNamesInfo *info, KeyAliasDef *def, enum merge_mode merge)
{
    AliasInfo *old, new;

    darray_foreach(old, info->aliases) {
        if (old->alias == def->alias) {
            if (def->real == old->real) {
                log_vrb(info->ctx, 1,
                        "Alias of %s for %s declared more than once; "
                        "First definition ignored\n",
                        KeyNameText(info->ctx, def->alias),
                        KeyNameText(info->ctx, def->real));
            }
            else {
                xkb_atom_t use, ignore;

                use = (merge == MERGE_AUGMENT ? old->real : def->real);
                ignore = (merge == MERGE_AUGMENT ? def->real : old->real);

                log_warn(info->ctx,
                         "Multiple definitions for alias %s; "
                         "Using %s, ignoring %s\n",
                         KeyNameText(info->ctx, old->alias),
                         KeyNameText(info->ctx, use),
                         KeyNameText(info->ctx, ignore));

                old->real = use;
            }

            old->merge = merge;
            return true;
        }
    }

    InitAliasInfo(&new, merge, def->alias, def->real);
    darray_append(info->aliases, new);
    return true;
}

static int
HandleKeyNameVar(KeyNamesInfo *info, VarDef *stmt)
{
    const char *elem, *field;
    ExprDef *arrayNdx;

    if (!ExprResolveLhs(info->ctx, stmt->name, &elem, &field, &arrayNdx))
        return false;

    if (elem) {
        log_err(info->ctx, "Unknown element %s encountered; "
                "Default for field %s ignored\n", elem, field);
        return false;
    }

    if (!istreq(field, "minimum") && !istreq(field, "maximum")) {
        log_err(info->ctx, "Unknown field encountered; "
                "Assignment to field %s ignored\n", field);
        return false;
    }

    /* We ignore explicit min/max statements, we always use computed. */
    return true;
}

static int
HandleLedNameDef(KeyNamesInfo *info, LedNameDef *def,
                 enum merge_mode merge)
{
    LedNameInfo ledi;
    xkb_atom_t name;

    if (def->ndx < 1 || def->ndx > XKB_MAX_LEDS) {
        info->errorCount++;
        log_err(info->ctx,
                "Illegal indicator index (%d) specified; must be between 1 .. %d; "
                "Ignored\n", def->ndx, XKB_MAX_LEDS);
        return false;
    }

    if (!ExprResolveString(info->ctx, def->name, &name)) {
        char buf[20];
        snprintf(buf, sizeof(buf), "%d", def->ndx);
        info->errorCount++;
        return ReportBadType(info->ctx, "indicator", "name", buf, "string");
    }

    ledi.merge = info->merge;
    ledi.file_id = info->file_id;
    ledi.name = name;
    return AddLedName(info, merge, &ledi, def->ndx - 1);
}

static void
HandleKeycodesFile(KeyNamesInfo *info, XkbFile *file, enum merge_mode merge)
{
    bool ok;

    free(info->name);
    info->name = strdup_safe(file->name);

    for (ParseCommon *stmt = file->defs; stmt; stmt = stmt->next) {
        switch (stmt->type) {
        case STMT_INCLUDE:
            ok = HandleIncludeKeycodes(info, (IncludeStmt *) stmt);
            break;
        case STMT_KEYCODE:
            ok = HandleKeycodeDef(info, (KeycodeDef *) stmt, merge);
            break;
        case STMT_ALIAS:
            ok = HandleAliasDef(info, (KeyAliasDef *) stmt, merge);
            break;
        case STMT_VAR:
            ok = HandleKeyNameVar(info, (VarDef *) stmt);
            break;
        case STMT_LED_NAME:
            ok = HandleLedNameDef(info, (LedNameDef *) stmt, merge);
            break;
        default:
            log_err(info->ctx,
                    "Keycode files may define key and indicator names only; "
                    "Ignoring %s\n", stmt_type_to_string(stmt->type));
            ok = false;
            break;
        }

        if (!ok)
            info->errorCount++;

        if (info->errorCount > 10) {
            log_err(info->ctx, "Abandoning keycodes file \"%s\"\n",
                    file->topName);
            break;
        }
    }
}

/***====================================================================***/

static void
CopyAliasesToKeymap(KeyNamesInfo *info, struct xkb_keymap *keymap)
{
    AliasInfo *alias;

    darray_foreach(alias, info->aliases) {
        struct xkb_key *key;
        struct xkb_key_alias new;

        /* Check that ->real is a key. */
        key = FindNamedKey(keymap, alias->real, false);
        if (!key) {
            log_vrb(info->ctx, 5,
                    "Attempt to alias %s to non-existent key %s; Ignored\n",
                    KeyNameText(info->ctx, alias->alias),
                    KeyNameText(info->ctx, alias->real));
            continue;
        }

        /* Check that ->alias is not a key. */
        key = FindNamedKey(keymap, alias->alias, false);
        if (key) {
            log_vrb(info->ctx, 5,
                    "Attempt to create alias with the name of a real key; "
                    "Alias \"%s = %s\" ignored\n",
                    KeyNameText(info->ctx, alias->alias),
                    KeyNameText(info->ctx, alias->real));
            continue;
        }

        /* Add the alias. */
        new.alias = alias->alias;
        new.real = alias->real;
        darray_append(keymap->key_aliases, new);
    }

    darray_free(info->aliases);
}

static bool
CopyKeyNamesToKeymap(struct xkb_keymap *keymap, KeyNamesInfo *info)
{
    xkb_keycode_t kc;
    xkb_led_index_t idx;
    LedNameInfo *ledi;

    keymap->keys = calloc(info->max_key_code + 1, sizeof(*keymap->keys));
    if (!keymap->keys)
        return false;

    keymap->min_key_code = info->min_key_code;
    keymap->max_key_code = info->max_key_code;

    for (kc = info->min_key_code; kc <= info->max_key_code; kc++) {
        keymap->keys[kc].keycode = kc;
        keymap->keys[kc].name = darray_item(info->key_names, kc).name;
    }

    keymap->keycodes_section_name = strdup_safe(info->name);

    darray_resize0(keymap->leds, darray_size(info->led_names));
    darray_enumerate(idx, ledi, info->led_names) {
        if (ledi->name == XKB_ATOM_NONE)
            continue;

        darray_item(keymap->leds, idx).name = ledi->name;
    }

    CopyAliasesToKeymap(info, keymap);

    return true;
}

/***====================================================================***/

bool
CompileKeycodes(XkbFile *file, struct xkb_keymap *keymap,
                enum merge_mode merge)
{
    KeyNamesInfo info;

    InitKeyNamesInfo(&info, keymap->ctx, file->id);

    HandleKeycodesFile(&info, file, merge);
    if (info.errorCount != 0)
        goto err_info;

    if (!CopyKeyNamesToKeymap(keymap, &info))
        goto err_info;

    ClearKeyNamesInfo(&info);
    return true;

err_info:
    ClearKeyNamesInfo(&info);
    return false;
}

/***====================================================================***/

struct xkb_key *
FindNamedKey(struct xkb_keymap *keymap, xkb_atom_t name, bool use_aliases)
{
    struct xkb_key *key;

    xkb_foreach_key(key, keymap)
        if (key->name == name)
            return key;

    if (use_aliases) {
        xkb_atom_t new_name;
        if (FindKeyNameForAlias(keymap, name, &new_name))
            return FindNamedKey(keymap, new_name, false);
    }

    return NULL;
}

bool
FindKeyNameForAlias(struct xkb_keymap *keymap, xkb_atom_t name,
                    xkb_atom_t *real_name)
{
    const struct xkb_key_alias *a;

    darray_foreach(a, keymap->key_aliases) {
        if (name == a->alias) {
            *real_name = a->real;
            return true;
        }
    }

    return false;
}
