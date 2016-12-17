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
#include <memory>
#include <string>
#include <vector>
#include <tr1/unordered_map>
#include "../../cache_storage_api.hh"

class InMemoryStorage
{
public:
    virtual ~InMemoryStorage();

    static cache_result_t get_key(const char* zdefault_db, const GWBUF* pquery, CACHE_KEY* pkey);

    virtual cache_result_t get_info(uint32_t what, json_t** ppInfo) const = 0;
    virtual cache_result_t get_value(const CACHE_KEY& key, uint32_t flags, GWBUF** ppresult) = 0;
    virtual cache_result_t put_value(const CACHE_KEY& key, const GWBUF* pvalue) = 0;
    virtual cache_result_t del_value(const CACHE_KEY& key) = 0;

    cache_result_t get_head(CACHE_KEY* pKey, GWBUF** ppHead) const;
    cache_result_t get_tail(CACHE_KEY* pKey, GWBUF** ppHead) const;
    cache_result_t get_size(uint64_t* pSize) const;
    cache_result_t get_items(uint64_t* pItems) const;

protected:
    InMemoryStorage(const std::string& name, uint32_t ttl);

    cache_result_t do_get_info(uint32_t what, json_t** ppInfo) const;
    cache_result_t do_get_value(const CACHE_KEY& key, uint32_t flags, GWBUF** ppresult);
    cache_result_t do_put_value(const CACHE_KEY& key, const GWBUF* pvalue);
    cache_result_t do_del_value(const CACHE_KEY& key);

private:
    InMemoryStorage(const InMemoryStorage&);
    InMemoryStorage& operator = (const InMemoryStorage&);

private:
    typedef std::vector<uint8_t> Value;

    struct Entry
    {
        Entry()
        : time(0)
        {}

        uint32_t time;
        Value    value;
    };

    struct Stats
    {
        Stats()
            : size(0)
            , items(0)
            , hits(0)
            , misses(0)
            , updates(0)
            , deletes(0)
        {}

        void fill(json_t* pbject) const;

        uint64_t size;       /*< The total size of the stored values. */
        uint64_t items;      /*< The number of stored items. */
        uint64_t hits;       /*< How many times a key was found in the cache. */
        uint64_t misses;     /*< How many times a key was not found in the cache. */
        uint64_t updates;    /*< How many times an existing key in the cache was updated. */
        uint64_t deletes;    /*< How many times an existing key in the cache was deleted. */
    };

    typedef std::tr1::unordered_map<CACHE_KEY, Entry> Entries;

    std::string name_;
    uint32_t    ttl_;
    Entries     entries_;
    Stats       stats_;
};
