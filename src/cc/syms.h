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
#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "file_desc.h"
#include "bcc_syms.h"

class ProcStat {
  std::string procfs_;
  ino_t inode_;
  ino_t getinode_();

public:
  ProcStat(int pid);
  bool is_stale();
  void reset() { inode_ = getinode_(); }
};

class SymbolCache {
public:
  virtual ~SymbolCache() = default;

  virtual void refresh() = 0;
  virtual bool resolve_addr(uint64_t addr, struct bcc_symbol *sym, bool demangle = true) = 0;
  virtual bool resolve_name(const char *module, const char *name,
                            uint64_t *addr) = 0;
};

class KSyms : SymbolCache {
  struct Symbol {
    Symbol(const char *name, uint64_t addr) : name(name), addr(addr) {}
    std::string name;
    uint64_t addr;

    bool operator<(const Symbol &rhs) const { return addr < rhs.addr; }
  };

  std::vector<Symbol> syms_;
  std::unordered_map<std::string, uint64_t> symnames_;
  static void _add_symbol(const char *, uint64_t, void *);

public:
  virtual bool resolve_addr(uint64_t addr, struct bcc_symbol *sym, bool demangle = true);
  virtual bool resolve_name(const char *unused, const char *name,
                            uint64_t *addr);
  virtual void refresh();
};

class ProcMountNSGuard;
class ProcSyms;

class ProcMountNS {
private:
  explicit ProcMountNS(int pid);

  ebpf::FileDesc self_fd_;
  ebpf::FileDesc target_fd_;

  friend class ProcMountNSGuard;
  friend class ProcSyms;
};

class ProcMountNSGuard {
public:
  explicit ProcMountNSGuard(ProcMountNS *mount_ns);
  explicit ProcMountNSGuard(int pid);

  ~ProcMountNSGuard();

private:
  void init();

  std::unique_ptr<ProcMountNS> mount_ns_instance_;
  ProcMountNS *mount_ns_;
  bool entered_;
};

class ProcSyms : SymbolCache {
  struct Symbol {
    Symbol(const std::string *name, uint64_t start, uint64_t size, int flags = 0)
        : name(name), start(start), size(size), flags(flags) {}
    const std::string *name;
    uint64_t start;
    uint64_t size;
    int flags;

    bool operator<(const struct Symbol& rhs) const {
      return start < rhs.start;
    }
  };

  enum class ModuleType {
    UNKNOWN,
    EXEC,
    SO,
    PERF_MAP
  };

  struct Module {
    struct Range {
      uint64_t start;
      uint64_t end;
      Range(uint64_t s, uint64_t e) : start(s), end(e) {}
    };

    Module(const char *name, ProcMountNS* mount_ns,
           struct bcc_symbol_option *option);
    bool init();

    std::string name_;
    std::vector<Range> ranges_;
    bool loaded_;
    ProcMountNS *mount_ns_;
    bcc_symbol_option *symbol_option_;
    ModuleType type_;

    std::unordered_set<std::string> symnames_;
    std::vector<Symbol> syms_;

    void load_sym_table();
    bool contains(uint64_t addr, uint64_t &offset) const;
    uint64_t start() const { return ranges_.begin()->start; }
    bool find_addr(uint64_t offset, struct bcc_symbol *sym);
    bool find_name(const char *symname, uint64_t *addr);

    static int _add_symbol(const char *symname, uint64_t start, uint64_t end,
                           int flags, void *p);
  };

  int pid_;
  std::vector<Module> modules_;
  ProcStat procstat_;
  std::unique_ptr<ProcMountNS> mount_ns_instance_;
  bcc_symbol_option symbol_option_;

  static int _add_module(const char *, uint64_t, uint64_t, bool, void *);
  bool load_modules();

public:
  ProcSyms(int pid, struct bcc_symbol_option *option = nullptr);
  virtual void refresh();
  virtual bool resolve_addr(uint64_t addr, struct bcc_symbol *sym, bool demangle = true);
  virtual bool resolve_name(const char *module, const char *name,
                            uint64_t *addr);
};
