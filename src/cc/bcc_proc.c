/*
 * Copyright (c) 2016 GitHub, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <stdio.h>
#include <math.h>

#include "bcc_perf_map.h"
#include "bcc_proc.h"
#include "bcc_elf.h"

static bool is_exe(const char *path) {
  struct stat s;
  if (access(path, X_OK) < 0)
    return false;

  if (stat(path, &s) < 0)
    return false;

  return S_ISREG(s.st_mode);
}

char *bcc_procutils_which(const char *binpath) {
  char buffer[4096];
  const char *PATH;

  if (strchr(binpath, '/'))
    return is_exe(binpath) ? strdup(binpath) : 0;

  if (!(PATH = getenv("PATH")))
    return 0;

  while (PATH) {
    const char *next = strchr(PATH, ':') ?: strchr(PATH, '\0');
    const size_t path_len = next - PATH;

    if (path_len) {
      memcpy(buffer, PATH, path_len);
      buffer[path_len] = '/';
      strcpy(buffer + path_len + 1, binpath);

      if (is_exe(buffer))
        return strdup(buffer);
    }

    PATH = *next ? (next + 1) : 0;
  }

  return 0;
}

int bcc_mapping_is_file_backed(const char *mapname) {
  return mapname[0] &&
    strncmp(mapname, "//anon", sizeof("//anon") - 1) &&
    strncmp(mapname, "/dev/zero", sizeof("/dev/zero") - 1) &&
    strncmp(mapname, "/anon_hugepage", sizeof("/anon_hugepage") - 1) &&
    strncmp(mapname, "[stack", sizeof("[stack") - 1) &&
    strncmp(mapname, "/SYSV", sizeof("/SYSV") - 1) &&
    strncmp(mapname, "[heap]", sizeof("[heap]") - 1);
}

int bcc_procutils_each_module(int pid, bcc_procutils_modulecb callback,
                              void *payload) {
  char procmap_filename[128];
  FILE *procmap;
  int ret;

  sprintf(procmap_filename, "/proc/%ld/maps", (long)pid);
  procmap = fopen(procmap_filename, "r");

  if (!procmap)
    return -1;

  do {
    char endline[4096];
    char perm[8], dev[8];
    long long begin, end, size, inode;

    ret = fscanf(procmap, "%llx-%llx %s %llx %s %lld", &begin, &end, perm,
                 &size, dev, &inode);

    if (!fgets(endline, sizeof(endline), procmap))
      break;

    if (ret == 6) {
      char *mapname = endline;
      char *newline = strchr(endline, '\n');

      if (newline)
        newline[0] = '\0';

      while (isspace(mapname[0])) mapname++;

      if (strchr(perm, 'x') && bcc_mapping_is_file_backed(mapname)) {
        if (callback(mapname, (uint64_t)begin, (uint64_t)end, payload) < 0)
          break;
      }
    }
  } while (ret && ret != EOF);

  fclose(procmap);

  // Add a mapping to /tmp/perf-pid.map for the entire address space. This will
  // be used if symbols aren't resolved in an earlier mapping.
  char map_path[4096];
  if (bcc_perf_map_path(map_path, sizeof(map_path), pid))
    callback(map_path, 0, -1, payload);

  return 0;
}

int bcc_procutils_each_ksym(bcc_procutils_ksymcb callback, void *payload) {
  char line[2048];
  FILE *kallsyms;

  /* root is needed to list ksym addresses */
  if (geteuid() != 0)
    return -1;

  kallsyms = fopen("/proc/kallsyms", "r");
  if (!kallsyms)
    return -1;

  if (!fgets(line, sizeof(line), kallsyms)) {
    fclose(kallsyms);
    return -1;
  }

  while (fgets(line, sizeof(line), kallsyms)) {
    char *symname, *endsym;
    unsigned long long addr;

    addr = strtoull(line, &symname, 16);
    endsym = symname = symname + 3;

    while (*endsym && !isspace(*endsym)) endsym++;

    *endsym = '\0';
    callback(symname, addr, payload);
  }

  fclose(kallsyms);
  return 0;
}

#define CACHE1_HEADER "ld.so-1.7.0"
#define CACHE1_HEADER_LEN (sizeof(CACHE1_HEADER) - 1)

#define CACHE2_HEADER "glibc-ld.so.cache"
#define CACHE2_HEADER_LEN (sizeof(CACHE2_HEADER) - 1)
#define CACHE2_VERSION "1.1"

struct ld_cache1_entry {
  int32_t flags;
  uint32_t key;
  uint32_t value;
};

struct ld_cache1 {
  char header[CACHE1_HEADER_LEN];
  uint32_t entry_count;
  struct ld_cache1_entry entries[0];
};

struct ld_cache2_entry {
  int32_t flags;
  uint32_t key;
  uint32_t value;
  uint32_t pad1_;
  uint64_t pad2_;
};

struct ld_cache2 {
  char header[CACHE2_HEADER_LEN];
  char version[3];
  uint32_t entry_count;
  uint32_t string_table_len;
  uint32_t pad_[5];
  struct ld_cache2_entry entries[0];
};

static int lib_cache_count;
static struct ld_lib {
  char *libname;
  char *path;
  int flags;
} * lib_cache;

static int read_cache1(const char *ld_map) {
  struct ld_cache1 *ldcache = (struct ld_cache1 *)ld_map;
  const char *ldstrings =
      (const char *)(ldcache->entries + ldcache->entry_count);
  uint32_t i;

  lib_cache =
      (struct ld_lib *)malloc(ldcache->entry_count * sizeof(struct ld_lib));
  lib_cache_count = (int)ldcache->entry_count;

  for (i = 0; i < ldcache->entry_count; ++i) {
    const char *key = ldstrings + ldcache->entries[i].key;
    const char *val = ldstrings + ldcache->entries[i].value;
    const int flags = ldcache->entries[i].flags;

    lib_cache[i].libname = strdup(key);
    lib_cache[i].path = strdup(val);
    lib_cache[i].flags = flags;
  }
  return 0;
}

static int read_cache2(const char *ld_map) {
  struct ld_cache2 *ldcache = (struct ld_cache2 *)ld_map;
  uint32_t i;

  if (memcmp(ld_map, CACHE2_HEADER, CACHE2_HEADER_LEN))
    return -1;

  lib_cache =
      (struct ld_lib *)malloc(ldcache->entry_count * sizeof(struct ld_lib));
  lib_cache_count = (int)ldcache->entry_count;

  for (i = 0; i < ldcache->entry_count; ++i) {
    const char *key = ld_map + ldcache->entries[i].key;
    const char *val = ld_map + ldcache->entries[i].value;
    const int flags = ldcache->entries[i].flags;

    lib_cache[i].libname = strdup(key);
    lib_cache[i].path = strdup(val);
    lib_cache[i].flags = flags;
  }
  return 0;
}

static int load_ld_cache(const char *cache_path) {
  struct stat st;
  size_t ld_size;
  const char *ld_map;
  int ret, fd = open(cache_path, O_RDONLY);

  if (fd < 0)
    return -1;

  if (fstat(fd, &st) < 0 || st.st_size < sizeof(struct ld_cache1)) {
    close(fd);
    return -1;
  }

  ld_size = st.st_size;
  ld_map = (const char *)mmap(NULL, ld_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (ld_map == MAP_FAILED) {
    close(fd);
    return -1;
  }

  if (memcmp(ld_map, CACHE1_HEADER, CACHE1_HEADER_LEN) == 0) {
    const struct ld_cache1 *cache1 = (struct ld_cache1 *)ld_map;
    size_t cache1_len = sizeof(struct ld_cache1) +
                        (cache1->entry_count * sizeof(struct ld_cache1_entry));
    cache1_len = (cache1_len + 0x7) & ~0x7ULL;

    if (ld_size > (cache1_len + sizeof(struct ld_cache2)))
      ret = read_cache2(ld_map + cache1_len);
    else
      ret = read_cache1(ld_map);
  } else {
    ret = read_cache2(ld_map);
  }

  munmap((void *)ld_map, ld_size);
  close(fd);
  return ret;
}

#define LD_SO_CACHE "/etc/ld.so.cache"
#define FLAG_TYPE_MASK 0x00ff
#define TYPE_ELF_LIBC6 0x0003
#define FLAG_ABI_MASK 0xff00
#define ABI_SPARC_LIB64 0x0100
#define ABI_IA64_LIB64 0x0200
#define ABI_X8664_LIB64 0x0300
#define ABI_S390_LIB64 0x0400
#define ABI_POWERPC_LIB64 0x0500

static bool match_so_flags(int flags) {
  if ((flags & FLAG_TYPE_MASK) != TYPE_ELF_LIBC6)
    return false;

  switch (flags & FLAG_ABI_MASK) {
  case ABI_SPARC_LIB64:
  case ABI_IA64_LIB64:
  case ABI_X8664_LIB64:
  case ABI_S390_LIB64:
  case ABI_POWERPC_LIB64:
    return (sizeof(void *) == 8);
  }

  return sizeof(void *) == 4;
}

static bool which_so_in_process(const char* libname, int pid, char* libpath) {
  int ret, found = false;
  char endline[4096], *mapname = NULL, *newline;
  char mappings_file[128];
  const size_t search_len = strlen(libname) + strlen("/lib.");
  char search1[search_len + 1];
  char search2[search_len + 1];

  sprintf(mappings_file, "/proc/%ld/maps", (long)pid);
  FILE *fp = fopen(mappings_file, "r");
  if (!fp)
    return NULL;

  snprintf(search1, search_len + 1, "/lib%s.", libname);
  snprintf(search2, search_len + 1, "/lib%s-", libname);

  do {
    ret = fscanf(fp, "%*x-%*x %*s %*x %*s %*d");
    if (!fgets(endline, sizeof(endline), fp))
      break;

    mapname = endline;
    newline = strchr(endline, '\n');
    if (newline)
      newline[0] = '\0';

    while (isspace(mapname[0])) mapname++;

    if (strstr(mapname, ".so") && (strstr(mapname, search1) ||
                                   strstr(mapname, search2))) {
      found = true;
      memcpy(libpath, mapname, strlen(mapname) + 1);
      break;
    }
  } while (ret != EOF);

  fclose(fp);
  return found;
}

char *bcc_procutils_which_so(const char *libname, int pid) {
  const size_t soname_len = strlen(libname) + strlen("lib.so");
  char soname[soname_len + 1];
  char libpath[4096];
  int i;

  if (strchr(libname, '/'))
    return strdup(libname);

  if (pid && which_so_in_process(libname, pid, libpath))
    return strdup(libpath);

  if (lib_cache_count < 0)
    return NULL;

  if (!lib_cache_count && load_ld_cache(LD_SO_CACHE) < 0) {
    lib_cache_count = -1;
    return NULL;
  }

  snprintf(soname, soname_len + 1, "lib%s.so", libname);

  for (i = 0; i < lib_cache_count; ++i) {
    if (!strncmp(lib_cache[i].libname, soname, soname_len) &&
        match_so_flags(lib_cache[i].flags)) {
      return strdup(lib_cache[i].path);
    }
  }
  return NULL;
}

void bcc_procutils_free(const char *ptr) {
  free((void *)ptr);
}

bool bcc_procutils_enter_mountns(int pid, struct ns_cookie *nc) {
  char curnspath[4096];
  char newnspath[4096];
  int oldns = -1;
  int newns = -1;
  struct stat ons_stat;
  struct stat nns_stat;

  if (nc == NULL)
    return false;

  nc->nsc_oldns = -1;
  nc->nsc_newns = -1;

  if (snprintf(curnspath, 4096, "/proc/self/ns/mnt") == 4096) {
    return false;
  }

  if (snprintf(newnspath, 4096, "/proc/%d/ns/mnt", pid) == 4096) {
    return false;
  }

  if ((oldns = open(curnspath, O_RDONLY)) < 0) {
    return false;
  }

  if ((newns = open(newnspath, O_RDONLY)) < 0) {
    goto errout;
  }

  if (fstat(oldns, &ons_stat) < 0) {
    goto errout;
  }

  if (fstat(newns, &nns_stat) < 0) {
    goto errout;
  }

  /*
   * Only switch to the new namespace if it doesn't match the existing
   * namespace.  This prevents us from getting an EPERM when trying to enter an
   * identical namespace.
   */
  if (ons_stat.st_ino == nns_stat.st_ino) {
    goto errout;
  }

  if (setns(newns, CLONE_NEWNS) < 0) {
    goto errout;
  }

  nc->nsc_oldns = oldns;
  nc->nsc_newns = newns;

  return true;

errout:
  if (oldns > -1) {
    (void) close(oldns);
  }
  if (newns > -1) {
    (void) close(newns);
  }
  return false;
}

bool bcc_procutils_exit_mountns(struct ns_cookie *nc) {
  bool rc = false;

  if (nc == NULL)
    return rc;

  if (nc->nsc_oldns == -1 || nc->nsc_newns == -1)
    return rc;

  if (setns(nc->nsc_oldns, CLONE_NEWNS) == 0) {
    rc = true;
  }

  if (nc->nsc_oldns > -1) {
    (void) close(nc->nsc_oldns);
    nc->nsc_oldns = -1;
  }
  if (nc->nsc_newns > -1) {
    (void) close(nc->nsc_newns);
    nc->nsc_newns = -1;
  }

  return rc;
}

/* Detects the following languages + C. */
const char *languages[] = {"java", "python", "ruby", "php", "node"};
const char *language_c = "c";
const int nb_languages = 5;

const char *bcc_procutils_language(int pid) {
  char procfilename[22], line[4096], pathname[32], *str;
  FILE *procfile;
  int i, ret;

  /* Look for clues in the absolute path to the executable. */
  sprintf(procfilename, "/proc/%ld/exe", (long)pid);
  if (realpath(procfilename, line)) {
    for (i = 0; i < nb_languages; i++)
      if (strstr(line, languages[i]))
        return languages[i];
  }


  sprintf(procfilename, "/proc/%ld/maps", (long)pid);
  procfile = fopen(procfilename, "r");
  if (!procfile)
    return NULL;

  /* Look for clues in memory mappings. */
  bool libc = false;
  do {
    char perm[8], dev[8];
    long long begin, end, size, inode;
    ret = fscanf(procfile, "%llx-%llx %s %llx %s %lld", &begin, &end, perm,
                 &size, dev, &inode);
    if (!fgets(line, sizeof(line), procfile))
      break;
    if (ret == 6) {
      char *mapname = line;
      char *newline = strchr(line, '\n');
      if (newline)
        newline[0] = '\0';
      while (isspace(mapname[0])) mapname++;
      for (i = 0; i < nb_languages; i++) {
        sprintf(pathname, "/lib%s", languages[i]);
        if (strstr(mapname, pathname))
          return languages[i];
        if ((str = strstr(mapname, "libc")) &&
            (str[4] == '-' || str[4] == '.'))
          libc = true;
      }
    }
  } while (ret && ret != EOF);

  fclose(procfile);

  /* Return C as the language if libc was found and nothing else. */
  return libc ? language_c : NULL;
}
