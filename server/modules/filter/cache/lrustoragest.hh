#pragma once
/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl.
 *
 * Change Date: 2019-07-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include <maxscale/cppdefs.hh>
#include "lrustorage.hh"

class LRUStorageST : public LRUStorage
{
public:
    ~LRUStorageST();

    static LRUStorageST* create(Storage* pstorage, uint64_t max_count, uint64_t max_size);

    cache_result_t get_info(uint32_t what,
                            json_t** ppInfo) const;

    cache_result_t get_value(const CACHE_KEY& key,
                             uint32_t flags,
                             GWBUF** ppValue) const;

    cache_result_t put_value(const CACHE_KEY& key,
                             const GWBUF* pValue);

    cache_result_t del_value(const CACHE_KEY& key);

    cache_result_t get_head(CACHE_KEY* pKey,
                            GWBUF** ppValue) const;

    cache_result_t get_tail(CACHE_KEY* pKey,
                            GWBUF** ppValue) const;

    cache_result_t get_size(uint64_t* pSize) const;

    cache_result_t get_items(uint64_t* pItems) const;

private:
    LRUStorageST(Storage* pstorage, uint64_t max_count, uint64_t max_size);

    LRUStorageST(const LRUStorageST&);
    LRUStorageST& operator = (const LRUStorageST&);
};
