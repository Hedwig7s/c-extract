// BEHOLD my SLOP (extract.c is hand-written though)
/*
 * extract_version.c
 *
 * C port of extract_version.py.
 *
 * Build (example):
 *   gcc extract_version.c main.c -o extract_version \
 *       $(pkg-config --cflags --libs glib-2.0 gio-2.0) \
 *       -lyyjson -lzip
 *
 * Reads version_list.txt from the current directory and interactively
 * prompts the user to choose a version to extract.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>  /* QueryPerformanceCounter/Frequency */
#else
#  include <time.h>     /* clock_gettime, CLOCK_MONOTONIC */
#endif

#include <glib.h>
#include <glib/gstdio.h>

#include "extract.h"

/* --------------------------------------------------------------------------
 * Types
 * -------------------------------------------------------------------------- */

/* A single depot entry: [archive, depot_id, manifest_id] */
typedef struct {
    char archive[64];
    char depot_id[32];
    char manifest_id[32];
} DepotEntry;

/* One parsed version with its list of depot entries */
typedef struct {
    char  version[128];   /* may have a _N dedup suffix */
    GArray *entries;      /* GArray of DepotEntry */
} VersionEntry;

/* The full version dictionary – an ordered list plus a hash for O(1) lookup */
typedef struct {
    GPtrArray  *order;    /* GPtrArray<VersionEntry*>, preserves insertion order */
    GHashTable *table;    /* version string -> VersionEntry* */
} VersionDict;

/* --------------------------------------------------------------------------
 * Shared-assets table (mirrors the Python dict, insertion order matters)
 * -------------------------------------------------------------------------- */

typedef struct {
    const char *version;
    const char *archive;
    const char *depot_id;
    const char *manifest_id;
} SharedAsset;

static const SharedAsset SHARED_ASSETS[] = {
    { "1.0.0.0", "srcbase", "305", "0" },
    { "1.0.3.2", "srcbase", "305", "1" },
    { "1.0.9.1", "srcbase", "305", "2" },
    { "1.0.9.3", "srcbase", "305", "3" },
    { "1.0.9.4", "srcbase", "305", "4" },
    { "1.0.9.5", "srcbase", "305", "5" },
    { NULL, NULL, NULL, NULL }  /* sentinel */
};
static const int NUM_SHARED_ASSETS =
    (int)(sizeof(SHARED_ASSETS) / sizeof(SHARED_ASSETS[0])) - 1;

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/* Returns 1 if version string starts with a digit followed by a dot */
static int is_version_line(const char *line) {
    return isdigit((unsigned char)line[0]) && strchr(line, '.') != NULL;
}

/*
 * is_newer(v1, v2) – returns 1 if v1 > v2, 0 otherwise.
 * Mirrors the Python implementation: compare dot-separated integer parts,
 * with a longer version winning on a tie.
 */
static int is_newer(const char *v1, const char *v2) {
    char **p1 = g_strsplit(v1, ".", -1);
    char **p2 = g_strsplit(v2, ".", -1);

    guint len1 = g_strv_length(p1);
    guint len2 = g_strv_length(p2);
    guint len  = MIN(len1, len2);

    int result  = 0;
    int decided = 0;

    for (guint i = 0; i < len; i++) {
        int n1 = atoi(p1[i]);
        int n2 = atoi(p2[i]);
        if (n1 > n2) { result = 1; decided = 1; break; }
        if (n1 < n2) { result = 0; decided = 1; break; }
    }
    if (!decided)
        result = (len1 > len2) ? 1 : 0;

    g_strfreev(p1);
    g_strfreev(p2);
    return result;
}

/*
 * Extract the numeric version prefix from a possibly-suffixed version string
 * (e.g. "1.0.9.5_2" → "1.0.9.5").
 * Returns a newly-allocated string; caller must g_free().
 */
static char *base_version(const char *version) {
    /* Find the first character that is neither a digit nor a dot */
    const char *p = version;
    while (*p && (isdigit((unsigned char)*p) || *p == '.'))
        p++;
    return g_strndup(version, (gsize)(p - version));
}

/* Find the shared_asset entry whose version key exactly matches 'version'. */
static const SharedAsset *shared_asset_exact(const char *version) {
    for (int i = 0; SHARED_ASSETS[i].version != NULL; i++) {
        if (strcmp(SHARED_ASSETS[i].version, version) == 0)
            return &SHARED_ASSETS[i];
    }
    return NULL;
}

/*
 * Find the appropriate shared asset for a given game version, replicating
 * the Python logic:
 *
 *   for v in shared_assets:          # in insertion order
 *       if is_newer(v, base_version):
 *           break
 *       else:
 *           shared_version = v
 *
 * i.e. the last shared asset that is NOT newer than base_version.
 * Returns NULL if none qualifies (shouldn't happen in practice).
 */
static const SharedAsset *find_shared_asset(const char *game_version) {
    char *bv = base_version(game_version);
    const SharedAsset *result = NULL;

    for (int i = 0; i < NUM_SHARED_ASSETS; i++) {
        if (is_newer(SHARED_ASSETS[i].version, bv)) {
            break;
        }
        result = &SHARED_ASSETS[i];
    }

    g_free(bv);
    return result;
}

/* --------------------------------------------------------------------------
 * Parsing
 * -------------------------------------------------------------------------- */

/*
 * Parse version_list.txt into a VersionDict.
 *
 * File format:
 *   <version>             <- line starting with a digit, e.g. "1.0.0.0"
 *   <archive> <id> <mid> <- data line, e.g. "srcbase 301 7"
 *   ...
 *
 * Duplicate version names get a "_N" suffix (N starts at 2) to match the
 * Python dedup logic.
 */
static VersionDict *file_to_dict(const char *text) {
    VersionDict *vd = g_new0(VersionDict, 1);
    vd->order = g_ptr_array_new();
    vd->table = g_hash_table_new(g_str_hash, g_str_equal);

    /* dedup counter: version string -> int (via GINT_TO_POINTER) */
    GHashTable *version_count = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    char **lines = g_strsplit(text, "\n", -1);
    VersionEntry *current = NULL;

    for (int i = 0; lines[i] != NULL; i++) {
        char *line = g_strstrip(lines[i]);
        if (*line == '\0') continue;

        if (is_version_line(line)) {
            /* Take the first whitespace-delimited token as the version */
            char **tokens = g_strsplit_set(line, " \t", 2);
            const char *raw_ver = tokens[0];

            /* Dedup */
            gpointer cnt_ptr = g_hash_table_lookup(version_count, raw_ver);
            int cnt = GPOINTER_TO_INT(cnt_ptr);
            char *ver_key;
            if (cnt == 0) {
                ver_key = g_strdup(raw_ver);
                g_hash_table_insert(version_count, g_strdup(raw_ver), GINT_TO_POINTER(1));
            } else {
                cnt++;
                ver_key = g_strdup_printf("%s_%d", raw_ver, cnt);
                g_hash_table_replace(version_count, g_strdup(raw_ver), GINT_TO_POINTER(cnt));
            }

            current = g_new0(VersionEntry, 1);
            g_strlcpy(current->version, ver_key, sizeof(current->version));
            current->entries = g_array_new(FALSE, TRUE, sizeof(DepotEntry));

            g_ptr_array_add(vd->order, current);
            g_hash_table_insert(vd->table, current->version, current);

            g_free(ver_key);
            g_strfreev(tokens);

        } else if (current != NULL) {
            /* Data line: "archive depot_id manifest_id" */
            char archive[64] = {0};
            char depot_id[32] = {0};
            char manifest_id[32] = {0};

            if (sscanf(line, "%63s %31s %31s", archive, depot_id, manifest_id) == 3 &&
                isdigit((unsigned char)depot_id[0]) &&
                isdigit((unsigned char)manifest_id[0])) {

                DepotEntry entry;
                g_strlcpy(entry.archive,     archive,     sizeof(entry.archive));
                g_strlcpy(entry.depot_id,    depot_id,    sizeof(entry.depot_id));
                g_strlcpy(entry.manifest_id, manifest_id, sizeof(entry.manifest_id));
                g_array_append_val(current->entries, entry);
            }
        }
    }

    g_strfreev(lines);
    g_hash_table_destroy(version_count);
    return vd;
}

/* --------------------------------------------------------------------------
 * Extraction
 * -------------------------------------------------------------------------- */

/*
 * Call extract_main() for one depot entry.
 *
 * Mirrors Python's extract_depot():
 *   data.append("")         <- path_filter (empty = extract everything)
 *   data.append(folder)     <- output directory
 *   extract.main(data)
 */
static int extract_depot(const DepotEntry *entry, const char *folder) {
    /* argv[0] is a dummy program name */
    char *argv[7];
    argv[0] = (char *)"extract_version";
    argv[1] = (char *)entry->archive;
    argv[2] = (char *)entry->depot_id;
    argv[3] = (char *)entry->manifest_id;
    argv[4] = (char *)"";       /* path_filter: empty = no filter */
    argv[5] = (char *)folder;
    argv[6] = NULL;

    return extract_main(6, argv);
}

/*
 * Prepend a DepotEntry built from (archive, depot_id, manifest_id) at
 * position 0 of the GArray.
 */
static void prepend_entry(GArray *entries,
                          const char *archive,
                          const char *depot_id,
                          const char *manifest_id) {
    DepotEntry e;
    g_strlcpy(e.archive,     archive,     sizeof(e.archive));
    g_strlcpy(e.depot_id,    depot_id,    sizeof(e.depot_id));
    g_strlcpy(e.manifest_id, manifest_id, sizeof(e.manifest_id));
    g_array_prepend_val(entries, e);
}

/* --------------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------------- */

int main(void) {
    /* ---- Read version_list.txt ---- */
    GError *gerr = NULL;
    char *text = NULL;
    gsize text_len = 0;
    if (!g_file_get_contents("version_list.txt", &text, &text_len, &gerr)) {
        g_printerr("Failed to read version_list.txt: %s\n", gerr->message);
        g_error_free(gerr);
        return 1;
    }

    VersionDict *vd = file_to_dict(text);
    g_free(text);

    /* ---- List available versions ---- */
    for (guint i = 0; i < vd->order->len; i++) {
        VersionEntry *ve = g_ptr_array_index(vd->order, i);
        printf("Version %s\n", ve->version);
    }

    /* ---- Interactive loop ---- */
    char input[256];
    while (1) {
        printf("Enter version: ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) {
            /* EOF */
            break;
        }
        /* Strip trailing newline */
        input[strcspn(input, "\r\n")] = '\0';

        VersionEntry *ve = g_hash_table_lookup(vd->table, input);
        if (!ve) {
            printf("Invalid version\n");
            continue;
        }

        /* ---- Determine install path ---- */
        char *cwd = g_get_current_dir();
        char *install_path = g_build_filename(cwd, ve->version, NULL);
        /* Ensure trailing separator so it's treated as a directory prefix */
        char *install_path_sep = g_strdup_printf("%s%c", install_path, G_DIR_SEPARATOR);
        g_free(cwd);
        g_free(install_path);

        /* ---- Determine shared asset version ---- */
        const SharedAsset *sa = shared_asset_exact(ve->version);
        if (sa == NULL) {
            sa = find_shared_asset(ve->version);
        }
        if (sa == NULL) {
            g_printerr("Could not determine shared asset for version '%s'\n", ve->version);
            g_free(install_path_sep);
            break;
        }

        /* ---- Prepend fixed depots (last prepend ends up first, so reverse order) ---- */
        prepend_entry(ve->entries, "srcbase", "307", "0");
        prepend_entry(ve->entries, "srcbase", "306", "0");
        prepend_entry(ve->entries, sa->archive, sa->depot_id, sa->manifest_id);
        prepend_entry(ve->entries, "srcbase", "208", "4");
        prepend_entry(ve->entries, "srcbase", "207", "4");
        prepend_entry(ve->entries, "srcbase", "206", "8");

        /* ---- Debug print ---- */
        printf("[");
        for (guint i = 0; i < ve->entries->len; i++) {
            DepotEntry *e = &g_array_index(ve->entries, DepotEntry, i);
            if (i > 0) printf(", ");
            printf("['%s', '%s', '%s']", e->archive, e->depot_id, e->manifest_id);
        }
        printf("]\n");

        /* ---- Extract all depots ---- */
#ifdef _WIN32
        LARGE_INTEGER qpf, qpc_start, qpc_end;
        QueryPerformanceFrequency(&qpf);
        QueryPerformanceCounter(&qpc_start);
#else
        struct timespec ts_start, ts_end;
        clock_gettime(CLOCK_MONOTONIC, &ts_start);
#endif

        int ret = 0;
        for (guint i = 0; i < ve->entries->len; i++) {
            DepotEntry *e = &g_array_index(ve->entries, DepotEntry, i);
            ret = extract_depot(e, install_path_sep);
            if (ret != 0) {
                g_printerr("Extraction failed for depot %s/%s (code %d)\n",
                           e->depot_id, e->manifest_id, ret);
                break;
            }
        }

#ifdef _WIN32
        QueryPerformanceCounter(&qpc_end);
        double elapsed = (double)(qpc_end.QuadPart - qpc_start.QuadPart)
                       / (double)qpf.QuadPart;
#else
        clock_gettime(CLOCK_MONOTONIC, &ts_end);
        double elapsed = (ts_end.tv_sec  - ts_start.tv_sec)
                       + (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
#endif
        printf("%.6f\n", elapsed);

        /* ---- Write steam_appid.txt ---- */
        if (ret == 0) {
            char *appid_path = g_build_filename(install_path_sep, "steam_appid.txt", NULL);
            FILE *fp = g_fopen(appid_path, "w");
            if (fp) { fputs("440", fp); fclose(fp); }
            else g_printerr("Warning: could not write %s\n", appid_path);
            g_free(appid_path);

            /* ---- Write start.bat ---- */
            char *bat_path = g_build_filename(install_path_sep, "start.bat", NULL);
            fp = g_fopen(bat_path, "w");
            if (fp) { fputs("hl2.exe -game tf -console -novid", fp); fclose(fp); }
            else g_printerr("Warning: could not write %s\n", bat_path);
            g_free(bat_path);
        }

        g_free(install_path_sep);
        break;
    }

    /* ---- Cleanup ---- */
    for (guint i = 0; i < vd->order->len; i++) {
        VersionEntry *ve = g_ptr_array_index(vd->order, i);
        g_array_free(ve->entries, TRUE);
        g_free(ve);
    }
    g_ptr_array_free(vd->order, TRUE);
    g_hash_table_destroy(vd->table);
    g_free(vd);

    return 0;
}
