#include <iostream>
#include <vector>
#include "OptimisticLockCoupling/Tree.h"
void loadKey(TID tid, Key& key) {
  key.setKeyLen(8);
  for (unsigned i = 0; i < 8; ++i) key[i] = (tid >> (56 - i * 8)) & 255;
}
int main() {
  for (unsigned size : {4, 5, 16, 17, 48, 49, 256}) {
    ART_OLC::Tree tree(loadKey);
    auto info = tree.getThreadInfo();
    std::vector<TID> ids;
    for (unsigned i = 0; i < size; ++i) {
      TID id = 256 + ((i * 137) % 256);
      ids.push_back(id);
      Key key; loadKey(id, key); tree.insert(key, id, info);
      for (TID expected : ids) {
        Key query; loadKey(expected, query);
        if (tree.lookup(query, info) != expected) return 1;
      }
    }
    for (unsigned i = 0; i < ids.size(); ++i) {
      Key key; loadKey(ids[i], key); tree.remove(key, ids[i], info);
      if (tree.lookup(key, info) != 0) return 2;
      for (unsigned j = i + 1; j < ids.size(); ++j) {
        Key query; loadKey(ids[j], query);
        if (tree.lookup(query, info) != ids[j]) return 3;
      }
    }
  }
  std::cout << "ART compatibility checks passed\n";
}
