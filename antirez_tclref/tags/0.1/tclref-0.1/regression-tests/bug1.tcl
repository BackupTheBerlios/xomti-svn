load references.so
namespace import ::ref::* ::struct::*

proc foo {} {
    set x [ref {TEST PASSED}]
    uplevel collect
    if {[catch {puts [getbyref $x]}]} {
	puts {TEST FAILED!!!}
    }
}

foo
