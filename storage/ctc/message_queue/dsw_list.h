/*
  Copyright (C) 2023. Huawei Technologies Co., Ltd. All rights reserved.
  
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#ifndef __dsw_list_pub_h__
#define __dsw_list_pub_h__

#include <stdio.h>
#include "dsw_typedef.h"
#ifdef __cplusplus
extern "C" {
#endif /* __cpluscplus */

#ifdef _PCLINT_
typedef struct list_head list_head_t;
#else
typedef struct list_head {
    struct list_head *prev;
    struct list_head *next;
} list_head_t;
#endif

static inline void list_init_head(list_head_t *head)
{
    head->next = head;
    head->prev = head;
}

static inline void __list_add(list_head_t *new_node, list_head_t *prev_node, list_head_t *next_node)
{
    new_node->prev = prev_node;
    new_node->next = next_node;
    prev_node->next = new_node;
    next_node->prev = new_node;
}

static inline void __list_del(list_head_t *prev, list_head_t *next)
{
    prev->next = next;
    next->prev = prev;
}

static inline void list_add_tail(list_head_t *new_node, list_head_t *head)
{
    __list_add(new_node, head->prev, head);
}

static inline void list_add_first(list_head_t *new_node, list_head_t *head)
{
    __list_add(new_node, head, head->next);
}

static inline void list_insert_node(list_head_t *new_node, list_head_t *prev_node, list_head_t *next_node)
{
    __list_add(new_node, prev_node, next_node);
}

static inline void list_del_node(list_head_t *node)
{
    __list_del(node->prev, node->next);
    list_init_head(node);
}

/*
 * @return the first node, which has been deleted;
 * @if return is NULL, the queue is empty
 */
static inline list_head_t *list_del_first_node(list_head_t *head)
{
    list_head_t *ret;

    ret = head->next;
    if (ret == head) {
        /* the list is free */
        return NULL;
    } else {
        list_del_node(ret);
    }

    return ret;
}

static inline list_head_t *list_get_first_node(list_head_t *head)
{
    list_head_t *ret;

    ret = head->next;
    if (ret == head) {
        /* the list is free */
        return NULL;
    }

    return head->next;
}

static inline list_head_t *list_get_tail_node(list_head_t *head)
{
    list_head_t *ret;

    ret = head->prev;
    if (ret == head) {
        /* the list is free */
        return NULL;
    }

    return head->prev;
}

/*
 * @return the last node, which has been deleted;
 * @if return is NULL, the queue is empty
 */
static inline list_head_t *list_del_tail_node(list_head_t *head)
{
    list_head_t *ret;

    ret = head->prev;
    if (ret == head) {
        /* the list is free */
        return NULL;
    } else {
        list_del_node(ret);
    }

    return ret;
}

/**
 * list_replace - replace old entry by new one
 * @old : the element to be replaced
 * @new : the new element to insert
 *
 * If @old was empty, it will be overwritten.
 */
static inline void list_replace(list_head_t *old_node, list_head_t *new_node)
{
    new_node->next = old_node->next;
    new_node->next->prev = new_node;
    new_node->prev = old_node->prev;
    new_node->prev->next = new_node;
    list_init_head(old_node);
}

/*
 * Description: to check whether the node is in queue.
 * return: @1 ture; @ 2 false.
 */
static inline dsw_int list_check_in_queue(list_head_t *node)
{
    if ((node->next == node) && (node->prev == node)) {
        return 0;
    }

    return 1;
}

static inline dsw_int list_check_null(list_head_t *node)
{
    if ((node->next != NULL) && (node->prev != NULL)) {
        return 0;
    }

    return 1;
}

/*
 * Description: to check whether the queue is empty.
 * return: @1 ture; @ 2 false.
 */
static inline dsw_int list_is_empty(list_head_t *head)
{
    if ((head->next == head) && (head->prev == head)) {
        return 1;
    }

    return 0;
}

/*
 * Description: Add second_list to the tail of first_list, and empty the second list
 * return: @1 ture; @ 2 false.
 */
static inline void list_merge(list_head_t *first_list, list_head_t *second_list)
{
    if (list_is_empty(second_list)) {
        return;
    }

    if (list_is_empty(first_list)) {
        list_replace(second_list, first_list);
        return;
    }

    list_head_t *first_list_end = first_list->prev;
    list_head_t *second_list_begin = second_list->next;
    list_head_t *second_list_end = second_list->prev;

    first_list_end->next = second_list_begin;
    second_list_begin->prev = first_list_end;
    second_list_end->next = first_list;
    first_list->prev = second_list_end;

    list_init_head(second_list);
}

/*
 * Description: to check whether this node is the tail of queue.
 * return: @1 ture; @ 2 false.
 */
static inline dsw_int list_node_is_tail(list_head_t *node, list_head_t *head)
{
    if (node == head->prev) {
        return 1;
    }

    return 0;
}

/*
 * Description: to check whether this node is the first of queue.
 * return: @1 ture; @ 2 false.
 */
static inline dsw_int list_node_is_first(list_head_t *node, list_head_t *head)
{
    if (node == head->next) {
        return 1;
    }

    return 0;
}

/**
 * __list_for_each  iterate over a list
 * @pos:    the &struct list_head to use as a loop cursor.
 * @head:   the head for your list.
 *
 */
#define list_for_each(node, head) for (node = (head)->next; node != (head); node = node->next)

/**
 * list_for_each_safe   -   iterate over a list safe against removal of list entry
 * @pos:    the &struct list_head to use as a loop counter.
 * @n:      another &struct list_head to use as temporary storage
 * @head:   the head for your list.
 */
#define list_for_each_safe(pos, n, head) for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)

#ifndef container_of

/**
 * container_of - cast a member of a structure out to the containing structure
 * @ptr:the pointer to the member.
 * @type:the type of the container struct this is embedded in.
 * @member:the name of the member within the struct.
 *
 */

/*lint --emacro((160), container_of) --emacro((42), container_of) --emacro((413), container_of) --emacro((545), container_of)*/
#define container_of(ptr, type, member)                                   \
    ({                                                                    \
        const DSW_TYPEOF(((type *)0)->member) *__mptr = (ptr);            \
        (type *)((char *)__mptr - ((unsigned long)&((type *)0)->member)); \
    })

#endif

/**
 * list_entry - get the struct for this entry
 * @ptr:the &struct list_head pointer.
 * @type:the type of the struct this is embedded in.
 * @member:the name of the list_struct within the struct.
 */

#define list_entry(ptr, type, member) container_of(ptr, type, member)


#define list_get_first_entry(ptr, type, member) list_entry((ptr)->next, type, member)


static inline void __list_splice(const struct list_head *list, struct list_head *prev, struct list_head *next)
{
    struct list_head *first = list->next;
    struct list_head *last = list->prev;
    first->prev = prev;
    prev->next = first;
    last->next = next;
    next->prev = last;
}


static inline void list_del_set_null(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
    entry->next = entry->prev = NULL;
}

static inline void list_splice_tail_init(struct list_head *list, struct list_head *head)
{
    if (!list_is_empty(list)) {
        __list_splice(list, head->prev, head);
        list_init_head(list);
    }
}


/*
 * Description: get list size.
 * return: @1 ture; @ 2 false.
 */
static inline dsw_int list_size(list_head_t *head)
{
    int size = 0;
    list_head_t *node = NULL;

    list_for_each(node, head)
    {
        size++;
    }

    return size;
}

static inline void list_splice_head_init(struct list_head *list, struct list_head *head)
{
    if (!list_is_empty(list)) {
        __list_splice(list, head, head->next);
        list_init_head(list);
    }
}

/* ************ List BEGIN ************ */
typedef struct dsw_list_s {
    list_head_t list_head;
    dsw_int node_num;
} dsw_list_t;

typedef struct dsw_lock_list_s {
    list_head_t list_head;
    dsw_int node_num;
    pthread_mutex_t mutex;
} dsw_lock_list_t;

static inline void dsw_list_init(dsw_list_t *list)
{
    list_init_head(&(list->list_head));
    list->node_num = 0;
}

static inline void dsw_lock_list_init(dsw_lock_list_t *lock_list)
{
    DSW_THREAD_MUTEX_INIT(&(lock_list->mutex), NULL);
    list_init_head(&(lock_list->list_head));
    lock_list->node_num = 0;
}

static inline void dsw_list_add_tail(list_head_t *node, dsw_list_t *list)
{
    list_add_tail(node, &(list->list_head));
    list->node_num += 1;
}

static inline void dsw_list_add_first(list_head_t *node, dsw_list_t *list)
{
    list_add_first(node, &(list->list_head));
    list->node_num += 1;
}

static inline void dsw_lock_list_add_first(list_head_t *node, dsw_lock_list_t *lock_list)
{
    DSW_THREAD_MUTEX_LOCK(&(lock_list->mutex));
    list_add_first(node, &(lock_list->list_head));
    lock_list->node_num += 1;
    DSW_THREAD_MUTEX_UNLOCK(&(lock_list->mutex));
}

static inline void dsw_lock_list_add_tail(list_head_t *node, dsw_lock_list_t *lock_list)
{
    DSW_THREAD_MUTEX_LOCK(&(lock_list->mutex));
    list_add_tail(node, &(lock_list->list_head));
    lock_list->node_num += 1;
    DSW_THREAD_MUTEX_UNLOCK(&(lock_list->mutex));
}

static inline void dsw_list_del_node(list_head_t *node, dsw_list_t *list)
{
    DSW_ASSERT_INNER(list->node_num > 0);
    list_del_node(node);
    list->node_num -= 1;
}

static inline void dsw_lock_list_del_node(list_head_t *node, dsw_lock_list_t *lock_list)
{
    DSW_THREAD_MUTEX_LOCK(&(lock_list->mutex));
    DSW_ASSERT_INNER(lock_list->node_num > 0);
    list_del_node(node);
    lock_list->node_num -= 1;
    DSW_THREAD_MUTEX_UNLOCK(&(lock_list->mutex));
}

static inline list_head_t *dsw_list_del_first_node(dsw_list_t *list)
{
    list_head_t *node = list_del_first_node(&(list->list_head));
    if (NULL != node) {
        DSW_ASSERT(list->node_num > 0);
        list->node_num -= 1;
        return node;
    } else {
        DSW_ASSERT(0 == list->node_num);
        return NULL;
    }
}

static inline list_head_t *dsw_list_get_first_node(dsw_list_t *list)
{
    list_head_t *node = list_get_first_node(&(list->list_head));
    if (NULL != node) {
        return node;
    } else {
        DSW_ASSERT(0 == list->node_num);
        return NULL;
    }
}

static inline list_head_t *dsw_list_del_tail_node(dsw_list_t *list)
{
    list_head_t *node = list_del_tail_node(&(list->list_head));
    if (NULL != node) {
        DSW_ASSERT(list->node_num > 0);
        list->node_num -= 1;
        return node;
    } else {
        DSW_ASSERT(0 == list->node_num);
        return NULL;
    }
}

static inline list_head_t *dsw_lock_list_del_first_node(dsw_lock_list_t *lock_list)
{
    DSW_THREAD_MUTEX_LOCK(&(lock_list->mutex));
    list_head_t *node = list_del_first_node(&(lock_list->list_head));
    if (NULL != node) {
        DSW_ASSERT(lock_list->node_num > 0);
        lock_list->node_num -= 1;
    }

    DSW_THREAD_MUTEX_UNLOCK(&(lock_list->mutex));
    return node;
}

static inline void dsw_list_merge(dsw_list_t *old_list, dsw_list_t *new_list)
{
    list_head_t *iter_node = dsw_list_del_first_node(old_list);
    while (iter_node) {
        dsw_list_add_tail(iter_node, new_list);
        iter_node = dsw_list_del_first_node(old_list);
    }
}

// �ж�node�Ƿ���������
static inline dsw_bool is_node_isolated(list_head_t *node)
{
    return ((node->next == node->prev) && (node->next == node));
}

/* ************ List End ************ */

#ifdef __cplusplus
}
#endif /* __cpluscplus */
#endif // __dsw_list_pub_h__
 