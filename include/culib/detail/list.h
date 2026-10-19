#pragma once

// 侵入式双向循环链表，照 Linux 的 list_head。表头是不带数据的节点，空表指向自己，
// 所以插入和摘除不分首尾。元素从 list_node 派生，链表本身不分配。

namespace wavvix {
namespace detail {

struct list_node {
    list_node() noexcept : next(this), prev(this) {}
    list_node(const list_node&) = delete;
    list_node& operator=(const list_node&) = delete;

    list_node* next;
    list_node* prev;
};

inline bool list_empty(const list_node& head) noexcept {
    return head.next == &head;
}

inline void list_add_tail(list_node& node, list_node& head) noexcept {
    node.next = &head;
    node.prev = head.prev;
    head.prev->next = &node;
    head.prev = &node;
}

// 摘下的节点自成空表，所以再摘一次什么也不做。
inline void list_del_init(list_node& node) noexcept {
    node.prev->next = node.next;
    node.next->prev = node.prev;
    node.next = &node;
    node.prev = &node;
}

// 把 list 的节点整串接到 head 的最前面，list 变成空表。
inline void list_splice_init(list_node& list, list_node& head) noexcept {
    if (list_empty(list)) {
        return;
    }
    list_node* const first = list.next;
    list_node* const last = list.prev;
    first->prev = &head;
    last->next = head.next;
    head.next->prev = last;
    head.next = first;
    list.next = &list;
    list.prev = &list;
}

}  // namespace detail
}  // namespace wavvix
