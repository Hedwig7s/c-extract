#ifndef EXTRACT_H
#define EXTRACT_H

/*
 * extract_main - extract files from a depot archive.
 *
 * Mirrors the original main() signature so it can be used as a library call.
 *
 * argv layout (argc must be 4, 5, or 6):
 *   argv[1]  archive_name   - base name used to build "manifests_<archive>.zip"
 *                             and "chunks_<archive>.zip"
 *   argv[2]  depot_id       - integer depot identifier
 *   argv[3]  manifest_id    - integer manifest identifier
 *   argv[4]  path_filter    - (optional) only extract paths with this prefix;
 *                             pass "" to extract everything
 *   argv[5]  output_path    - (optional) directory to write files into;
 *                             defaults to "<depot_id>/<manifest_id>"
 *
 * Returns 0 on success, non-zero on failure (same codes as the original main).
 */
int extract_main(int argc, char *argv[]);

#endif /* EXTRACT_H */
