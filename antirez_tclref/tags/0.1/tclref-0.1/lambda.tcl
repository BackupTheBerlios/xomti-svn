# A lambda implementation that uses references for the automatic
# memory managment.

load references.so
namespace import ::ref::* ::struct::*

namespace eval ::lambda {}

# Used to generate a different procedure name for every lambda
set ::lambda::id 0

# Creates an anonymous function
proc ::lambda::lambda {args body} {
    incr ::lambda::id

    set procname ::lambda::anonymous$::lambda::id
    proc $procname [concat my $args] $body
    set ref [ref $procname]
    interp alias {} $ref {} $procname $ref
    setfinalizer $ref ::lambda::finalizer
    return $ref
}

proc ::lambda::finalizer ref {
    set procname [getbyref $ref]
    interp alias {} $ref {}
    rename $procname {}
    puts "Lambda $procname collected"
}

# Trivial example
set f [::lambda::lambda x {expr {$x+1}}]

puts [$f 4]
