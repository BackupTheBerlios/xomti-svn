set auto_path [concat [file normalize [file dirname [info script]]/../../..] $auto_path]

package require antirez_tclref 0.1

namespace import ::ref::* ::struct::*

for {set x 0} {$x < 10000} {incr x} {
    getbyref [ref x]
}
puts "TEST PASSED"

proc foo {} {
    set x [ref {TEST PASSED}]
    uplevel collect
    if {[catch {puts [getbyref $x]}]} {
	puts {TEST FAILED!!!}
    }
}

foo
