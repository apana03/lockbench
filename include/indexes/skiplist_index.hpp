#pragma once
#include <atomic>
#include <cstdint>
#include <optional>
#include <random>
#include <utility>

// Concurrent skip list with per-node locking (pessimistic hand-over-hand).
// Templated on lock type. Simplifications:
//   - deleted nodes are leaked (no epoch-based reclamation); this is what
//     keeps the OCC read path safe (readers can always dereference any node
//     they observe, even if it is logically deleted).
//   - reads do hand-over-hand at level 0 only; upper levels are used only
//     during write-side predecessor search.
template <class Lock>
class skiplist_index {
public:
  using key_type   = std::uint64_t;
  using value_type = std::uint64_t;

  static constexpr int MAX_LEVEL = 16;

  skiplist_index() {
    head_ = new node(/*lvl*/ MAX_LEVEL - 1, /*key*/ 0, /*val*/ 0, /*is_sentinel*/ true);
    tail_ = new node(/*lvl*/ MAX_LEVEL - 1, /*key*/ 0, /*val*/ 0, /*is_sentinel*/ true);
    head_->is_neg_inf = true;
    tail_->is_pos_inf = true;
    for (int i = 0; i < MAX_LEVEL; ++i) head_->next[i] = tail_;
  }

  ~skiplist_index() {
    // Leak-friendly destruction: walk level-0 list and delete each node.
    node* n = head_;
    while (n) {
      node* nx = n->next[0];
      delete n;
      n = nx;
    }
  }

  skiplist_index(const skiplist_index&) = delete;
  skiplist_index& operator=(const skiplist_index&) = delete;

  // Exclusive lookup - multi-level hand-over-hand for O(log N) traversal.
  // Acquisition order is strictly left-to-right in the list (matches put/remove's
  // top-down-level pred acquisition), so no deadlock.
  std::optional<value_type> get(key_type key) noexcept {
    node* pred = head_;
    pred->lock.lock();
    for (int lvl = MAX_LEVEL - 1; lvl > 0; --lvl) {
      // Walk right at this level using hand-over-hand.
      while (node_lt(pred->next[lvl], key)) {
        node* x = pred->next[lvl];
        x->lock.lock();
        pred->lock.unlock();
        pred = x;
      }
    }
    // Level 0 walk
    while (node_lt(pred->next[0], key)) {
      node* x = pred->next[0];
      x->lock.lock();
      pred->lock.unlock();
      pred = x;
    }
    node* curr = pred->next[0];
    curr->lock.lock();
    std::optional<value_type> result;
    if (!curr->is_pos_inf && !curr->is_neg_inf && curr->key == key &&
        !curr->marked.load(std::memory_order_acquire)) {
      result = curr->val;
    }
    curr->lock.unlock();
    pred->lock.unlock();
    return result;
  }

  // Shared lookup - only available for locks with read_lock/read_unlock.
  template <class L = Lock>
  auto get_shared(key_type key) noexcept
      -> decltype(std::declval<L>().read_lock(), std::optional<value_type>{}) {
    node* pred = head_;
    pred->lock.read_lock();
    for (int lvl = MAX_LEVEL - 1; lvl > 0; --lvl) {
      while (node_lt(pred->next[lvl], key)) {
        node* x = pred->next[lvl];
        x->lock.read_lock();
        pred->lock.read_unlock();
        pred = x;
      }
    }
    while (node_lt(pred->next[0], key)) {
      node* x = pred->next[0];
      x->lock.read_lock();
      pred->lock.read_unlock();
      pred = x;
    }
    node* curr = pred->next[0];
    curr->lock.read_lock();
    std::optional<value_type> result;
    if (!curr->is_pos_inf && !curr->is_neg_inf && curr->key == key &&
        !curr->marked.load(std::memory_order_acquire)) {
      result = curr->val;
    }
    curr->lock.read_unlock();
    pred->lock.read_unlock();
    return result;
  }

  // Optimistic lookup - only available for locks with read_begin/read_validate.
  // Multi-level traversal: version-validate each hop, restart on any mismatch.
  // Safe to dereference pointers because nodes are never freed.
  template <class L = Lock>
  auto get_optimistic(key_type key) noexcept
      -> decltype(std::declval<const L>().read_begin(), std::optional<value_type>{}) {
    for (;;) {
      node* pred = head_;
      auto v_pred = pred->lock.read_begin();
      bool restart = false;

      for (int lvl = MAX_LEVEL - 1; lvl >= 0 && !restart; --lvl) {
        while (true) {
          node* next = pred->next[lvl];
          if (!pred->lock.read_validate(v_pred)) { restart = true; break; }
          if (!node_lt(next, key)) break;
          auto v_next = next->lock.read_begin();
          pred = next;
          v_pred = v_next;
        }
      }
      if (restart) continue;

      node* curr = pred->next[0];
      auto v_curr = curr->lock.read_begin();
      if (!pred->lock.read_validate(v_pred)) continue;

      bool hit = !curr->is_pos_inf && !curr->is_neg_inf && curr->key == key &&
                 !curr->marked.load(std::memory_order_acquire);
      value_type v = hit ? curr->val : 0;
      if (!curr->lock.read_validate(v_curr)) continue;
      if (!pred->lock.read_validate(v_pred)) continue;
      return hit ? std::optional<value_type>{v} : std::nullopt;
    }
  }

  // Insert or update. Returns true if a new key was inserted.
  //
  // Lock order: top-down across levels (leftmost pred first), because higher-
  // level preds are further left in the list. This matches get()'s left-to-
  // right hand-over-hand order, which is required to prevent deadlock.
  bool put(key_type key, value_type val) noexcept {
    node* preds[MAX_LEVEL];
    node* succs[MAX_LEVEL];
    for (;;) {
      int lfound = find(key, preds, succs);
      if (lfound != -1) {
        node* found = succs[lfound];
        found->lock.lock();
        if (found->marked.load(std::memory_order_acquire)) {
          found->lock.unlock();
          continue;
        }
        while (!found->fully_linked.load(std::memory_order_acquire)) cpu_relax();
        found->val = val;
        found->lock.unlock();
        return false;
      }

      int new_level = random_level();
      node* locked[MAX_LEVEL];
      int locked_count = 0;
      bool valid = true;
      for (int lvl = new_level - 1; lvl >= 0 && valid; --lvl) {
        node* pred = preds[lvl];
        node* succ = succs[lvl];
        bool is_dup = false;
        for (int j = 0; j < locked_count; ++j) {
          if (locked[j] == pred) { is_dup = true; break; }
        }
        if (!is_dup) {
          pred->lock.lock();
          locked[locked_count++] = pred;
        }
        if (pred->marked.load(std::memory_order_acquire) ||
            (succ != nullptr && succ->marked.load(std::memory_order_acquire)) ||
            pred->next[lvl] != succ) {
          valid = false;
        }
      }
      if (!valid) {
        for (int j = 0; j < locked_count; ++j) locked[j]->lock.unlock();
        continue;
      }

      node* n = new node(new_level - 1, key, val, /*is_sentinel*/ false);
      for (int lvl = 0; lvl < new_level; ++lvl) n->next[lvl] = succs[lvl];
      for (int lvl = 0; lvl < new_level; ++lvl) preds[lvl]->next[lvl] = n;
      n->fully_linked.store(true, std::memory_order_release);
      for (int j = 0; j < locked_count; ++j) locked[j]->lock.unlock();
      return true;
    }
  }

  // Remove. Returns true if the key existed.
  //
  // Lock order: preds top-down across levels (leftmost first), then victim
  // last. This matches get()'s left-to-right order (preds are all to the left
  // of victim in list order). top_level is immutable so it can be read without
  // holding the lock.
  bool remove(key_type key) noexcept {
    node* preds[MAX_LEVEL];
    node* succs[MAX_LEVEL];

    for (;;) {
      int lfound = find(key, preds, succs);
      if (lfound == -1) return false;
      node* victim = succs[lfound];
      int victim_top = victim->top_level;    // immutable — safe to read unlocked
      if (!victim->fully_linked.load(std::memory_order_acquire) ||
          victim_top != lfound ||
          victim->marked.load(std::memory_order_acquire)) {
        return false;
      }

      node* locked[MAX_LEVEL];
      int locked_count = 0;
      bool valid = true;
      for (int lvl = victim_top; lvl >= 0 && valid; --lvl) {
        node* pred = preds[lvl];
        bool is_dup = false;
        for (int j = 0; j < locked_count; ++j) {
          if (locked[j] == pred) { is_dup = true; break; }
        }
        if (!is_dup) {
          pred->lock.lock();
          locked[locked_count++] = pred;
        }
        if (pred->marked.load(std::memory_order_acquire) ||
            pred->next[lvl] != victim) {
          valid = false;
        }
      }
      if (!valid) {
        for (int j = 0; j < locked_count; ++j) locked[j]->lock.unlock();
        continue;
      }
      victim->lock.lock();
      if (victim->marked.load(std::memory_order_acquire)) {
        victim->lock.unlock();
        for (int j = 0; j < locked_count; ++j) locked[j]->lock.unlock();
        return false;
      }
      victim->marked.store(true, std::memory_order_release);
      for (int lvl = victim_top; lvl >= 0; --lvl)
        preds[lvl]->next[lvl] = victim->next[lvl];
      victim->lock.unlock();
      for (int j = 0; j < locked_count; ++j) locked[j]->lock.unlock();
      return true;
    }
  }

private:
  struct node {
    alignas(64) Lock  lock{};
    key_type          key;
    value_type        val;
    int               top_level;    // highest valid index in next[]
    std::atomic<bool> marked{false};
    std::atomic<bool> fully_linked{false};
    bool              is_neg_inf = false;
    bool              is_pos_inf = false;
    node*             next[MAX_LEVEL]{};

    node(int top_level_, key_type k, value_type v, bool is_sentinel)
      : key(k), val(v), top_level(top_level_) {
      if (is_sentinel) fully_linked.store(true, std::memory_order_relaxed);
    }
  };

  node* head_ = nullptr;
  node* tail_ = nullptr;

  // key ordering with sentinels: head < any real key < tail
  static bool node_lt(const node* n, key_type key) noexcept {
    if (n->is_neg_inf) return true;
    if (n->is_pos_inf) return false;
    return n->key < key;
  }

  // Unlocked search. Fills preds/succs for each level and returns the level at
  // which key was found (-1 otherwise). succs[lvl] is the first node with
  // key >= requested at that level.
  int find(key_type key, node** preds, node** succs) noexcept {
    int lfound = -1;
    node* pred = head_;
    for (int lvl = MAX_LEVEL - 1; lvl >= 0; --lvl) {
      node* curr = pred->next[lvl];
      while (node_lt(curr, key)) {
        pred = curr;
        curr = pred->next[lvl];
      }
      if (lfound == -1 && !curr->is_pos_inf && !curr->is_neg_inf &&
          curr->key == key)
        lfound = lvl;
      preds[lvl] = pred;
      succs[lvl] = curr;
    }
    return lfound;
  }

  static int random_level() noexcept {
    thread_local std::mt19937_64 rng(
        std::hash<std::thread::id>{}(std::this_thread::get_id()) ^ 0x9E3779B97F4A7C15ULL);
    int lvl = 1;
    // geometric with p=1/2
    while ((rng() & 1) && lvl < MAX_LEVEL) ++lvl;
    return lvl;
  }
};
