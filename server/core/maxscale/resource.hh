#pragma once
/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2019-07-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

/** @file REST API resources */

#include <maxscale/cppdefs.hh>

#include <string>
#include <deque>

#include <maxscale/server.h>

#include "http.hh"
#include "httprequest.hh"
#include "httpresponse.hh"
#include "monitor.h"
#include "service.h"
#include "filter.h"
#include "session.h"

typedef HttpResponse (*ResourceCallback)(const HttpRequest& request);

class Resource
{
    Resource(const Resource&);
    Resource& operator = (const Resource&);
public:

    Resource(ResourceCallback cb, int components, ...);
    ~Resource();

    /**
     * @brief Check if a request matches this resource
     *
     * @param request Request to match
     *
     * @return True if this request matches this resource
     */
    bool match(const HttpRequest& request) const;

    /**
     * @brief Handle a HTTP request
     *
     * @param request Request to handle
     *
     * @return Response to the request
     */
    HttpResponse call(const HttpRequest& request) const;

private:

    bool matching_variable_path(const std::string& path, const std::string& target) const;

    ResourceCallback        m_cb;   /**< Resource handler callback */
    std::deque<std::string> m_path; /**< Path components */
};

/**
 * @brief Handle a HTTP request
 *
 * @param request Request to handle
 *
 * @return Response to request
 */
HttpResponse resource_handle_request(const HttpRequest& request);
