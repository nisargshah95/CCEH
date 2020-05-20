#include <iostream>
#include <cmath>
#include <thread>
#include <bitset>
#include <cassert>
#include <unordered_map>
#include "util/hash.h"
#include "src/CCEH.h"

extern size_t perfCounter;

int Segment::Insert(Key_t& key, Value_t value, size_t loc, size_t key_hash) {
  // std::cout << "Segment Insert: loc = " << loc << std::endl;
#ifdef INPLACE
  if (sema == -1) return 2;
  if ((key_hash >> (8*sizeof(key_hash)-local_depth)) != pattern) return 2;
  auto lock = sema;
  int ret = 1;
  while (!CAS(&sema, &lock, lock+1)) {
    lock = sema;
  }
  Key_t LOCK = INVALID;
  // std::cout << "knumslot = " << kNumSlot << std::endl;
  for (unsigned i = 0; i < kNumPairPerCacheLine * kNumCacheLine; ++i) {
    // std::cout << "i = " << i << std::endl;
    auto slot = (loc + i) % kNumSlot;
    // std::cout << "slot = " << slot << std::endl;
    auto _key = _[slot].key;
    // std::cout << &_[slot].key << " " << _[slot].key << " " << local_depth << " " << pattern << std::endl;
    if ((h(&_[slot].key,sizeof(Key_t)) >> (8*sizeof(key_hash)-local_depth)) != pattern) {
      // std::cout << "CAS invalid\n";
      CAS(&_[slot].key, &_key, INVALID);
    }
    if (CAS(&_[slot].key, &LOCK, SENTINEL)) {
      // std::cout << "set key\n";
      _[slot].value = value;
      mfence();
      _[slot].key = key;
      ret = 0;
      break;
    } else {
      // std::cout << "else invalid\n";
      LOCK = INVALID;
    }
  }
  lock = sema;
  while (!CAS(&sema, &lock, lock-1)) {
    lock = sema;
  }
  return ret;
#else
  if (sema == -1) return 2;
  if ((key_hash >> (8*sizeof(key_hash)-local_depth)) != pattern) return 2;
  auto lock = sema;
  int ret = 1;
  while (!CAS(&sema, &lock, lock+1)) {
    lock = sema;
  }
  Key_t LOCK = INVALID;
  for (unsigned i = 0; i < kNumPairPerCacheLine * kNumCacheLine; ++i) {
    auto slot = (loc + i) % kNumSlot;
    if (CAS(&_[slot].key, &LOCK, SENTINEL)) {
      _[slot].value = value;
      mfence();
      _[slot].key = key;
      ret = 0;
      break;
    } else {
      LOCK = INVALID;
    }
  }
  lock = sema;
  while (!CAS(&sema, &lock, lock-1)) {
    lock = sema;
  }
  return ret;
#endif
}

void Segment::Insert4split(Key_t& key, Value_t value, size_t loc) {
  for (unsigned i = 0; i < kNumPairPerCacheLine * kNumCacheLine; ++i) {
    auto slot = (loc+i) % kNumSlot;
    if (_[slot].key == INVALID) {
      _[slot].key = key;
      _[slot].value = value;
      return;
    }
  }
}

Segment** Segment::Split(void) {
  // std::cout << "Segment Split\n";
  using namespace std;
  int64_t lock = 0;
  if (!CAS(&sema, &lock, -1)) return nullptr;

#ifdef INPLACE
#ifdef PMEM
  Segment** split = (Segment**) nvm_reserve(2*sizeof(Segment*));
  clflush((char*)split, sizeof(Segment));
  nvm_activate(split, NULL, NULL, NULL, NULL);
  split[0] = this;
  split[1] = (Segment*) nvm_reserve(sizeof(Segment));
  split[1]->init(local_depth+1);
  clflush((char*)split[1], sizeof(Segment));
  nvm_activate(split[1], NULL, NULL, NULL, NULL);
#else
  Segment** split = new Segment*[2];
  split[0] = this;
  split[1] = new Segment(local_depth+1);
#endif

  for (unsigned i = 0; i < kNumSlot; ++i) {
    auto key_hash = h(&_[i].key, sizeof(Key_t));
    if (key_hash & ((size_t) 1 << ((sizeof(Key_t)*8 - local_depth - 1)))) {
      split[1]->Insert4split
        (_[i].key, _[i].value, (key_hash & kMask)*kNumPairPerCacheLine);
    }
  }
  local_depth = local_depth + 1;
  clflush((char*)&local_depth, sizeof(size_t));

  return split;
#else

#ifdef PMEM
  Segment** split = nvm_reserve(2*sizeof(Segment*));
  nvm_activate(split, NULL, NULL, NULL, NULL);
  split[0] = nvm_reserve(sizeof(Segment));
  nvm_activate(split[1], NULL, NULL, NULL, NULL);
  split[0]->init(local_depth+1);
  split[1] = nvm_reserve(sizeof(Segment));
  nvm_activate(split[1], NULL, NULL, NULL, NULL);
  split[1]->init(local_depth+1);
#else
  Segment** split = new Segment*[2];
  split[0] = new Segment(local_depth+1);
  split[1] = new Segment(local_depth+1);
#endif

  for (unsigned i = 0; i < kNumSlot; ++i) {
    auto key_hash = h(&_[i].key, sizeof(Key_t));
    if (key_hash & ((size_t) 1 << ((sizeof(Key_t)*8 - local_depth - 1)))) {
      split[1]->Insert4split
        (_[i].key, _[i].value, (key_hash & kMask)*kNumPairPerCacheLine);
    } else {
      split[0]->Insert4split
        (_[i].key, _[i].value, (key_hash & kMask)*kNumPairPerCacheLine);
    }
  }
  clflush((char*)split[0], sizeof(Segment));
  clflush((char*)split[1], sizeof(Segment));
  return split;
#endif
}

CCEH::CCEH(void)
: dir{new Directory(0)}
{
  for (unsigned i = 0; i < dir->capacity; ++i) {
    dir->_[i] = new Segment(0);
    dir->_[i]->pattern = i;
  }
}

#ifdef PMEM
CCEH::CCEH(size_t initCap)
{
  dir = (Directory*) nvm_reserve_id("cceh_dir", sizeof(Directory));
  clflush((char*)dir, sizeof(Directory));
  // nvm_activate(dir, NULL, NULL, NULL, NULL);
  nvm_activate_id("cceh_dir");
  dir->init(static_cast<size_t>(log2(initCap)));
  for (unsigned i = 0; i < dir->capacity; ++i) {
    // dir->_[i] = (Segment*) nvm_reserve(sizeof(Segment));
    Segment* seg = (Segment*) nvm_reserve(sizeof(Segment));
    // clflush((char*)(dir->_[i]), sizeof(Segment));
    clflush((char*)(seg), sizeof(Segment));
    nvm_activate((void*) seg, (void**) &dir->_[i], (void*) seg, NULL, NULL);
    // seg->init(static_cast<size_t>(log2(initCap)));
    // nvm_activate(dir->_[i], NULL, NULL, NULL, NULL);
    dir->_[i] = (Segment*) nvm_abs(dir->_[i]);
    dir->_[i]->init(static_cast<size_t>(log2(initCap)));
    dir->_[i]->pattern = i;
  }
  // std::cout << "values after init: " << &dir->_[49]->_[268].key << " " << dir->_[49]->_[268].key << std::endl;
}
#else
CCEH::CCEH(size_t initCap)
: dir{new Directory(static_cast<size_t>(log2(initCap)))}
{
  for (unsigned i = 0; i < dir->capacity; ++i) {
    dir->_[i] = new Segment(static_cast<size_t>(log2(initCap)));
    dir->_[i]->pattern = i;
  }
  // std::cout << "values after init: " << &dir->_[49]->_[268].key << " " << dir->_[49]->_[268].key << std::endl;
}
#endif

CCEH::~CCEH(void)
{ }

void Directory::LSBUpdate(int local_depth, int global_depth, int dir_cap, int x, Segment** s) {
  int depth_diff = global_depth - local_depth;
  if (depth_diff == 0) {
    if ((x % dir_cap) >= dir_cap/2) {
      _[x-dir_cap/2] = s[0];
      clflush((char*)&_[x-dir_cap/2], sizeof(Segment*));
      _[x] = s[1];
      clflush((char*)&_[x], sizeof(Segment*));
    } else {
      _[x] = s[0];
      clflush((char*)&_[x], sizeof(Segment*));
      _[x+dir_cap/2] = s[1];
      clflush((char*)&_[x+dir_cap/2], sizeof(Segment*));
    }
  } else {
    if ((x%dir_cap) >= dir_cap/2) {
      LSBUpdate(local_depth+1, global_depth, dir_cap/2, x-dir_cap/2, s);
      LSBUpdate(local_depth+1, global_depth, dir_cap/2, x, s);
    } else {
      LSBUpdate(local_depth+1, global_depth, dir_cap/2, x, s);
      LSBUpdate(local_depth+1, global_depth, dir_cap/2, x+dir_cap/2, s);
    }
  }
  return;
}

void CCEH::Insert(Key_t& key, Value_t value) {
  // std::cout << "Insert\n";
STARTOVER:
  // std::cout << "STARTOVER\n";
  auto key_hash = h(&key, sizeof(key));
  auto y = (key_hash & kMask) * kNumPairPerCacheLine;

RETRY:
  // std::cout << "RETRY\n";
  auto x = (key_hash >> (8*sizeof(key_hash)-dir->depth));
  // std::cout << "segment: " << x << std::endl;
  auto target = dir->_[x];
  auto ret = target->Insert(key, value, y, key_hash);

  if (ret == 1) {
    // std::cout << "ret 1\n";
    timer.Start();
    Segment** s = target->Split();
    timer.Stop();
    breakdown += timer.GetSeconds();
    if (s == nullptr) {
      // another thread is doing split
      goto RETRY;
    }

    s[0]->pattern = (key_hash >> (8*sizeof(key_hash)-s[0]->local_depth+1)) << 1;
    s[1]->pattern = ((key_hash >> (8*sizeof(key_hash)-s[1]->local_depth+1)) << 1) + 1;

    // Directory management
    while (!dir->Acquire()) {
      asm("nop");
    }
    { // CRITICAL SECTION - directory update
      // std::cout << "dir update\n";
      x = (key_hash >> (8*sizeof(key_hash)-dir->depth));
#ifdef INPLACE
      if (dir->_[x]->local_depth-1 < dir->depth) {  // normal split
#else
      if (dir->_[x]->local_depth < dir->depth) {  // normal split
#endif
        unsigned depth_diff = dir->depth - s[0]->local_depth;
        if (depth_diff == 0) {
          if (x%2 == 0) {
            dir->_[x+1] = s[1];
#ifdef INPLACE
            clflush((char*) &dir->_[x+1], 8);
#else
            mfence();
            dir->_[x] = s[0];
            clflush((char*) &dir->_[x], 16);
#endif
          } else {
            dir->_[x] = s[1];
#ifdef INPLACE
            clflush((char*) &dir->_[x], 8);
#else
            mfence();
            dir->_[x-1] = s[0];
            clflush((char*) &dir->_[x-1], 16);
#endif
          }
        } else {
          int chunk_size = pow(2, dir->depth - (s[0]->local_depth - 1));
          x = x - (x % chunk_size);
          for (unsigned i = 0; i < chunk_size/2; ++i) {
            dir->_[x+chunk_size/2+i] = s[1];
          }
          clflush((char*)&dir->_[x+chunk_size/2], sizeof(void*)*chunk_size/2);
#ifndef INPLACE
          for (unsigned i = 0; i < chunk_size/2; ++i) {
            dir->_[x+i] = s[0];
          }
          clflush((char*)&dir->_[x], sizeof(void*)*chunk_size/2);
#endif
        }
    while (!dir->Release()) {
      asm("nop");
    }
      } else {  // directory doubling
        // std::cout << "dir doubling\n";
        auto dir_old = dir;
        auto d = dir->_;
        // auto _dir = new Segment*[dir->capacity*2];
#ifdef PMEM
        auto _dir = (Directory*) nvm_reserve(sizeof(Directory));
        _dir->init(dir->depth+1);
        clflush((char*)_dir, sizeof(Directory));
        nvm_activate(_dir, NULL, NULL, NULL, NULL);
#else
        auto _dir = new Directory(dir->depth+1);
#endif
        for (unsigned i = 0; i < dir->capacity; ++i) {
          if (i == x) {
            _dir->_[2*i] = s[0];
            _dir->_[2*i+1] = s[1];
          } else {
            _dir->_[2*i] = d[i];
            _dir->_[2*i+1] = d[i];
          }
        }
        clflush((char*)&_dir->_[0], sizeof(Segment*)*_dir->capacity);
        clflush((char*)&_dir, sizeof(Directory));
        dir = _dir;
        clflush((char*)&dir, sizeof(void*));
#ifdef PMEM
        // nvm_free(dir_old, NULL, NULL, NULL, NULL);
#else
        delete dir_old;
#endif
        // TODO: requiered to do this atomically
      }
#ifdef INPLACE
      s[0]->sema = 0;
#endif
    }  // End of critical section
    goto RETRY;
  } else if (ret == 2) {
    // std::cout << "ret 2\n";
    // Insert(key, value);
    goto STARTOVER;
  } else {
    // std::cout << "else ret\n";
    clflush((char*)&dir->_[x]->_[y], 64);
  }
}

// This function does not allow resizing
bool CCEH::InsertOnly(Key_t& key, Value_t value) {
  auto key_hash = h(&key, sizeof(key));
  auto x = (key_hash >> (8*sizeof(key_hash)-dir->depth));
  auto y = (key_hash & kMask) * kNumPairPerCacheLine;

  auto ret = dir->_[x]->Insert(key, value, y, key_hash);
  if (ret == 0) {
    clflush((char*)&dir->_[x]->_[y], 64);
    return true;
  }

  return false;
}

// TODO
bool CCEH::Delete(Key_t& key) {
  return false;
}

Value_t CCEH::Get(Key_t& key) {
  auto key_hash = h(&key, sizeof(key));
  auto x = (key_hash >> (8*sizeof(key_hash)-dir->depth));
  auto y = (key_hash & kMask) * kNumPairPerCacheLine;

  auto dir_ = dir->_[x];

#ifdef INPLACE
  auto sema = dir->_[x]->sema;
  while (!CAS(&dir->_[x]->sema, &sema, sema+1)) {
    sema = dir->_[x]->sema;
  }
#endif

  for (unsigned i = 0; i < kNumPairPerCacheLine * kNumCacheLine; ++i) {
    auto slot = (y+i) % Segment::kNumSlot;
    if (dir_->_[slot].key == key) {
#ifdef INPLACE
      sema = dir->_[x]->sema;
      while (!CAS(&dir->_[x]->sema, &sema, sema-1)) {
        sema = dir->_[x]->sema;
      }
#endif
      return dir_->_[slot].value;
    }
  }

#ifdef INPLACE
  sema = dir->_[x]->sema;
  while (!CAS(&dir->_[x]->sema, &sema, sema-1)) {
    sema = dir->_[x]->sema;
  }
#endif
  return NONE;
}

double CCEH::Utilization(void) {
  size_t sum = 0;
  std::unordered_map<Segment*, bool> set;
  for (size_t i = 0; i < dir->capacity; ++i) {
    set[dir->_[i]] = true;
  }
  for (auto& elem: set) {
    for (unsigned i = 0; i < Segment::kNumSlot; ++i) {
#ifdef INPLACE
      auto key_hash = h(&elem.first->_[i].key, sizeof(elem.first->_[i].key));
      if (key_hash >> (8*sizeof(key_hash)-elem.first->local_depth) == elem.first->pattern) sum++;
#else
      if (elem.first->_[i].key != INVALID) sum++;
#endif
    }
  }
  return ((double)sum)/((double)set.size()*Segment::kNumSlot)*100.0;
}

size_t CCEH::Capacity(void) {
  std::unordered_map<Segment*, bool> set;
  for (size_t i = 0; i < dir->capacity; ++i) {
    set[dir->_[i]] = true;
  }
  return set.size() * Segment::kNumSlot;
}

size_t Segment::numElem(void) {
  size_t sum = 0;
  for (unsigned i = 0; i < kNumSlot; ++i) {
    if (_[i].key != INVALID) {
      sum++;
    }
  }
  return sum;
}

bool CCEH::Recovery(void) {
  bool recovered = false;
  size_t i = 0;
  while (i < dir->capacity) {
    size_t depth_cur = dir->_[i]->local_depth;
    size_t stride = pow(2, dir->depth - depth_cur);
    size_t buddy = i + stride;
    if (buddy == dir->capacity) break;
    for (int j = buddy - 1; i < j; j--) {
      if (dir->_[j]->local_depth != depth_cur) {
        dir->_[j] = dir->_[i];
      }
    }
    i = i+stride;
  }
  if (recovered) {
    clflush((char*)&dir->_[0], sizeof(void*)*dir->capacity);
  }
  return recovered;
}

// for debugging
Value_t CCEH::FindAnyway(Key_t& key) {
  using namespace std;
  for (size_t i = 0; i < dir->capacity; ++i) {
     for (size_t j = 0; j < Segment::kNumSlot; ++j) {
       if (dir->_[i]->_[j].key == key) {
         auto key_hash = h(&key, sizeof(key));
         auto x = (key_hash >> (8*sizeof(key_hash)-dir->depth));
         auto y = (key_hash & kMask) * kNumPairPerCacheLine;
         cout << bitset<32>(i) << endl << bitset<32>((x>>1)) << endl << bitset<32>(x) << endl;
         return dir->_[i]->_[j].value;
       }
     }
  }
  return NONE;
}

void Directory::SanityCheck(void* addr) {
  using namespace std;
  for (unsigned i = 0; i < capacity; ++i) {
    if (_[i] == addr) {
      cout << i << " " << _[i]->sema << endl;
      exit(1);
    }
  }
}
