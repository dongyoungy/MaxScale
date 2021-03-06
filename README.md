# MariaDB MaxScale for DBSeer

This version of MariaDB MaxScale contains a custom router plugin, called **dbseerroute**, that logs necessary information for DBSeer, such as SQL statements, latency, etc.

## Documentation

For information about installing and using MaxScale, please refer to the 
[documentation](Documentation/Documentation-Contents.md) (look at *Building MaxScale from Source Code* as you need to build from source). 

For information about using the dbseerroute plugin, please refer to the [documentation](Documentation/Routers/DBSeerRoute.md).

For information about DBSeer, please visit the DBSeer github [page](https://github.com/barzan/dbseer).

For information about DBSeer middleware, please visit the DBSeer middleware github [page](https://github.com/dongyoungy/dbseer_middleware).

### Running this version of MaxScale locally

If you installed this MaxScale locally, you should specify each path that MaxScale requires in the command line. For example, you may need to execute MaxScale with the following command:

```
./bin/maxscale -f /home/dyoon/maxscale/maxscale.cnf -L /home/dyoon/maxscale/log -D /home/dyoon/maxscale/data -P /home/dyoon/maxscale/run -B /home/dyoon/maxscale/lib64/maxscale --language=/home/dyoon/mariadb/share/english
```

You need to create and specify appropriate directories for log, data and pid directories. You also need to specify `libdir` (**-B**) directory as the library  (e.g., *lib64/maxscale*) directory in your local MaxScale installation path.

## MaxScale by MariaDB Corporation

The MariaDB Corporation MaxScale is an intelligent proxy that allows forwarding of
database statements to one or more database servers using complex rules,
a semantic understanding of the database statements and the roles of
the various servers within the backend cluster of databases.

MaxScale is designed to provide load balancing and high availability
functionality transparently to the applications. In addition it provides
a highly scalable and flexible architecture, with plugin components to
support different protocols and routing decisions.

MaxScale is implemented in C and makes extensive use of the
asynchronous I/O capabilities of the Linux operating system. The epoll
system is used to provide the event driven framework for the input and
output via sockets.

The protocols are implemented as external shared object modules which
can be loaded at runtime. These modules support a fixed interface,
communicating the entries points via a structure consisting of a set of
function pointers. This structure is called the "module object".

The code that routes the queries to the database servers is also loaded
as external shared objects and are referred to as routing modules.

An Google Group exists for MaxScale that can be used to discuss ideas,
issues and communicate with the MaxScale community.
	Send email to maxscale@googlegroups.com
	or use the [forum](http://groups.google.com/forum/#!forum/maxscale) interface
	
Bugs can be reported in the MariaDB Corporation bugs database
	[https://mariadb.atlassian.net/projects/MXS/issues](https://mariadb.atlassian.net/projects/MXS/issues)

