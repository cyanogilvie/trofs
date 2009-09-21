package require Tcl 8.4
set env(TROFS_LIBRARY) \
	[file join [file dirname [file dirname [info script]]] library]
package require -exact trofs 0.4.4
namespace import ::trofs::*
