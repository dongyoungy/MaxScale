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
#include "maxscale/resource.hh"

#include <list>
#include <sstream>
#include <map>

#include <maxscale/alloc.h>
#include <maxscale/jansson.hh>
#include <maxscale/spinlock.hh>
#include <maxscale/json_api.h>
#include <maxscale/housekeeper.h>

#include "maxscale/httprequest.hh"
#include "maxscale/httpresponse.hh"
#include "maxscale/session.h"
#include "maxscale/filter.h"
#include "maxscale/monitor.h"
#include "maxscale/service.h"
#include "maxscale/config_runtime.h"
#include "maxscale/modules.h"
#include "maxscale/worker.h"
#include "maxscale/http.hh"

using std::list;
using std::map;
using std::string;
using std::stringstream;
using mxs::SpinLock;
using mxs::SpinLockGuard;

/**
 * Class that keeps track of resource modification times
 */
class ResourceWatcher
{
public:

    ResourceWatcher() :
        m_init(time(NULL))
    {
    }

    void modify(const string& path)
    {
        map<string, uint64_t>::iterator it = m_etag.find(path);

        if (it != m_etag.end())
        {
            it->second++;
        }
        else
        {
            // First modification
            m_etag[path] = 1;
        }

        m_last_modified[path] = time(NULL);
    }

    time_t last_modified(const string& path) const
    {
        map<string, time_t>::const_iterator it = m_last_modified.find(path);

        if (it != m_last_modified.end())
        {
            return it->second;
        }

        // Resource has not yet been updated
        return m_init;
    }

    time_t etag(const string& path) const
    {
        map<string, uint64_t>::const_iterator it = m_etag.find(path);

        if (it != m_etag.end())
        {
            return it->second;
        }

        // Resource has not yet been updated
        return 0;
    }

private:
    time_t m_init;
    map<string, time_t> m_last_modified;
    map<string, uint64_t> m_etag;
};

Resource::Resource(ResourceCallback cb, int components, ...) :
    m_cb(cb)
{
    va_list args;
    va_start(args, components);

    for (int i = 0; i < components; i++)
    {
        string part = va_arg(args, const char*);
        m_path.push_back(part);
    }
    va_end(args);
}

Resource::~Resource()
{
}

bool Resource::match(const HttpRequest& request) const
{
    bool rval = false;

    if (request.uri_part_count() == m_path.size())
    {
        rval = true;

        for (size_t i = 0; i < request.uri_part_count(); i++)
        {
            if (m_path[i] != request.uri_part(i) &&
                !matching_variable_path(m_path[i], request.uri_part(i)))
            {
                rval = false;
                break;
            }
        }
    }

    return rval;
}

HttpResponse Resource::call(const HttpRequest& request) const
{
    return m_cb(request);
};

bool Resource::matching_variable_path(const string& path, const string& target) const
{
    bool rval = false;

    if (path[0] == ':')
    {
        if ((path == ":service" && service_find(target.c_str())) ||
            (path == ":server" && server_find_by_unique_name(target.c_str())) ||
            (path == ":filter" && filter_def_find(target.c_str())) ||
            (path == ":monitor" && monitor_find(target.c_str())) ||
            (path == ":module" && get_module(target.c_str(), NULL)))
        {
            rval = true;
        }
        else if (path == ":session")
        {
            size_t id = atoi(target.c_str());
            MXS_SESSION* ses = session_get_by_id(id);

            if (ses)
            {
                session_put_ref(ses);
                rval = true;
            }
        }
        else if (path == ":thread")
        {
            char* end;
            int id = strtol(target.c_str(), &end, 10);

            if (*end == '\0' && mxs_worker_get(id))
            {
                rval = true;
            }
        }
    }

    return rval;
}

HttpResponse cb_stop_monitor(const HttpRequest& request)
{
    MXS_MONITOR* monitor = monitor_find(request.uri_part(1).c_str());
    monitorStop(monitor);
    return HttpResponse(MHD_HTTP_NO_CONTENT);
}

HttpResponse cb_start_monitor(const HttpRequest& request)
{
    MXS_MONITOR* monitor = monitor_find(request.uri_part(1).c_str());
    monitorStart(monitor, monitor->parameters);
    return HttpResponse(MHD_HTTP_NO_CONTENT);
}

HttpResponse cb_stop_service(const HttpRequest& request)
{
    SERVICE* service = service_find(request.uri_part(1).c_str());
    serviceStop(service);
    return HttpResponse(MHD_HTTP_NO_CONTENT);
}

HttpResponse cb_start_service(const HttpRequest& request)
{
    SERVICE* service = service_find(request.uri_part(1).c_str());
    serviceStart(service);
    return HttpResponse(MHD_HTTP_NO_CONTENT);
}

HttpResponse cb_create_server(const HttpRequest& request)
{
    json_t* json = request.get_json();

    if (json && runtime_create_server_from_json(json))
    {
        return HttpResponse(MHD_HTTP_NO_CONTENT);
    }

    return HttpResponse(MHD_HTTP_FORBIDDEN);
}

HttpResponse cb_alter_server(const HttpRequest& request)
{
    json_t* json = request.get_json();

    if (json)
    {
        SERVER* server = server_find_by_unique_name(request.uri_part(1).c_str());

        if (server && runtime_alter_server_from_json(server, json))
        {
            return HttpResponse(MHD_HTTP_NO_CONTENT);
        }
    }

    return HttpResponse(MHD_HTTP_FORBIDDEN);
}

HttpResponse cb_create_monitor(const HttpRequest& request)
{
    json_t* json = request.get_json();

    if (json && runtime_create_monitor_from_json(json))
    {
        return HttpResponse(MHD_HTTP_NO_CONTENT);
    }

    return HttpResponse(MHD_HTTP_FORBIDDEN);
}

HttpResponse cb_create_service_listener(const HttpRequest& request)
{
    json_t* json = request.get_json();
    SERVICE* service = service_find(request.uri_part(1).c_str());

    if (service && json && runtime_create_listener_from_json(service, json))
    {
        return HttpResponse(MHD_HTTP_NO_CONTENT);
    }

    return HttpResponse(MHD_HTTP_FORBIDDEN);
}

HttpResponse cb_alter_monitor(const HttpRequest& request)
{
    json_t* json = request.get_json();

    if (json)
    {
        MXS_MONITOR* monitor = monitor_find(request.uri_part(1).c_str());

        if (monitor && runtime_alter_monitor_from_json(monitor, json))
        {
            return HttpResponse(MHD_HTTP_NO_CONTENT);
        }
    }

    return HttpResponse(MHD_HTTP_FORBIDDEN);
}

HttpResponse cb_alter_service(const HttpRequest& request)
{
    json_t* json = request.get_json();

    if (json)
    {
        SERVICE* service = service_find(request.uri_part(1).c_str());

        if (service && runtime_alter_service_from_json(service, json))
        {
            return HttpResponse(MHD_HTTP_NO_CONTENT);
        }
    }

    return HttpResponse(MHD_HTTP_FORBIDDEN);
}

HttpResponse cb_alter_logs(const HttpRequest& request)
{
    json_t* json = request.get_json();

    if (json && runtime_alter_logs_from_json(json))
    {
        return HttpResponse(MHD_HTTP_NO_CONTENT);
    }

    return HttpResponse(MHD_HTTP_FORBIDDEN);
}

HttpResponse cb_delete_server(const HttpRequest& request)
{
    SERVER* server = server_find_by_unique_name(request.uri_part(1).c_str());

    if (server && runtime_destroy_server(server))
    {
        return HttpResponse(MHD_HTTP_NO_CONTENT);
    }

    return HttpResponse(MHD_HTTP_FORBIDDEN);
}

HttpResponse cb_delete_monitor(const HttpRequest& request)
{
    MXS_MONITOR* monitor = monitor_find(request.uri_part(1).c_str());

    if (monitor && runtime_destroy_monitor(monitor))
    {
        return HttpResponse(MHD_HTTP_NO_CONTENT);
    }

    return HttpResponse(MHD_HTTP_FORBIDDEN);
}

HttpResponse cb_all_servers(const HttpRequest& request)
{
    return HttpResponse(MHD_HTTP_OK, server_list_to_json(request.host()));
}

HttpResponse cb_get_server(const HttpRequest& request)
{
    SERVER* server = server_find_by_unique_name(request.uri_part(1).c_str());

    if (server)
    {
        return HttpResponse(MHD_HTTP_OK, server_to_json(server, request.host()));
    }

    return HttpResponse(MHD_HTTP_INTERNAL_SERVER_ERROR);
}

HttpResponse cb_all_services(const HttpRequest& request)
{
    return HttpResponse(MHD_HTTP_OK, service_list_to_json(request.host()));
}

HttpResponse cb_get_service(const HttpRequest& request)
{
    SERVICE* service = service_find(request.uri_part(1).c_str());

    if (service)
    {
        return HttpResponse(MHD_HTTP_OK, service_to_json(service, request.host()));
    }

    return HttpResponse(MHD_HTTP_INTERNAL_SERVER_ERROR);
}

HttpResponse cb_get_service_listeners(const HttpRequest& request)
{
    SERVICE* service = service_find(request.uri_part(1).c_str());
    return HttpResponse(MHD_HTTP_OK, service_listeners_to_json(service, request.host()));
}

HttpResponse cb_all_filters(const HttpRequest& request)
{
    return HttpResponse(MHD_HTTP_OK, filter_list_to_json(request.host()));
}

HttpResponse cb_get_filter(const HttpRequest& request)
{
    MXS_FILTER_DEF* filter = filter_def_find(request.uri_part(1).c_str());

    if (filter)
    {
        return HttpResponse(MHD_HTTP_OK, filter_to_json(filter, request.host()));
    }

    return HttpResponse(MHD_HTTP_INTERNAL_SERVER_ERROR);
}

HttpResponse cb_all_monitors(const HttpRequest& request)
{
    return HttpResponse(MHD_HTTP_OK, monitor_list_to_json(request.host()));
}

HttpResponse cb_get_monitor(const HttpRequest& request)
{
    MXS_MONITOR* monitor = monitor_find(request.uri_part(1).c_str());

    if (monitor)
    {
        return HttpResponse(MHD_HTTP_OK, monitor_to_json(monitor, request.host()));
    }

    return HttpResponse(MHD_HTTP_INTERNAL_SERVER_ERROR);
}

HttpResponse cb_all_sessions(const HttpRequest& request)
{
    return HttpResponse(MHD_HTTP_OK, session_list_to_json(request.host()));
}

HttpResponse cb_get_session(const HttpRequest& request)
{
    int id = atoi(request.uri_part(1).c_str());
    MXS_SESSION* session = session_get_by_id(id);

    if (session)
    {
        json_t* json = session_to_json(session, request.host());
        session_put_ref(session);
        return HttpResponse(MHD_HTTP_OK, json);
    }

    return HttpResponse(MHD_HTTP_NOT_FOUND);
}

HttpResponse cb_maxscale(const HttpRequest& request)
{
    return HttpResponse(MHD_HTTP_OK, config_paths_to_json(request.host()));
}

HttpResponse cb_logs(const HttpRequest& request)
{
    return HttpResponse(MHD_HTTP_OK, mxs_logs_to_json(request.host()));
}

HttpResponse cb_flush(const HttpRequest& request)
{
    int code = MHD_HTTP_INTERNAL_SERVER_ERROR;

    // Flush logs
    if (mxs_log_rotate() == 0)
    {
        code = MHD_HTTP_NO_CONTENT;
    }

    return HttpResponse(code);
}

HttpResponse cb_all_threads(const HttpRequest& request)
{
    return HttpResponse(MHD_HTTP_OK, mxs_worker_list_to_json(request.host()));
}

HttpResponse cb_thread(const HttpRequest& request)
{
    int id = atoi(request.last_uri_part().c_str());
    return HttpResponse(MHD_HTTP_OK, mxs_worker_to_json(request.host(), id));
}

HttpResponse cb_tasks(const HttpRequest& request)
{
    return HttpResponse(MHD_HTTP_OK, hk_tasks_json(request.host()));
}

HttpResponse cb_all_modules(const HttpRequest& request)
{
    return HttpResponse(MHD_HTTP_OK, module_list_to_json(request.host()));
}

HttpResponse cb_module(const HttpRequest& request)
{
    const MXS_MODULE* module = get_module(request.last_uri_part().c_str(), NULL);
    return HttpResponse(MHD_HTTP_OK, module_to_json(module, request.host()));
}

HttpResponse cb_send_ok(const HttpRequest& request)
{
    return HttpResponse(MHD_HTTP_OK);
}

class RootResource
{
    RootResource(const RootResource&);
    RootResource& operator=(const RootResource&);
public:
    typedef std::shared_ptr<Resource> SResource;
    typedef list<SResource> ResourceList;

    RootResource()
    {
        // Special resources required by OPTION etc.
        m_get.push_back(SResource(new Resource(cb_send_ok, 0)));
        m_get.push_back(SResource(new Resource(cb_send_ok, 1, "*")));

        m_get.push_back(SResource(new Resource(cb_all_servers, 1, "servers")));
        m_get.push_back(SResource(new Resource(cb_get_server, 2, "servers", ":server")));

        m_get.push_back(SResource(new Resource(cb_all_services, 1, "services")));
        m_get.push_back(SResource(new Resource(cb_get_service, 2, "services", ":service")));
        m_get.push_back(SResource(new Resource(cb_get_service_listeners, 3,
                                               "services", ":service", "listeners")));

        m_get.push_back(SResource(new Resource(cb_all_filters, 1, "filters")));
        m_get.push_back(SResource(new Resource(cb_get_filter, 2, "filters", ":filter")));

        m_get.push_back(SResource(new Resource(cb_all_monitors, 1, "monitors")));
        m_get.push_back(SResource(new Resource(cb_get_monitor, 2, "monitors", ":monitor")));

        m_get.push_back(SResource(new Resource(cb_all_sessions, 1, "sessions")));
        m_get.push_back(SResource(new Resource(cb_get_session, 2, "sessions", ":session")));

        m_get.push_back(SResource(new Resource(cb_maxscale, 1, "maxscale")));
        m_get.push_back(SResource(new Resource(cb_all_threads, 2, "maxscale", "threads")));
        m_get.push_back(SResource(new Resource(cb_thread, 3, "maxscale", "threads", ":thread")));
        m_get.push_back(SResource(new Resource(cb_logs, 2, "maxscale", "logs")));
        m_get.push_back(SResource(new Resource(cb_tasks, 2, "maxscale", "tasks")));
        m_get.push_back(SResource(new Resource(cb_all_modules, 2, "maxscale", "modules")));
        m_get.push_back(SResource(new Resource(cb_module, 3, "maxscale", "modules", ":module")));

        /** Create new resources */
        m_post.push_back(SResource(new Resource(cb_flush, 3, "maxscale", "logs", "flush")));
        m_post.push_back(SResource(new Resource(cb_create_server, 1, "servers")));
        m_post.push_back(SResource(new Resource(cb_create_monitor, 1, "monitors")));
        m_post.push_back(SResource(new Resource(cb_create_service_listener, 3,
                                                "services", ":service", "listeners")));

        /** Update resources */
        m_put.push_back(SResource(new Resource(cb_alter_server, 2, "servers", ":server")));
        m_put.push_back(SResource(new Resource(cb_alter_monitor, 2, "monitors", ":monitor")));
        m_put.push_back(SResource(new Resource(cb_alter_service, 2, "services", ":service")));
        m_put.push_back(SResource(new Resource(cb_alter_logs, 2, "maxscale", "logs")));

        /** Change resource states */
        m_put.push_back(SResource(new Resource(cb_stop_monitor, 3, "monitors", ":monitor", "stop")));
        m_put.push_back(SResource(new Resource(cb_start_monitor, 3, "monitors", ":monitor", "start")));
        m_put.push_back(SResource(new Resource(cb_stop_service, 3, "services", ":service", "stop")));
        m_put.push_back(SResource(new Resource(cb_start_service, 3, "services", ":service", "start")));

        m_delete.push_back(SResource(new Resource(cb_delete_server, 2, "servers", ":server")));
        m_delete.push_back(SResource(new Resource(cb_delete_monitor, 2, "monitors", ":monitor")));
    }

    ~RootResource()
    {
    }

    ResourceList::const_iterator find_resource(const ResourceList& list, const HttpRequest& request) const
    {
        for (ResourceList::const_iterator it = list.begin(); it != list.end(); it++)
        {
            Resource& r = *(*it);

            if (r.match(request))
            {
                return it;
            }
        }

        return list.end();
    }

    HttpResponse process_request_type(const ResourceList& list, const HttpRequest& request)
    {
        ResourceList::const_iterator it = find_resource(list, request);

        if (it != list.end())
        {
            Resource& r = *(*it);
            return r.call(request);
        }

        return HttpResponse(MHD_HTTP_NOT_FOUND);
    }

    string get_supported_methods(const HttpRequest& request)
    {
        list<string> l;

        if (find_resource(m_get, request) != m_get.end())
        {
            l.push_back(MHD_HTTP_METHOD_GET);
        }
        if (find_resource(m_put, request) != m_put.end())
        {
            l.push_back(MHD_HTTP_METHOD_PUT);
        }
        if (find_resource(m_post, request) != m_post.end())
        {
            l.push_back(MHD_HTTP_METHOD_POST);
        }
        if (find_resource(m_delete, request) != m_delete.end())
        {
            l.push_back(MHD_HTTP_METHOD_DELETE);
        }

        stringstream rval;

        if (l.size() > 0)
        {
            rval << l.front();
            l.pop_front();
        }

        for (list<string>::iterator it = l.begin(); it != l.end(); it++)
        {
            rval << ", " << *it;
        }

        return rval.str();
    }

    HttpResponse process_request(const HttpRequest& request)
    {
        if (request.get_verb() == MHD_HTTP_METHOD_GET)
        {
            return process_request_type(m_get, request);
        }
        else if (request.get_verb() == MHD_HTTP_METHOD_PUT)
        {
            return process_request_type(m_put, request);
        }
        else if (request.get_verb() == MHD_HTTP_METHOD_POST)
        {
            return process_request_type(m_post, request);
        }
        else if (request.get_verb() == MHD_HTTP_METHOD_DELETE)
        {
            return process_request_type(m_delete, request);
        }
        else if (request.get_verb() == MHD_HTTP_METHOD_OPTIONS)
        {
            string methods = get_supported_methods(request);

            if (methods.size() > 0)
            {
                HttpResponse response(MHD_HTTP_OK);
                response.add_header(HTTP_RESPONSE_HEADER_ACCEPT, methods);
                return response;
            }
        }
        else if (request.get_verb() == MHD_HTTP_METHOD_HEAD)
        {
            /** Do a GET and just drop the body of the response */
            HttpResponse response = process_request_type(m_get, request);
            response.drop_response();
            return response;
        }

        return HttpResponse(MHD_HTTP_METHOD_NOT_ALLOWED);
    }

private:

    ResourceList m_get;    /**< GET request handlers */
    ResourceList m_put;    /**< PUT request handlers */
    ResourceList m_post;   /**< POST request handlers */
    ResourceList m_delete; /**< DELETE request handlers */
};

static RootResource resources; /**< Core resource set */
static ResourceWatcher watcher; /**< Modification watcher */
static SpinLock resource_lock;

static bool request_modifies_data(const string& verb)
{
    return verb == MHD_HTTP_METHOD_POST ||
           verb == MHD_HTTP_METHOD_PUT ||
           verb == MHD_HTTP_METHOD_DELETE;
}

static bool request_reads_data(const string& verb)
{
    return verb == MHD_HTTP_METHOD_GET ||
           verb == MHD_HTTP_METHOD_HEAD;
}

bool request_precondition_met(const HttpRequest& request, HttpResponse& response)
{
    bool rval = true;
    string str;
    const string& uri = request.get_uri();

    if ((str = request.get_header(MHD_HTTP_HEADER_IF_MODIFIED_SINCE)).length())
    {
        if (watcher.last_modified(uri) <= http_from_date(str))
        {
            rval = false;
            response = HttpResponse(MHD_HTTP_NOT_MODIFIED);
        }
    }
    else if ((str = request.get_header(MHD_HTTP_HEADER_IF_UNMODIFIED_SINCE)).length())
    {
        if (watcher.last_modified(uri) > http_from_date(str))
        {
            rval = false;
            response = HttpResponse(MHD_HTTP_PRECONDITION_FAILED);
        }
    }
    else if ((str = request.get_header(MHD_HTTP_HEADER_IF_MATCH)).length())
    {
        str = str.substr(1, str.length() - 2);

        if (watcher.etag(uri) != strtol(str.c_str(), NULL, 10))
        {
            rval = false;
            response = HttpResponse(MHD_HTTP_PRECONDITION_FAILED);
        }
    }
    else if ((str = request.get_header(MHD_HTTP_HEADER_IF_NONE_MATCH)).length())
    {
        str = str.substr(1, str.length() - 2);

        if (watcher.etag(uri) == strtol(str.c_str(), NULL, 10))
        {
            rval = false;
            response = HttpResponse(MHD_HTTP_NOT_MODIFIED);
        }
    }

    return rval;
}

HttpResponse resource_handle_request(const HttpRequest& request)
{
    MXS_DEBUG("%s %s %s", request.get_verb().c_str(), request.get_uri().c_str(),
              request.get_json_str().c_str());

    SpinLockGuard guard(resource_lock);
    HttpResponse rval;

    if (request_precondition_met(request, rval))
    {
        rval = resources.process_request(request);

        if (request_modifies_data(request.get_verb()))
        {
            switch (rval.get_code())
            {
            case MHD_HTTP_OK:
            case MHD_HTTP_NO_CONTENT:
            case MHD_HTTP_CREATED:
                watcher.modify(request.get_uri());
                break;

            default:
                break;
            }
        }
        else if (request_reads_data(request.get_verb()))
        {
            const string& uri = request.get_uri();

            rval.add_header(HTTP_RESPONSE_HEADER_LAST_MODIFIED,
                            http_to_date(watcher.last_modified(uri)));

            stringstream ss;
            ss << "\"" << watcher.etag(uri) << "\"";
            rval.add_header(HTTP_RESPONSE_HEADER_ETAG, ss.str());
        }
    }

    return rval;
}
