set auto_path [concat [file normalize [file dirname [info script]]/../..] $auto_path]

package require xomti_tcl_internals 0.1

foreach e [list {expr {1.0}} {expr {10}} {expr {wide(10)}} {set a blabla} {list 1 2 3} {concat {1 2 3} {4  5}} {lindex [info commands] 0}] {
    puts "le type de type de \[$e\] est \"[Xomti_Tcl_Internals::get_type [eval $e]]\""
}

