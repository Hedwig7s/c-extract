#include "yyjson.h"
#include <errno.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <zip.h>
#include <zipconf.h>

#include <extract.h>

#define BUFFER_SIZE (1024*1024)

int string_to_int(const char *str, int *out) {
    char *endptr;
    long value;
    errno = 0;
    value = strtol(str, &endptr, 10);
    if (errno == ERANGE || value > INT_MAX || value < INT_MIN || endptr == str || *endptr != '\0') {
        return 0;
    }
    *out = (int)value;
    return 1;
}

int starts_with(const char *str, const char *prefix, const size_t len_pre) {
    return strncmp(str, prefix, len_pre) == 0;
}

void print_zip_error(int ze, const char *action, char *mz_name) {
    zip_error_t error;
    zip_error_init_with_code(&error, ze);
    g_printerr("Failed to %s archive '%s': %s\n", action, mz_name, zip_error_strerror(&error));
    zip_error_fini(&error);
}

int extract(yyjson_val *file_object, zip_t *zipf, const char *subdir,
            const char *filename, GHashTable *dir_cache, int *chunk_zip_error,
            char *chunk_zip_name, char *read_buffer) {

    char *out_path = g_build_filename(subdir, filename, NULL);

    int flags = yyjson_get_int(yyjson_obj_get(file_object, "flags"));
    if (flags & 64) { // it's a folder
        g_mkdir_with_parents(out_path, 0755);
        g_free(out_path);
        return 0;
    }

    char *out_dir = g_path_get_dirname(out_path);
    if (!g_hash_table_contains(dir_cache, out_dir)) {
        g_mkdir_with_parents(out_dir, 0755);
        g_hash_table_insert(dir_cache, g_strdup(out_dir), GINT_TO_POINTER(1));
    }
    g_free(out_dir);

    FILE *fp = g_fopen(out_path, "wb");
    if (!fp) {
        g_printerr("Failed to open %s\n", out_path);
        g_free(out_path);
        return -2;
    }

    yyjson_val *chunks = yyjson_obj_get(file_object, "chunks");
    if (!yyjson_is_arr(chunks)) {
        g_printerr("\nChunks in item %s is not an array\n", filename);
        fclose(fp);
        g_free(out_path);
        return -3;
    }

    yyjson_val *chunk_item;
    size_t idx, max;
    yyjson_arr_foreach(chunks, idx, max, chunk_item) {
        const char *chunk = yyjson_get_str(chunk_item);
        if (!chunk || !strcmp(chunk, "0000000000000000000000000000000000000000")) {
            break;
        }

        static char path_template[44];
        memcpy(path_template, chunk, 2);
        path_template[2] = G_DIR_SEPARATOR;
        memcpy(path_template + 3, chunk, 40);
        path_template[43] = '\0';

        zip_file_t *chunk_file = zip_fopen(zipf, path_template, ZIP_RDONLY);
        if (chunk_file == NULL) {
            print_zip_error(*chunk_zip_error, "open chunk in", chunk_zip_name);
            fclose(fp);
            g_free(out_path);
            return -5;
        }

        zip_int64_t read_bytes;
        while ((read_bytes = zip_fread(chunk_file, read_buffer, BUFFER_SIZE)) > 0) {
            if (fwrite(read_buffer, 1, read_bytes, fp) != (size_t)read_bytes) {
                g_printerr("\nFailed to write to output file %s\n", filename);
                zip_fclose(chunk_file);
                fclose(fp);
                g_free(out_path);
                return -7;
            }
        }
        zip_fclose(chunk_file);
    }

    fclose(fp);
    g_free(out_path);
    return 0;
}

int extract_main(int argc, char *argv[]) {
    if (argc < 4 || argc > 6) {
        g_printerr("Usage: %s archive_name depot_id manifest_id [path_filter] [output_path]\n", argv[0]);
        return 1;
    }

    const char *archive = argv[1];
    int depot_id, manifest_id;
    if (!string_to_int(argv[2], &depot_id) || !string_to_int(argv[3], &manifest_id)) {
        puts("depot_id and manifest_id must be integers\n");
        return 2;
    }

    const char *root = (argc > 4) ? argv[4] : "";
    size_t root_len = strlen(root);
    char path_buff[50];
    const char *out_base_path = (argc > 5) ? argv[5] : (snprintf(path_buff, 50, "%d%c%d", depot_id, G_DIR_SEPARATOR, manifest_id), path_buff);

    char manifest_zip_name[100];
    snprintf(manifest_zip_name, sizeof(manifest_zip_name), "manifests_%s.zip", archive);

    int m_ze;
    zip_t *manifest_zip = zip_open(manifest_zip_name, ZIP_RDONLY, &m_ze);
    if (!manifest_zip) {
        print_zip_error(m_ze, "open", manifest_zip_name);
        return 4;
    }

    char manifest_json_name[50];
    snprintf(manifest_json_name, sizeof(manifest_json_name), "%d/%d.json", depot_id, manifest_id);

    zip_stat_t manifest_stat;
    if (zip_stat(manifest_zip, manifest_json_name, 0, &manifest_stat) != 0) {
        g_printerr("Manifest not found in zip\n");
        zip_close(manifest_zip);
        return 5;
    }

    zip_file_t *manifest_file = zip_fopen_index(manifest_zip, manifest_stat.index, 0);
    char *manifest_data = g_malloc(manifest_stat.size);
    zip_fread(manifest_file, manifest_data, manifest_stat.size);
    zip_fclose(manifest_file);
    zip_close(manifest_zip);

    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_opts(manifest_data, manifest_stat.size, 0, NULL, &err);
    g_free(manifest_data);

    if (!doc) {
        g_printerr("JSON Error: %s at %zu\n", err.msg, err.pos);
        return 8;
    }

    yyjson_val *manifest_root = yyjson_doc_get_root(doc);
    yyjson_val *files = yyjson_obj_get(manifest_root, "files");

    char chunk_zip_name[100];
    snprintf(chunk_zip_name, sizeof(chunk_zip_name), "chunks_%s.zip", archive);
    int cze;
    zip_t *chunk_zip = zip_open(chunk_zip_name, ZIP_RDONLY, &cze);

    GHashTable *dir_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *read_buffer = g_malloc(BUFFER_SIZE);

    yyjson_val *key, *val;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(files, &iter);

    int ret = 0;
    int count = 0;

    while ((key = yyjson_obj_iter_next(&iter))) {
        val = yyjson_obj_iter_get_val(key);
        const char *filename = yyjson_get_str(key);

        if (!starts_with(filename, root, root_len)) continue;

        ret = extract(val, chunk_zip, out_base_path, filename, dir_cache, &cze, chunk_zip_name, read_buffer);

        if (++count % 100 == 0) {
            fprintf(stderr, "\rExtracted %d files...", count);
        }

        if (ret != 0) break;
    }

    g_hash_table_destroy(dir_cache);
    yyjson_doc_free(doc);
    zip_close(chunk_zip);
    g_free(read_buffer);

    fprintf(stderr, "\nDone. Total files: %d\n", count);
    return ret;
}
