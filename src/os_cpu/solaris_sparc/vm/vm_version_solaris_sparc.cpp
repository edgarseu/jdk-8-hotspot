/*
 * Copyright (c) 2006, 2014, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "runtime/os.hpp"
#include "vm_version_sparc.hpp"

#include <sys/auxv.h>
#include <sys/auxv_SPARC.h>
#include <sys/systeminfo.h>
#include <kstat.h>
#include <picl.h>
#include <dlfcn.h>
#include <link.h>

extern "C" static int PICL_visit_cpu_helper(picl_nodehdl_t nodeh, void *result);

// Functions from the library we need (signatures should match those in picl.h)
extern "C" {
  typedef int (*picl_initialize_func_t)(void);
  typedef int (*picl_shutdown_func_t)(void);
  typedef int (*picl_get_root_func_t)(picl_nodehdl_t *nodehandle);
  typedef int (*picl_walk_tree_by_class_func_t)(picl_nodehdl_t rooth,
      const char *classname, void *c_args,
      int (*callback_fn)(picl_nodehdl_t hdl, void *args));
  typedef int (*picl_get_prop_by_name_func_t)(picl_nodehdl_t nodeh, const char *nm,
      picl_prophdl_t *ph);
  typedef int (*picl_get_propval_func_t)(picl_prophdl_t proph, void *valbuf, size_t sz);
  typedef int (*picl_get_propinfo_func_t)(picl_prophdl_t proph, picl_propinfo_t *pi);
}

class PICL {
  // Pointers to functions in the library
  picl_initialize_func_t _picl_initialize;
  picl_shutdown_func_t _picl_shutdown;
  picl_get_root_func_t _picl_get_root;
  picl_walk_tree_by_class_func_t _picl_walk_tree_by_class;
  picl_get_prop_by_name_func_t _picl_get_prop_by_name;
  picl_get_propval_func_t _picl_get_propval;
  picl_get_propinfo_func_t _picl_get_propinfo;
  // Handle to the library that is returned by dlopen
  void *_dl_handle;

  bool open_library();
  void close_library();

  template<typename FuncType> bool bind(FuncType& func, const char* name);
  bool bind_library_functions();

  // Get a value of the integer property. The value in the tree can be either 32 or 64 bit
  // depending on the platform. The result is converted to int.
  int get_int_property(picl_nodehdl_t nodeh, const char* name, int* result) {
    picl_propinfo_t pinfo;
    picl_prophdl_t proph;
    if (_picl_get_prop_by_name(nodeh, name, &proph) != PICL_SUCCESS ||
        _picl_get_propinfo(proph, &pinfo) != PICL_SUCCESS) {
      return PICL_FAILURE;
    }

    if (pinfo.type != PICL_PTYPE_INT && pinfo.type != PICL_PTYPE_UNSIGNED_INT) {
      assert(false, "Invalid property type");
      return PICL_FAILURE;
    }
    if (pinfo.size == sizeof(int64_t)) {
      int64_t val;
      if (_picl_get_propval(proph, &val, sizeof(int64_t)) != PICL_SUCCESS) {
        return PICL_FAILURE;
      }
      *result = static_cast<int>(val);
    } else if (pinfo.size == sizeof(int32_t)) {
      int32_t val;
      if (_picl_get_propval(proph, &val, sizeof(int32_t)) != PICL_SUCCESS) {
        return PICL_FAILURE;
      }
      *result = static_cast<int>(val);
    } else {
      assert(false, "Unexpected integer property size");
      return PICL_FAILURE;
    }
    return PICL_SUCCESS;
  }

  // Visitor and a state machine that visits integer properties and verifies that the
  // values are the same. Stores the unique value observed.
  class UniqueValueVisitor {
    PICL *_picl;
    enum {
      INITIAL,        // Start state, no assignments happened
      ASSIGNED,       // Assigned a value
      INCONSISTENT    // Inconsistent value seen
    } _state;
    int _value;
  public:
    UniqueValueVisitor(PICL* picl) : _picl(picl), _state(INITIAL) { }
    int value() {
      assert(_state == ASSIGNED, "Precondition");
      return _value;
    }
    void set_value(int value) {
      assert(_state == INITIAL, "Precondition");
      _value = value;
      _state = ASSIGNED;
    }
    bool is_initial()       { return _state == INITIAL;      }
    bool is_assigned()      { return _state == ASSIGNED;     }
    bool is_inconsistent()  { return _state == INCONSISTENT; }
    void set_inconsistent() { _state = INCONSISTENT;         }

    bool visit(picl_nodehdl_t nodeh, const char* name) {
      assert(!is_inconsistent(), "Precondition");
      int curr;
      if (_picl->get_int_property(nodeh, name, &curr) == PICL_SUCCESS) {
        if (!is_assigned()) { // first iteration
          set_value(curr);
        } else if (curr != value()) { // following iterations
          set_inconsistent();
        }
        return true;
      }
      return false;
    }
  };

  class CPUVisitor {
    UniqueValueVisitor _l1_visitor;
    UniqueValueVisitor _l2_visitor;
    int _limit; // number of times visit() can be run
  public:
    CPUVisitor(PICL *picl, int limit) : _l1_visitor(picl), _l2_visitor(picl), _limit(limit) {}
    static int visit(picl_nodehdl_t nodeh, void *arg) {
      CPUVisitor *cpu_visitor = static_cast<CPUVisitor*>(arg);
      UniqueValueVisitor* l1_visitor = cpu_visitor->l1_visitor();
      UniqueValueVisitor* l2_visitor = cpu_visitor->l2_visitor();
      if (!l1_visitor->is_inconsistent()) {
        l1_visitor->visit(nodeh, "l1-dcache-line-size");
      }
      static const char* l2_data_cache_line_property_name = NULL;
      // On the first visit determine the name of the l2 cache line size property and memoize it.
      if (l2_data_cache_line_property_name == NULL) {
        assert(!l2_visitor->is_inconsistent(), "First iteration cannot be inconsistent");
        l2_data_cache_line_property_name = "l2-cache-line-size";
        if (!l2_visitor->visit(nodeh, l2_data_cache_line_property_name)) {
          l2_data_cache_line_property_name = "l2-dcache-line-size";
          l2_visitor->visit(nodeh, l2_data_cache_line_property_name);
        }
      } else {
        if (!l2_visitor->is_inconsistent()) {
          l2_visitor->visit(nodeh, l2_data_cache_line_property_name);
        }
      }

      if (l1_visitor->is_inconsistent() && l2_visitor->is_inconsistent()) {
        return PICL_WALK_TERMINATE;
      }
      cpu_visitor->_limit--;
      if (cpu_visitor->_limit <= 0) {
        return PICL_WALK_TERMINATE;
      }
      return PICL_WALK_CONTINUE;
    }
    UniqueValueVisitor* l1_visitor() { return &_l1_visitor; }
    UniqueValueVisitor* l2_visitor() { return &_l2_visitor; }
  };
  int _L1_data_cache_line_size;
  int _L2_data_cache_line_size;
public:
  static int visit_cpu(picl_nodehdl_t nodeh, void *state) {
    return CPUVisitor::visit(nodeh, state);
  }

  PICL(bool is_fujitsu, bool is_sun4v) : _L1_data_cache_line_size(0), _L2_data_cache_line_size(0), _dl_handle(NULL) {
    if (!open_library()) {
      return;
    }
    if (_picl_initialize() == PICL_SUCCESS) {
      picl_nodehdl_t rooth;
      if (_picl_get_root(&rooth) == PICL_SUCCESS) {
        const char* cpu_class = "cpu";
        // If it's a Fujitsu machine, it's a "core"
        if (is_fujitsu) {
          cpu_class = "core";
        }
        CPUVisitor cpu_visitor(this, (is_sun4v && !is_fujitsu) ? 1 : os::processor_count());
        _picl_walk_tree_by_class(rooth, cpu_class, &cpu_visitor, PICL_visit_cpu_helper);
        if (cpu_visitor.l1_visitor()->is_assigned()) { // Is there a value?
          _L1_data_cache_line_size = cpu_visitor.l1_visitor()->value();
        }
        if (cpu_visitor.l2_visitor()->is_assigned()) {
          _L2_data_cache_line_size = cpu_visitor.l2_visitor()->value();
        }
      }
      _picl_shutdown();
    }
    close_library();
  }

  unsigned int L1_data_cache_line_size() const { return _L1_data_cache_line_size; }
  unsigned int L2_data_cache_line_size() const { return _L2_data_cache_line_size; }
};


extern "C" static int PICL_visit_cpu_helper(picl_nodehdl_t nodeh, void *result) {
  return PICL::visit_cpu(nodeh, result);
}

template<typename FuncType>
bool PICL::bind(FuncType& func, const char* name) {
  func = reinterpret_cast<FuncType>(dlsym(_dl_handle, name));
  return func != NULL;
}

bool PICL::bind_library_functions() {
  assert(_dl_handle != NULL, "library should be open");
  return bind(_picl_initialize,         "picl_initialize"        ) &&
         bind(_picl_shutdown,           "picl_shutdown"          ) &&
         bind(_picl_get_root,           "picl_get_root"          ) &&
         bind(_picl_walk_tree_by_class, "picl_walk_tree_by_class") &&
         bind(_picl_get_prop_by_name,   "picl_get_prop_by_name"  ) &&
         bind(_picl_get_propval,        "picl_get_propval"       ) &&
         bind(_picl_get_propinfo,       "picl_get_propinfo"      );
}

bool PICL::open_library() {
  _dl_handle = dlopen("libpicl.so.1", RTLD_LAZY);
  if (_dl_handle == NULL) {
    warning("PICL (libpicl.so.1) is missing. Performance will not be optimal.");
    return false;
  }
  if (!bind_library_functions()) {
    assert(false, "unexpected PICL API change");
    close_library();
    return false;
  }
  return true;
}

void PICL::close_library() {
  assert(_dl_handle != NULL, "library should be open");
  dlclose(_dl_handle);
  _dl_handle = NULL;
}

// We need to keep these here as long as we have to build on Solaris
// versions before 10.
#ifndef SI_ARCHITECTURE_32
#define SI_ARCHITECTURE_32      516     /* basic 32-bit SI_ARCHITECTURE */
#endif

#ifndef SI_ARCHITECTURE_64
#define SI_ARCHITECTURE_64      517     /* basic 64-bit SI_ARCHITECTURE */
#endif

static void do_sysinfo(int si, const char* string, int* features, int mask) {
  char   tmp;
  size_t bufsize = sysinfo(si, &tmp, 1);

  // All SI defines used below must be supported.
  guarantee(bufsize != -1, "must be supported");

  char* buf = (char*) malloc(bufsize);

  if (buf == NULL)
    return;

  if (sysinfo(si, buf, bufsize) == bufsize) {
    // Compare the string.
    if (strcmp(buf, string) == 0) {
      *features |= mask;
    }
  }

  free(buf);
}

int VM_Version::platform_features(int features) {
  // getisax(2), SI_ARCHITECTURE_32, and SI_ARCHITECTURE_64 are
  // supported on Solaris 10 and later.
  if (os::Solaris::supports_getisax()) {

    // Check 32-bit architecture.
    do_sysinfo(SI_ARCHITECTURE_32, "sparc", &features, v8_instructions_m);

    // Check 64-bit architecture.
    do_sysinfo(SI_ARCHITECTURE_64, "sparcv9", &features, generic_v9_m);

    // Extract valid instruction set extensions.
    uint_t avs[2];
    uint_t avn = os::Solaris::getisax(avs, 2);
    assert(avn <= 2, "should return two or less av's");
    uint_t av = avs[0];

#ifndef PRODUCT
    if (PrintMiscellaneous && Verbose) {
      tty->print("getisax(2) returned: " PTR32_FORMAT, av);
      if (avn > 1) {
        tty->print(", " PTR32_FORMAT, avs[1]);
      }
      tty->cr();
    }
#endif

    if (av & AV_SPARC_MUL32)  features |= hardware_mul32_m;
    if (av & AV_SPARC_DIV32)  features |= hardware_div32_m;
    if (av & AV_SPARC_FSMULD) features |= hardware_fsmuld_m;
    if (av & AV_SPARC_V8PLUS) features |= v9_instructions_m;
    if (av & AV_SPARC_POPC)   features |= hardware_popc_m;
    if (av & AV_SPARC_VIS)    features |= vis1_instructions_m;
    if (av & AV_SPARC_VIS2)   features |= vis2_instructions_m;
    if (avn > 1) {
      uint_t av2 = avs[1];
#ifndef AV2_SPARC_SPARC5
#define AV2_SPARC_SPARC5 0x00000008 /* The 29 new fp and sub instructions */
#endif
      if (av2 & AV2_SPARC_SPARC5)       features |= sparc5_instructions_m;
    }

    // Next values are not defined before Solaris 10
    // but Solaris 8 is used for jdk6 update builds.
#ifndef AV_SPARC_ASI_BLK_INIT
#define AV_SPARC_ASI_BLK_INIT 0x0080  /* ASI_BLK_INIT_xxx ASI */
#endif
    if (av & AV_SPARC_ASI_BLK_INIT) features |= blk_init_instructions_m;

#ifndef AV_SPARC_FMAF
#define AV_SPARC_FMAF 0x0100        /* Fused Multiply-Add */
#endif
    if (av & AV_SPARC_FMAF)         features |= fmaf_instructions_m;

#ifndef AV_SPARC_FMAU
#define    AV_SPARC_FMAU    0x0200  /* Unfused Multiply-Add */
#endif
    if (av & AV_SPARC_FMAU)         features |= fmau_instructions_m;

#ifndef AV_SPARC_VIS3
#define    AV_SPARC_VIS3    0x0400  /* VIS3 instruction set extensions */
#endif
    if (av & AV_SPARC_VIS3)         features |= vis3_instructions_m;

#ifndef AV_SPARC_CBCOND
#define AV_SPARC_CBCOND 0x10000000  /* compare and branch instrs supported */
#endif
    if (av & AV_SPARC_CBCOND)       features |= cbcond_instructions_m;

#ifndef AV_SPARC_AES
#define AV_SPARC_AES 0x00020000  /* aes instrs supported */
#endif
    if (av & AV_SPARC_AES)       features |= aes_instructions_m;

#ifndef AV_SPARC_SHA1
#define AV_SPARC_SHA1   0x00400000  /* sha1 instruction supported */
#endif
    if (av & AV_SPARC_SHA1)         features |= sha1_instruction_m;

#ifndef AV_SPARC_SHA256
#define AV_SPARC_SHA256 0x00800000  /* sha256 instruction supported */
#endif
    if (av & AV_SPARC_SHA256)       features |= sha256_instruction_m;

#ifndef AV_SPARC_SHA512
#define AV_SPARC_SHA512 0x01000000  /* sha512 instruction supported */
#endif
    if (av & AV_SPARC_SHA512)       features |= sha512_instruction_m;

  } else {
    // getisax(2) failed, use the old legacy code.
#ifndef PRODUCT
    if (PrintMiscellaneous && Verbose)
      tty->print_cr("getisax(2) is not supported.");
#endif

    char   tmp;
    size_t bufsize = sysinfo(SI_ISALIST, &tmp, 1);
    char*  buf     = (char*) malloc(bufsize);

    if (buf != NULL) {
      if (sysinfo(SI_ISALIST, buf, bufsize) == bufsize) {
        // Figure out what kind of sparc we have
        char *sparc_string = strstr(buf, "sparc");
        if (sparc_string != NULL) {              features |= v8_instructions_m;
          if (sparc_string[5] == 'v') {
            if (sparc_string[6] == '8') {
              if (sparc_string[7] == '-') {      features |= hardware_mul32_m;
                                                 features |= hardware_div32_m;
              } else if (sparc_string[7] == 'p') features |= generic_v9_m;
              else                               features |= generic_v8_m;
            } else if (sparc_string[6] == '9')   features |= generic_v9_m;
          }
        }

        // Check for visualization instructions
        char *vis = strstr(buf, "vis");
        if (vis != NULL) {                       features |= vis1_instructions_m;
          if (vis[3] == '2')                     features |= vis2_instructions_m;
        }
      }
      free(buf);
    }
  }

  // Determine the machine type.
  do_sysinfo(SI_MACHINE, "sun4v", &features, sun4v_m);

  {
    // Using kstat to determine the machine type.
    kstat_ctl_t* kc = kstat_open();
    kstat_t* ksp = kstat_lookup(kc, (char*)"cpu_info", -1, NULL);
    const char* implementation = "UNKNOWN";
    if (ksp != NULL) {
      if (kstat_read(kc, ksp, NULL) != -1 && ksp->ks_data != NULL) {
        kstat_named_t* knm = (kstat_named_t *)ksp->ks_data;
        for (int i = 0; i < ksp->ks_ndata; i++) {
          if (strcmp((const char*)&(knm[i].name),"implementation") == 0) {
#ifndef KSTAT_DATA_STRING
#define KSTAT_DATA_STRING   9
#endif
            if (knm[i].data_type == KSTAT_DATA_CHAR) {
              // VM is running on Solaris 8 which does not have value.str.
              implementation = &(knm[i].value.c[0]);
            } else if (knm[i].data_type == KSTAT_DATA_STRING) {
              // VM is running on Solaris 10.
#ifndef KSTAT_NAMED_STR_PTR
              // Solaris 8 was used to build VM, define the structure it misses.
              struct str_t {
                union {
                  char *ptr;     /* NULL-term string */
                  char __pad[8]; /* 64-bit padding */
                } addr;
                uint32_t len;    /* # bytes for strlen + '\0' */
              };
#define KSTAT_NAMED_STR_PTR(knptr) (( (str_t*)&((knptr)->value) )->addr.ptr)
#endif
              implementation = KSTAT_NAMED_STR_PTR(&knm[i]);
            }
#ifndef PRODUCT
            if (PrintMiscellaneous && Verbose) {
              tty->print_cr("cpu_info.implementation: %s", implementation);
            }
#endif
            // Convert to UPPER case before compare.
            char* impl = strdup(implementation);

            for (int i = 0; impl[i] != 0; i++)
              impl[i] = (char)toupper((uint)impl[i]);
            if (strstr(impl, "SPARC64") != NULL) {
              features |= sparc64_family_m;
            } else if (strstr(impl, "SPARC-M") != NULL) {
              // M-series SPARC is based on T-series.
              features |= (M_family_m | T_family_m);
            } else if (strstr(impl, "SPARC-T") != NULL) {
              features |= T_family_m;
              if (strstr(impl, "SPARC-T1") != NULL) {
                features |= T1_model_m;
              }
            } else {
              if (strstr(impl, "SPARC") == NULL) {
#ifndef PRODUCT
                // kstat on Solaris 8 virtual machines (branded zones)
                // returns "(unsupported)" implementation.
                warning("kstat cpu_info implementation = '%s', should contain SPARC", impl);
#endif
                implementation = "SPARC";
              }
            }
            free((void*)impl);
            break;
          }
        } // for(
      }
    }
    assert(strcmp(implementation, "UNKNOWN") != 0,
           "unknown cpu info (changed kstat interface?)");
    kstat_close(kc);
  }

  // Figure out cache line sizes using PICL
  PICL picl((features & sparc64_family_m) != 0, (features & sun4v_m) != 0);
  _L2_data_cache_line_size = picl.L2_data_cache_line_size();

  return features;
}
