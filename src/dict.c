/*
 * Copyright © 2022 John Viega
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *  Name:           dict.c
 *  Description:    High-level dictionary based on witchhat.
 *
 *  Author:         John Viega, john@zork.org
 */

#include <hatrack.h>

static hatrack_hash_t hatrack_dict_get_hash_value(hatrack_dict_t *, void *);
static void           hatrack_dict_record_cleanup(void *);

hatrack_dict_t *
hatrack_dict_new(uint32_t key_type)
{
    hatrack_dict_t *ret;

    ret = (hatrack_dict_t *)malloc(sizeof(hatrack_dict_t));

    hatrack_dict_init(ret, key_type);

    return ret;
}

void
hatrack_dict_delete(hatrack_dict_t *self)
{
    hatrack_dict_cleanup(self);

    free(self);

    return;
}

void
hatrack_dict_init(hatrack_dict_t *self, uint32_t key_type)
{
    witchhat_init(&self->witchhat_instance);

    switch (key_type) {
    case HATRACK_DICT_KEY_TYPE_INT:
    case HATRACK_DICT_KEY_TYPE_REAL:
    case HATRACK_DICT_KEY_TYPE_CSTR:
    case HATRACK_DICT_KEY_TYPE_PTR:
    case HATRACK_DICT_KEY_TYPE_OBJ_INT:
    case HATRACK_DICT_KEY_TYPE_OBJ_REAL:
    case HATRACK_DICT_KEY_TYPE_OBJ_CSTR:
    case HATRACK_DICT_KEY_TYPE_OBJ_PTR:
    case HATRACK_DICT_KEY_TYPE_OBJ_CUSTOM:
        self->key_type = key_type;
        break;
    default:
        abort();
    }

    self->hash_info.offsets.hash_offset  = 0;
    self->hash_info.offsets.cache_offset = HATRACK_DICT_NO_CACHE;
    self->free_handler                   = NULL;

    return;
}

void
hatrack_dict_cleanup(hatrack_dict_t *self)
{
    uint64_t           i;
    witchhat_store_t * store;
    witchhat_bucket_t *bucket;
    hatrack_hash_t     hv;
    witchhat_record_t  record;

    if (self->free_handler) {
        store = self->witchhat_instance.store_current;

        for (i = 0; i <= store->last_slot; i++) {
            bucket = &store->buckets[i];
            hv     = atomic_load(&bucket->hv);

            if (hatrack_bucket_unreserved(hv)) {
                continue;
            }

            record = atomic_load(&bucket->record);

            if (!record.info) {
                continue;
            }

            (*self->free_handler)((hatrack_dict_item_t *)record.item);
        }
    }

    mmm_retire(atomic_load(&self->witchhat_instance.store_current));

    return;
}

void
hatrack_dict_set_hash_offset(hatrack_dict_t *self, int32_t offset)
{
    self->hash_info.offsets.hash_offset = offset;

    return;
}

void
hatrack_dict_set_cache_offset(hatrack_dict_t *self, int32_t offset)
{
    self->hash_info.offsets.cache_offset = offset;

    return;
}

void
hatrack_dict_set_custom_hash(hatrack_dict_t *self, hatrack_hash_function_t func)
{
    self->hash_info.custom_hash = func;

    return;
}

void
hatrack_dict_set_free_handler(hatrack_dict_t *self, hatrack_free_handler_t func)
{
    self->free_handler = func;

    return;
}

void *
hatrack_dict_get(hatrack_dict_t *self, void *key, bool *found)
{
    hatrack_hash_t       hv;
    hatrack_dict_item_t *item;

    hv   = hatrack_dict_get_hash_value(self, key);
    item = witchhat_get(&self->witchhat_instance, hv, found);

    if (!item) {
        if (found) {
            *found = false;
        }

        return NULL;
    }

    if (found) {
        *found = true;
    }

    return item->value;
}

void
hatrack_dict_put(hatrack_dict_t *self, void *key, void *value)
{
    hatrack_hash_t       hv;
    hatrack_dict_item_t *new_item;
    hatrack_dict_item_t *old_item;

    hv = hatrack_dict_get_hash_value(self, key);

    mmm_start_basic_op();

    new_item        = mmm_alloc_committed(sizeof(hatrack_dict_item_t));
    new_item->key   = key;
    new_item->value = value;

    old_item = witchhat_put(&self->witchhat_instance, hv, new_item, NULL);

    if (old_item) {
        if (self->free_handler) {
            old_item->associated_dict = self;
            mmm_add_cleanup_handler(old_item, hatrack_dict_record_cleanup);
        }

        mmm_retire(old_item);
    }

    mmm_end_op();

    return;
}

bool
hatrack_dict_replace(hatrack_dict_t *self, void *key, void *value)
{
    hatrack_hash_t       hv;
    hatrack_dict_item_t *new_item;
    hatrack_dict_item_t *old_item;

    hv = hatrack_dict_get_hash_value(self, key);

    mmm_start_basic_op();

    new_item        = mmm_alloc_committed(sizeof(hatrack_dict_item_t));
    new_item->key   = key;
    new_item->value = value;

    old_item = witchhat_replace(&self->witchhat_instance, hv, new_item, NULL);

    if (old_item) {
        if (self->free_handler) {
            old_item->associated_dict = self;
            mmm_add_cleanup_handler(old_item, hatrack_dict_record_cleanup);
        }

        mmm_retire(old_item);
        mmm_end_op();

        return true;
    }

    mmm_retire_unused(new_item);
    mmm_end_op();

    return false;
}

bool
hatrack_dict_add(hatrack_dict_t *self, void *key, void *value)
{
    hatrack_hash_t       hv;
    hatrack_dict_item_t *new_item;

    hv = hatrack_dict_get_hash_value(self, key);

    mmm_start_basic_op();

    new_item        = mmm_alloc_committed(sizeof(hatrack_dict_item_t));
    new_item->key   = key;
    new_item->value = value;

    if (witchhat_add(&self->witchhat_instance, hv, new_item)) {
        mmm_end_op();

        return true;
    }

    mmm_retire_unused(new_item);
    mmm_end_op();

    return false;
}

bool
hatrack_dict_remove(hatrack_dict_t *self, void *key)
{
    hatrack_hash_t       hv;
    hatrack_dict_item_t *old_item;

    hv = hatrack_dict_get_hash_value(self, key);

    mmm_start_basic_op();

    old_item = witchhat_remove(&self->witchhat_instance, hv, NULL);

    if (old_item) {
        if (self->free_handler) {
            old_item->associated_dict = self;
            mmm_add_cleanup_handler(old_item, hatrack_dict_record_cleanup);
        }

        mmm_retire(old_item);
        mmm_end_op();

        return true;
    }

    mmm_end_op();

    return false;
}

hatrack_dict_key_t *
hatrack_dict_keys(hatrack_dict_t *self, uint64_t *num)
{
    hatrack_view_t *     view;
    hatrack_dict_key_t * ret;
    hatrack_dict_item_t *item;
    uint64_t             alloc_len;
    uint32_t             i;

    view      = witchhat_view(&self->witchhat_instance, num, false);
    alloc_len = sizeof(hatrack_dict_key_t) * *num;
    ret       = (hatrack_dict_key_t *)malloc(alloc_len);

    for (i = 0; i < *num; i++) {
        item   = (hatrack_dict_item_t *)view[i].item;
        ret[i] = item->key;
    }

    free(view);

    return ret;
}

hatrack_dict_value_t *
hatrack_dict_values(hatrack_dict_t *self, uint64_t *num)
{
    hatrack_view_t *      view;
    hatrack_dict_value_t *ret;
    hatrack_dict_item_t * item;
    uint64_t              alloc_len;
    uint32_t              i;

    view      = witchhat_view(&self->witchhat_instance, num, false);
    alloc_len = sizeof(hatrack_dict_key_t) * *num;
    ret       = (hatrack_dict_key_t *)malloc(alloc_len);

    for (i = 0; i < *num; i++) {
        item   = (hatrack_dict_item_t *)view[i].item;
        ret[i] = item->value;
    }

    free(view);

    return ret;
}

hatrack_dict_item_t *
hatrack_dict_items(hatrack_dict_t *self, uint64_t *num)
{
    hatrack_view_t *     view;
    hatrack_dict_item_t *ret;
    hatrack_dict_item_t *item;
    uint64_t             alloc_len;
    uint32_t             i;

    view      = witchhat_view(&self->witchhat_instance, num, false);
    alloc_len = sizeof(hatrack_dict_key_t) * *num;
    ret       = (hatrack_dict_item_t *)calloc(1, alloc_len);

    for (i = 0; i < *num; i++) {
        item         = (hatrack_dict_item_t *)view[i].item;
        ret[i].key   = item->key;
        ret[i].value = item->value;
    }

    free(view);

    return ret;
}

hatrack_dict_key_t *
hatrack_dict_keys_sort(hatrack_dict_t *self, uint64_t *num)
{
    hatrack_view_t *     view;
    hatrack_dict_key_t * ret;
    hatrack_dict_item_t *item;
    uint64_t             alloc_len;
    uint32_t             i;

    view      = witchhat_view(&self->witchhat_instance, num, true);
    alloc_len = sizeof(hatrack_dict_key_t) * *num;
    ret       = (hatrack_dict_key_t *)malloc(alloc_len);

    for (i = 0; i < *num; i++) {
        item   = (hatrack_dict_item_t *)view[i].item;
        ret[i] = item->key;
    }

    free(view);

    return ret;
}

hatrack_dict_value_t *
hatrack_dict_values_sort(hatrack_dict_t *self, uint64_t *num)
{
    hatrack_view_t *      view;
    hatrack_dict_value_t *ret;
    hatrack_dict_item_t * item;
    uint64_t              alloc_len;
    uint32_t              i;

    view      = witchhat_view(&self->witchhat_instance, num, true);
    alloc_len = sizeof(hatrack_dict_key_t) * *num;
    ret       = (hatrack_dict_key_t *)malloc(alloc_len);

    for (i = 0; i < *num; i++) {
        item   = (hatrack_dict_item_t *)view[i].item;
        ret[i] = item->value;
    }

    free(view);

    return ret;
}

hatrack_dict_item_t *
hatrack_dict_items_sort(hatrack_dict_t *self, uint64_t *num)
{
    hatrack_view_t *     view;
    hatrack_dict_item_t *ret;
    hatrack_dict_item_t *item;
    uint64_t             alloc_len;
    uint32_t             i;

    view      = witchhat_view(&self->witchhat_instance, num, true);
    alloc_len = sizeof(hatrack_dict_key_t) * *num;
    ret       = (hatrack_dict_item_t *)calloc(1, alloc_len);

    for (i = 0; i < *num; i++) {
        item         = (hatrack_dict_item_t *)view[i].item;
        ret[i].key   = item->key;
        ret[i].value = item->value;
    }

    free(view);

    return ret;
}

static hatrack_hash_t
hatrack_dict_get_hash_value(hatrack_dict_t *self, void *key)
{
    hatrack_hash_t hv;
    int32_t        offset;

    switch (self->key_type) {
    case HATRACK_DICT_KEY_TYPE_OBJ_CUSTOM:
        return (*self->hash_info.custom_hash)(key);

    case HATRACK_DICT_KEY_TYPE_INT:
        return hash_int((uint64_t)key);

    case HATRACK_DICT_KEY_TYPE_REAL:
        return hash_double(*(double *)key);

    case HATRACK_DICT_KEY_TYPE_CSTR:
        return hash_cstr((char *)key);

    case HATRACK_DICT_KEY_TYPE_PTR:
        return hash_pointer(key);

    default:
        break;
    }

    offset = self->hash_info.offsets.cache_offset;

    if (offset != (int32_t)HATRACK_DICT_NO_CACHE) {
        hv = *(hatrack_hash_t *)(((uint8_t *)key) + offset);

        if (!hatrack_bucket_unreserved(hv)) {
            return hv;
        }
    }

    switch (self->key_type) {
    case HATRACK_DICT_KEY_TYPE_OBJ_INT:
        hv = hash_int((uint64_t)key);
        break;
    case HATRACK_DICT_KEY_TYPE_OBJ_REAL:
        hv = hash_double(*(double *)key);
        break;
    case HATRACK_DICT_KEY_TYPE_OBJ_CSTR:
        hv = hash_cstr((char *)key);
        break;
    case HATRACK_DICT_KEY_TYPE_OBJ_PTR:
        hv = hash_pointer(key);
        break;
    default:
        abort();
    }

    if (offset != (int32_t)HATRACK_DICT_NO_CACHE) {
        *(hatrack_hash_t *)(((uint8_t *)key) + offset) = hv;
    }

    return hv;
}

static void
hatrack_dict_record_cleanup(void *void_record)
{
    hatrack_dict_t *     dict;
    hatrack_dict_item_t *record;

    record = (hatrack_dict_item_t *)void_record;
    dict   = (hatrack_dict_t *)record->associated_dict;

    (*dict->free_handler)(record);

    return;
}