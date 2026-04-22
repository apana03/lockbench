#pragma once
#include <atomic>
#include <cstdint>
#include <optional>
#include <utility>

template <class Lock, std::size_t FANOUT = 16>
class bptree_index {
public:
  using key_type   = std::uint64_t;
  using value_type = std::uint64_t;

  bptree_index() {
    node* root = new node();
    root->is_leaf = true;
    root->n = 0;
    root_.store(root, std::memory_order_release);
  }

  ~bptree_index() { free_subtree(root_.load(std::memory_order_relaxed)); }

  bptree_index(const bptree_index&) = delete;
  bptree_index& operator=(const bptree_index&) = delete;

  // Exclusive lookup with lock coupling.
  std::optional<value_type> get(key_type key) noexcept {
    root_latch_.lock();
    node* cur = root_.load(std::memory_order_acquire);
    cur->lock.lock();
    root_latch_.unlock();
    while (!cur->is_leaf) {
      int i = upper_slot(cur, key);
      node* child = cur->children[i];
      child->lock.lock();
      cur->lock.unlock();
      cur = child;
    }
    std::optional<value_type> result = find_in_leaf(cur, key);
    cur->lock.unlock();
    return result;
  }

  // Shared lookup - only available for locks with read_lock/read_unlock.
  template <class L = Lock>
  auto get_shared(key_type key) noexcept
      -> decltype(std::declval<L>().read_lock(), std::optional<value_type>{}) {
    root_latch_.read_lock();
    node* cur = root_.load(std::memory_order_acquire);
    cur->lock.read_lock();
    root_latch_.read_unlock();
    while (!cur->is_leaf) {
      int i = upper_slot(cur, key);
      node* child = cur->children[i];
      child->lock.read_lock();
      cur->lock.read_unlock();
      cur = child;
    }
    std::optional<value_type> result = find_in_leaf(cur, key);
    cur->lock.read_unlock();
    return result;
  }

  // Optimistic lookup - only available for locks with read_begin/read_validate.
  template <class L = Lock>
  auto get_optimistic(key_type key) noexcept
      -> decltype(std::declval<const L>().read_begin(), std::optional<value_type>{}) {
    for (;;) {
      node* cur = root_.load(std::memory_order_acquire);
      auto v = cur->lock.read_begin();
      bool restart = false;
      while (!cur->is_leaf) {
        int i = upper_slot(cur, key);
        node* child = cur->children[i];
        if (!cur->lock.read_validate(v)) { restart = true; break; }
        cur = child;
        v = cur->lock.read_begin();
      }
      if (restart) continue;
      std::optional<value_type> result = find_in_leaf(cur, key);
      if (!cur->lock.read_validate(v)) continue;
      return result;
    }
  }

  // Insert or update. Returns true if a new key was inserted.
  bool put(key_type key, value_type val) noexcept {
    root_latch_.lock();
    node* held[MAX_HEIGHT + 1];
    int held_n = 0;
    bool root_latch_held = true;

    node* cur = root_.load(std::memory_order_acquire);
    cur->lock.lock();
    held[held_n++] = cur;

    while (!cur->is_leaf) {
      int i = upper_slot(cur, key);
      node* child = cur->children[i];
      child->lock.lock();
      if (child->n < FANOUT) {
        // safe - release all ancestors (and root_latch_ if still held)
        for (int j = 0; j < held_n; ++j) held[j]->lock.unlock();
        if (root_latch_held) { root_latch_.unlock(); root_latch_held = false; }
        held_n = 0;
      }
      held[held_n++] = child;
      cur = child;
    }

    // cur is the leaf; ancestors above "safe" boundary are unlocked.
    bool inserted;
    if (cur->n < FANOUT) {
      inserted = leaf_insert_or_update(cur, key, val);
      for (int j = 0; j < held_n; ++j) held[j]->lock.unlock();
      if (root_latch_held) root_latch_.unlock();
      return inserted;
    }

    // Check for update-in-place before splitting.
    int idx = leaf_find_exact(cur, key);
    if (idx >= 0) {
      cur->vals[idx] = val;
      for (int j = 0; j < held_n; ++j) held[j]->lock.unlock();
      if (root_latch_held) root_latch_.unlock();
      return false;
    }

    // Leaf needs to split. Propagate sep_key up through held ancestors.
    key_type sep_key;
    node* right = split_leaf(cur, key, val, sep_key);
    // held_n - 1 is cur (the leaf). Walk ancestors from held_n - 2 down to 0.
    int stack_i = held_n - 2;
    node* new_right = right;
    key_type push_key = sep_key;
    while (stack_i >= 0) {
      node* parent = held[stack_i];
      if (parent->n < FANOUT) {
        internal_insert(parent, push_key, new_right);
        // release everything and return
        for (int j = 0; j < held_n; ++j) held[j]->lock.unlock();
        if (root_latch_held) root_latch_.unlock();
        return true;
      }
      // parent is full - split it
      key_type parent_sep;
      node* parent_right = split_internal(parent, push_key, new_right, parent_sep);
      push_key = parent_sep;
      new_right = parent_right;
      --stack_i;
    }

    // Root split - build a new root. root_latch_ must still be held.
    node* old_root = held[0];
    node* new_root = new node();
    new_root->is_leaf = false;
    new_root->n = 1;
    new_root->keys[0] = push_key;
    new_root->children[0] = old_root;
    new_root->children[1] = new_right;
    root_.store(new_root, std::memory_order_release);
    for (int j = 0; j < held_n; ++j) held[j]->lock.unlock();
    if (root_latch_held) root_latch_.unlock();
    return true;
  }

  // Remove. Returns true if the key existed. Does NOT merge under-full leaves,
  // so simple exclusive lock-coupling is sufficient (no risk of concurrent
  // split pulling our target key away under us, because holding a node's lock
  // blocks any splitter that would touch it).
  bool remove(key_type key) noexcept {
    root_latch_.lock();
    node* cur = root_.load(std::memory_order_acquire);
    cur->lock.lock();
    root_latch_.unlock();
    while (!cur->is_leaf) {
      int i = upper_slot(cur, key);
      node* child = cur->children[i];
      child->lock.lock();
      cur->lock.unlock();
      cur = child;
    }
    bool removed = leaf_remove(cur, key);
    cur->lock.unlock();
    return removed;
  }

private:
  static constexpr int MAX_HEIGHT = 32;

  struct node {
    alignas(64) Lock  lock{};
    bool              is_leaf = false;
    std::uint16_t     n = 0;
    key_type          keys[FANOUT];
    // One of these is used depending on is_leaf. Keeping both in-line avoids
    // a second allocation and matches hash_index's single-struct style.
    value_type        vals[FANOUT]{};           // leaf
    node*             children[FANOUT + 1]{};   // internal
    node*             next_leaf = nullptr;      // unused for now
  };

  std::atomic<node*> root_{nullptr};
  Lock               root_latch_{};

  static int upper_slot(const node* n, key_type key) noexcept {
    // For internals: return smallest i such that keys[i] > key.
    // Linear probe is fine for small FANOUT.
    int i = 0;
    while (i < n->n && n->keys[i] <= key) ++i;
    return i;
  }

  static int leaf_find_exact(const node* n, key_type key) noexcept {
    for (int i = 0; i < n->n; ++i) {
      if (n->keys[i] == key) return i;
    }
    return -1;
  }

  static std::optional<value_type> find_in_leaf(const node* n, key_type key) noexcept {
    for (int i = 0; i < n->n; ++i) {
      if (n->keys[i] == key) return n->vals[i];
    }
    return std::nullopt;
  }

  static bool leaf_insert_or_update(node* n, key_type key, value_type val) noexcept {
    // Update-in-place if present.
    for (int i = 0; i < n->n; ++i) {
      if (n->keys[i] == key) { n->vals[i] = val; return false; }
    }
    // Sorted insert.
    int pos = 0;
    while (pos < n->n && n->keys[pos] < key) ++pos;
    for (int i = n->n; i > pos; --i) {
      n->keys[i] = n->keys[i - 1];
      n->vals[i] = n->vals[i - 1];
    }
    n->keys[pos] = key;
    n->vals[pos] = val;
    ++n->n;
    return true;
  }

  static bool leaf_remove(node* n, key_type key) noexcept {
    for (int i = 0; i < n->n; ++i) {
      if (n->keys[i] == key) {
        for (int j = i; j + 1 < n->n; ++j) {
          n->keys[j] = n->keys[j + 1];
          n->vals[j] = n->vals[j + 1];
        }
        --n->n;
        return true;
      }
    }
    return false;
  }

  // Split a full leaf, inserting (key, val) into the correct half. Returns
  // the new right sibling and writes the separator (== first key of right) to *out_sep.
  node* split_leaf(node* left, key_type key, value_type val, key_type& out_sep) noexcept {
    // Gather FANOUT + 1 entries into temp arrays, sorted.
    key_type   ks[FANOUT + 1];
    value_type vs[FANOUT + 1];
    int total = 0;
    bool inserted = false;
    for (int i = 0; i < left->n; ++i) {
      if (!inserted && key < left->keys[i]) {
        ks[total] = key; vs[total] = val; ++total; inserted = true;
      }
      // Update-in-place is handled before split by caller, so no dedup here.
      ks[total] = left->keys[i]; vs[total] = left->vals[i]; ++total;
    }
    if (!inserted) { ks[total] = key; vs[total] = val; ++total; }

    int mid = total / 2;
    node* right = new node();
    right->is_leaf = true;
    right->n = static_cast<std::uint16_t>(total - mid);
    for (int i = 0; i < right->n; ++i) {
      right->keys[i] = ks[mid + i];
      right->vals[i] = vs[mid + i];
    }
    left->n = static_cast<std::uint16_t>(mid);
    for (int i = 0; i < left->n; ++i) {
      left->keys[i] = ks[i];
      left->vals[i] = vs[i];
    }
    right->next_leaf = left->next_leaf;
    left->next_leaf = right;
    out_sep = right->keys[0];
    return right;
  }

  // Insert (sep, right) into a non-full internal node.
  static void internal_insert(node* parent, key_type sep, node* right) noexcept {
    int pos = 0;
    while (pos < parent->n && parent->keys[pos] < sep) ++pos;
    for (int i = parent->n; i > pos; --i) {
      parent->keys[i] = parent->keys[i - 1];
    }
    for (int i = parent->n + 1; i > pos + 1; --i) {
      parent->children[i] = parent->children[i - 1];
    }
    parent->keys[pos] = sep;
    parent->children[pos + 1] = right;
    ++parent->n;
  }

  // Split a full internal node that is receiving (sep, right).
  // Writes the promoted separator to *out_sep and returns the new right internal node.
  node* split_internal(node* left, key_type sep, node* right_child,
                       key_type& out_sep) noexcept {
    key_type ks[FANOUT + 1];
    node*    cs[FANOUT + 2];
    // Existing children layout: cs[0..n].
    for (int i = 0; i < left->n; ++i) ks[i] = left->keys[i];
    for (int i = 0; i <= left->n; ++i) cs[i] = left->children[i];
    // Now insert (sep, right_child) into sorted position.
    int pos = 0;
    while (pos < left->n && ks[pos] < sep) ++pos;
    for (int i = left->n; i > pos; --i) ks[i] = ks[i - 1];
    for (int i = left->n + 1; i > pos + 1; --i) cs[i] = cs[i - 1];
    ks[pos] = sep;
    cs[pos + 1] = right_child;
    int total = left->n + 1;   // number of keys now

    // Promote ks[mid] upward; left gets ks[0..mid-1] and cs[0..mid];
    // new right gets ks[mid+1..total-1] and cs[mid+1..total].
    int mid = total / 2;
    out_sep = ks[mid];

    node* new_right = new node();
    new_right->is_leaf = false;
    new_right->n = static_cast<std::uint16_t>(total - mid - 1);
    for (int i = 0; i < new_right->n; ++i) new_right->keys[i] = ks[mid + 1 + i];
    for (int i = 0; i <= new_right->n; ++i) new_right->children[i] = cs[mid + 1 + i];

    left->n = static_cast<std::uint16_t>(mid);
    for (int i = 0; i < left->n; ++i) left->keys[i] = ks[i];
    for (int i = 0; i <= left->n; ++i) left->children[i] = cs[i];
    return new_right;
  }

  static void free_subtree(node* n) noexcept {
    if (!n) return;
    if (!n->is_leaf) {
      for (int i = 0; i <= n->n; ++i) free_subtree(n->children[i]);
    }
    delete n;
  }
};
