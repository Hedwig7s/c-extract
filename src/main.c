#include <cjson/cJSON.h>
#include <errno.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <zip.h>
#include <zipconf.h>
#define STB_DS_IMPLEMENTATION
#include <../include/stb_ds.h>

#define BUFFER_SIZE (1024*1024)

typedef struct {
  char *key;
  int value;
} path_cache_map;

void free_created_dirs(path_cache_map *created_dirs) {
  if (created_dirs) {
    for (size_t i = 0; i < shlenu(created_dirs); i++) {
      g_free(created_dirs[i].key);
    }
    shfree(created_dirs);
  }
}

int string_to_int(const char *str, int *out) {
  char *endptr;
  long value;

  errno = 0;
  value = strtol(str, &endptr, 10);

  if (errno == ERANGE || value > INT_MAX || value < INT_MIN) {
    return 0; // overflow or underflow
  }

  // Check if no digits were found
  if (endptr == str) {
    return 0;
  }

  if (*endptr != '\0') {
    return 0;
  }

  *out = (int)value;
  return 1;
}

int starts_with(const char *str, const char *prefix, const size_t len_pre) {
  size_t len_str = strlen(str);

  if (len_pre > len_str) {
    return 0;
  }

  return strncmp(str, prefix, len_pre) == 0;
}

void print_zip_error(int ze, const char *action, char *mz_name) {
  zip_error_t error;
  zip_error_init_with_code(&error, ze);
  g_printerr("Failed to %s archive '%s': %s\n", action, mz_name,
             zip_error_strerror(&error));
  zip_error_fini(&error);
}
int extract(cJSON *file_object, zip_t *zipf, const char *subdir,
            const char *filename, path_cache_map **created_dirs, int *chunk_zip_error, char *chunk_zip_name, char *read_buffer) {
  static char out_path[PATH_MAX];
  snprintf(out_path, sizeof(out_path), "%s%c%s", subdir, G_DIR_SEPARATOR,
           filename);

  cJSON *flags = cJSON_GetObjectItemCaseSensitive(file_object, "flags");
  if (!cJSON_IsNumber(flags)) {
    return -1;
  }
  if (flags->valueint & 64) { // it's a folder
    g_mkdir_with_parents(out_path, 0755);
    return 0;
  }

  char *out_dir = g_path_get_dirname(out_path);
  if (shgeti(*created_dirs, out_dir) < 0) {
    g_mkdir_with_parents(out_dir, 0755);
    shput(*created_dirs, g_strdup(out_dir), 1);
  }
  g_free(out_dir);

  FILE *fp = g_fopen(out_path, "wb");
  if (!fp) {
    g_printerr("Failed to open %s\n", out_path);

    return -2;
  }

  putc('\r', stderr);
  fputs("\33[2K\r", stderr);
  fputs(filename, stderr);
  fflush(stderr);

  cJSON *chunks = cJSON_GetObjectItemCaseSensitive(file_object, "chunks");
  if (!cJSON_IsArray(chunks)) {
    g_printerr("\nChunks in item %s is not an array\n", filename);
    fclose(fp);

    return -3;
  }
  cJSON *chunk_item = NULL;
  cJSON_ArrayForEach(chunk_item, chunks) {
    if (!cJSON_IsString(chunk_item)) {
      g_printerr("\n%s contains non-string chunk\n", filename);
      fclose(fp);

      return -4;
    }
    const char *chunk = chunk_item->valuestring;
    if (!strcmp(chunk, "0000000000000000000000000000000000000000")) {
      break;
    }
    static char path_template[44]; // XX/XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
    memcpy(path_template, chunk, 2);
    path_template[2] = G_DIR_SEPARATOR;
    memcpy(path_template + 3, chunk, 40);
    path_template[43] = '\0';
    zip_file_t *chunk_file = zip_fopen(zipf, path_template, ZIP_RDONLY);
    if (chunk_file == NULL) {
      print_zip_error(*chunk_zip_error, "open chunk in", chunk_zip_name);
      fclose(fp);

      return -5;
    }
    int read = 0;
    do {
      read = zip_fread(chunk_file, read_buffer, BUFFER_SIZE);
      if (read < 0) {
        print_zip_error(*chunk_zip_error, "read chunk in", chunk_zip_name);
        return -6;
      }
      int wrote = fwrite(read_buffer, sizeof(char), read, fp);
      if (wrote != read) {
        g_printerr("\nFailed to write to output file, expected to write %d, "
                   "wrote %d\n",
                   read, wrote);
        fflush(fp);
        fclose(fp);

        return -7;
      }
    } while (read == BUFFER_SIZE);
  }

  fflush(fp);
  fclose(fp);
  return 0;
}

int main(int argc, char *argv[]) {
  if (argc < 4 || argc > 6) {
    g_printerr(
        "Usage: %s archive_name depot_id manifest_id [path_filter] [output_path]\n\
Examples:\n\
    %s tf2 441 5 tf/maps/ tf2_maps\n\
    %s engine 216 0\n",
        argv[0], argv[0], argv[0]);
    return 1;
  }
  const char *archive = argv[1];
  int depot_id, manifest_id;
  int success = string_to_int(argv[2], &depot_id);
  if (!success) {
    puts("depot_id must be an integer\n");
    return 2;
  }

  success = string_to_int(argv[3], &manifest_id);
  if (!success) {
    puts("manifest_id must be an integer\n");
    return 3;
  }
  const char *root = "";
  if (argc > 4)
    root = argv[4];
  size_t root_len = strlen(root);
  const char *path = "";
  char path_buff[50];
  if (argc > 5) {
    path = argv[5];
  } else {
    snprintf(path_buff, 50, "%d%c%d", depot_id, G_DIR_SEPARATOR, manifest_id);
    path = path_buff;
  }
  char manifest_zip_name[100];
  snprintf(manifest_zip_name, sizeof(manifest_zip_name), "manifests_%s.zip",
           archive);
  int m_ze;
  zip_t *manifest_zip = zip_open(manifest_zip_name, ZIP_RDONLY, &m_ze);
  if (manifest_zip == NULL) {
    print_zip_error(m_ze, "open", manifest_zip_name);
    return 4;
  }
  char manifest_json_name[50];
  snprintf(manifest_json_name, sizeof(manifest_json_name), "%d/%d.json",
           depot_id, manifest_id);
  zip_stat_t manifest_stat;
  int zse = zip_stat(manifest_zip, manifest_json_name, 0, &manifest_stat);
  if (zse != 0) {
    print_zip_error(zse, "stat manifest in", manifest_zip_name);
    return 5;
  }
  zip_file_t *manifest_file =
      zip_fopen_index(manifest_zip, manifest_stat.index, 0);
  if (manifest_file == NULL) {
    print_zip_error(m_ze, "open manifest in", manifest_zip_name);
    return 6;
  }
  char *manifest_data = g_malloc(manifest_stat.size + 1);
  if (!manifest_data) {
    g_printerr("Failed to allocate memory for manifest\n");
    zip_fclose(manifest_file);
    zip_close(manifest_zip);
    return 11;
  }
  zip_int64_t read =
      zip_fread(manifest_file, manifest_data, manifest_stat.size);
  zip_fclose(manifest_file);
  zip_close(manifest_zip);
  if (read != (zip_int64_t)manifest_stat.size) {
    g_printerr("Size mismatch reading manifest. Expected %lu, got %lu\n",
               manifest_stat.size, read);
    g_free(manifest_data);
    return 7;
  }
  manifest_data[manifest_stat.size] = '\0';
  cJSON *manifest_json = cJSON_Parse(manifest_data);
  g_free(manifest_data);
  if (manifest_json == NULL) {
    const char *error_ptr = cJSON_GetErrorPtr();
    if (error_ptr != NULL) {
      g_printerr("Error loading manifest json: %s\n", error_ptr);
    }
    return 8;
  }
  cJSON *name = cJSON_GetObjectItemCaseSensitive(manifest_json, "name");

  if (cJSON_IsString(name)) {
    printf("Extracting from %s v%d\n", name->valuestring, manifest_id);
  }

  char chunk_zip_name[100];
  snprintf(chunk_zip_name, sizeof(chunk_zip_name), "chunks_%s.zip", archive);
  int cze;
  zip_t *chunk_zip = zip_open(chunk_zip_name, ZIP_RDONLY, &cze);
  if (chunk_zip == NULL) {
    print_zip_error(cze, "open", chunk_zip_name);
    return 9;
  }

  path_cache_map *created_dirs = NULL;

  cJSON *files = cJSON_GetObjectItemCaseSensitive(manifest_json, "files");

  if (!cJSON_IsObject(files)) {
    g_printerr("files is not an object in manifest. Files: %s\n",
               cJSON_Print(files));
    return 10;
  }
  cJSON *item = NULL;
  int ret = 0;
  char read_buffer[BUFFER_SIZE];
  cJSON_ArrayForEach(item, files) {
    const char *filename = item->string;
    if (!starts_with(filename, root, root_len)) {
      continue;
    }
    if (!cJSON_IsObject(item)) {
      g_printerr("WARNING: Entry %s in files is not an object\n", filename);
      continue;
    }
    ret = extract(item, chunk_zip, path, filename, &created_dirs,
                  &cze, chunk_zip_name, read_buffer);
    if (ret != 0) {
      break;
    }
  }
  free_created_dirs(created_dirs);
  cJSON_Delete(manifest_json);
  zip_close(chunk_zip);

  putc('\n', stderr);
  return ret;
}
