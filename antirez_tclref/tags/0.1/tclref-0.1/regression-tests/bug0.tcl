load references.so
namespace import ::ref::* ::struct::*

for {set x 0} {$x < 10000} {incr x} {
    getbyref [ref x]
}
puts "TEST PASSED"
