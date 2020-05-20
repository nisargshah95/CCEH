#ifndef CCEH_H_
#define CCEH_H_

#include <cstring>
#include <cmath>
#include <vector>
#include <iostream>
#include <nvm_malloc.h>
#include "util/pair.h"
#include "util/persist.h"
#include "src/hash.h"

constexpr size_t kSegmentBits = 8;
constexpr size_t kMask = (1 << kSegmentBits)-1;
constexpr size_t kShift = kSegmentBits;
constexpr size_t kSegmentSize = (1 << kSegmentBits) * 16 * 4;
constexpr size_t kNumPairPerCacheLine = 4;
constexpr size_t kNumCacheLine = 4;

struct Segment {
  static const size_t kNumSlot = kSegmentSize/sizeof(Pair);

  Segment(void)
  : local_depth{0}
  { }

  Segment(size_t depth)
  :local_depth{depth}
  { }

  void init(size_t depth) {
    local_depth = depth;
    for (unsigned i = 0; i < kNumSlot; i++) {
      _[i].key = INVALID;
    }
  }

  ~Segment(void) {
  }

  void* operator new(size_t size) {
    void* ret;
    posix_memalign(&ret, 64, size);
    return ret;
  }

  void* operator new[](size_t size) {
    void* ret;
    posix_memalign(&ret, 64, size);
    return ret;
  }

  int Insert(Key_t&, Value_t, size_t, size_t);
  void Insert4split(Key_t&, Value_t, size_t);
  bool Put(Key_t&, Value_t, size_t);
  Segment** Split(void);

  Pair _[kNumSlot];
  size_t local_depth;
  int64_t sema = 0;
  size_t pattern = 0;
  size_t numElem(void); 
};

struct Directory {
  static const size_t kDefaultDepth = 10;
  Segment** _;
  size_t capacity;
  size_t depth;
  bool lock;
  int sema = 0 ;

  Directory(void) {
    depth = kDefaultDepth;
    capacity = pow(2, depth);
#ifdef PMEM
    _ = (Segment**) nvm_reserve(capacity * sizeof(Segment*));
    clflush((char*)_, capacity * sizeof(Segment*));
    nvm_activate(_, NULL, NULL, NULL, NULL);
#else
    _ = new Segment*[capacity];
#endif
    lock = false;
    sema = 0;
  }

  Directory(size_t _depth) {
    depth = _depth;
    capacity = pow(2, depth);
#ifdef PMEM
    _ = (Segment**) nvm_reserve(capacity * sizeof(Segment*));
    clflush((char*)_, capacity * sizeof(Segment*));
    nvm_activate(_, NULL, NULL, NULL, NULL);
#else
    _ = new Segment*[capacity];
#endif
    lock = false;
    sema = 0;
  }

  void init(size_t _depth) {
    depth = _depth;
    capacity = pow(2, depth);
#ifdef PMEM
    // printf("reserving %lu bytes\n", capacity * sizeof(Segment*));
    _ = (Segment**) nvm_reserve(capacity * sizeof(Segment*));
    clflush((char*)_, capacity * sizeof(Segment*));
    nvm_activate(_, NULL, NULL, NULL, NULL);
#else
    _ = new Segment*[capacity];
#endif
    lock = false;
    sema = 0;
  }

  ~Directory(void) {
#ifdef PMEM
    nvm_free(_, NULL, NULL, NULL, NULL);
#else
    delete [] _;
#endif
  }

  bool Acquire(void) {
    bool unlocked = false;
    return CAS(&lock, &unlocked, true);
  }

  bool Release(void) {
    bool locked = true;
    return CAS(&lock, &locked, false);
  }
  
  void SanityCheck(void*);
  void LSBUpdate(int, int, int, int, Segment**);
};

class CCEH : public Hash {
  public:
    CCEH(void);
    CCEH(size_t);
    ~CCEH(void);
    void Insert(Key_t&, Value_t);
    bool InsertOnly(Key_t&, Value_t);
    bool Delete(Key_t&);
    Value_t Get(Key_t&);
    Value_t FindAnyway(Key_t&);
    double Utilization(void);
    size_t Capacity(void);
    bool Recovery(void);

    void print_meta(void) {
      std::cout << dir->depth << "," << dir->capacity << "," << Capacity() << "," << Capacity()/Segment::kNumSlot << std::endl;
    }

    void* operator new(size_t size) {
      void *ret;
      posix_memalign(&ret, 64, size);
      return ret;
    }

  private:
    size_t global_depth;
    Directory* dir;
};

#endif  // EXTENDIBLE_PTR_H_
